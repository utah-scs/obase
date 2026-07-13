#include <iostream>
#include <string>
#include <vector>
#include <queue>
#include <sstream>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <cerrno>
#include <sys/stat.h>
#include <zstd.h>
#include <algorithm>
#include <unordered_map>
#include <map>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <iomanip>
#include <limits>
#include <arpa/inet.h>
#include <sys/un.h>
#include <unistd.h>
#include <sys/socket.h>
#include <time.h>

// g++ -o twitterZstReplay.x twitterZstReplay.cc -lzstd -pthread

// Define rstatus enum for zstd reader
typedef enum
{
    OK = 0,
    ERR = -1,
    MY_EOF = -2
} rstatus;

// Define operation types
enum OpType
{
    GET,
    SET,
    DEL,
    INCR,
    UNKNOWN
};

// Simple logging macros
#define ERROR(fmt, ...) fprintf(stderr, "ERROR: " fmt, ##__VA_ARGS__)
#define WARN(fmt, ...) fprintf(stderr, "WARN: " fmt, ##__VA_ARGS__)
#define DEBUG(fmt, ...) fprintf(stderr, "DEBUG: " fmt, ##__VA_ARGS__)

#define LINE_DELIM '\n'

// Structure to represent a trace operation
struct TraceOperation
{
    std::string key;
    OpType op_type;
    size_t value_size;
    std::string client_id;
    time_t timestamp;
    int ttl;
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
    uint64_t del_ops = 0;
    uint64_t successful_ops = 0;
    uint64_t failed_ops = 0;
    double total_latency_us = 0;
    double max_latency_us = 0;
    double min_latency_us = std::numeric_limits<double>::max();

    // Histograms for latency values (in microseconds)
    std::map<int64_t, int64_t> get_latency_histogram;
    std::map<int64_t, int64_t> set_latency_histogram;
    std::map<int64_t, int64_t> del_latency_histogram;

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
        else if (op_type == DEL)
        {
            del_latency_histogram[latency_us]++;
        }
    }
};

// Operation statistics tracking with thread-safe updates
struct OpStats
{
    std::atomic<uint64_t> total_ops{0};
    std::atomic<uint64_t> get_ops{0};
    std::atomic<uint64_t> set_ops{0};
    std::atomic<uint64_t> del_ops{0};
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
    std::map<int64_t, int64_t> del_latency_histogram;

    // For interval statistics
    std::atomic<uint64_t> interval_total_ops{0};
    std::atomic<uint64_t> interval_get_ops{0};
    std::atomic<uint64_t> interval_set_ops{0};
    std::atomic<uint64_t> interval_del_ops{0};
    std::map<int64_t, int64_t> interval_get_latency_histogram;
    std::map<int64_t, int64_t> interval_set_latency_histogram;
    std::map<int64_t, int64_t> interval_del_latency_histogram;

    mutable std::mutex latency_mutex; // Protects the latency fields and histograms

    // Update global stats from a thread's stats
    void updateFromThreadStats(const ThreadStats &thread_stats)
    {
        total_ops += thread_stats.total_ops;
        get_ops += thread_stats.get_ops;
        set_ops += thread_stats.set_ops;
        del_ops += thread_stats.del_ops;
        successful_ops += thread_stats.successful_ops;
        failed_ops += thread_stats.failed_ops;

        // Also update interval counters
        interval_total_ops += thread_stats.total_ops;
        interval_get_ops += thread_stats.get_ops;
        interval_set_ops += thread_stats.set_ops;
        interval_del_ops += thread_stats.del_ops;

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

        for (const auto &entry : thread_stats.del_latency_histogram)
        {
            del_latency_histogram[entry.first] += entry.second;
            interval_del_latency_histogram[entry.first] += entry.second;
        }
    }

    // Merge another OpStats instance into this one
    void merge(const OpStats &other)
    {
        total_ops += other.total_ops.load();
        get_ops += other.get_ops.load();
        set_ops += other.set_ops.load();
        del_ops += other.del_ops.load();
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

        for (const auto &entry : other.del_latency_histogram)
        {
            del_latency_histogram[entry.first] += entry.second;
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
    void reportIntervalStats(double seconds)
    {
        current += seconds;
        std::lock_guard<std::mutex> lock(latency_mutex);

        uint64_t interval_ops = interval_total_ops.load();
        double ops_per_sec = interval_ops / seconds;

        uint64_t interval_gets = interval_get_ops.load();
        uint64_t interval_sets = interval_set_ops.load();
        uint64_t interval_dels = interval_del_ops.load();

        double get_ops_per_sec = interval_gets / seconds;
        double set_ops_per_sec = interval_sets / seconds;
        double del_ops_per_sec = interval_dels / seconds;

        // Calculate latency statistics
        LatencyStats get_latency_stats = calculateLatencyStats(interval_get_latency_histogram);
        LatencyStats set_latency_stats = calculateLatencyStats(interval_set_latency_histogram);
        LatencyStats del_latency_stats = calculateLatencyStats(interval_del_latency_histogram);

        // Output the statistics
        std::cout << std::fixed << std::setprecision(1) << current << " sec: ";
        std::cout << "Total Ops/sec: " << std::fixed << std::setprecision(2) << ops_per_sec << "; ";
        std::cout << "GET Ops/sec: " << get_ops_per_sec << "; ";
        std::cout << "SET Ops/sec: " << set_ops_per_sec << "; ";
        std::cout << "DEL Ops/sec: " << del_ops_per_sec << "; ";

        // Output latency statistics per operation type
        reportLatencyStats("GET", get_latency_stats);
        reportLatencyStats("SET", set_latency_stats);
        reportLatencyStats("DEL", del_latency_stats);

        std::cout << std::endl;

        // Clear interval data directly (already have the lock)
        interval_total_ops = 0;
        interval_get_ops = 0;
        interval_set_ops = 0;
        interval_del_ops = 0;
        interval_get_latency_histogram.clear();
        interval_set_latency_histogram.clear();
        interval_del_latency_histogram.clear();
    }

    // Clear interval statistics
    void clearIntervalStats()
    {
        std::lock_guard<std::mutex> lock(latency_mutex);
        interval_total_ops = 0;
        interval_get_ops = 0;
        interval_set_ops = 0;
        interval_del_ops = 0;
        interval_get_latency_histogram.clear();
        interval_set_latency_histogram.clear();
        interval_del_latency_histogram.clear();
    }
};

// Thread-safe bounded queue for operation processing
template <typename T>
class BoundedQueue
{
private:
    std::queue<T> queue;
    mutable std::mutex mutex;
    std::condition_variable not_empty_cv;
    std::condition_variable not_full_cv;
    size_t capacity;
    bool shutdown;

public:
    // Constructor with queue capacity
    BoundedQueue(size_t max_size) : capacity(max_size), shutdown(false) {}

    // Add an item to the queue, blocking if queue is full
    void push(const T &item)
    {
        std::unique_lock<std::mutex> lock(mutex);

        // Wait until there's room in the queue or shutdown is signaled
        not_full_cv.wait(lock, [this]
                         { return queue.size() < capacity || shutdown; });

        // If shutdown, don't push
        if (shutdown)
            return;

        queue.push(item);
        not_empty_cv.notify_one();
    }

    // Get an item from the queue, blocking if queue is empty
    bool pop(T &item)
    {
        std::unique_lock<std::mutex> lock(mutex);

        // Wait until the queue has items or shutdown is signaled
        not_empty_cv.wait(lock, [this]
                          { return !queue.empty() || shutdown; });

        // If shutdown and queue is empty, return false
        if (queue.empty() && shutdown)
        {
            return false;
        }

        item = queue.front();
        queue.pop();
        not_full_cv.notify_one();
        return true;
    }

    // Signal the queue is shutting down
    void signal_shutdown()
    {
        std::lock_guard<std::mutex> lock(mutex);
        shutdown = true;
        not_empty_cv.notify_all();
        not_full_cv.notify_all();
    }

    // Get current queue size
    size_t size() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return queue.size();
    }

    // Check if queue is empty
    bool empty() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return queue.empty();
    }

    // Get capacity
    size_t get_capacity() const
    {
        return capacity;
    }
};

// zstd_reader structure definition
typedef struct zstd_reader
{
    FILE *ifile;
    ZSTD_DStream *zds;

    size_t buff_in_sz;
    void *buff_in;
    size_t buff_out_sz;
    void *buff_out;

    size_t buff_out_read_pos;

    ZSTD_inBuffer input;
    ZSTD_outBuffer output;

    rstatus status;
} zstd_reader;

// Function prototypes for zstd reader
zstd_reader *create_zstd_reader(const char *trace_path);
void free_zstd_reader(zstd_reader *reader);
size_t zstd_reader_read_line(zstd_reader *reader, char **line_start, char **line_end);
size_t _read_from_file(zstd_reader *reader);
rstatus _decompress_from_buff(zstd_reader *reader);

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
        std::cerr << "Failed to connect to UNIX domain socket: " << strerror(errno) << "\n";
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
        std::cerr << "Failed to connect to TCP socket: " << strerror(errno) << "\n";
        close(sockfd);
        return -1;
    }

    return sockfd;
}

// Implementation of zstdReader.c functions
zstd_reader *create_zstd_reader(const char *trace_path)
{
    zstd_reader *reader = (zstd_reader *)malloc(sizeof(zstd_reader));

    reader->ifile = fopen(trace_path, "rb");
    if (reader->ifile == NULL)
    {
        std::cerr << "Cannot open " << trace_path << ": " << strerror(errno) << std::endl;
        free(reader);
        return nullptr;
    }

    reader->buff_in_sz = ZSTD_DStreamInSize();
    reader->buff_in = malloc(reader->buff_in_sz);
    reader->input.src = reader->buff_in;
    reader->input.size = 0;
    reader->input.pos = 0;

    reader->buff_out_sz = ZSTD_DStreamOutSize() * 2;
    reader->buff_out = malloc(reader->buff_out_sz);
    reader->output.dst = reader->buff_out;
    reader->output.size = reader->buff_out_sz;
    reader->output.pos = 0;

    reader->buff_out_read_pos = 0;
    reader->status = OK;

    reader->zds = ZSTD_createDStream();
    if (reader->zds == NULL)
    {
        std::cerr << "Failed to create ZSTD_DStream" << std::endl;
        fclose(reader->ifile);
        free(reader->buff_in);
        free(reader->buff_out);
        free(reader);
        return nullptr;
    }

    // Initialize the stream
    size_t init_result = ZSTD_initDStream(reader->zds);
    if (ZSTD_isError(init_result))
    {
        std::cerr << "ZSTD_initDStream error: " << ZSTD_getErrorName(init_result) << std::endl;
        ZSTD_freeDStream(reader->zds);
        fclose(reader->ifile);
        free(reader->buff_in);
        free(reader->buff_out);
        free(reader);
        return nullptr;
    }

    return reader;
}

void free_zstd_reader(zstd_reader *reader)
{
    if (reader)
    {
        if (reader->zds)
        {
            ZSTD_freeDStream(reader->zds);
        }
        if (reader->ifile)
        {
            fclose(reader->ifile);
        }
        free(reader->buff_in);
        free(reader->buff_out);
        free(reader);
    }
}

size_t _read_from_file(zstd_reader *reader)
{
    size_t read_sz;
    read_sz = fread(reader->buff_in, 1, reader->buff_in_sz, reader->ifile);
    if (read_sz < reader->buff_in_sz)
    {
        if (feof(reader->ifile))
        {
            reader->status = MY_EOF;
        }
        else
        {
            assert(ferror(reader->ifile));
            reader->status = ERR;
            return 0;
        }
    }

    reader->input.size = read_sz;
    reader->input.pos = 0;

    return read_sz;
}

rstatus _decompress_from_buff(zstd_reader *reader)
{
    /* move the unread decompresed data to the head of buff_out */
    void *buff_start = (char *)reader->buff_out + reader->buff_out_read_pos;
    size_t buff_left_sz = reader->output.pos - reader->buff_out_read_pos;
    memmove(reader->buff_out, buff_start, buff_left_sz);
    reader->output.pos = buff_left_sz;
    reader->buff_out_read_pos = 0;
    size_t old_pos = buff_left_sz;

    if (reader->input.pos >= reader->input.size)
    {
        size_t read_sz = _read_from_file(reader);
        if (read_sz == 0)
        {
            if (reader->status == MY_EOF)
            {
                return MY_EOF;
            }
            else
            {
                ERROR("read from file error\n");
                return ERR;
            }
        }
    }

    reader->output.dst = (char *)reader->buff_out + reader->output.pos;
    reader->output.size = reader->buff_out_sz - reader->output.pos;
    reader->output.pos = 0;

    size_t const ret = ZSTD_decompressStream(reader->zds, &(reader->output), &(reader->input));
    reader->output.dst = reader->buff_out;
    reader->output.size = reader->buff_out_sz;
    reader->output.pos += old_pos;

    if (ret != 0)
    {
        if (ZSTD_isError(ret))
        {
            printf("%zu\n", ret);
            WARN("zstd decompression error: %s\n", ZSTD_getErrorName(ret));
        }
    }

    return OK;
}

size_t zstd_reader_read_line(zstd_reader *reader, char **line_start, char **line_end)
{
    bool has_data_in_line_buff = false;

    if (reader->buff_out_read_pos < reader->output.pos)
    {
        /* there are still content in output buffer */
        *line_start = (char *)reader->buff_out + reader->buff_out_read_pos;
        void *buff_start = (char *)reader->buff_out + reader->buff_out_read_pos;
        size_t buff_left_sz = reader->output.pos - reader->buff_out_read_pos;
        *line_end = (char *)memchr(buff_start, LINE_DELIM, buff_left_sz);
        if (*line_end == NULL)
        {
            /* cannot find end of line, copy left over bytes, and decompress the next frame */
            has_data_in_line_buff = true;
        }
        else
        {
            /* find a line in buff_out */
            assert(**line_end == LINE_DELIM);
            size_t sz = *line_end - *line_start + 1;
            reader->buff_out_read_pos += sz;

            return sz;
        }
    }

    rstatus status = _decompress_from_buff(reader);
    if (status != OK)
    {
        if (status == MY_EOF && has_data_in_line_buff)
        {
            *(((char *)(reader->buff_out)) + reader->output.pos) = '\n';
            reader->output.pos += 1;
        }
        else
        {
            return 0;
        }
    }
    else if (reader->output.pos < reader->output.size / 4)
    {
        /* input buffer does not have enough content, read more from file */
        status = _decompress_from_buff(reader);
    }

    *line_start = (char *)reader->buff_out;
    *line_end = (char *)memchr(*line_start, LINE_DELIM, reader->output.pos);
    if (*line_end == NULL)
    {
        // If we still can't find a line ending, try to decompress more
        status = _decompress_from_buff(reader);
        if (status != OK)
        {
            return 0;
        }
        *line_end = (char *)memchr(*line_start, LINE_DELIM, reader->output.pos);
        if (*line_end == NULL)
        {
            // If we still can't find a line ending after another decompress attempt
            // Let's mark the end of the buffer as the line end as a fallback
            *line_end = (char *)reader->buff_out + reader->output.pos - 1;
            **line_end = LINE_DELIM; // Force a line ending
        }
    }

    assert(*line_end != NULL);
    assert(**line_end == LINE_DELIM);
    size_t sz = *line_end - *line_start + 1;
    reader->buff_out_read_pos = sz;

    return sz;
}

// Function to split a string by a delimiter
std::vector<std::string> split(const std::string &s, char delimiter)
{
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream token_stream(s);
    while (std::getline(token_stream, token, delimiter))
    {
        tokens.push_back(token);
    }
    return tokens;
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

// Function to perform a GET operation
std::pair<bool, int> performGetOperation(int sockfd, const std::string &key, ThreadStats &stats)
{
    auto start_time = std::chrono::high_resolution_clock::now();

    std::string command = "get " + key;

    if (send(sockfd, (command + "\r\n").c_str(), command.length() + 2, 0) == -1)
    {
        std::cerr << "Thread " << stats.thread_id << ": Failed to send GET command: " << command << std::endl;
        return {false, -1};
    }

    // char buffer[8192];
    char buffer[65536];
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
        return {false, -1};
    }

    buffer[bytesReceived] = '\0';

    // if (strncmp(buffer, "NOT_FOUND", 9) == 0 || strncmp(buffer, "ERR", 3) == 0)
    // {
    //     // Key not found - This is expected in some cases
    //     stats.failed_ops++;
    //     return false;
    // }

    stats.successful_ops++;
    return {true, bytesReceived};
}

// Function to perform a SET operation
bool performSetOperation(int sockfd, const std::string &key, size_t size, ThreadStats &stats)
{
    auto start_time = std::chrono::high_resolution_clock::now();

    char *randomValue = generateRandomString(size);
    std::string command = "set " + key + " 0 0 " + std::to_string(size) + "\r\n" + randomValue;
    free(randomValue);

    if (send(sockfd, (command + "\r\n").c_str(), command.length() + 2, 0) == -1)
    {
        std::cerr << "Thread " << stats.thread_id << ": Failed to send SET command: " << command << std::endl;
        return false;
    }

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
    // if (strncmp(buffer, "STORED", 6) != 0)
    // {
    //     stats.failed_ops++;
    //     return false;
    // }

    stats.successful_ops++;
    return true;
}

// Function to perform a DELETE operation
bool performDelOperation(int sockfd, const std::string &key, ThreadStats &stats)
{
    auto start_time = std::chrono::high_resolution_clock::now();

    std::string command = "delete " + key;

    if (send(sockfd, (command + "\r\n").c_str(), command.length() + 2, 0) == -1)
    {
        std::cerr << "Thread " << stats.thread_id << ": Failed to send DELETE command: " << command << std::endl;
        return false;
    }

    char buffer[65536];
    int bytesReceived = recv(sockfd, buffer, sizeof(buffer) - 1, 0);

    auto end_time = std::chrono::high_resolution_clock::now();
    int64_t latency_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
    double latency_us_d = static_cast<double>(latency_us);

    // Update thread-local stats
    stats.total_ops++;
    stats.del_ops++;
    stats.total_latency_us += latency_us_d;
    stats.max_latency_us = std::max(stats.max_latency_us, latency_us_d);
    stats.min_latency_us = std::min(stats.min_latency_us, latency_us_d);
    stats.add_latency(DEL, latency_us);

    if (bytesReceived <= 0)
    {
        std::cerr << "Thread " << stats.thread_id << ": Failed to receive response for DELETE: " << command << std::endl;
        stats.failed_ops++;
        return false;
    }

    buffer[bytesReceived] = '\0';

    if (strncmp(buffer, "NOT_FOUND", 9) == 0 || strncmp(buffer, "ERR", 3) == 0)
    {
        // Key not found - This might be expected
        stats.failed_ops++;
        return false;
    }

    stats.successful_ops++;
    return true;
}

// Function to read trace from zst file and populate the operation queue
void read_zst_trace(const std::string &trace_path,
                    BoundedQueue<TraceOperation> &op_queue,
                    std::atomic<bool> &thread_error,
                    std::atomic<uint64_t> &total_ops)
{
    // Create a zstd reader
    zstd_reader *reader = create_zstd_reader(trace_path.c_str());
    if (!reader)
    {
        std::cerr << "Failed to create zstd reader for file: " << trace_path << std::endl;
        thread_error.store(true);
        return;
    }

    std::cout << "Successfully opened zst file, parsing trace..." << std::endl;

    size_t line_count = 0;
    size_t valid_ops_count = 0;

    while (!thread_error.load())
    {
        char *line_start = nullptr;
        char *line_end = nullptr;

        size_t bytes_read = zstd_reader_read_line(reader, &line_start, &line_end);
        if (bytes_read == 0)
        {
            std::cout << "End of file reached after " << line_count << " lines." << std::endl;
            break;
        }

        // Create a string from the line (excluding the newline character)
        std::string line(line_start, line_end - line_start);
        line_count++;

        // Parse the line according to the trace format
        std::vector<std::string> fields = split(line, ',');

        // Validate that we have at least the minimum number of fields
        if (fields.size() < 7)
        {
            continue; // Skip malformed lines
        }

        // Parse trace fields
        // timestamp,anonymized key,key size,value size,client id,operation,TTL

        TraceOperation op;
        try
        {
            op.timestamp = std::stol(fields[0]);
            op.key = fields[1];
            // key_size = std::stoul(fields[2]); // We can derive this from the key
            op.value_size = std::stoul(fields[3]);
            op.client_id = fields[4];

            // Parse operation type
            std::string op_str = fields[5];
            std::transform(op_str.begin(), op_str.end(), op_str.begin(), ::tolower);

            if (op_str == "get" || op_str == "gets")
            {
                op.op_type = GET;
            }
            else if (op_str == "set" || op_str == "add" || op_str == "replace" ||
                     op_str == "cas" || op_str == "append" || op_str == "prepend")
            {
                op.op_type = SET;
            }
            else if (op_str == "delete" || op_str == "del")
            {
                op.op_type = DEL;
            }
            else if (op_str == "incr")
            {
                op.op_type = INCR;
            }
            else
            {
                op.op_type = UNKNOWN;
                continue; // Skip unknown operations
            }

            op.ttl = fields.size() > 6 ? std::stoi(fields[6]) : 0;

            // Add operation to the queue
            op_queue.push(op);
            valid_ops_count++;
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error parsing line " << line_count << ": " << e.what() << " - " << line << std::endl;
            continue;
        }

        // Report progress periodically
        if (line_count % 1000000 == 0)
        {
            std::cout << "Processed " << line_count << " lines, queued "
                      << valid_ops_count << " valid operations" << std::endl;
            std::cout << "Current queue size: " << op_queue.size() << " / " << op_queue.get_capacity() << std::endl;
        }
    }

    // Set total operations count
    total_ops.store(valid_ops_count);

    std::cout << "Finished parsing trace file. Total lines: " << line_count
              << ", valid operations: " << valid_ops_count << std::endl;

    // Free the reader
    free_zstd_reader(reader);
}

// Worker thread function to process operations from the queue
void worker_thread(int thread_id,
                   BoundedQueue<TraceOperation> &op_queue,
                   OpStats &global_stats,
                   const std::string &server_address,
                   int port,
                   std::atomic<bool> &thread_error,
                   std::atomic<uint64_t> &progress_counter,
                   std::mutex &stats_mutex)
{
    ThreadStats thread_stats;
    thread_stats.thread_id = thread_id;

    // Connect to server
    int sockfd;
    if (server_address[0] == '/')
    {
        sockfd = connectUnixSocket(server_address);
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

    // Process operations from the queue
    TraceOperation op;
    while (!thread_error.load() && op_queue.pop(op))
    {
        bool success = false;

        if (op.op_type == GET)
        {
            std::pair<bool, int> result = performGetOperation(sockfd, op.key, thread_stats);
        }
        else if (op.op_type == SET)
        {
            success = performSetOperation(sockfd, op.key, op.value_size, thread_stats);
        }
        else if (op.op_type == DEL)
        {
            success = performDelOperation(sockfd, op.key, thread_stats);
        }
        else if (op.op_type == INCR)
        {
            // read-modify-write operation
            // std::pair<bool, int> result = performGetOperation(sockfd, op.key, thread_stats);
            // if (result.first)
            // {
            //     // perform set operation
            //     success = performSetOperation(sockfd, op.key, result.second, thread_stats);
            // }

            // success = performSetOperation(sockfd, op.key, 82, thread_stats);
        }

        // to simulate some delay
        // nanosleep((const struct timespec[]){{0, 8000000}}, NULL);
        nanosleep((const struct timespec[]){{0, 100000}}, NULL);

        // Update progress
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

int main(int argc, char *argv[])
{
    if (argc < 4)
    {
        printf("Usage: %s zst_trace_file server_address port [num_threads] [queue_size]\n", argv[0]);
        return 1;
    }

    // Parse command line arguments
    std::string trace_file = argv[1];
    std::string server_address = argv[2];
    int port = std::stoi(argv[3]);

    // Optional number of threads (default: use hardware concurrency)
    int num_threads = std::thread::hardware_concurrency();
    if (argc > 4)
    {
        num_threads = std::stoi(argv[4]);
    }

    // Optional queue size (default: 100,000 operations)
    // size_t queue_size = 100000;
    size_t queue_size = 10000000;
    // size_t queue_size = 100000000;
    if (argc > 5)
    {
        queue_size = std::stoul(argv[5]);
    }

    printf("Using %d worker threads with queue size of %zu operations\n", num_threads, queue_size);

    // Initialize random number generator
    srand(time(NULL));

    // Create operation queue and control variables
    BoundedQueue<TraceOperation> op_queue(queue_size);
    std::atomic<bool> thread_error(false);
    std::atomic<uint64_t> total_ops(0);
    std::atomic<uint64_t> progress_counter(0);
    std::mutex stats_mutex;

    // Global statistics
    OpStats global_stats;
    auto global_start_time = std::chrono::high_resolution_clock::now();

    // Start the trace reader thread
    std::cout << "Starting trace reader thread..." << std::endl;
    std::thread reader_thread(read_zst_trace,
                              std::ref(trace_file),
                              std::ref(op_queue),
                              std::ref(thread_error),
                              std::ref(total_ops));

    // Wait a bit for the reader to start parsing the file
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // Create and start worker threads
    std::cout << "Starting worker threads..." << std::endl;
    std::vector<std::thread> worker_threads;
    for (int i = 0; i < num_threads; i++)
    {
        worker_threads.emplace_back(worker_thread,
                                    i,
                                    std::ref(op_queue),
                                    std::ref(global_stats),
                                    std::ref(server_address),
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

    bool is_reader_finished = false;

    // Continue monitoring until all operations are processed or an error occurs
    while ((progress_counter.load() < total_ops.load() || !is_reader_finished) && !thread_error.load())
    {
        // Sleep for a short time
        std::this_thread::sleep_for(std::chrono::seconds(1));

        // Check if reader thread has finished
        if (reader_thread.joinable() &&
            reader_thread.native_handle() &&
            pthread_tryjoin_np(reader_thread.native_handle(), nullptr) == 0)
        {
            is_reader_finished = true;
        }

        // Calculate elapsed time since last report
        auto now = std::chrono::high_resolution_clock::now();
        double elapsed_seconds = std::chrono::duration<double>(now - last_report_time).count();

        // Check if it's time to report (every 10 seconds)
        if (elapsed_seconds >= report_interval_seconds)
        {
            uint64_t current_progress = progress_counter.load();
            double percent_complete = total_ops.load() > 0
                                          ? static_cast<double>(current_progress) / total_ops.load() * 100.0
                                          : 0.0;

            report_count++;

            // Report detailed statistics
            try
            {
                global_stats.reportIntervalStats(report_interval_seconds);

                std::cout << "Progress: " << current_progress << "/" << total_ops.load()
                          << " operations (" << std::fixed << std::setprecision(1) << percent_complete << "%)" << std::endl;
                std::cout << "Queue size: " << op_queue.size() << " / " << op_queue.get_capacity() << std::endl;

                // Calculate and display throughput
                uint64_t ops_since_last_report = current_progress - last_reported_progress;
                double throughput = ops_since_last_report / elapsed_seconds;
                std::cout << "Current throughput: " << std::fixed << std::setprecision(2)
                          << throughput << " ops/sec" << std::endl;
            }
            catch (const std::exception &e)
            {
                std::cerr << "Exception during reporting: " << e.what() << std::endl;
            }

            last_reported_progress = current_progress;
            last_report_time = now;
        }
    }

    // Signal shutdown to the queue
    op_queue.signal_shutdown();

    // Wait for the reader thread to finish if not already done
    if (reader_thread.joinable())
    {
        reader_thread.join();
    }

    // Wait for all worker threads to finish
    for (auto &thread : worker_threads)
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }

    auto global_end_time = std::chrono::high_resolution_clock::now();
    double total_seconds = std::chrono::duration<double>(global_end_time - global_start_time).count();

    // Print overall summary
    printf("\n=== Overall Trace Replay Summary ===\n");
    printf("Total operations: %lu\n", global_stats.total_ops.load());
    printf("GET operations: %lu\n", global_stats.get_ops.load());
    printf("SET operations: %lu\n", global_stats.set_ops.load());
    printf("DEL operations: %lu\n", global_stats.del_ops.load());
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
        LatencyStats del_latency_stats = global_stats.calculateLatencyStats(global_stats.del_latency_histogram);

        printf("\nLatency statistics (microseconds):\n");
        std::cout << "GET operations - ";
        global_stats.reportLatencyStats("GET", get_latency_stats);
        std::cout << std::endl;
        std::cout << "SET operations - ";
        global_stats.reportLatencyStats("SET", set_latency_stats);
        std::cout << std::endl;
        std::cout << "DEL operations - ";
        global_stats.reportLatencyStats("DEL", del_latency_stats);
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