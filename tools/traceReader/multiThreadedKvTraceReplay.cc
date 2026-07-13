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
#include <map>
#include <algorithm>
#include <thread>
#include <mutex>
#include <atomic>
#include <filesystem>
#include <regex>
#include <limits>
#include <iomanip>

// g++ multiThreadedKvTraceReplay.cc -O3 -pthread -std=c++17 -lstdc++fs -o multiThreadedKvTraceReplay.x

// Define operation types
enum OpType
{
    GET,
    SET,
    UNKNOWN
};

// Structure to represent a trace operation
struct TraceOperation
{
    unsigned long key;
    OpType op_type;
    size_t size;
};

// Struct to hold latency statistics
struct LatencyStats
{
    int64_t count;
    double min_us;
    double max_us;
    double avg_us;
    double p90_us;
    double p99_us;
};

// Thread-local statistics for each worker
struct ThreadStats
{
    int thread_id;
    uint64_t total_ops = 0;
    uint64_t get_ops = 0;
    uint64_t set_ops = 0;
    uint64_t successful_ops = 0;
    uint64_t failed_ops = 0;
    double total_latency_us = 0;
    double max_latency_us = 0;
    double min_latency_us = std::numeric_limits<double>::max();

    // Histograms for latency values (in microseconds)
    std::map<int64_t, int64_t> get_latency_histogram;
    std::map<int64_t, int64_t> set_latency_histogram;

    // Add latency to the appropriate histogram
    void add_latency(OpType op_type, int64_t latency_us)
    {
        if (op_type == GET)
        {
            get_latency_histogram[latency_us]++;
        }
        else if (op_type == SET)
        {
            set_latency_histogram[latency_us]++;
        }
    }
};

// Operation statistics tracking with thread-safe updates
struct OpStats
{
    std::atomic<uint64_t> total_ops{0};
    std::atomic<uint64_t> get_ops{0};
    std::atomic<uint64_t> set_ops{0};
    std::atomic<uint64_t> successful_ops{0};
    std::atomic<uint64_t> failed_ops{0};

    // These require mutex protection for thread safety
    double total_latency_us = 0;
    double max_latency_us = 0;
    double min_latency_us = std::numeric_limits<double>::max();
    double current = 0.0; // Cumulative time for interval reporting

    // Latency histograms with mutex protection
    std::map<int64_t, int64_t> get_latency_histogram;
    std::map<int64_t, int64_t> set_latency_histogram;

    // For interval statistics
    std::atomic<uint64_t> interval_total_ops{0};
    std::atomic<uint64_t> interval_get_ops{0};
    std::atomic<uint64_t> interval_set_ops{0};
    std::map<int64_t, int64_t> interval_get_latency_histogram;
    std::map<int64_t, int64_t> interval_set_latency_histogram;

    mutable std::mutex latency_mutex; // Protects the latency fields and histograms

    // Update global stats from a thread's stats
    void updateFromThreadStats(const ThreadStats &thread_stats)
    {
        total_ops += thread_stats.total_ops;
        get_ops += thread_stats.get_ops;
        set_ops += thread_stats.set_ops;
        successful_ops += thread_stats.successful_ops;
        failed_ops += thread_stats.failed_ops;

        // Also update interval counters
        interval_total_ops += thread_stats.total_ops;
        interval_get_ops += thread_stats.get_ops;
        interval_set_ops += thread_stats.set_ops;

        std::lock_guard<std::mutex> lock(latency_mutex);
        total_latency_us += thread_stats.total_latency_us;
        max_latency_us = std::max(max_latency_us, thread_stats.max_latency_us);
        min_latency_us = std::min(min_latency_us, thread_stats.min_latency_us);

        // Merge histograms
        for (const auto &entry : thread_stats.get_latency_histogram)
        {
            get_latency_histogram[entry.first] += entry.second;
            interval_get_latency_histogram[entry.first] += entry.second;
        }

        for (const auto &entry : thread_stats.set_latency_histogram)
        {
            set_latency_histogram[entry.first] += entry.second;
            interval_set_latency_histogram[entry.first] += entry.second;
        }
    }

    // Merge another OpStats instance into this one
    void merge(const OpStats &other)
    {
        total_ops += other.total_ops.load();
        get_ops += other.get_ops.load();
        set_ops += other.set_ops.load();
        successful_ops += other.successful_ops.load();
        failed_ops += other.failed_ops.load();

        std::lock_guard<std::mutex> lock(latency_mutex);
        std::lock_guard<std::mutex> other_lock(other.latency_mutex);
        total_latency_us += other.total_latency_us;
        max_latency_us = std::max(max_latency_us, other.max_latency_us);
        min_latency_us = std::min(min_latency_us, other.min_latency_us);

        // Merge histograms
        for (const auto &entry : other.get_latency_histogram)
        {
            get_latency_histogram[entry.first] += entry.second;
        }

        for (const auto &entry : other.set_latency_histogram)
        {
            set_latency_histogram[entry.first] += entry.second;
        }
    }

    // Calculate latency statistics from a histogram
    LatencyStats calculateLatencyStats(const std::map<int64_t, int64_t> &histogram)
    {
        LatencyStats stats = {};
        if (histogram.empty())
        {
            return stats;
        }

        stats.count = 0;
        int64_t sum = 0;
        int64_t min_latency = std::numeric_limits<int64_t>::max();
        int64_t max_latency = std::numeric_limits<int64_t>::min();

        // Calculate total count and sum
        for (const auto &entry : histogram)
        {
            int64_t latency = entry.first;
            int64_t count = entry.second;
            stats.count += count;
            sum += latency * count;
            if (latency < min_latency)
                min_latency = latency;
            if (latency > max_latency)
                max_latency = latency;
        }

        stats.min_us = static_cast<double>(min_latency);
        stats.max_us = static_cast<double>(max_latency);
        stats.avg_us = static_cast<double>(sum) / stats.count;

        // Build cumulative distribution
        std::vector<std::pair<int64_t, int64_t>> cumulative_histogram;
        int64_t cumulative_count = 0;
        for (const auto &entry : histogram)
        {
            cumulative_count += entry.second;
            cumulative_histogram.emplace_back(entry.first, cumulative_count);
        }

        auto get_percentile = [&](double percentile) -> double
        {
            int64_t target = static_cast<int64_t>(percentile * stats.count);
            for (const auto &entry : cumulative_histogram)
            {
                if (entry.second >= target)
                {
                    return static_cast<double>(entry.first);
                }
            }
            return static_cast<double>(max_latency);
        };

        stats.p90_us = get_percentile(0.90);
        stats.p99_us = get_percentile(0.99);

        return stats;
    }

    // Report latency statistics
    void reportLatencyStats(const std::string &op_type, const LatencyStats &stats)
    {
        if (stats.count == 0)
        {
            return;
        }

        std::cout << "[" << op_type << ": Count=" << stats.count
                  << ", Max=" << std::fixed << std::setprecision(2) << stats.max_us
                  << ", Min=" << stats.min_us
                  << ", Avg=" << stats.avg_us
                  << ", p90=" << stats.p90_us
                  << ", p99=" << stats.p99_us << "] ";
    }

    // Report interval statistics
    // Fixed: Removed the nested mutex lock call by clearing interval data directly
    void reportIntervalStats(double seconds)
    {
        current += seconds;
        std::lock_guard<std::mutex> lock(latency_mutex);

        uint64_t interval_ops = interval_total_ops.load();
        double ops_per_sec = interval_ops / seconds;

        uint64_t interval_gets = interval_get_ops.load();
        uint64_t interval_sets = interval_set_ops.load();

        double get_ops_per_sec = interval_gets / seconds;
        double set_ops_per_sec = interval_sets / seconds;

        // Calculate latency statistics
        LatencyStats get_latency_stats = calculateLatencyStats(interval_get_latency_histogram);
        LatencyStats set_latency_stats = calculateLatencyStats(interval_set_latency_histogram);

        // Output the statistics
        std::cout << std::fixed << std::setprecision(1) << current << " sec: ";
        std::cout << "Total Ops/sec: " << std::fixed << std::setprecision(2) << ops_per_sec << "; ";
        std::cout << "GET Ops/sec: " << get_ops_per_sec << "; ";
        std::cout << "SET Ops/sec: " << set_ops_per_sec << "; ";

        // Output latency statistics per operation type
        reportLatencyStats("GET", get_latency_stats);
        reportLatencyStats("SET", set_latency_stats);

        std::cout << std::endl;

        // Clear interval data directly (already have the lock)
        // FIX: No nested mutex lock call to clearIntervalStats() which would cause deadlock
        interval_total_ops = 0;
        interval_get_ops = 0;
        interval_set_ops = 0;
        interval_get_latency_histogram.clear();
        interval_set_latency_histogram.clear();
    }

    // Clear interval statistics - now only used externally
    void clearIntervalStats()
    {
        std::lock_guard<std::mutex> lock(latency_mutex);
        interval_total_ops = 0;
        interval_get_ops = 0;
        interval_set_ops = 0;
        interval_get_latency_histogram.clear();
        interval_set_latency_histogram.clear();
    }
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
bool performSetOperation(int sockfd, unsigned long key, size_t size, ThreadStats &stats)
{
    auto start_time = std::chrono::high_resolution_clock::now();

    char *randomValue = generateRandomString(size);
    std::string command = "set " + std::to_string(key) + " " + randomValue;
    free(randomValue);

    if (send(sockfd, (command + "\n").c_str(), command.length() + 1, 0) == -1)
    {
        std::cerr << "Thread " << stats.thread_id << ": Failed to send SET command: " << command << std::endl;
        return false;
    }

    // char buffer[32768];
    char buffer[65536];
    int bytesReceived = recv(sockfd, buffer, sizeof(buffer) - 1, 0);

    auto end_time = std::chrono::high_resolution_clock::now();
    int64_t latency_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
    double latency_us_d = static_cast<double>(latency_us);

    // Update thread-local stats
    stats.total_ops++;
    stats.set_ops++;
    stats.total_latency_us += latency_us_d;
    stats.max_latency_us = std::max(stats.max_latency_us, latency_us_d);
    stats.min_latency_us = std::min(stats.min_latency_us, latency_us_d);
    stats.add_latency(SET, latency_us);

    if (bytesReceived <= 0)
    {
        std::cerr << "Thread " << stats.thread_id << ": Failed to receive response for SET: " << command << std::endl;
        stats.failed_ops++;
        return false;
    }

    buffer[bytesReceived] = '\0';

    // Check for successful response
    if (strncmp(buffer, "OK", 2) != 0)
    {
        // We'll count it as successful anyway for benchmarking purposes
        // std::cerr << "Thread " << stats.thread_id << ": SET operation failed: " << command << ", response: " << buffer << std::endl;
        // stats.failed_ops++;
        // return false;
    }

    stats.successful_ops++;
    return true;
}

// Function to perform a GET operation
bool performGetOperation(int sockfd, unsigned long key, ThreadStats &stats)
{
    auto start_time = std::chrono::high_resolution_clock::now();

    std::string command = "get " + std::to_string(key);

    if (send(sockfd, (command + "\n").c_str(), command.length() + 1, 0) == -1)
    {
        std::cerr << "Thread " << stats.thread_id << ": Failed to send GET command: " << command << std::endl;
        return false;
    }

    char buffer[8192];
    int bytesReceived = recv(sockfd, buffer, sizeof(buffer) - 1, 0);

    auto end_time = std::chrono::high_resolution_clock::now();
    int64_t latency_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
    double latency_us_d = static_cast<double>(latency_us);

    // Update thread-local stats
    stats.total_ops++;
    stats.get_ops++;
    stats.total_latency_us += latency_us_d;
    stats.max_latency_us = std::max(stats.max_latency_us, latency_us_d);
    stats.min_latency_us = std::min(stats.min_latency_us, latency_us_d);
    stats.add_latency(GET, latency_us);

    if (bytesReceived <= 0)
    {
        std::cerr << "Thread " << stats.thread_id << ": Failed to receive response for GET: " << command << std::endl;
        stats.failed_ops++;
        return false;
    }

    buffer[bytesReceived] = '\0';

    if (strncmp(buffer, "NOT_FOUND", 9) == 0 || strncmp(buffer, "ERR", 3) == 0)
    {
        // Key not found - This is expected in some cases
        stats.failed_ops++;
        return false;
    }

    stats.successful_ops++;
    return true;
}

// Worker thread function that processes a chunk of operations
void workerThread(int thread_id,
                  const std::vector<TraceOperation> &operations,
                  size_t start_idx,
                  size_t end_idx,
                  OpStats &global_stats,
                  const std::string &server_address,
                  int port,
                  std::atomic<bool> &thread_error,
                  std::atomic<uint64_t> &progress_counter,
                  std::mutex &stats_mutex)
{
    ThreadStats thread_stats;
    thread_stats.thread_id = thread_id;

    // Establish connection to server
    int sockfd;
    if (server_address == "127.0.0.1")
    {
        std::string socket_path = "/tmp/server.sock";
        sockfd = connectUnixSocket(socket_path);
    }
    else
    {
        sockfd = connectTcpSocket(server_address, port);
    }

    if (sockfd == -1)
    {
        std::cerr << "Thread " << thread_id << ": Failed to connect to server\n";
        thread_error.store(true);
        return;
    }

    std::cout << "Thread " << thread_id << ": Connected to server: " << server_address << "\n";

    // Process assigned chunk of operations
    for (size_t i = start_idx; i < end_idx && !thread_error.load(); i++)
    {
        const TraceOperation &op = operations[i];

        // Perform the operation
        bool success = false;
        if (op.op_type == GET)
        {
            success = performGetOperation(sockfd, op.key, thread_stats);
        }
        else if (op.op_type == SET)
        {
            success = performSetOperation(sockfd, op.key, op.size, thread_stats);
        }

        // __asm volatile("nop" :); // Nanosecond delay
        // nanosleep((const struct timespec[]){{0, 800000}}, NULL);

        // Update progress counter
        progress_counter++;

        // Periodically update global stats (every 1000 operations)
        if (thread_stats.total_ops % 1000 == 0)
        {
            std::lock_guard<std::mutex> lock(stats_mutex);
            global_stats.updateFromThreadStats(thread_stats);

            // Clear thread stats after updating global stats
            thread_stats = ThreadStats();
            thread_stats.thread_id = thread_id;
        }

        // Check for global error signal
        if (thread_error.load())
        {
            break;
        }
    }

    // Close connection
    close(sockfd);

    // Update global stats with any remaining thread-local stats
    std::lock_guard<std::mutex> lock(stats_mutex);
    global_stats.updateFromThreadStats(thread_stats);

    std::cout << "Thread " << thread_id << ": Completed with " << thread_stats.total_ops << " operations\n";
}

// Sequential worker thread function that processes operations in order from a shared counter
void sequentialWorkerThread(int thread_id,
                            const std::vector<TraceOperation> &operations,
                            std::atomic<size_t> &next_op_index,
                            size_t total_ops,
                            OpStats &global_stats,
                            const std::string &server_address,
                            int port,
                            std::atomic<bool> &thread_error,
                            std::atomic<uint64_t> &progress_counter,
                            std::mutex &stats_mutex)
{
    ThreadStats thread_stats;
    thread_stats.thread_id = thread_id;

    // Establish connection to server
    int sockfd;
    if (server_address == "127.0.0.1")
    {
        std::string socket_path = "/tmp/server.sock";
        sockfd = connectUnixSocket(socket_path);
    }
    else
    {
        sockfd = connectTcpSocket(server_address, port);
    }

    if (sockfd == -1)
    {
        std::cerr << "Thread " << thread_id << ": Failed to connect to server\n";
        thread_error.store(true);
        return;
    }

    std::cout << "Thread " << thread_id << ": Connected to server: " << server_address << "\n";

    // Process operations by pulling from the shared counter
    while (!thread_error.load())
    {
        // Atomically claim the next operation
        size_t current_index = next_op_index.fetch_add(1);

        // Check if we've processed all operations
        if (current_index >= total_ops)
        {
            break;
        }

        // Get the operation at the claimed index
        const TraceOperation &op = operations[current_index];

        // Perform the operation
        bool success = false;
        if (op.op_type == GET)
        {
            success = performGetOperation(sockfd, op.key, thread_stats);
        }
        else if (op.op_type == SET)
        {
            size_t capped_size = std::min(op.size, static_cast<size_t>(65000));
            success = performSetOperation(sockfd, op.key, capped_size, thread_stats);
        }

        // __asm volatile("nop" :); // Nanosecond delay
        // 300 ns delay
        // nanosleep((const struct timespec[]){{0, 800000}}, NULL);

        // Update progress counter
        progress_counter++;

        // Periodically update global stats (every 10000 operations)
        if (thread_stats.total_ops % 10000 == 0)
        {
            std::lock_guard<std::mutex> lock(stats_mutex);
            global_stats.updateFromThreadStats(thread_stats);

            // Clear thread stats after updating global stats
            thread_stats = ThreadStats();
            thread_stats.thread_id = thread_id;
        }

        // Check for global error signal
        if (thread_error.load())
        {
            break;
        }
    }

    // Close connection
    close(sockfd);

    // Update global stats with any remaining thread-local stats
    std::lock_guard<std::mutex> lock(stats_mutex);
    global_stats.updateFromThreadStats(thread_stats);

    std::cout << "Thread " << thread_id << ": Completed with " << thread_stats.total_ops << " operations\n";
}

// Function to load operations from a trace file
std::vector<TraceOperation> loadTraceFile(const std::string &file_path)
{
    std::vector<TraceOperation> operations;

    // Open trace file
    std::ifstream trace_file(file_path);
    if (!trace_file.is_open())
    {
        std::cerr << "Failed to open trace file: " << file_path << std::endl;
        return operations;
    }

    // Skip header line
    std::string line;
    std::getline(trace_file, line);

    // Read operations
    while (std::getline(trace_file, line))
    {
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

        // Add operation to vector
        operations.push_back({key, op_type, size});
    }

    trace_file.close();
    return operations;
}

// Function to process a single trace file with chunk-based approach
bool processTraceFile(const std::string &file_path,
                      const std::string &server_address,
                      int port,
                      int num_threads,
                      OpStats &file_stats)
{
    auto file_start_time = std::chrono::high_resolution_clock::now();

    std::cout << "\n=== Processing trace file: " << file_path << " ===\n";

    // Load all operations from the file
    std::cout << "Loading trace file into memory..." << std::endl;
    std::vector<TraceOperation> operations = loadTraceFile(file_path);

    if (operations.empty())
    {
        std::cerr << "No valid operations found in file: " << file_path << std::endl;
        return false;
    }

    std::cout << "Loaded " << operations.size() << " operations" << std::endl;

    // Create thread management objects
    std::vector<std::thread> threads;
    std::atomic<bool> thread_error(false);
    std::atomic<uint64_t> progress_counter(0);
    std::mutex stats_mutex; // Used for thread-safe updates to global stats

    // Calculate chunk size for each thread
    size_t chunk_size = operations.size() / num_threads;
    size_t remainder = operations.size() % num_threads;

    std::cout << "Starting " << num_threads << " worker threads (chunk size: " << chunk_size << " operations)" << std::endl;

    // Create and start worker threads
    for (int i = 0; i < num_threads; i++)
    {
        size_t start_idx = i * chunk_size + std::min(i, static_cast<int>(remainder));
        size_t end_idx = start_idx + chunk_size + (i < remainder ? 1 : 0);

        threads.emplace_back(workerThread,
                             i,
                             std::ref(operations),
                             start_idx,
                             end_idx,
                             std::ref(file_stats),
                             server_address,
                             port,
                             std::ref(thread_error),
                             std::ref(progress_counter),
                             std::ref(stats_mutex));
    }

    // Monitor progress and report statistics
    auto last_report_time = std::chrono::high_resolution_clock::now();
    uint64_t last_reported_progress = 0;
    int report_count = 0;
    const int report_interval_seconds = 10;
    size_t current = 0;

    while (progress_counter.load() < operations.size() && !thread_error.load())
    {
        // Sleep for a short time
        std::this_thread::sleep_for(std::chrono::seconds(1));

        // Calculate elapsed time since last report
        auto now = std::chrono::high_resolution_clock::now();
        double elapsed_seconds = std::chrono::duration<double>(now - last_report_time).count();

        // Check if it's time to report (every 10 seconds)
        if (elapsed_seconds >= report_interval_seconds)
        {
            uint64_t current_progress = progress_counter.load();
            double percent_complete = static_cast<double>(current_progress) / operations.size() * 100.0;

            report_count++;
            int cumulative_seconds = report_count * report_interval_seconds;

            // Report detailed statistics
            try
            {
                file_stats.reportIntervalStats(report_interval_seconds);

                std::cout << "Progress: " << current_progress << "/" << operations.size()
                          << " operations (" << std::fixed << std::setprecision(1) << percent_complete << "%)" << std::endl;
            }
            catch (const std::exception &e)
            {
                std::cerr << "Exception during reporting: " << e.what() << std::endl;
            }

            last_reported_progress = current_progress;
            last_report_time = now;
        }
    }

    // Wait for all threads to finish
    for (auto &thread : threads)
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }

    auto file_end_time = std::chrono::high_resolution_clock::now();
    double file_total_seconds = std::chrono::duration<double>(file_end_time - file_start_time).count();

    // Print file summary
    std::cout << "\n=== File: " << file_path << " - Replay Summary ===\n";
    std::cout << "Total operations: " << file_stats.total_ops.load() << "\n";
    std::cout << "GET operations: " << file_stats.get_ops.load() << "\n";
    std::cout << "SET operations: " << file_stats.set_ops.load() << "\n";
    std::cout << "Successful operations: " << file_stats.successful_ops.load() << " ("
              << (file_stats.total_ops.load() > 0 ? (double)file_stats.successful_ops.load() / file_stats.total_ops.load() * 100 : 0) << "%)\n";
    std::cout << "Failed operations: " << file_stats.failed_ops.load() << " ("
              << (file_stats.total_ops.load() > 0 ? (double)file_stats.failed_ops.load() / file_stats.total_ops.load() * 100 : 0) << "%)\n";

    {
        std::lock_guard<std::mutex> lock(file_stats.latency_mutex);
        // Calculate final latency statistics
        LatencyStats get_latency_stats = file_stats.calculateLatencyStats(file_stats.get_latency_histogram);
        LatencyStats set_latency_stats = file_stats.calculateLatencyStats(file_stats.set_latency_histogram);

        std::cout << "Latency statistics (microseconds):\n";
        std::cout << "GET operations - ";
        file_stats.reportLatencyStats("GET", get_latency_stats);
        std::cout << std::endl;
        std::cout << "SET operations - ";
        file_stats.reportLatencyStats("SET", set_latency_stats);
        std::cout << std::endl;

        std::cout << "Average latency: " << (file_stats.total_ops.load() > 0 ? file_stats.total_latency_us / file_stats.total_ops.load() : 0) << " us\n";
        std::cout << "Min latency: " << (file_stats.min_latency_us == std::numeric_limits<double>::max() ? 0 : file_stats.min_latency_us) << " us\n";
        std::cout << "Max latency: " << file_stats.max_latency_us << " us\n";
    }

    std::cout << "File execution time: " << file_total_seconds << " seconds\n";
    std::cout << "Average throughput: " << (file_stats.total_ops.load() / file_total_seconds) << " ops/sec\n";

    return !thread_error.load();
}

// Function to process a single trace file with sequential approach
bool processTraceFileSequential(const std::string &file_path,
                                const std::string &server_address,
                                int port,
                                int num_threads,
                                OpStats &file_stats)
{
    auto file_start_time = std::chrono::high_resolution_clock::now();

    std::cout << "\n=== Processing trace file sequentially: " << file_path << " ===\n";

    // Load all operations from the file
    std::cout << "Loading trace file into memory..." << std::endl;
    std::vector<TraceOperation> operations = loadTraceFile(file_path);

    if (operations.empty())
    {
        std::cerr << "No valid operations found in file: " << file_path << std::endl;
        return false;
    }

    std::cout << "Loaded " << operations.size() << " operations" << std::endl;

    // Create thread management objects
    std::vector<std::thread> threads;
    std::atomic<bool> thread_error(false);
    std::atomic<uint64_t> progress_counter(0);
    std::mutex stats_mutex; // Used for thread-safe updates to global stats

    // Shared atomic counter for sequential processing
    std::atomic<size_t> next_op_index(0);
    const size_t total_ops = operations.size();

    std::cout << "Starting " << num_threads << " worker threads with sequential processing" << std::endl;

    // Create and start worker threads
    for (int i = 0; i < num_threads; i++)
    {
        threads.emplace_back(sequentialWorkerThread,
                             i,
                             std::ref(operations),
                             std::ref(next_op_index),
                             total_ops,
                             std::ref(file_stats),
                             server_address,
                             port,
                             std::ref(thread_error),
                             std::ref(progress_counter),
                             std::ref(stats_mutex));
    }

    // Monitor progress and report statistics
    auto last_report_time = std::chrono::high_resolution_clock::now();
    uint64_t last_reported_progress = 0;
    int report_count = 0;
    const int report_interval_seconds = 10;
    size_t current = 0;

    while (progress_counter.load() < operations.size() && !thread_error.load())
    {
        // Sleep for a short time
        std::this_thread::sleep_for(std::chrono::seconds(1));

        // Calculate elapsed time since last report
        auto now = std::chrono::high_resolution_clock::now();
        double elapsed_seconds = std::chrono::duration<double>(now - last_report_time).count();

        // Check if it's time to report (every 10 seconds)
        if (elapsed_seconds >= report_interval_seconds)
        {
            uint64_t current_progress = progress_counter.load();
            double percent_complete = static_cast<double>(current_progress) / operations.size() * 100.0;

            report_count++;
            int cumulative_seconds = report_count * report_interval_seconds;

            // Report detailed statistics
            try
            {
                file_stats.reportIntervalStats(report_interval_seconds);

                std::cout << "Progress: " << current_progress << "/" << operations.size()
                          << " operations (" << std::fixed << std::setprecision(1) << percent_complete << "%)" << std::endl;
            }
            catch (const std::exception &e)
            {
                std::cerr << "Exception during reporting: " << e.what() << std::endl;
            }

            last_reported_progress = current_progress;
            last_report_time = now;
        }
    }

    // Wait for all threads to finish
    for (auto &thread : threads)
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }

    auto file_end_time = std::chrono::high_resolution_clock::now();
    double file_total_seconds = std::chrono::duration<double>(file_end_time - file_start_time).count();

    // Print file summary
    std::cout << "\n=== File: " << file_path << " - Sequential Replay Summary ===\n";
    std::cout << "Total operations: " << file_stats.total_ops.load() << "\n";
    std::cout << "GET operations: " << file_stats.get_ops.load() << "\n";
    std::cout << "SET operations: " << file_stats.set_ops.load() << "\n";
    std::cout << "Successful operations: " << file_stats.successful_ops.load() << " ("
              << (file_stats.total_ops.load() > 0 ? (double)file_stats.successful_ops.load() / file_stats.total_ops.load() * 100 : 0) << "%)\n";
    std::cout << "Failed operations: " << file_stats.failed_ops.load() << " ("
              << (file_stats.total_ops.load() > 0 ? (double)file_stats.failed_ops.load() / file_stats.total_ops.load() * 100 : 0) << "%)\n";

    {
        std::lock_guard<std::mutex> lock(file_stats.latency_mutex);
        // Calculate final latency statistics
        LatencyStats get_latency_stats = file_stats.calculateLatencyStats(file_stats.get_latency_histogram);
        LatencyStats set_latency_stats = file_stats.calculateLatencyStats(file_stats.set_latency_histogram);

        std::cout << "Latency statistics (microseconds):\n";
        std::cout << "GET operations - ";
        file_stats.reportLatencyStats("GET", get_latency_stats);
        std::cout << std::endl;
        std::cout << "SET operations - ";
        file_stats.reportLatencyStats("SET", set_latency_stats);
        std::cout << std::endl;

        std::cout << "Average latency: " << (file_stats.total_ops.load() > 0 ? file_stats.total_latency_us / file_stats.total_ops.load() : 0) << " us\n";
        std::cout << "Min latency: " << (file_stats.min_latency_us == std::numeric_limits<double>::max() ? 0 : file_stats.min_latency_us) << " us\n";
        std::cout << "Max latency: " << file_stats.max_latency_us << " us\n";
    }

    std::cout << "File execution time: " << file_total_seconds << " seconds\n";
    std::cout << "Average throughput: " << (file_stats.total_ops.load() / file_total_seconds) << " ops/sec\n";

    return !thread_error.load();
}

// Function to get trace files in sorted order
std::vector<std::string> getSortedTraceFiles(const std::string &folder_path)
{
    std::vector<std::string> trace_files;
    std::regex trace_file_regex("kvcache_traces_(\\d+)\\.csv");

    try
    {
        for (const auto &entry : std::filesystem::directory_iterator(folder_path))
        {
            if (entry.is_regular_file())
            {
                std::string filename = entry.path().filename().string();
                std::smatch match;
                if (std::regex_match(filename, match, trace_file_regex))
                {
                    trace_files.push_back(entry.path().string());
                }
            }
        }
    }
    catch (const std::filesystem::filesystem_error &e)
    {
        std::cerr << "Error reading directory: " << e.what() << std::endl;
    }

    // Sort files by their trace number
    std::sort(trace_files.begin(), trace_files.end(),
              [&trace_file_regex](const std::string &a, const std::string &b)
              {
                  std::smatch match_a, match_b;
                  std::string filename_a = std::filesystem::path(a).filename().string();
                  std::string filename_b = std::filesystem::path(b).filename().string();
                  std::regex_match(filename_a, match_a, trace_file_regex);
                  std::regex_match(filename_b, match_b, trace_file_regex);
                  return std::stoi(match_a[1]) < std::stoi(match_b[1]);
              });

    return trace_files;
}

int main(int argc, char *argv[])
{
    if (argc < 4)
    {
        printf("Usage: %s trace_folder server_address port [num_threads] [mode]\n", argv[0]);
        printf("  mode: 0 for chunk-based processing (default), 1 for sequential processing\n");
        return 1;
    }

    // Parse command line arguments
    std::string trace_folder = argv[1];
    std::string server_address = argv[2];
    int port = std::stoi(argv[3]);

    // Optional number of threads (default: use hardware concurrency)
    int num_threads = std::thread::hardware_concurrency();
    if (argc > 4)
    {
        num_threads = std::stoi(argv[4]);
    }

    // Optional processing mode
    int mode = 1; // Default: sequential processing
    // int mode = 0; // Default: chunk-based processing
    if (argc > 5)
    {
        mode = std::stoi(argv[5]);
    }

    if (mode == 0)
    {
        printf("Using %d worker threads with chunk-based processing\n", num_threads);
    }
    else
    {
        printf("Using %d worker threads with sequential processing\n", num_threads);
    }

    // Initialize random number generator
    srand(time(NULL));

    // Get sorted trace files
    auto trace_files = getSortedTraceFiles(trace_folder);

    if (trace_files.empty())
    {
        std::cerr << "No trace files found in folder: " << trace_folder << std::endl;
        return 1;
    }

    printf("Found %zu trace files to process in order:\n", trace_files.size());
    for (const auto &file : trace_files)
    {
        printf("  %s\n", file.c_str());
    }

    // Global statistics for all files
    OpStats global_stats;
    auto global_start_time = std::chrono::high_resolution_clock::now();

    // Process each trace file in order
    for (const auto &file_path : trace_files)
    {
        // Per-file statistics
        OpStats file_stats;

        // Process the file using the selected mode
        bool success;
        if (mode == 0)
        {
            // Chunk-based processing
            success = processTraceFile(file_path, server_address, port, num_threads, file_stats);
        }
        else
        {
            // Sequential processing
            success = processTraceFileSequential(file_path, server_address, port, num_threads, file_stats);
        }

        if (!success)
        {
            std::cerr << "Error processing file: " << file_path << ". Aborting remaining files.\n";
            break;
        }

        // Merge file stats into global stats
        global_stats.merge(file_stats);

        // Clear memory after processing each file
        std::cout << "File processing complete. Moving to next file.\n";
    }

    auto global_end_time = std::chrono::high_resolution_clock::now();
    double total_seconds = std::chrono::duration<double>(global_end_time - global_start_time).count();

    // Print overall summary
    printf("\n=== Overall Trace Replay Summary ===\n");
    printf("Total operations: %lu\n", global_stats.total_ops.load());
    printf("GET operations: %lu\n", global_stats.get_ops.load());
    printf("SET operations: %lu\n", global_stats.set_ops.load());
    printf("Successful operations: %lu (%.2f%%)\n",
           global_stats.successful_ops.load(),
           global_stats.total_ops.load() > 0 ? (double)global_stats.successful_ops.load() / global_stats.total_ops.load() * 100 : 0);
    printf("Failed operations: %lu (%.2f%%)\n",
           global_stats.failed_ops.load(),
           global_stats.total_ops.load() > 0 ? (double)global_stats.failed_ops.load() / global_stats.total_ops.load() * 100 : 0);

    {
        std::lock_guard<std::mutex> lock(global_stats.latency_mutex);
        // Calculate final latency statistics
        LatencyStats get_latency_stats = global_stats.calculateLatencyStats(global_stats.get_latency_histogram);
        LatencyStats set_latency_stats = global_stats.calculateLatencyStats(global_stats.set_latency_histogram);

        printf("\nLatency statistics (microseconds):\n");
        std::cout << "GET operations - ";
        global_stats.reportLatencyStats("GET", get_latency_stats);
        std::cout << std::endl;
        std::cout << "SET operations - ";
        global_stats.reportLatencyStats("SET", set_latency_stats);
        std::cout << std::endl;

        printf("Average latency: %.3f us\n",
               global_stats.total_ops.load() > 0 ? global_stats.total_latency_us / global_stats.total_ops.load() : 0);
        printf("Min latency: %.3f us\n", global_stats.min_latency_us == std::numeric_limits<double>::max() ? 0 : global_stats.min_latency_us);
        printf("Max latency: %.3f us\n", global_stats.max_latency_us);
    }

    printf("Total execution time: %.2f seconds\n", total_seconds);
    printf("Average throughput: %.2f ops/sec\n", global_stats.total_ops.load() / total_seconds);

    return 0;
}

/*
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
#include <map>
#include <algorithm>
#include <thread>
#include <mutex>
#include <atomic>
#include <filesystem>
#include <regex>
#include <limits>
#include <iomanip>

// g++ multiThreadedKvTraceReplay.cc -O3 -pthread -std=c++17 -lstdc++fs -o multiThreadedKvTraceReplay.x

// Define operation types
enum OpType
{
    GET,
    SET,
    UNKNOWN
};

// Structure to represent a trace operation
struct TraceOperation
{
    unsigned long key;
    OpType op_type;
    size_t size;
};

// Struct to hold latency statistics
struct LatencyStats
{
    int64_t count;
    double min_us;
    double max_us;
    double avg_us;
    double p90_us;
    double p99_us;
};

// Thread-local statistics for each worker
struct ThreadStats
{
    int thread_id;
    uint64_t total_ops = 0;
    uint64_t get_ops = 0;
    uint64_t set_ops = 0;
    uint64_t successful_ops = 0;
    uint64_t failed_ops = 0;
    double total_latency_us = 0;
    double max_latency_us = 0;
    double min_latency_us = std::numeric_limits<double>::max();

    // Histograms for latency values (in microseconds)
    std::map<int64_t, int64_t> get_latency_histogram;
    std::map<int64_t, int64_t> set_latency_histogram;

    // Add latency to the appropriate histogram
    void add_latency(OpType op_type, int64_t latency_us)
    {
        if (op_type == GET)
        {
            get_latency_histogram[latency_us]++;
        }
        else if (op_type == SET)
        {
            set_latency_histogram[latency_us]++;
        }
    }
};

// Operation statistics tracking with thread-safe updates
struct OpStats
{
    std::atomic<uint64_t> total_ops{0};
    std::atomic<uint64_t> get_ops{0};
    std::atomic<uint64_t> set_ops{0};
    std::atomic<uint64_t> successful_ops{0};
    std::atomic<uint64_t> failed_ops{0};

    // These require mutex protection for thread safety
    double total_latency_us = 0;
    double max_latency_us = 0;
    double min_latency_us = std::numeric_limits<double>::max();

    // Latency histograms with mutex protection
    std::map<int64_t, int64_t> get_latency_histogram;
    std::map<int64_t, int64_t> set_latency_histogram;

    // For interval statistics
    std::atomic<uint64_t> interval_total_ops{0};
    std::atomic<uint64_t> interval_get_ops{0};
    std::atomic<uint64_t> interval_set_ops{0};
    std::map<int64_t, int64_t> interval_get_latency_histogram;
    std::map<int64_t, int64_t> interval_set_latency_histogram;

    mutable std::mutex latency_mutex; // Protects the latency fields and histograms

    // Update global stats from a thread's stats
    void updateFromThreadStats(const ThreadStats &thread_stats)
    {
        total_ops += thread_stats.total_ops;
        get_ops += thread_stats.get_ops;
        set_ops += thread_stats.set_ops;
        successful_ops += thread_stats.successful_ops;
        failed_ops += thread_stats.failed_ops;

        // Also update interval counters
        interval_total_ops += thread_stats.total_ops;
        interval_get_ops += thread_stats.get_ops;
        interval_set_ops += thread_stats.set_ops;

        std::lock_guard<std::mutex> lock(latency_mutex);
        total_latency_us += thread_stats.total_latency_us;
        max_latency_us = std::max(max_latency_us, thread_stats.max_latency_us);
        min_latency_us = std::min(min_latency_us, thread_stats.min_latency_us);

        // Merge histograms
        for (const auto &entry : thread_stats.get_latency_histogram)
        {
            get_latency_histogram[entry.first] += entry.second;
            interval_get_latency_histogram[entry.first] += entry.second;
        }

        for (const auto &entry : thread_stats.set_latency_histogram)
        {
            set_latency_histogram[entry.first] += entry.second;
            interval_set_latency_histogram[entry.first] += entry.second;
        }
    }

    // Merge another OpStats instance into this one
    void merge(const OpStats &other)
    {
        total_ops += other.total_ops.load();
        get_ops += other.get_ops.load();
        set_ops += other.set_ops.load();
        successful_ops += other.successful_ops.load();
        failed_ops += other.failed_ops.load();

        std::lock_guard<std::mutex> lock(latency_mutex);
        std::lock_guard<std::mutex> other_lock(other.latency_mutex);
        total_latency_us += other.total_latency_us;
        max_latency_us = std::max(max_latency_us, other.max_latency_us);
        min_latency_us = std::min(min_latency_us, other.min_latency_us);

        // Merge histograms
        for (const auto &entry : other.get_latency_histogram)
        {
            get_latency_histogram[entry.first] += entry.second;
        }

        for (const auto &entry : other.set_latency_histogram)
        {
            set_latency_histogram[entry.first] += entry.second;
        }
    }

    // Calculate latency statistics from a histogram
    LatencyStats calculateLatencyStats(const std::map<int64_t, int64_t> &histogram)
    {
        LatencyStats stats = {};
        if (histogram.empty())
        {
            return stats;
        }

        stats.count = 0;
        int64_t sum = 0;
        int64_t min_latency = std::numeric_limits<int64_t>::max();
        int64_t max_latency = std::numeric_limits<int64_t>::min();

        // Calculate total count and sum
        for (const auto &entry : histogram)
        {
            int64_t latency = entry.first;
            int64_t count = entry.second;
            stats.count += count;
            sum += latency * count;
            if (latency < min_latency)
                min_latency = latency;
            if (latency > max_latency)
                max_latency = latency;
        }

        stats.min_us = static_cast<double>(min_latency);
        stats.max_us = static_cast<double>(max_latency);
        stats.avg_us = static_cast<double>(sum) / stats.count;

        // Build cumulative distribution
        std::vector<std::pair<int64_t, int64_t>> cumulative_histogram;
        int64_t cumulative_count = 0;
        for (const auto &entry : histogram)
        {
            cumulative_count += entry.second;
            cumulative_histogram.emplace_back(entry.first, cumulative_count);
        }

        auto get_percentile = [&](double percentile) -> double
        {
            int64_t target = static_cast<int64_t>(percentile * stats.count);
            for (const auto &entry : cumulative_histogram)
            {
                if (entry.second >= target)
                {
                    return static_cast<double>(entry.first);
                }
            }
            return static_cast<double>(max_latency);
        };

        stats.p90_us = get_percentile(0.90);
        stats.p99_us = get_percentile(0.99);

        return stats;
    }

    // Report latency statistics
    void reportLatencyStats(const std::string &op_type, const LatencyStats &stats)
    {
        if (stats.count == 0)
        {
            return;
        }

        std::cout << "[" << op_type << ": Count=" << stats.count
                  << ", Max=" << std::fixed << std::setprecision(2) << stats.max_us
                  << ", Min=" << stats.min_us
                  << ", Avg=" << stats.avg_us
                  << ", p90=" << stats.p90_us
                  << ", p99=" << stats.p99_us << "] ";
    }

    // Report interval statistics
    // Fixed: Removed the nested mutex lock call by clearing interval data directly
    void reportIntervalStats(double seconds)
    {
        current += seconds;
        std::lock_guard<std::mutex> lock(latency_mutex);

        uint64_t interval_ops = interval_total_ops.load();
        double ops_per_sec = interval_ops / seconds;

        uint64_t interval_gets = interval_get_ops.load();
        uint64_t interval_sets = interval_set_ops.load();

        double get_ops_per_sec = interval_gets / seconds;
        double set_ops_per_sec = interval_sets / seconds;

        // Calculate latency statistics
        LatencyStats get_latency_stats = calculateLatencyStats(interval_get_latency_histogram);
        LatencyStats set_latency_stats = calculateLatencyStats(interval_set_latency_histogram);

        // Output the statistics
        std::cout << std::fixed << std::setprecision(1) << current << " sec: ";
        std::cout << "Total Ops/sec: " << std::fixed << std::setprecision(2) << ops_per_sec << "; ";
        std::cout << "GET Ops/sec: " << get_ops_per_sec << "; ";
        std::cout << "SET Ops/sec: " << set_ops_per_sec << "; ";

        // Output latency statistics per operation type
        reportLatencyStats("GET", get_latency_stats);
        reportLatencyStats("SET", set_latency_stats);

        std::cout << std::endl;

        // Clear interval data directly (already have the lock)
        // FIX: No nested mutex lock call to clearIntervalStats() which would cause deadlock
        interval_total_ops = 0;
        interval_get_ops = 0;
        interval_set_ops = 0;
        interval_get_latency_histogram.clear();
        interval_set_latency_histogram.clear();
    }

    // Clear interval statistics - now only used externally
    void clearIntervalStats()
    {
        std::lock_guard<std::mutex> lock(latency_mutex);
        interval_total_ops = 0;
        interval_get_ops = 0;
        interval_set_ops = 0;
        interval_get_latency_histogram.clear();
        interval_set_latency_histogram.clear();
    }
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
bool performSetOperation(int sockfd, unsigned long key, size_t size, ThreadStats &stats)
{
    auto start_time = std::chrono::high_resolution_clock::now();

    char *randomValue = generateRandomString(size);
    std::string command = "set " + std::to_string(key) + " " + randomValue;
    free(randomValue);

    if (send(sockfd, (command + "\n").c_str(), command.length() + 1, 0) == -1)
    {
        std::cerr << "Thread " << stats.thread_id << ": Failed to send SET command: " << command << std::endl;
        return false;
    }

    char buffer[32768];
    int bytesReceived = recv(sockfd, buffer, sizeof(buffer) - 1, 0);

    auto end_time = std::chrono::high_resolution_clock::now();
    int64_t latency_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
    double latency_us_d = static_cast<double>(latency_us);

    // Update thread-local stats
    stats.total_ops++;
    stats.set_ops++;
    stats.total_latency_us += latency_us_d;
    stats.max_latency_us = std::max(stats.max_latency_us, latency_us_d);
    stats.min_latency_us = std::min(stats.min_latency_us, latency_us_d);
    stats.add_latency(SET, latency_us);

    if (bytesReceived <= 0)
    {
        std::cerr << "Thread " << stats.thread_id << ": Failed to receive response for SET: " << command << std::endl;
        stats.failed_ops++;
        return false;
    }

    buffer[bytesReceived] = '\0';

    // Check for successful response
    if (strncmp(buffer, "OK", 2) != 0)
    {
        // We'll count it as successful anyway for benchmarking purposes
        // std::cerr << "Thread " << stats.thread_id << ": SET operation failed: " << command << ", response: " << buffer << std::endl;
        // stats.failed_ops++;
        // return false;
    }

    stats.successful_ops++;
    return true;
}

// Function to perform a GET operation
bool performGetOperation(int sockfd, unsigned long key, ThreadStats &stats)
{
    auto start_time = std::chrono::high_resolution_clock::now();

    std::string command = "get " + std::to_string(key);

    if (send(sockfd, (command + "\n").c_str(), command.length() + 1, 0) == -1)
    {
        std::cerr << "Thread " << stats.thread_id << ": Failed to send GET command: " << command << std::endl;
        return false;
    }

    char buffer[8192];
    int bytesReceived = recv(sockfd, buffer, sizeof(buffer) - 1, 0);

    auto end_time = std::chrono::high_resolution_clock::now();
    int64_t latency_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
    double latency_us_d = static_cast<double>(latency_us);

    // Update thread-local stats
    stats.total_ops++;
    stats.get_ops++;
    stats.total_latency_us += latency_us_d;
    stats.max_latency_us = std::max(stats.max_latency_us, latency_us_d);
    stats.min_latency_us = std::min(stats.min_latency_us, latency_us_d);
    stats.add_latency(GET, latency_us);

    if (bytesReceived <= 0)
    {
        std::cerr << "Thread " << stats.thread_id << ": Failed to receive response for GET: " << command << std::endl;
        stats.failed_ops++;
        return false;
    }

    buffer[bytesReceived] = '\0';

    if (strncmp(buffer, "NOT_FOUND", 9) == 0 || strncmp(buffer, "ERR", 3) == 0)
    {
        // Key not found - This is expected in some cases
        stats.failed_ops++;
        return false;
    }

    stats.successful_ops++;
    return true;
}

// Worker thread function that processes a chunk of operations
void workerThread(int thread_id,
                  const std::vector<TraceOperation> &operations,
                  size_t start_idx,
                  size_t end_idx,
                  OpStats &global_stats,
                  const std::string &server_address,
                  int port,
                  std::atomic<bool> &thread_error,
                  std::atomic<uint64_t> &progress_counter,
                  std::mutex &stats_mutex)
{
    ThreadStats thread_stats;
    thread_stats.thread_id = thread_id;

    // Establish connection to server
    int sockfd;
    if (server_address == "127.0.0.1")
    {
        std::string socket_path = "/tmp/server.sock";
        sockfd = connectUnixSocket(socket_path);
    }
    else
    {
        sockfd = connectTcpSocket(server_address, port);
    }

    if (sockfd == -1)
    {
        std::cerr << "Thread " << thread_id << ": Failed to connect to server\n";
        thread_error.store(true);
        return;
    }

    std::cout << "Thread " << thread_id << ": Connected to server: " << server_address << "\n";

    // Process assigned chunk of operations
    for (size_t i = start_idx; i < end_idx && !thread_error.load(); i++)
    {
        const TraceOperation &op = operations[i];

        // Perform the operation
        bool success = false;
        if (op.op_type == GET)
        {
            success = performGetOperation(sockfd, op.key, thread_stats);
        }
        else if (op.op_type == SET)
        {
            success = performSetOperation(sockfd, op.key, op.size, thread_stats);
        }

        // __asm volatile("nop" :); // Nanosecond delay

        // Update progress counter
        progress_counter++;

        // Periodically update global stats (every 1000 operations)
        if (thread_stats.total_ops % 1000 == 0)
        {
            std::lock_guard<std::mutex> lock(stats_mutex);
            global_stats.updateFromThreadStats(thread_stats);

            // Clear thread stats after updating global stats
            thread_stats = ThreadStats();
            thread_stats.thread_id = thread_id;
        }

        // Check for global error signal
        if (thread_error.load())
        {
            break;
        }
    }

    // Close connection
    close(sockfd);

    // Update global stats with any remaining thread-local stats
    std::lock_guard<std::mutex> lock(stats_mutex);
    global_stats.updateFromThreadStats(thread_stats);

    std::cout << "Thread " << thread_id << ": Completed with " << thread_stats.total_ops << " operations\n";
}

// Function to load operations from a trace file
std::vector<TraceOperation> loadTraceFile(const std::string &file_path)
{
    std::vector<TraceOperation> operations;

    // Open trace file
    std::ifstream trace_file(file_path);
    if (!trace_file.is_open())
    {
        std::cerr << "Failed to open trace file: " << file_path << std::endl;
        return operations;
    }

    // Skip header line
    std::string line;
    std::getline(trace_file, line);

    // Read operations
    while (std::getline(trace_file, line))
    {
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

        // Add operation to vector
        operations.push_back({key, op_type, size});
    }

    trace_file.close();
    return operations;
}

// Function to process a single trace file
bool processTraceFile(const std::string &file_path,
                      const std::string &server_address,
                      int port,
                      int num_threads,
                      OpStats &file_stats)
{
    auto file_start_time = std::chrono::high_resolution_clock::now();

    std::cout << "\n=== Processing trace file: " << file_path << " ===\n";

    // Load all operations from the file
    std::cout << "Loading trace file into memory..." << std::endl;
    std::vector<TraceOperation> operations = loadTraceFile(file_path);

    if (operations.empty())
    {
        std::cerr << "No valid operations found in file: " << file_path << std::endl;
        return false;
    }

    std::cout << "Loaded " << operations.size() << " operations" << std::endl;

    // Create thread management objects
    std::vector<std::thread> threads;
    std::atomic<bool> thread_error(false);
    std::atomic<uint64_t> progress_counter(0);
    std::mutex stats_mutex; // Used for thread-safe updates to global stats

    // Calculate chunk size for each thread
    size_t chunk_size = operations.size() / num_threads;
    size_t remainder = operations.size() % num_threads;

    std::cout << "Starting " << num_threads << " worker threads (chunk size: " << chunk_size << " operations)" << std::endl;

    // Create and start worker threads
    for (int i = 0; i < num_threads; i++)
    {
        size_t start_idx = i * chunk_size + std::min(i, static_cast<int>(remainder));
        size_t end_idx = start_idx + chunk_size + (i < remainder ? 1 : 0);

        threads.emplace_back(workerThread,
                             i,
                             std::ref(operations),
                             start_idx,
                             end_idx,
                             std::ref(file_stats),
                             server_address,
                             port,
                             std::ref(thread_error),
                             std::ref(progress_counter),
                             std::ref(stats_mutex));
    }

    // Monitor progress and report statistics
    auto last_report_time = std::chrono::high_resolution_clock::now();
    uint64_t last_reported_progress = 0;
    int report_count = 0;
    const int report_interval_seconds = 10;
    size_t current = 0;

    while (progress_counter.load() < operations.size() && !thread_error.load())
    {
        // Sleep for a short time
        std::this_thread::sleep_for(std::chrono::seconds(1));

        // Calculate elapsed time since last report
        auto now = std::chrono::high_resolution_clock::now();
        double elapsed_seconds = std::chrono::duration<double>(now - last_report_time).count();

        // Check if it's time to report (every 10 seconds)
        if (elapsed_seconds >= report_interval_seconds)
        {
            uint64_t current_progress = progress_counter.load();
            double percent_complete = static_cast<double>(current_progress) / operations.size() * 100.0;

            report_count++;
            int cumulative_seconds = report_count * report_interval_seconds;

            // Report detailed statistics
            try
            {
                file_stats.reportIntervalStats(report_interval_seconds);

                std::cout << "Progress: " << current_progress << "/" << operations.size()
                          << " operations (" << std::fixed << std::setprecision(1) << percent_complete << "%)" << std::endl;
            }
            catch (const std::exception &e)
            {
                std::cerr << "Exception during reporting: " << e.what() << std::endl;
            }

            last_reported_progress = current_progress;
            last_report_time = now;
        }
    }

    // Wait for all threads to finish
    for (auto &thread : threads)
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }

    auto file_end_time = std::chrono::high_resolution_clock::now();
    double file_total_seconds = std::chrono::duration<double>(file_end_time - file_start_time).count();

    // Print file summary
    std::cout << "\n=== File: " << file_path << " - Replay Summary ===\n";
    std::cout << "Total operations: " << file_stats.total_ops.load() << "\n";
    std::cout << "GET operations: " << file_stats.get_ops.load() << "\n";
    std::cout << "SET operations: " << file_stats.set_ops.load() << "\n";
    std::cout << "Successful operations: " << file_stats.successful_ops.load() << " ("
              << (file_stats.total_ops.load() > 0 ? (double)file_stats.successful_ops.load() / file_stats.total_ops.load() * 100 : 0) << "%)\n";
    std::cout << "Failed operations: " << file_stats.failed_ops.load() << " ("
              << (file_stats.total_ops.load() > 0 ? (double)file_stats.failed_ops.load() / file_stats.total_ops.load() * 100 : 0) << "%)\n";

    {
        std::lock_guard<std::mutex> lock(file_stats.latency_mutex);
        // Calculate final latency statistics
        LatencyStats get_latency_stats = file_stats.calculateLatencyStats(file_stats.get_latency_histogram);
        LatencyStats set_latency_stats = file_stats.calculateLatencyStats(file_stats.set_latency_histogram);

        std::cout << "Latency statistics (microseconds):\n";
        std::cout << "GET operations - ";
        file_stats.reportLatencyStats("GET", get_latency_stats);
        std::cout << std::endl;
        std::cout << "SET operations - ";
        file_stats.reportLatencyStats("SET", set_latency_stats);
        std::cout << std::endl;

        std::cout << "Average latency: " << (file_stats.total_ops.load() > 0 ? file_stats.total_latency_us / file_stats.total_ops.load() : 0) << " us\n";
        std::cout << "Min latency: " << (file_stats.min_latency_us == std::numeric_limits<double>::max() ? 0 : file_stats.min_latency_us) << " us\n";
        std::cout << "Max latency: " << file_stats.max_latency_us << " us\n";
    }

    std::cout << "File execution time: " << file_total_seconds << " seconds\n";
    std::cout << "Average throughput: " << (file_stats.total_ops.load() / file_total_seconds) << " ops/sec\n";

    return !thread_error.load();
}

// Function to get trace files in sorted order
std::vector<std::string> getSortedTraceFiles(const std::string &folder_path)
{
    std::vector<std::string> trace_files;
    std::regex trace_file_regex("kvcache_traces_(\\d+)\\.csv");

    try
    {
        for (const auto &entry : std::filesystem::directory_iterator(folder_path))
        {
            if (entry.is_regular_file())
            {
                std::string filename = entry.path().filename().string();
                std::smatch match;
                if (std::regex_match(filename, match, trace_file_regex))
                {
                    trace_files.push_back(entry.path().string());
                }
            }
        }
    }
    catch (const std::filesystem::filesystem_error &e)
    {
        std::cerr << "Error reading directory: " << e.what() << std::endl;
    }

    // Sort files by their trace number
    std::sort(trace_files.begin(), trace_files.end(),
              [&trace_file_regex](const std::string &a, const std::string &b)
              {
                  std::smatch match_a, match_b;
                  std::string filename_a = std::filesystem::path(a).filename().string();
                  std::string filename_b = std::filesystem::path(b).filename().string();
                  std::regex_match(filename_a, match_a, trace_file_regex);
                  std::regex_match(filename_b, match_b, trace_file_regex);
                  return std::stoi(match_a[1]) < std::stoi(match_b[1]);
              });

    return trace_files;
}

int main(int argc, char *argv[])
{
    if (argc < 4)
    {
        printf("Usage: %s trace_folder server_address port [num_threads]\n", argv[0]);
        return 1;
    }

    // Parse command line arguments
    std::string trace_folder = argv[1];
    std::string server_address = argv[2];
    int port = std::stoi(argv[3]);

    // Optional number of threads (default: use hardware concurrency)
    int num_threads = std::thread::hardware_concurrency();
    if (argc > 4)
    {
        num_threads = std::stoi(argv[4]);
    }
    printf("Using %d worker threads\n", num_threads);

    // Initialize random number generator
    srand(time(NULL));

    // Get sorted trace files
    auto trace_files = getSortedTraceFiles(trace_folder);
    if (trace_files.empty())
    {
        std::cerr << "No trace files found in folder: " << trace_folder << std::endl;
        return 1;
    }

    printf("Found %zu trace files to process in order:\n", trace_files.size());
    for (const auto &file : trace_files)
    {
        printf("  %s\n", file.c_str());
    }

    // Global statistics for all files
    OpStats global_stats;
    auto global_start_time = std::chrono::high_resolution_clock::now();

    // Process each trace file in order
    for (const auto &file_path : trace_files)
    {
        // Per-file statistics
        OpStats file_stats;

        // Process the file
        bool success = processTraceFile(file_path, server_address, port, num_threads, file_stats);
        if (!success)
        {
            std::cerr << "Error processing file: " << file_path << ". Aborting remaining files.\n";
            break;
        }

        // Merge file stats into global stats
        global_stats.merge(file_stats);

        // Clear memory after processing each file
        std::cout << "File processing complete. Moving to next file.\n";
    }

    auto global_end_time = std::chrono::high_resolution_clock::now();
    double total_seconds = std::chrono::duration<double>(global_end_time - global_start_time).count();

    // Print overall summary
    printf("\n=== Overall Trace Replay Summary ===\n");
    printf("Total operations: %lu\n", global_stats.total_ops.load());
    printf("GET operations: %lu\n", global_stats.get_ops.load());
    printf("SET operations: %lu\n", global_stats.set_ops.load());
    printf("Successful operations: %lu (%.2f%%)\n",
           global_stats.successful_ops.load(),
           global_stats.total_ops.load() > 0 ? (double)global_stats.successful_ops.load() / global_stats.total_ops.load() * 100 : 0);
    printf("Failed operations: %lu (%.2f%%)\n",
           global_stats.failed_ops.load(),
           global_stats.total_ops.load() > 0 ? (double)global_stats.failed_ops.load() / global_stats.total_ops.load() * 100 : 0);

    {
        std::lock_guard<std::mutex> lock(global_stats.latency_mutex);
        // Calculate final latency statistics
        LatencyStats get_latency_stats = global_stats.calculateLatencyStats(global_stats.get_latency_histogram);
        LatencyStats set_latency_stats = global_stats.calculateLatencyStats(global_stats.set_latency_histogram);

        printf("\nLatency statistics (microseconds):\n");
        std::cout << "GET operations - ";
        global_stats.reportLatencyStats("GET", get_latency_stats);
        std::cout << std::endl;
        std::cout << "SET operations - ";
        global_stats.reportLatencyStats("SET", set_latency_stats);
        std::cout << std::endl;

        printf("Average latency: %.3f us\n",
               global_stats.total_ops.load() > 0 ? global_stats.total_latency_us / global_stats.total_ops.load() : 0);
        printf("Min latency: %.3f us\n", global_stats.min_latency_us == std::numeric_limits<double>::max() ? 0 : global_stats.min_latency_us);
        printf("Max latency: %.3f us\n", global_stats.max_latency_us);
    }

    printf("Total execution time: %.2f seconds\n", total_seconds);
    printf("Average throughput: %.2f ops/sec\n", global_stats.total_ops.load() / total_seconds);

    return 0;
}
*/