#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/un.h>
#include <unistd.h>
#include <sys/socket.h>
#include <time.h>
#include <chrono>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>

// Define operation types
enum OpType
{
    GET,
    SET,
    UNKNOWN
};

// Operation statistics tracking
struct OpStats
{
    uint64_t total_ops = 0;
    uint64_t get_ops = 0;
    uint64_t set_ops = 0;
    uint64_t successful_ops = 0;
    uint64_t failed_ops = 0;
    double total_latency_ms = 0;
    double max_latency_ms = 0;
    double min_latency_ms = std::numeric_limits<double>::max();
};

// Function to connect to the server using UNIX domain socket
int connectUnixSocket(const std::string &socketPath)
{
    int sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sockfd == -1)
    {
        std::cerr << "Failed to create UNIX domain socket\n";
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socketPath.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) == -1)
    {
        std::cerr << "Failed to connect to UNIX domain socket\n";
        close(sockfd);
        return -1;
    }

    return sockfd;
}

// Function to connect to the server using TCP socket
int connectTcpSocket(const std::string &ip, int port)
{
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1)
    {
        std::cerr << "Failed to create TCP socket\n";
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0)
    {
        std::cerr << "Invalid IP address format\n";
        close(sockfd);
        return -1;
    }

    if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) == -1)
    {
        std::cerr << "Failed to connect to TCP socket\n";
        close(sockfd);
        return -1;
    }

    return sockfd;
}

// Function to generate a random string of a given length
char *generateRandomString(size_t size)
{
    char *str = (char *)malloc(size + 1);
    if (!str)
        return NULL;

    const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for (size_t i = 0; i < size; i++)
    {
        int key = rand() % (int)(sizeof(charset) - 1);
        str[i] = charset[key];
    }
    str[size] = '\0';
    return str;
}

// Function to perform a SET operation
bool performSetOperation(int sockfd, unsigned long key, size_t size, OpStats &stats)
{
    auto start_time = std::chrono::high_resolution_clock::now();

    char *randomValue = generateRandomString(size);
    std::string command = "set " + std::to_string(key) + " " + randomValue;
    free(randomValue);

    if (send(sockfd, (command + "\n").c_str(), command.length() + 1, 0) == -1)
    {
        std::cerr << "Failed to send SET command: " << command << std::endl;
        return false;
    }

    char buffer[32768];
    int bytesReceived = recv(sockfd, buffer, sizeof(buffer) - 1, 0);

    auto end_time = std::chrono::high_resolution_clock::now();
    double latency_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();

    // Update stats
    stats.total_ops++;
    stats.set_ops++;
    stats.total_latency_ms += latency_ms;
    stats.max_latency_ms = std::max(stats.max_latency_ms, latency_ms);
    stats.min_latency_ms = std::min(stats.min_latency_ms, latency_ms);

    if (bytesReceived <= 0)
    {
        std::cerr << "Failed to receive response for SET: " << command << std::endl;
        stats.failed_ops++;
        return false;
    }

    buffer[bytesReceived] = '\0';

    // if (strncmp(buffer, "OK", 2) != 0)
    // {
    //     std::cerr << "SET operation failed: " << command << ", response: " << buffer << std::endl;
    //     stats.failed_ops++;
    //     return false;
    // }

    stats.successful_ops++;
    return true;
}

// Function to perform a GET operation
bool performGetOperation(int sockfd, unsigned long key, OpStats &stats)
{
    auto start_time = std::chrono::high_resolution_clock::now();

    std::string command = "get " + std::to_string(key);

    if (send(sockfd, (command + "\n").c_str(), command.length() + 1, 0) == -1)
    {
        std::cerr << "Failed to send GET command: " << command << std::endl;
        return false;
    }

    char buffer[8192];
    int bytesReceived = recv(sockfd, buffer, sizeof(buffer) - 1, 0);

    auto end_time = std::chrono::high_resolution_clock::now();
    double latency_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();

    // Update stats
    stats.total_ops++;
    stats.get_ops++;
    stats.total_latency_ms += latency_ms;
    stats.max_latency_ms = std::max(stats.max_latency_ms, latency_ms);
    stats.min_latency_ms = std::min(stats.min_latency_ms, latency_ms);

    if (bytesReceived <= 0)
    {
        std::cerr << "Failed to receive response for GET: " << command << std::endl;
        stats.failed_ops++;
        return false;
    }

    buffer[bytesReceived] = '\0';

    if (strncmp(buffer, "NOT_FOUND", 9) == 0)
    {
        std::cerr << "Key not found: " << key << std::endl;
        stats.failed_ops++;
        return false;
    }

    stats.successful_ops++;
    return true;
}

int main(int argc, char *argv[])
{
    if (argc < 4)
    {
        printf("Usage: %s trace_path socket_path_or_ip port [ops_per_second]\n", argv[0]);
        return 1;
    }

    // Parse command line arguments
    std::string trace_path = argv[1];
    std::string server_address = argv[2];
    int port = std::stoi(argv[3]);

    // Optional rate limiting
    int ops_per_second = -1; // Default: no rate limiting
    if (argc > 4)
    {
        ops_per_second = std::stoi(argv[4]);
        printf("Rate limiting enabled: %d ops/second\n", ops_per_second);
    }

    // Initialize random number generator
    srand(time(NULL));

    // Connect to server
    int sockfd;
    if (server_address != "127.0.0.1")
    {
        sockfd = connectTcpSocket(server_address, port);
    }
    else
    {
        server_address = "/tmp/server.sock";
        sockfd = connectUnixSocket(server_address);
    }

    if (sockfd == -1)
    {
        std::cerr << "Failed to connect to server\n";
        return 1;
    }

    printf("Connected to server: %s\n", server_address.c_str());

    // Open trace file
    std::ifstream trace_file(trace_path);
    if (!trace_file.is_open())
    {
        std::cerr << "Failed to open trace file: " << trace_path << std::endl;
        close(sockfd);
        return 1;
    }

    // Stats tracking
    OpStats stats;

    // Calculate microseconds per operation if rate limiting is enabled
    int64_t microseconds_per_op = -1;
    if (ops_per_second > 0)
    {
        microseconds_per_op = 1000000 / ops_per_second;
    }

    // Skip header line
    std::string line;
    std::getline(trace_file, line);

    auto replay_start_time = std::chrono::high_resolution_clock::now();
    auto next_op_time = replay_start_time;

    printf("Starting trace replay...\n");

    // Process trace file line by line
    uint64_t line_count = 0;
    while (std::getline(trace_file, line))
    {
        line_count++;
        std::stringstream ss(line);
        std::string field;

        // Parse key
        std::getline(ss, field, ',');
        unsigned long key = std::stoul(field);

        // Parse operation
        std::getline(ss, field, ',');
        std::string op_str = field;
        OpType op_type = UNKNOWN;
        if (op_str == "GET")
            op_type = GET;
        else if (op_str == "SET")
            op_type = SET;
        else
            continue; // Skip unknown operations

        // Parse size (only needed for SET)
        size_t size = 0;
        if (op_type == SET)
        {
            std::getline(ss, field, ',');
            size = std::stoul(field);
        }

        // Rate limiting: sleep if we're ahead of schedule
        if (microseconds_per_op > 0)
        {
            auto now = std::chrono::high_resolution_clock::now();
            if (now < next_op_time)
            {
                auto sleep_time = std::chrono::duration_cast<std::chrono::microseconds>(
                    next_op_time - now);
                usleep(sleep_time.count());
            }
            next_op_time += std::chrono::microseconds(microseconds_per_op);
        }

        // Perform the operation
        if (op_type == GET)
        {
            performGetOperation(sockfd, key, stats);
        }
        else if (op_type == SET)
        {
            performSetOperation(sockfd, key, size, stats);
        }

        // Print progress every 1000000 operations
        if (line_count % 1000000 == 0)
        {
            auto elapsed = std::chrono::high_resolution_clock::now() - replay_start_time;
            double seconds = std::chrono::duration<double>(elapsed).count();
            double ops_per_sec = line_count / seconds;

            printf("Progress: %lu operations, %.2f ops/sec\n", line_count, ops_per_sec);
        }
    }

    auto replay_end_time = std::chrono::high_resolution_clock::now();
    double total_seconds = std::chrono::duration<double>(replay_end_time - replay_start_time).count();

    // Print summary
    printf("\n=== Trace Replay Summary ===\n");
    printf("Total operations: %lu\n", stats.total_ops);
    printf("GET operations: %lu\n", stats.get_ops);
    printf("SET operations: %lu\n", stats.set_ops);
    printf("Successful operations: %lu (%.2f%%)\n",
           stats.successful_ops,
           stats.total_ops > 0 ? (double)stats.successful_ops / stats.total_ops * 100 : 0);
    printf("Failed operations: %lu (%.2f%%)\n",
           stats.failed_ops,
           stats.total_ops > 0 ? (double)stats.failed_ops / stats.total_ops * 100 : 0);
    printf("Average latency: %.3f ms\n",
           stats.total_ops > 0 ? stats.total_latency_ms / stats.total_ops : 0);
    printf("Min latency: %.3f ms\n", stats.min_latency_ms == std::numeric_limits<double>::max() ? 0 : stats.min_latency_ms);
    printf("Max latency: %.3f ms\n", stats.max_latency_ms);
    printf("Total execution time: %.2f seconds\n", total_seconds);
    printf("Average throughput: %.2f ops/sec\n", stats.total_ops / total_seconds);

    // Clean up
    trace_file.close();
    close(sockfd);

    return 0;
}