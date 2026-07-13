#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <cerrno>
#include <sys/stat.h>
#include <zstd.h>
#include <algorithm>
#include <unordered_set>
#include <thread>
#include <mutex>
#include <atomic>
#include <random>
#include <chrono>
#include <arpa/inet.h>
#include <sys/un.h>
#include <unistd.h>
#include <sys/socket.h>

// g++ -o twitterZstLoader.x twitterZstLoader.cc -lzstd -pthread
// ./twitterZstLoader.x /mnt/nvme/data/twitter/cluster12.sort.zst /tmp/server.sock 0 6

// Define rstatus enum
typedef enum
{
    OK = 0,
    ERR = -1,
    MY_EOF = -2
} rstatus;

// Simple logging macros
#define ERROR(fmt, ...) fprintf(stderr, "ERROR: " fmt, ##__VA_ARGS__)
#define WARN(fmt, ...) fprintf(stderr, "WARN: " fmt, ##__VA_ARGS__)
#define DEBUG(fmt, ...) fprintf(stderr, "DEBUG: " fmt, ##__VA_ARGS__)

#define LINE_DELIM '\n'

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

// Structure to hold key-value pairs for insertion
struct KeyValue
{
    std::string key;
    size_t value_size;
};

// Global variables for thread synchronization
std::mutex progress_mutex;
std::atomic<size_t> total_inserts_completed(0);

// Implementation of zstdReader functions
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
char *generateRandomString(size_t size, std::mt19937 &gen)
{
    char *str = (char *)malloc(size + 1);
    if (!str)
        return NULL;

    const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::uniform_int_distribution<> dist(0, sizeof(charset) - 2);

    for (size_t i = 0; i < size; i++)
    {
        str[i] = charset[dist(gen)];
    }
    str[size] = '\0';
    return str;
}

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

// Function to handle inserting a batch of key-value pairs into the KV store
void worker_thread(int thread_id,
                   const std::vector<KeyValue> &kv_pairs,
                   size_t start_idx,
                   size_t end_idx,
                   const std::string &ipOrSocketPath,
                   int port,
                   bool useUnixSocket,
                   size_t report_freq)
{

    // Create a thread-local random number generator
    std::random_device rd;
    std::mt19937 gen(rd() + thread_id); // Use thread_id to ensure different seeds

    // Connect to the KV store server
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
            std::cerr << "Thread " << thread_id << ": Failed to connect to server" << std::endl;
        }
        return;
    }

    {
        std::lock_guard<std::mutex> lock(progress_mutex);
        std::cout << "Thread " << thread_id << ": Connected to server, processing "
                  << (end_idx - start_idx) << " keys" << std::endl;
    }

    size_t local_success_count = 0;
    size_t last_report = 0;

    // Process each key-value pair in the assigned range
    for (size_t i = start_idx; i < end_idx; i++)
    {
        const auto &kv = kv_pairs[i];

        // Generate a random value of the specified size
        char *value = generateRandomString(kv.value_size, gen);
        if (!value)
            continue;

        // Format the SET command
        std::string command = "set " + kv.key + " " + value;
        free(value);

        // Send the command to the server
        if (send(sockfd, (command + "\r\n").c_str(), command.length() + 2, 0) == -1)
        {
            std::lock_guard<std::mutex> lock(progress_mutex);
            std::cerr << "Thread " << thread_id << ": Failed to send command" << std::endl;
            break;
        }

        // Read the response
        char buffer[4096];
        int bytesReceived = read(sockfd, buffer, sizeof(buffer) - 1);
        if (bytesReceived <= 0)
        {
            std::lock_guard<std::mutex> lock(progress_mutex);
            std::cerr << "Thread " << thread_id << ": Failed to receive response or connection closed" << std::endl;
            break;
        }
        buffer[bytesReceived] = '\0';

        // Add a small sleep to avoid flooding the server
        usleep(10);

        local_success_count++;
        total_inserts_completed.fetch_add(1);

        // Report progress periodically
        if (local_success_count - last_report >= report_freq)
        {
            std::lock_guard<std::mutex> lock(progress_mutex);
            std::cout << "Thread " << thread_id << ": " << local_success_count << "/"
                      << (end_idx - start_idx) << " operations completed" << std::endl;
            last_report = local_success_count;
        }
    }

    // Final report for this thread
    {
        std::lock_guard<std::mutex> lock(progress_mutex);
        std::cout << "Thread " << thread_id << ": Completed " << local_success_count << "/"
                  << (end_idx - start_idx) << " operations" << std::endl;
    }

    // Clean up
    close(sockfd);
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

int main(int argc, char *argv[])
{
    if (argc < 5)
    {
        std::cerr << "Usage: " << argv[0] << " <zst_trace_file> <ipOrSocketPath> <port> <num_threads>" << std::endl;
        return 1;
    }

    const char *trace_file = argv[1];
    std::string ipOrSocketPath = argv[2];
    int port = std::stoi(argv[3]);
    int num_threads = std::stoi(argv[4]);

    // If num_threads is <= 0, use system default
    if (num_threads <= 0)
    {
        num_threads = std::thread::hardware_concurrency();
        std::cout << "Using " << num_threads << " threads (system default)" << std::endl;
    }
    else
    {
        std::cout << "Using " << num_threads << " threads as specified" << std::endl;
    }

    // Determine connection type
    bool useUnixSocket = false;
    if (ipOrSocketPath[0] == '/')
    {
        useUnixSocket = true;
        std::cout << "Using UNIX domain socket: " << ipOrSocketPath << std::endl;
    }
    else
    {
        std::cout << "Using TCP socket: " << ipOrSocketPath << ":" << port << std::endl;
    }

    // Maximum number of unique keys to extract
    // const size_t MAX_UNIQUE_KEYS = 20000000;
    // const size_t MAX_UNIQUE_KEYS = 30000000;
    const size_t MAX_UNIQUE_KEYS = 50000000;

    // Create a zstd reader
    zstd_reader *reader = create_zstd_reader(trace_file);
    if (!reader)
    {
        std::cerr << "Failed to create zstd reader for file: " << trace_file << std::endl;
        return 1;
    }

    std::cout << "Successfully opened zst file, extracting up to " << MAX_UNIQUE_KEYS << " unique keys..." << std::endl;

    // Use an unordered_map to store unique keys and their value sizes
    std::unordered_map<std::string, size_t> unique_keys;
    size_t lines_processed = 0;
    size_t report_interval = 1000000; // Report progress every million lines

    auto start_time = std::chrono::high_resolution_clock::now();

    // Read lines until we've found MAX_UNIQUE_KEYS unique keys or reached EOF
    while (unique_keys.size() < MAX_UNIQUE_KEYS)
    {
        char *line_start = nullptr;
        char *line_end = nullptr;

        size_t bytes_read = zstd_reader_read_line(reader, &line_start, &line_end);
        if (bytes_read == 0)
        {
            std::cout << "End of file reached after " << lines_processed << " lines." << std::endl;
            break;
        }

        // Create a string from the line (excluding the newline character)
        std::string line(line_start, line_end - line_start);
        lines_processed++;

        // Parse the line according to the format
        // timestamp,anonymized key,key size,value size,client id,operation,TTL
        std::vector<std::string> fields = split(line, ',');

        if (fields.size() < 4)
        {
            continue; // Skip malformed lines
        }

        // Extract key and value size
        std::string key = fields[1];
        size_t value_size = 0;
        try
        {
            value_size = std::stoul(fields[3]);
        }
        catch (...)
        {
            // value_size = 256; // Default if parsing fails
            value_size = 600;
        }

        // Store the key if it's not already seen
        if (unique_keys.find(key) == unique_keys.end())
        {
            unique_keys[key] = value_size > 0 ? value_size : 600; // Default to 600 if value_size is 0
        }

        // Report progress periodically
        if (lines_processed % report_interval == 0)
        {
            std::cout << "Processed " << lines_processed << " lines, found "
                      << unique_keys.size() << " unique keys" << std::endl;
        }
    }

    // Clean up the reader
    free_zstd_reader(reader);

    auto extraction_end_time = std::chrono::high_resolution_clock::now();
    auto extraction_duration = std::chrono::duration_cast<std::chrono::seconds>(
                                   extraction_end_time - start_time)
                                   .count();

    std::cout << "Key extraction completed in " << extraction_duration << " seconds." << std::endl;
    std::cout << "Found " << unique_keys.size() << " unique keys in " << lines_processed << " lines." << std::endl;

    // Calculate total storage requirements
    uint64_t total_key_bytes = 0;
    uint64_t total_value_bytes = 0;

    // Convert unique keys to a vector for shuffling and batching
    std::vector<KeyValue> kv_pairs;
    kv_pairs.reserve(unique_keys.size());

    for (const auto &[key, value_size] : unique_keys)
    {
        kv_pairs.push_back({key, value_size});
        total_key_bytes += key.size();
        total_value_bytes += value_size;
    }

    std::cout << "Total storage requirement: Keys = " << bytesToHumanReadable(total_key_bytes)
              << ", Values = " << bytesToHumanReadable(total_value_bytes) << std::endl;

    // Shuffle the key-value pairs
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(kv_pairs.begin(), kv_pairs.end(), g);

    std::cout << "Shuffled " << kv_pairs.size() << " keys. Starting insertion..." << std::endl;

    // Start insertion with multiple threads
    auto insertion_start_time = std::chrono::high_resolution_clock::now();

    // Create threads for insertion
    std::vector<std::thread> threads;
    size_t keys_per_thread = kv_pairs.size() / num_threads;
    size_t remainder = kv_pairs.size() % num_threads;
    size_t report_freq = 10000; // Report every 10K insertions per thread

    size_t start_idx = 0;
    for (int i = 0; i < num_threads; i++)
    {
        size_t thread_keys = keys_per_thread + (i < remainder ? 1 : 0);
        size_t end_idx = start_idx + thread_keys;

        threads.emplace_back(
            worker_thread,
            i,
            std::ref(kv_pairs),
            start_idx,
            end_idx,
            std::ref(ipOrSocketPath),
            port,
            useUnixSocket,
            report_freq);

        start_idx = end_idx;
    }

    // Join all threads
    for (auto &t : threads)
    {
        t.join();
    }

    auto insertion_end_time = std::chrono::high_resolution_clock::now();
    auto insertion_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  insertion_end_time - insertion_start_time)
                                  .count();

    std::cout << "All threads completed." << std::endl;
    std::cout << "Finished inserting " << total_inserts_completed.load() << "/"
              << kv_pairs.size() << " keys successfully in " << insertion_duration << " ms" << std::endl;
    std::cout << "Average throughput: " << (total_inserts_completed.load() * 1000.0) / insertion_duration
              << " operations/second" << std::endl;

    return 0;
}