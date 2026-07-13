#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/un.h>
#include <unistd.h>
#include <sys/socket.h>
#include <time.h>
#include <iostream>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <unordered_set>
#include <unordered_map>
#include <utility>
#include <random>
#include <climits>
#include <cmath>
#include <thread>
#include <mutex>
#include <atomic>

struct TraceEntry
{
    unsigned long key;
    std::string op;
    size_t size;
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

// Convert bytes to human-readable format
std::string bytesToHumanReadable(uint64_t bytes)
{
    const char *suffix[] = {"B", "KB", "MB", "GB", "TB"};
    int suffixIndex = 0;
    double size = static_cast<double>(bytes);

    while (size >= 1024 && suffixIndex < 4)
    {
        size /= 1024;
        suffixIndex++;
    }

    char buffer[100];
    snprintf(buffer, sizeof(buffer), "%.2f %s", size, suffix[suffixIndex]);
    return std::string(buffer);
}

// Calculate the number of digits in a number
size_t countDigits(unsigned long num)
{
    return num == 0 ? 1 : static_cast<size_t>(std::log10(num) + 1);
}

// Thread-safe progress reporting
std::mutex progress_mutex;
std::atomic<size_t> total_success_count(0);

// Function to divide the file into chunks for parallel processing
std::vector<std::pair<size_t, size_t>> divide_file_for_threads(const std::string &filename, int num_threads)
{
    std::vector<std::pair<size_t, size_t>> chunks;

    // Get file size
    std::ifstream file(filename, std::ios::ate | std::ios::binary);
    if (!file.is_open())
    {
        std::cerr << "Failed to open file for size calculation: " << filename << std::endl;
        return chunks;
    }

    size_t file_size = file.tellg();
    file.close();

    // Skip header line
    file.open(filename);
    std::string header;
    std::getline(file, header);
    size_t header_size = file.tellg();
    file.close();

    // Calculate chunk size
    size_t chunk_size = (file_size - header_size) / num_threads;

    // Create chunks
    for (int i = 0; i < num_threads; i++)
    {
        size_t start = header_size + i * chunk_size;
        size_t end = (i == num_threads - 1) ? file_size : start + chunk_size;

        // For all chunks except the first, find the next line boundary
        if (i > 0)
        {
            file.open(filename);
            file.seekg(start);
            char c;
            // Find the next newline to ensure we start at a line boundary
            while (file.get(c) && c != '\n' && start < end)
            {
                start++;
            }
            file.close();
        }

        if (start < end)
        {
            chunks.push_back({start, end});
        }
    }

    return chunks;
}

// Mutex for thread-safe access to the set_sizes map
std::mutex set_sizes_mutex;
std::atomic<uint64_t> total_lines_processed(0);

void process_file_chunk(
    int thread_id,
    const std::string &filename,
    size_t start_pos,
    size_t end_pos,
    std::unordered_map<unsigned long, size_t> &shared_set_sizes,
    std::mutex &set_sizes_mutex,
    std::vector<TraceEntry> &local_operations,
    unsigned long sets_limit)
{
    std::ifstream file(filename);
    if (!file.is_open())
    {
        std::lock_guard<std::mutex> lock(progress_mutex);
        std::cerr << "Thread " << thread_id << ": Failed to open file: " << filename << std::endl;
        return;
    }

    file.seekg(start_pos);

    // If not the first chunk, discard partial line
    if (start_pos > 0)
    {
        std::string partial_line;
        std::getline(file, partial_line);
    }

    std::string line;
    uint64_t local_lines = 0;
    uint64_t local_keys_found = 0;

    while (file.tellg() < end_pos && file.tellg() != -1 && std::getline(file, line))
    {
        local_lines++;
        total_lines_processed.fetch_add(1);

        // Parse the line
        std::stringstream ss(line);
        std::string field;

        // Parse key
        std::getline(ss, field, ',');
        unsigned long key = std::stoul(field);

        // Parse operation
        std::getline(ss, field, ',');
        std::string op = field;

        // Parse size
        std::getline(ss, field, ',');
        size_t size = std::stoul(field);

        // Skip op_count and key_size
        std::getline(ss, field, ',');
        std::getline(ss, field, ',');

        bool should_add = false;
        {
            std::lock_guard<std::mutex> lock(set_sizes_mutex);

            if (shared_set_sizes.size() >= sets_limit)
            {
                break;
            }

            if (op == "SET")
            {
                if (shared_set_sizes.find(key) == shared_set_sizes.end())
                {
                    shared_set_sizes[key] = size;
                    should_add = true;
                }
            }
            else if (op == "GET")
            {
                if (shared_set_sizes.find(key) == shared_set_sizes.end())
                {
                    // Use default size only if size is 0
                    // size_t default_size = 255;
                    size_t default_size = 600;
                    size_t value_size = (size == 0) ? default_size : size;
                    shared_set_sizes[key] = value_size;
                    op = "SET"; // Convert to SET
                    size = value_size;
                    should_add = true;
                }
            }
        }

        if (should_add)
        {
            local_operations.push_back({key, op, size});
            local_keys_found++;
        }

        // Progress reporting
        if (local_lines % 1000000 == 0)
        {
            std::lock_guard<std::mutex> lock(progress_mutex);
            printf("Thread %d: Processed %lu lines, found %lu unique keys\n",
                   thread_id, local_lines, local_keys_found);
        }
    }

    // Final report
    {
        std::lock_guard<std::mutex> lock(progress_mutex);
        printf("Thread %d completed: Processed %lu lines, found %lu unique keys\n",
               thread_id, local_lines, local_keys_found);
    }

    file.close();
}

void worker_thread(int thread_id,
                   const std::vector<TraceEntry> &operations,
                   size_t start_idx,
                   size_t end_idx,
                   const std::string &ipOrSocketPath,
                   int port,
                   bool useUnixSocket)
{
    // Create a local RNG for this thread
    std::random_device rd;
    std::mt19937 rng(rd() + thread_id);             // Use thread_id to ensure different seeds
    std::uniform_int_distribution<int> dist(0, 61); // For charset of size 62

    const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

    // Connect to the server
    int sockfd;
    if (useUnixSocket)
    {
        sockfd = connectUnixSocket(ipOrSocketPath);
    }
    else
    {
        sockfd = connectTcpSocket(ipOrSocketPath, port);
    }

    if (sockfd == -1)
    {
        {
            std::lock_guard<std::mutex> lock(progress_mutex);
            fprintf(stderr, "Thread %d: Failed to connect to server\n", thread_id);
        }
        return;
    }

    {
        std::lock_guard<std::mutex> lock(progress_mutex);
        printf("Thread %d: Connected to server, processing %lu operations\n",
               thread_id, end_idx - start_idx);
    }

    size_t local_success_count = 0;
    size_t last_report = 0;

    for (size_t i = start_idx; i < end_idx; i++)
    {
        const auto &entry = operations[i];

        // Generate random value with thread-local RNG for better performance
        char *randomValue = (char *)malloc(entry.size + 1);
        if (!randomValue)
            continue;

        for (size_t j = 0; j < entry.size; j++)
        {
            randomValue[j] = charset[dist(rng)];
        }
        randomValue[entry.size] = '\0';

        std::string command = "set " + std::to_string(entry.key) + " " + randomValue;
        free(randomValue);

        if (send(sockfd, (command + "\n").c_str(), command.length() + 1, 0) == -1)
        {
            std::lock_guard<std::mutex> lock(progress_mutex);
            fprintf(stderr, "Thread %d: Failed to send command\n", thread_id);
            break;
        }

        // char buffer[32768];
        char buffer[65536];
        int bytesReceived = read(sockfd, buffer, sizeof(buffer) - 1);
        if (bytesReceived <= 0)
        {
            std::lock_guard<std::mutex> lock(progress_mutex);
            fprintf(stderr, "Thread %d: Failed to receive response or connection closed\n", thread_id);
            break;
        }
        buffer[bytesReceived] = '\0';

        // Add a small sleep to avoid flooding
        usleep(10);

        local_success_count++;
        total_success_count.fetch_add(1);

        // Report progress every 100000 operations per thread
        if (local_success_count - last_report >= 100000)
        {
            std::lock_guard<std::mutex> lock(progress_mutex);
            printf("Thread %d: %lu/%lu operations completed\n",
                   thread_id, local_success_count, end_idx - start_idx);
            last_report = local_success_count;
        }
    }

    // Final report for this thread
    {
        std::lock_guard<std::mutex> lock(progress_mutex);
        printf("Thread %d: Completed %lu/%lu operations\n",
               thread_id, local_success_count, end_idx - start_idx);
    }

    // Clean up
    close(sockfd);
}

int main(int argc, char *argv[])
{
    if (argc < 6)
    {
        printf("Usage: %s trace_path socket_path_or_ip port <num of SETs (-1 for all)> <num_threads>\n", argv[0]);
        return 1;
    }

    // Initialize the random number generator
    srand((unsigned)time(NULL));

    // Number of threads
    int num_threads = std::stoi(argv[5]);
    if (num_threads <= 0)
    {
        num_threads = std::thread::hardware_concurrency();
        printf("Using %d threads (system default)\n", num_threads);
    }
    else
    {
        printf("Using %d threads as specified\n", num_threads);
    }

    // Track which keys we've seen SET operations for
    std::unordered_map<unsigned long, size_t> set_sizes;

    // Create a vector to store only the operations we need to perform
    std::vector<TraceEntry> operations_to_perform;

    // Set limit from command line
    unsigned long sets_limit = (argv[4][0] == '-' && argv[4][1] == '1') ? ULONG_MAX : std::stoul(argv[4]);

    // 1. Divide the file into chunks for multi-threaded processing
    auto file_chunks = divide_file_for_threads(argv[1], num_threads);

    if (file_chunks.empty())
    {
        std::cerr << "Failed to divide file into chunks" << std::endl;
        return 1;
    }

    // 2. Create thread-local vectors for operations
    std::vector<std::vector<TraceEntry>> thread_local_operations(file_chunks.size());

    // 3. Launch file processing threads
    std::vector<std::thread> file_threads;
    printf("Starting %lu file processing threads...\n", file_chunks.size());

    for (size_t i = 0; i < file_chunks.size(); i++)
    {
        file_threads.emplace_back(
            process_file_chunk,
            i, // thread ID
            std::string(argv[1]),
            file_chunks[i].first,
            file_chunks[i].second,
            std::ref(set_sizes),
            std::ref(set_sizes_mutex),
            std::ref(thread_local_operations[i]),
            sets_limit);
    }

    // 4. Wait for file processing to complete
    for (auto &t : file_threads)
    {
        t.join();
    }

    // 5. Merge operation vectors from all threads
    for (const auto &local_ops : thread_local_operations)
    {
        operations_to_perform.insert(
            operations_to_perform.end(),
            local_ops.begin(),
            local_ops.end());
    }

    printf("Finished processing trace file. Total lines: %lu, unique keys: %lu\n",
           total_lines_processed.load(), set_sizes.size());

    // Calculate total storage requirements
    uint64_t total_key_bytes = 0;
    uint64_t total_value_bytes = 0;

    for (const auto &key_size_pair : set_sizes)
    {
        total_key_bytes += countDigits(key_size_pair.first);
        total_value_bytes += key_size_pair.second;
    }

    // Display information and prompt user
    printf("Found %lu unique keys to SET\n", operations_to_perform.size());
    printf("Total storage requirement: Keys = %s, Values = %s\n",
           bytesToHumanReadable(total_key_bytes).c_str(),
           bytesToHumanReadable(total_value_bytes).c_str());

    // Shuffle operations for random order
    std::shuffle(operations_to_perform.begin(), operations_to_perform.end(), std::default_random_engine(42));

    // Determine connection type
    std::string ipOrSocketPath = argv[2];
    int port = std::stoi(argv[3]);
    bool useUnixSocket = false;

    if (ipOrSocketPath == "127.0.0.1")
    {
        ipOrSocketPath = "/tmp/server.sock";
        useUnixSocket = true;
    }

    // Create threads
    std::vector<std::thread> threads;
    size_t ops_per_thread = operations_to_perform.size() / num_threads;
    size_t remainder = operations_to_perform.size() % num_threads;

    auto start_time = std::chrono::high_resolution_clock::now();

    printf("Starting %d worker threads...\n", num_threads);

    size_t start_idx = 0;
    for (int i = 0; i < num_threads; i++)
    {
        size_t thread_ops = ops_per_thread + (i < remainder ? 1 : 0);
        size_t end_idx = start_idx + thread_ops;

        threads.emplace_back(worker_thread,
                             i,
                             std::ref(operations_to_perform),
                             start_idx,
                             end_idx,
                             std::ref(ipOrSocketPath),
                             port,
                             useUnixSocket);

        start_idx = end_idx;
    }

    // Join all threads
    for (auto &t : threads)
    {
        t.join();
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

    printf("All threads completed.\n");
    printf("Finished setting %lu/%lu objects successfully in %ld ms\n",
           total_success_count.load(), operations_to_perform.size(), duration);
    printf("Average throughput: %.2f operations/second\n",
           (total_success_count.load() * 1000.0) / duration);

    return 0;
}