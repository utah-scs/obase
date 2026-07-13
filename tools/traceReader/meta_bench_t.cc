#include <iostream>
#include <string>
#include <vector>
#include <random>
#include <chrono>
#include <thread>
#include <cmath>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <atomic>
#include <cassert>
#include <algorithm>
#include <iomanip>
#include <string.h>
#include <cstring>
#include <sstream>
#include <fstream>
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h> // Added for TCP/IP support
#include <unistd.h>
#include <numeric> // For std::accumulate
#include <map>
#include <queue>
#include <barrier> // For thread synchronization

/*****
 * g++ -std=c++20 -O2 -pthread -o meta_bench.x meta_bench_t.cc
./meta_bench.x \
  -benchmarks "mixgraph" \
  -keyrange_dist_a 14.18 \
  -keyrange_dist_b -2.917 \
  -keyrange_dist_c 0.0164 \
  -keyrange_dist_d -0.08082 \
  -keyrange_num 30 \
  -key_dist_a 0.002312 \
  -key_dist_b 0.3467 \
  -value_k 0.2615 \
  -value_sigma 25.45 \
  -iter_k 2.517 \
  -iter_sigma 14.236 \
  -mix_get_ratio 0.85 \
  -mix_put_ratio 0.14 \
  -mix_seek_ratio 0.01 \
  -sine_mix_rate_interval_ms 5000 \
  -sine_a 1000 \
  -sine_b 0.000073 \
  -sine_d 4500 \
  -reads 420000000 \
  -num 50000000 \
  -key_size 48 \
  -t 8 # number of clients
  -ip "127.0.0.1" # optional: if provided, use TCP instead of Unix socket
  -port 6379 # optional: required if using TCP
 */

// Operation Specification for the AccessPatternGenerator
struct OperationSpec
{
    enum OpType
    {
        GET,
        PUT,
        SEEK
    };
    OpType op_type;
    int64_t key_rand;
    int value_size; // Only relevant for PUT
};

// Argument Parsing
class Arguments
{
public:
    std::string benchmarks;
    int key_size;
    int value_size;
    int64_t num;
    int keyrange_num;
    double value_k;
    double value_sigma;
    double iter_k;
    double iter_sigma;
    double mix_get_ratio;
    double mix_put_ratio;
    double mix_seek_ratio;
    int sine_mix_rate_interval_ms;
    double sine_a;
    double sine_b;
    double sine_d;
    int64_t reads;
    double key_dist_a;
    double key_dist_b;
    double keyrange_dist_a;
    double keyrange_dist_b;
    double keyrange_dist_c;
    double keyrange_dist_d;
    int threads;    // Number of client threads
    std::string ip; // IP address for TCP connection
    int port;       // Port for TCP connection
    bool use_tcp;   // Flag to indicate whether to use TCP or Unix socket

    Arguments();
    bool ParseArguments(int argc, char *argv[]);
    void PrintUsage();
};

Arguments::Arguments()
{
    // Set default values
    benchmarks = "fillrandom";
    key_size = 48;
    value_size = 100;
    num = 1000000;
    keyrange_num = 1;
    value_k = 0.2615;
    value_sigma = 25.45;
    iter_k = 2.517;
    iter_sigma = 14.236;
    mix_get_ratio = 0.85;
    mix_put_ratio = 0.14;
    mix_seek_ratio = 0.01;
    sine_mix_rate_interval_ms = 0;
    sine_a = 0.0;
    sine_b = 0.0;
    sine_d = 0.0;
    reads = 1000000;
    key_dist_a = 0.0;
    key_dist_b = 0.0;
    keyrange_dist_a = 0.0;
    keyrange_dist_b = 0.0;
    keyrange_dist_c = 0.0;
    keyrange_dist_d = 0.0;
    threads = 1;     // Default to single-threaded
    ip = "";         // Default to empty (use Unix socket)
    port = 0;        // Default port
    use_tcp = false; // Default to Unix socket
}

bool Arguments::ParseArguments(int argc, char *argv[])
{
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "-benchmarks") == 0 && i + 1 < argc)
        {
            benchmarks = argv[++i];
        }
        else if (strcmp(argv[i], "-key_size") == 0 && i + 1 < argc)
        {
            key_size = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "-value_size") == 0 && i + 1 < argc)
        {
            value_size = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "-num") == 0 && i + 1 < argc)
        {
            num = atoll(argv[++i]);
        }
        else if (strcmp(argv[i], "-keyrange_num") == 0 && i + 1 < argc)
        {
            keyrange_num = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "-value_k") == 0 && i + 1 < argc)
        {
            value_k = atof(argv[++i]);
        }
        else if (strcmp(argv[i], "-value_sigma") == 0 && i + 1 < argc)
        {
            value_sigma = atof(argv[++i]);
        }
        else if (strcmp(argv[i], "-iter_k") == 0 && i + 1 < argc)
        {
            iter_k = atof(argv[++i]);
        }
        else if (strcmp(argv[i], "-iter_sigma") == 0 && i + 1 < argc)
        {
            iter_sigma = atof(argv[++i]);
        }
        else if (strcmp(argv[i], "-mix_get_ratio") == 0 && i + 1 < argc)
        {
            mix_get_ratio = atof(argv[++i]);
        }
        else if (strcmp(argv[i], "-mix_put_ratio") == 0 && i + 1 < argc)
        {
            mix_put_ratio = atof(argv[++i]);
        }
        else if (strcmp(argv[i], "-mix_seek_ratio") == 0 && i + 1 < argc)
        {
            mix_seek_ratio = atof(argv[++i]);
        }
        else if (strcmp(argv[i], "-sine_mix_rate_interval_ms") == 0 && i + 1 < argc)
        {
            sine_mix_rate_interval_ms = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "-sine_a") == 0 && i + 1 < argc)
        {
            sine_a = atof(argv[++i]);
        }
        else if (strcmp(argv[i], "-sine_b") == 0 && i + 1 < argc)
        {
            sine_b = atof(argv[++i]);
        }
        else if (strcmp(argv[i], "-sine_d") == 0 && i + 1 < argc)
        {
            sine_d = atof(argv[++i]);
        }
        else if (strcmp(argv[i], "-reads") == 0 && i + 1 < argc)
        {
            reads = atoll(argv[++i]);
        }
        else if (strcmp(argv[i], "-key_dist_a") == 0 && i + 1 < argc)
        {
            key_dist_a = atof(argv[++i]);
        }
        else if (strcmp(argv[i], "-key_dist_b") == 0 && i + 1 < argc)
        {
            key_dist_b = atof(argv[++i]);
        }
        else if (strcmp(argv[i], "-keyrange_dist_a") == 0 && i + 1 < argc)
        {
            keyrange_dist_a = atof(argv[++i]);
        }
        else if (strcmp(argv[i], "-keyrange_dist_b") == 0 && i + 1 < argc)
        {
            keyrange_dist_b = atof(argv[++i]);
        }
        else if (strcmp(argv[i], "-keyrange_dist_c") == 0 && i + 1 < argc)
        {
            keyrange_dist_c = atof(argv[++i]);
        }
        else if (strcmp(argv[i], "-keyrange_dist_d") == 0 && i + 1 < argc)
        {
            keyrange_dist_d = atof(argv[++i]);
        }
        else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc)
        {
            threads = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "-ip") == 0 && i + 1 < argc)
        {
            ip = argv[++i];
            use_tcp = true;
        }
        else if (strcmp(argv[i], "-port") == 0 && i + 1 < argc)
        {
            port = atoi(argv[++i]);
            use_tcp = true;
        }
        else
        {
            std::cerr << "Unknown or incomplete argument: " << argv[i] << std::endl;
            PrintUsage();
            return false;
        }
    }

    // Validate TCP arguments
    if (use_tcp && (ip.empty() || port <= 0))
    {
        std::cerr << "When using TCP, both IP address and port must be provided" << std::endl;
        PrintUsage();
        return false;
    }

    return true;
}

void Arguments::PrintUsage()
{
    std::cout << "Usage: meta_bench [options]\n"
              << "Options:\n"
              << "  -benchmarks <fillrandom,mixgraph>\n"
              << "  -key_size <size>\n"
              << "  -value_size <size>\n"
              << "  -num <number of operations>\n"
              << "  -keyrange_num <number of key ranges>\n"
              << "  -value_k <value>\n"
              << "  -value_sigma <value>\n"
              << "  -iter_k <value>\n"
              << "  -iter_sigma <value>\n"
              << "  -mix_get_ratio <ratio>\n"
              << "  -mix_put_ratio <ratio>\n"
              << "  -mix_seek_ratio <ratio>\n"
              << "  -sine_mix_rate_interval_milliseconds <ms>\n"
              << "  -sine_a <value>\n"
              << "  -sine_b <value>\n"
              << "  -sine_d <value>\n"
              << "  -reads <number of read operations>\n"
              << "  -key_dist_a <value>\n"
              << "  -key_dist_b <value>\n"
              << "  -keyrange_dist_a <value>\n"
              << "  -keyrange_dist_b <value>\n"
              << "  -keyrange_dist_c <value>\n"
              << "  -keyrange_dist_d <value>\n"
              << "  -t <number of threads>\n"
              << "  -ip <ip address>  Optional: use TCP instead of Unix socket\n"
              << "  -port <port>      Optional: required when using TCP\n"
              << " Change report_interval_seconds for frequency of intermediate reports\n";
}

// Struct to hold latency statistics
struct LatencyStats
{
    int64_t count;
    double min;
    double max;
    double avg;
    double p90;
    double p99;
    double p99_9;
    double p99_99;
};

// Thread-safe Statistics Tracking
class Stats
{
public:
    Stats();
    void Start();
    void Stop();
    void AddOperation(const std::string &op_type, int64_t bytes = 0);
    void Report();
    void AddLatency(const std::string &op_type, int64_t latency_us);
    void ReportIntermediate();
    LatencyStats CalculateLatencyStats(const std::map<int64_t, int64_t> &histogram);
    void ReportLatencyStats(const std::string &op_type, const LatencyStats &stats);
    void ClearLatencyData();

private:
    std::chrono::steady_clock::time_point start_time_;
    std::chrono::steady_clock::time_point end_time_;
    std::atomic<int64_t> get_count_;
    std::atomic<int64_t> put_count_;
    std::atomic<int64_t> seek_count_;
    std::atomic<int64_t> bytes_read_;
    std::atomic<int64_t> bytes_written_;
    int report_count_;

    // Mutexes for thread-safe access
    std::mutex latency_mutex_;
    std::mutex report_mutex_;

    // Histograms for latency values
    std::map<int64_t, int64_t> get_latency_histogram_;
    std::map<int64_t, int64_t> put_latency_histogram_;
    std::map<int64_t, int64_t> seek_latency_histogram_;

    // Last report time and counts for intermediate reporting
    std::chrono::steady_clock::time_point last_report_time_;
    int64_t last_total_ops_;
    int64_t last_get_count_;
    int64_t last_put_count_;
    int64_t last_seek_count_;

    // Total operations for cumulative stats
    std::atomic<int64_t> total_get_ops_;
    std::atomic<int64_t> total_put_ops_;
    std::atomic<int64_t> total_seek_ops_;

    // Cumulative histograms
    std::map<int64_t, int64_t> total_get_latency_histogram_;
    std::map<int64_t, int64_t> total_put_latency_histogram_;
    std::map<int64_t, int64_t> total_seek_latency_histogram_;
};

Stats::Stats()
    : get_count_(0), put_count_(0), seek_count_(0),
      bytes_read_(0), bytes_written_(0),
      last_total_ops_(0), last_get_count_(0), last_put_count_(0), last_seek_count_(0),
      total_get_ops_(0), total_put_ops_(0), total_seek_ops_(0), report_count_(0)
{
    start_time_ = std::chrono::steady_clock::now();
    last_report_time_ = start_time_;
}

void Stats::Start()
{
    start_time_ = std::chrono::steady_clock::now();
}

void Stats::Stop()
{
    end_time_ = std::chrono::steady_clock::now();
}

void Stats::AddLatency(const std::string &op_type, int64_t latency_us)
{
    // int64_t bucket = latency_us / 1000; // Convert to milliseconds
    int64_t bucket = latency_us;
    std::lock_guard<std::mutex> lock(latency_mutex_);
    if (op_type == "GET")
    {
        get_latency_histogram_[bucket]++;
        total_get_latency_histogram_[bucket]++;
    }
    else if (op_type == "PUT")
    {
        put_latency_histogram_[bucket]++;
        total_put_latency_histogram_[bucket]++;
    }
    else if (op_type == "SEEK")
    {
        seek_latency_histogram_[bucket]++;
        total_seek_latency_histogram_[bucket]++;
    }
}

// Method to calculate latency statistics
LatencyStats Stats::CalculateLatencyStats(const std::map<int64_t, int64_t> &histogram)
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

    stats.min = static_cast<double>(min_latency);
    stats.max = static_cast<double>(max_latency);
    stats.avg = static_cast<double>(sum) / stats.count;

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

    stats.p90 = get_percentile(0.90);
    stats.p99 = get_percentile(0.99);
    stats.p99_9 = get_percentile(0.999);
    stats.p99_99 = get_percentile(0.9999);

    return stats;
}

// Method to report latency statistics
void Stats::ReportLatencyStats(const std::string &op_type, const LatencyStats &stats)
{
    if (stats.count == 0)
    {
        // std::cout << "[" << op_type << ": No operations performed in this interval]\n";
        return;
    }

    // Convert latency values from microseconds to milliseconds
    double max_us = stats.max;
    double min_us = stats.min;
    double avg_us = stats.avg;
    double p90_us = stats.p90;
    double p99_us = stats.p99;
    double p99_9_us = stats.p99_9;
    double p99_99_us = stats.p99_99;

    std::cout << "[" << op_type << ": Count=" << stats.count
              << ", Max=" << max_us
              << ", Min=" << min_us
              << ", Avg=" << std::fixed << std::setprecision(2) << avg_us
              << ", 90=" << p90_us
              << ", 99=" << stats.p99
              << ", 99.9=" << p99_9_us
              << ", 99.99=" << p99_99_us << "] ";
}

// Method to clear latency data after each interval
void Stats::ClearLatencyData()
{
    std::lock_guard<std::mutex> lock(latency_mutex_);
    get_latency_histogram_.clear();
    put_latency_histogram_.clear();
    seek_latency_histogram_.clear();
}

// Method to report intermediate statistics
void Stats::ReportIntermediate()
{
    std::lock_guard<std::mutex> lock(report_mutex_);

    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(now - last_report_time_).count();
    double seconds = duration / 1000000.0; // Corrected to divide by 1,000,000.0 for seconds

    report_count_++;
    int cumulative_seconds = report_count_ * seconds;

    int64_t total_ops = get_count_ + put_count_ + seek_count_;
    int64_t interval_ops = total_ops - last_total_ops_;

    double ops_per_sec = interval_ops / seconds;

    int64_t interval_gets = get_count_ - last_get_count_;
    int64_t interval_puts = put_count_ - last_put_count_;
    int64_t interval_seeks = seek_count_ - last_seek_count_;

    double get_ops_per_sec = interval_gets / seconds;
    double put_ops_per_sec = interval_puts / seconds;
    double seek_ops_per_sec = interval_seeks / seconds;

    // Collect latency statistics
    LatencyStats get_latency_stats = CalculateLatencyStats(get_latency_histogram_);
    LatencyStats put_latency_stats = CalculateLatencyStats(put_latency_histogram_);
    LatencyStats seek_latency_stats = CalculateLatencyStats(seek_latency_histogram_);

    // Output the statistics
    std::cout << cumulative_seconds << " sec: ";
    std::cout << "Total Ops/sec: " << std::fixed << std::setprecision(2) << ops_per_sec << "; ";
    std::cout << "GET Ops/sec: " << get_ops_per_sec << "; ";
    std::cout << "PUT Ops/sec: " << put_ops_per_sec << "; ";
    std::cout << "SEEK Ops/sec: " << seek_ops_per_sec << "; ";

    // Output latency statistics per operation type
    ReportLatencyStats("GET", get_latency_stats);
    ReportLatencyStats("PUT", put_latency_stats);
    ReportLatencyStats("SEEK", seek_latency_stats);

    std::cout << std::endl;
    // std::cout.flush();

    // Update last report data
    last_total_ops_ = total_ops;
    last_get_count_ = get_count_;
    last_put_count_ = put_count_;
    last_seek_count_ = seek_count_;
    last_report_time_ = now;

    // Clear latency data for the next interval
    ClearLatencyData();
}

void Stats::AddOperation(const std::string &op_type, int64_t bytes)
{
    if (op_type == "GET")
    {
        ++get_count_;
        bytes_read_ += bytes;
        ++total_get_ops_;
    }
    else if (op_type == "PUT")
    {
        ++put_count_;
        bytes_written_ += bytes;
        ++total_put_ops_;
    }
    else if (op_type == "SEEK")
    {
        ++seek_count_;
        bytes_read_ += bytes;
        ++total_seek_ops_;
    }
}

void Stats::Report()
{
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time_ - start_time_).count();
    double seconds = duration / 1000000.0;
    int64_t total_ops = get_count_ + put_count_ + seek_count_;
    double ops_per_sec = total_ops / seconds;

    std::cout << "\nBenchmark Results:\n";
    std::cout << "Total Time: " << seconds << " seconds\n";
    std::cout << "Total Operations: " << total_ops << "\n";
    std::cout << "Operations per Second: " << ops_per_sec << "\n";
    std::cout << "GET Operations: " << get_count_ << "\n";
    std::cout << "PUT Operations: " << put_count_ << "\n";
    std::cout << "SEEK Operations: " << seek_count_ << "\n";
    std::cout << "Bytes Read: " << bytes_read_ << "\n";
    std::cout << "Bytes Written: " << bytes_written_ << "\n";

    // Collect cumulative latency statistics
    LatencyStats get_latency_stats = CalculateLatencyStats(total_get_latency_histogram_);
    LatencyStats put_latency_stats = CalculateLatencyStats(total_put_latency_histogram_);
    LatencyStats seek_latency_stats = CalculateLatencyStats(total_seek_latency_histogram_);

    // Output cumulative latency statistics
    std::cout << "Latency Statistics (microseconds):\n";
    ReportLatencyStats("GET", get_latency_stats);
    std::cout << std::endl;
    ReportLatencyStats("PUT", put_latency_stats);
    std::cout << std::endl;
    ReportLatencyStats("SEEK", seek_latency_stats);
    std::cout << std::endl;
}

// Distribution Functions
class Distribution
{
public:
    Distribution(int keyrange_num, double value_theta, double value_k, double value_sigma);

    int64_t GenerateKeyID(); // Will be defined as needed
    int GenerateValueSize();
    int64_t DistGetKeyID(int64_t ini_rand, double key_dist_a, double key_dist_b);
    int64_t GenerateKeyOffset(int64_t keyrange_size, double key_dist_a, double key_dist_b);
    void InitiateExpDistribution(int64_t total_keys, double prefix_a, double prefix_b, double prefix_c, double prefix_d);

    double PowerLawCdfInversion(double u, double a, double b);
    int64_t ParetoCdfInversion(double u, double theta, double k, double sigma);

private:
    int keyrange_num_;
    double value_theta_;
    double value_k_;
    double value_sigma_;
    std::mt19937_64 rng_;
    std::uniform_real_distribution<double> uniform_dist_;

    // For key range distribution
    struct KeyrangeUnit
    {
        int64_t keyrange_start;
        int64_t keyrange_access;
        int64_t keyrange_keys;
    };
    std::vector<KeyrangeUnit> keyrange_set_;
    int64_t keyrange_rand_max_;
    int64_t keyrange_size_;
};

Distribution::Distribution(int keyrange_num, double value_theta, double value_k, double value_sigma)
    : keyrange_num_(keyrange_num),
      value_theta_(value_theta),
      value_k_(value_k),
      value_sigma_(value_sigma),
      rng_(std::random_device{}()),
      uniform_dist_(0.0, 1.0),
      keyrange_rand_max_(0),
      keyrange_size_(0) {}

void Distribution::InitiateExpDistribution(int64_t total_keys, double prefix_a, double prefix_b, double prefix_c, double prefix_d)
{
    keyrange_size_ = total_keys / keyrange_num_; // Calculate the size of each key range
    int64_t amplify = 0;
    int64_t keyrange_start = 0;

    // Step 1: Calculate key range probabilities and their cumulative distribution
    for (int64_t pfx = keyrange_num_; pfx >= 1; --pfx)
    {
        // Calculate the probability for the current key range based on the two-term exponential distribution
        double keyrange_p = prefix_a * exp(prefix_b * pfx) + prefix_c * exp(prefix_d * pfx);

        // If the probability is too small (close to zero), set it to zero to avoid computational issues
        if (keyrange_p < 1e-16)
        {
            keyrange_p = 0.0;
        }

        // Step 2: Calculate the amplification factor
        // The amplification factor helps us extend the probability of each key range from [0, 1] to [0, amplify]
        // This ensures that all key ranges get assigned a range of integers greater than or equal to zero
        if (amplify == 0 && keyrange_p > 0)
        {
            amplify = static_cast<int64_t>(std::floor(1 / keyrange_p)) + 1;
        }

        // Step 3: Create the KeyrangeUnit for the current key range
        KeyrangeUnit p_unit;
        p_unit.keyrange_start = keyrange_start;

        // If the probability is zero, set the access range size to zero for this key range
        if (keyrange_p == 0.0)
        {
            p_unit.keyrange_access = 0;
        }
        else
        {
            // Otherwise, calculate the access range size based on the amplification factor and the probability
            p_unit.keyrange_access = static_cast<int64_t>(std::floor(amplify * keyrange_p));
        }

        // Assign the number of keys to this key range (this is the same for all key ranges)
        p_unit.keyrange_keys = keyrange_size_;

        // Add the unit to the key range set
        keyrange_set_.push_back(p_unit);

        // Update the starting point for the next key range
        keyrange_start += p_unit.keyrange_access;
    }

    // Step 4: Store the maximum random value range for selecting key ranges
    keyrange_rand_max_ = keyrange_start;

    // Step 5: Shuffle the key ranges to avoid clustering
    // We use std::mt19937_64 for random number generation, with a seed based on a random device
    std::random_device rd;
    std::mt19937_64 rng(rd());                                     // Random number generator
    std::shuffle(keyrange_set_.begin(), keyrange_set_.end(), rng); // Shuffle key ranges

    // Step 6: Recalculate the starting positions after shuffling
    int64_t offset = 0;
    for (auto &p_unit : keyrange_set_)
    {
        p_unit.keyrange_start = offset;
        offset += p_unit.keyrange_access;
    }
}

int64_t Distribution::DistGetKeyID(int64_t ini_rand, double key_dist_a, double key_dist_b)
{
    int64_t keyrange_rand = ini_rand % keyrange_rand_max_;

    // Binary search to find the key range
    int64_t start = 0;
    int64_t end = keyrange_set_.size();
    while (start + 1 < end)
    {
        int64_t mid = start + (end - start) / 2;
        if (keyrange_rand < keyrange_set_[mid].keyrange_start)
        {
            end = mid;
        }
        else
        {
            start = mid;
        }
    }
    int64_t keyrange_id = start;
    int64_t key_offset = GenerateKeyOffset(keyrange_size_, key_dist_a, key_dist_b);

    return keyrange_size_ * keyrange_id + key_offset;
}

int64_t Distribution::GenerateKeyOffset(int64_t keyrange_size, double key_dist_a, double key_dist_b)
{
    if (key_dist_a == 0.0 || key_dist_b == 0.0)
    {
        return rng_() % keyrange_size;
    }
    else
    {
        double u = uniform_dist_(rng_);
        double key_seed = PowerLawCdfInversion(u, key_dist_a, key_dist_b);
        std::mt19937_64 key_rng(static_cast<uint64_t>(key_seed));
        return key_rng() % keyrange_size;
    }
}

double Distribution::PowerLawCdfInversion(double u, double a, double b)
{
    return pow(u / a, 1.0 / b);
}

int64_t Distribution::GenerateKeyID()
{
    // This function will be implemented in the main function based on parameters
    return 0;
}

int Distribution::GenerateValueSize()
{
    double u = uniform_dist_(rng_);
    int64_t pareto_value = ParetoCdfInversion(u, value_theta_, value_k_, value_sigma_);
    int size = static_cast<int>(std::ceil(pareto_value));

    // Enforce minimum and maximum size constraints
    const int min_value_size = 10;
    const int max_value_size = 1 * 1024 * 1024; // 1 MB

    if (size < min_value_size)
    {
        size = min_value_size;
    }
    else if (size > max_value_size)
    {
        size = max_value_size;
    }

    return size;
}

// Pareto Inverse CDF function
int64_t Distribution::ParetoCdfInversion(double u, double theta, double k, double sigma)
{
    double ret;
    if (k == 0.0)
    {
        ret = theta - sigma * std::log(u);
    }
    else
    {
        ret = theta + (sigma / k) * (std::pow(u, -k) - 1);
    }
    return static_cast<int64_t>(std::ceil(ret));
}

// Query Decider
class QueryDecider
{
public:
    QueryDecider();
    void Initiate(const std::vector<double> &ratios);
    int GetQueryType(int64_t rand_num);

private:
    std::vector<int> type_thresholds_;
    int range_;
};

QueryDecider::QueryDecider()
    : range_(0) {}

void QueryDecider::Initiate(const std::vector<double> &ratios)
{
    int range_max = 1000;
    double sum = 0.0;
    for (auto ratio : ratios)
    {
        sum += ratio;
    }
    range_ = 0;
    for (auto ratio : ratios)
    {
        range_ += static_cast<int>(ceil(range_max * (ratio / sum)));
        type_thresholds_.push_back(range_);
    }
}

int QueryDecider::GetQueryType(int64_t rand_num)
{
    if (rand_num < 0)
    {
        rand_num = -rand_num;
    }
    int pos = static_cast<int>(rand_num % range_);
    for (size_t i = 0; i < type_thresholds_.size(); ++i)
    {
        if (pos < type_thresholds_[i])
        {
            return static_cast<int>(i);
        }
    }
    return 0;
}

// Sine Wave Function
double SineRate(double time_seconds, double sine_a, double sine_b, double sine_d)
{
    return sine_a * sin(sine_b * time_seconds) + sine_d;
}

// Global Rate Limiter
class GlobalRateLimiter
{
public:
    GlobalRateLimiter(int64_t initial_rate);
    void Request(int64_t bytes, OperationSpec::OpType op_type);
    void UpdateRates(double elapsed_seconds, const Arguments &args);

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    int64_t read_rate_;
    int64_t write_rate_;
    int64_t read_tokens_available_;
    int64_t write_tokens_available_;
    std::chrono::steady_clock::time_point last_refill_time_;
    int64_t refill_period_us_;

    void RefillTokens();
};

GlobalRateLimiter::GlobalRateLimiter(int64_t initial_rate)
    : read_rate_(initial_rate),
      write_rate_(initial_rate),
      read_tokens_available_(0),
      write_tokens_available_(0),
      refill_period_us_(100000) // 100 milliseconds
{
    last_refill_time_ = std::chrono::steady_clock::now();
}

void GlobalRateLimiter::RefillTokens()
{
    auto now = std::chrono::steady_clock::now();
    auto time_since_last_refill = std::chrono::duration_cast<std::chrono::microseconds>(now - last_refill_time_).count();

    if (time_since_last_refill >= refill_period_us_)
    {
        read_tokens_available_ = read_rate_ * refill_period_us_ / 1000000;
        write_tokens_available_ = write_rate_ * refill_period_us_ / 1000000;
        last_refill_time_ = now;
    }
}

void GlobalRateLimiter::Request(int64_t bytes, OperationSpec::OpType op_type)
{
    std::unique_lock<std::mutex> lock(mutex_);
    auto now = std::chrono::steady_clock::now();

    // Refill tokens if needed
    RefillTokens();

    // Determine which token bucket to use
    int64_t &tokens_available = (op_type == OperationSpec::GET || op_type == OperationSpec::SEEK) ? read_tokens_available_ : write_tokens_available_;

    // Wait until enough tokens are available
    while (tokens_available < bytes)
    {
        auto wait_duration = std::chrono::microseconds(refill_period_us_);
        cv_.wait_for(lock, wait_duration);
        now = std::chrono::steady_clock::now();

        // Refill tokens
        RefillTokens();
    }

    // Consume tokens
    tokens_available -= bytes;
}

void GlobalRateLimiter::UpdateRates(double elapsed_seconds, const Arguments &args)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (args.sine_mix_rate_interval_ms > 0)
    {
        double mix_rate = SineRate(elapsed_seconds, args.sine_a, args.sine_b, args.sine_d);
        if (mix_rate < 0)
            mix_rate = 0;

        double read_ratio = args.mix_get_ratio + args.mix_seek_ratio;
        double write_ratio = args.mix_put_ratio;

        read_rate_ = static_cast<int64_t>(mix_rate * read_ratio);
        write_rate_ = static_cast<int64_t>(mix_rate * write_ratio);
    }
}

// Centralized Access Pattern Generator
class AccessPatternGenerator
{
public:
    AccessPatternGenerator(const Arguments &args);
    ~AccessPatternGenerator();

    OperationSpec GetNextOperation();

private:
    const Arguments &args_;
    Distribution dist_;
    QueryDecider query_decider_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<OperationSpec> operation_queue_;
    std::mt19937_64 rng_;
    std::uniform_real_distribution<double> uniform_dist_;
    std::uniform_int_distribution<int64_t> rand_dist_;

    std::thread generator_thread_;
    std::atomic<bool> running_;

    bool use_prefix_modeling_;
    bool use_dist_modeling_;

    void GeneratorThreadFunc();
    std::vector<OperationSpec> GenerateBatchOfOperations(int batch_size);
};

AccessPatternGenerator::AccessPatternGenerator(const Arguments &args)
    : args_(args),
      dist_(args.keyrange_num, 0.0, args.value_k, args.value_sigma),
      rng_(std::random_device{}()),
      uniform_dist_(0.0, 1.0),
      rand_dist_(0, args.num - 1),
      running_(true),
      use_prefix_modeling_(false),
      use_dist_modeling_(false)
{
    // Initialize operation ratios
    std::vector<double> ratios = {args.mix_get_ratio, args.mix_put_ratio, args.mix_seek_ratio};
    query_decider_.Initiate(ratios);

    // Initialize distributions
    if (args.keyrange_dist_a != 0.0 || args.keyrange_dist_b != 0.0 ||
        args.keyrange_dist_c != 0.0 || args.keyrange_dist_d != 0.0)
    {
        use_prefix_modeling_ = true;
        dist_.InitiateExpDistribution(args.num, args.keyrange_dist_a, args.keyrange_dist_b,
                                      args.keyrange_dist_c, args.keyrange_dist_d);
    }

    if (args.key_dist_a != 0.0 && args.key_dist_b != 0.0 && args.keyrange_dist_a == 0.0)
    {
        use_dist_modeling_ = true;
    }

    // Start the generator thread
    generator_thread_ = std::thread(&AccessPatternGenerator::GeneratorThreadFunc, this);
}

AccessPatternGenerator::~AccessPatternGenerator()
{
    running_ = false;
    cv_.notify_all();
    if (generator_thread_.joinable())
    {
        generator_thread_.join();
    }
}

void AccessPatternGenerator::GeneratorThreadFunc()
{
    const int BATCH_SIZE = 1000;      // Generate 1000 operations at a time
    const int MAX_QUEUE_SIZE = 10000; // Maximum queue size to avoid memory issues

    while (running_)
    {
        // Generate a batch of operations if queue is not too large
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (operation_queue_.size() < MAX_QUEUE_SIZE)
            {
                auto batch = GenerateBatchOfOperations(BATCH_SIZE);
                for (auto &op : batch)
                {
                    operation_queue_.push(std::move(op));
                }
                cv_.notify_all();
            }
        }

        // Sleep a bit to avoid spinning
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

std::vector<OperationSpec> AccessPatternGenerator::GenerateBatchOfOperations(int batch_size)
{
    std::vector<OperationSpec> batch;
    batch.reserve(batch_size);

    for (int i = 0; i < batch_size; i++)
    {
        OperationSpec op;

        // Determine operation type using the query decider
        int64_t rand_num = rand_dist_(rng_);
        int query_type = query_decider_.GetQueryType(rand_num);

        // Set operation type
        op.op_type = static_cast<OperationSpec::OpType>(query_type);

        // Generate key based on distribution parameters
        int64_t key_rand = 0;
        if (use_dist_modeling_)
        {
            // Use power-law distribution for key hotness
            double u = uniform_dist_(rng_);
            double key_seed = dist_.PowerLawCdfInversion(u, args_.key_dist_a, args_.key_dist_b);
            std::mt19937_64 key_rng(static_cast<uint64_t>(key_seed));
            key_rand = key_rng() % args_.num;
        }
        else if (use_prefix_modeling_)
        {
            // Use exponential distribution for key range hotness
            key_rand = dist_.DistGetKeyID(rand_num, args_.key_dist_a, args_.key_dist_b) % args_.num;
        }
        else
        {
            key_rand = rand_num % args_.num;
        }

        op.key_rand = key_rand;

        // For PUT operations, generate value size
        if (op.op_type == OperationSpec::PUT)
        {
            op.value_size = dist_.GenerateValueSize();
        }
        else
        {
            op.value_size = 0;
        }

        batch.push_back(std::move(op));
    }

    return batch;
}

OperationSpec AccessPatternGenerator::GetNextOperation()
{
    std::unique_lock<std::mutex> lock(mutex_);

    // Wait for an operation to be available
    cv_.wait(lock, [this]()
             { return !operation_queue_.empty(); });

    // Get the next operation
    OperationSpec op = std::move(operation_queue_.front());
    operation_queue_.pop();

    return op;
}

// Function to generate keys of specified size
void GenerateKey(int64_t key_rand, int key_size, std::string &key, std::mt19937_64 &rng)
{
    std::stringstream ss;
    ss << std::setw(2 * sizeof(int64_t)) << std::setfill('0') << std::hex << key_rand;
    key = ss.str();

    // Fill the remaining bytes with 'y' if needed
    while (key.length() < static_cast<size_t>(key_size))
    {
        key += 'y'; // or any character you prefer
    }

    // Trim the key if it's longer than key_size
    if (key.length() > static_cast<size_t>(key_size))
    {
        key = key.substr(0, key_size);
    }
}

// Function to connect to server using either Unix domain socket or TCP
int connectToCrestDB(const std::string &socketPath, const std::string &ip, int port, bool use_tcp)
{
    if (use_tcp)
    {
        // Use TCP socket
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
            std::cerr << "Failed to connect to TCP socket at " << ip << ":" << port << "\n";
            close(sockfd);
            return -1;
        }

        return sockfd;
    }
    else
    {
        // Use UNIX domain socket
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
            std::cerr << "Failed to connect to UNIX domain socket at " << socketPath << "\n";
            close(sockfd);
            return -1;
        }

        return sockfd;
    }
}

// Benchmark Worker Class
class BenchmarkWorker
{
public:
    BenchmarkWorker(int thread_id, const Arguments &args, Stats &stats,
                    AccessPatternGenerator &pattern_generator, GlobalRateLimiter &rate_limiter);
    void Run();

private:
    int thread_id_;
    const Arguments &args_;
    Stats &stats_;
    AccessPatternGenerator &pattern_generator_;
    GlobalRateLimiter &rate_limiter_;
    int sockfd_;
    std::mt19937_64 rng_;

    int64_t operations_performed_;
    int64_t operations_target_;

    bool ConnectToServer();
    void RunFillRandom();
    void RunMixGraph();

    void PerformGet(const std::string &key);
    void PerformPut(const std::string &key, int value_size);
    void PerformSeek(const std::string &key);
};

BenchmarkWorker::BenchmarkWorker(int thread_id, const Arguments &args, Stats &stats,
                                 AccessPatternGenerator &pattern_generator, GlobalRateLimiter &rate_limiter)
    : thread_id_(thread_id),
      args_(args),
      stats_(stats),
      pattern_generator_(pattern_generator),
      rate_limiter_(rate_limiter),
      sockfd_(-1),
      rng_(std::random_device{}()),
      operations_performed_(0)
{
    // For fillrandom, each thread does an equal share of operations
    if (args_.benchmarks == "fillrandom")
    {
        int64_t keys_per_thread = args_.num / args_.threads;
        operations_target_ = (thread_id_ == args_.threads - 1) ? (args_.num - (keys_per_thread * (args_.threads - 1))) : keys_per_thread;
    }
    // For mixgraph, each thread does an equal share of operations
    else if (args_.benchmarks == "mixgraph")
    {
        int64_t ops_per_thread = args_.reads / args_.threads;
        operations_target_ = (thread_id_ == args_.threads - 1) ? (args_.reads - (ops_per_thread * (args_.threads - 1))) : ops_per_thread;
    }
}

bool BenchmarkWorker::ConnectToServer()
{
    if (args_.use_tcp)
    {
        sockfd_ = connectToCrestDB("", args_.ip, args_.port, true);
    }
    else
    {
        sockfd_ = connectToCrestDB("/tmp/server.sock", "", 0, false);
    }
    return sockfd_ != -1;
}

void BenchmarkWorker::Run()
{
    if (!ConnectToServer())
    {
        std::cerr << "Thread " << thread_id_ << " failed to connect to server\n";
        return;
    }

    if (args_.benchmarks == "fillrandom")
    {
        RunFillRandom();
    }
    else if (args_.benchmarks == "mixgraph")
    {
        RunMixGraph();
    }

    // Close the connection
    close(sockfd_);
}

void BenchmarkWorker::RunFillRandom()
{
    // Calculate the key range for this thread
    int64_t keys_per_thread = args_.num / args_.threads;
    int64_t start_key = thread_id_ * keys_per_thread;
    int64_t end_key = (thread_id_ == args_.threads - 1) ? args_.num : (thread_id_ + 1) * keys_per_thread;

    // Create a vector of key indices for this thread's range
    std::vector<int64_t> key_indices(end_key - start_key);
    for (int64_t i = 0; i < key_indices.size(); ++i)
    {
        key_indices[i] = start_key + i;
    }

    // Shuffle the key indices to ensure random order of insertion
    std::shuffle(key_indices.begin(), key_indices.end(), rng_);

    for (int64_t i = 0; i < key_indices.size(); ++i)
    {
        // Generate key of specified size
        std::string key;
        GenerateKey(key_indices[i], args_.key_size, key, rng_);

        // Generate value of specified size
        std::string value(args_.value_size, 'x');

        PerformPut(key, args_.value_size);
        operations_performed_++;
    }
}

void BenchmarkWorker::RunMixGraph()
{
    const int rate_limiter_request_interval = 100;  // Request tokens every 100 operations
    const int rate_limiter_bytes_per_request = 100; // Request 100 bytes per rate limiter request

    int64_t gets = 0;
    int64_t puts = 0;
    int64_t seeks = 0;

    auto benchmark_start_time = std::chrono::steady_clock::now();

    while (operations_performed_ < operations_target_)
    {
        // Get the next operation to perform
        OperationSpec op = pattern_generator_.GetNextOperation();

        // Generate key string
        std::string key;
        GenerateKey(op.key_rand, args_.key_size, key, rng_);

        // Perform the operation
        auto op_start = std::chrono::steady_clock::now();

        switch (op.op_type)
        {
        case OperationSpec::GET:
            PerformGet(key);
            gets++;
            // Rate limiter requests for read operations
            if (args_.sine_mix_rate_interval_ms > 0 && (gets + seeks) % rate_limiter_request_interval == 0)
            {
                rate_limiter_.Request(rate_limiter_bytes_per_request, OperationSpec::GET);
            }
            break;

        case OperationSpec::PUT:
            PerformPut(key, op.value_size);
            puts++;
            // Rate limiter requests for write operations
            if (args_.sine_mix_rate_interval_ms > 0 && puts % rate_limiter_request_interval == 0)
            {
                rate_limiter_.Request(rate_limiter_bytes_per_request, OperationSpec::PUT);
            }
            break;

        case OperationSpec::SEEK:
            PerformSeek(key);
            seeks++;
            // Rate limiter requests for read operations
            if (args_.sine_mix_rate_interval_ms > 0 && (gets + seeks) % rate_limiter_request_interval == 0)
            {
                rate_limiter_.Request(rate_limiter_bytes_per_request, OperationSpec::SEEK);
            }
            break;
        }

        // sleep to simulate delay
        // nanosleep((const struct timespec[]){{0, 500000}}, NULL);

        operations_performed_++;

        // Update rate limiter periodically based on elapsed time
        if (args_.sine_mix_rate_interval_ms > 0)
        {
            auto now = std::chrono::steady_clock::now();
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - benchmark_start_time).count();
            if (elapsed_ms % args_.sine_mix_rate_interval_ms < 100)
            { // Update every interval (with a small window)
                double elapsed_seconds = elapsed_ms / 1000.0;
                rate_limiter_.UpdateRates(elapsed_seconds, args_);
            }
        }
    }
}

void BenchmarkWorker::PerformGet(const std::string &key)
{
    std::string command = "get " + key + "\n";
    auto op_start = std::chrono::steady_clock::now();

    if (send(sockfd_, command.c_str(), command.length(), 0) == -1)
    {
        std::cerr << "Thread " << thread_id_ << " failed to send GET command\n";
        return;
    }

    // Read response
    char buffer[65536];
    int bytesReceived = read(sockfd_, buffer, sizeof(buffer) - 1);
    if (bytesReceived <= 0)
    {
        std::cerr << "Thread " << thread_id_ << " failed to receive response or connection closed\n";
        return;
    }
    buffer[bytesReceived] = '\0';

    // Check if response is ERR
    if (strcmp(buffer, "ERR\n") == 0)
    {
        // Key not found - normal for benchmark
    }

    auto op_end = std::chrono::steady_clock::now();
    int64_t latency_us = std::chrono::duration_cast<std::chrono::microseconds>(op_end - op_start).count();

    stats_.AddOperation("GET", args_.value_size);
    stats_.AddLatency("GET", latency_us);
}

void BenchmarkWorker::PerformPut(const std::string &key, int value_size)
{
    // Generate value of specified size
    std::string value(value_size, 'x');

    std::string command = "set " + key + " " + value + "\n";
    auto op_start = std::chrono::steady_clock::now();

    if (send(sockfd_, command.c_str(), command.length(), 0) == -1)
    {
        std::cerr << "Thread " << thread_id_ << " failed to send PUT command\n";
        return;
    }

    // Read response
    char buffer[65536];
    int bytesReceived = read(sockfd_, buffer, sizeof(buffer) - 1);
    if (bytesReceived <= 0)
    {
        std::cerr << "Thread " << thread_id_ << " failed to receive response or connection closed\n";
        return;
    }
    buffer[bytesReceived] = '\0';

    // Check if response is OK
    if (strcmp(buffer, "OK\n") != 0)
    {
        // std::cerr << "Thread " << thread_id_ << " unexpected response from server: " << buffer << std::endl;
    }

    auto op_end = std::chrono::steady_clock::now();
    int64_t latency_us = std::chrono::duration_cast<std::chrono::microseconds>(op_end - op_start).count();

    stats_.AddOperation("PUT", value_size);
    stats_.AddLatency("PUT", latency_us);
}

void BenchmarkWorker::PerformSeek(const std::string &key)
{
    // SEEK operation - Not implemented for CrestDB, just simulate latency
    auto op_start = std::chrono::steady_clock::now();

    // Simulate a SEEK operation with a small delay
    std::this_thread::sleep_for(std::chrono::microseconds(100));

    auto op_end = std::chrono::steady_clock::now();
    int64_t latency_us = std::chrono::duration_cast<std::chrono::microseconds>(op_end - op_start).count();

    stats_.AddOperation("SEEK", args_.value_size);
    stats_.AddLatency("SEEK", latency_us);
}

void StatusReporter(Stats &stats, std::atomic<bool> &benchmark_running)
{
    const int report_interval_seconds = 10;
    while (benchmark_running)
    {
        std::this_thread::sleep_for(std::chrono::seconds(report_interval_seconds));
        stats.ReportIntermediate();
    }
}

int main(int argc, char *argv[])
{
    Arguments args;
    if (!args.ParseArguments(argc, argv))
    {
        return 1;
    }

    // Display connection info
    if (args.use_tcp)
    {
        std::cout << "Running benchmark with " << args.threads << " threads using TCP connection to "
                  << args.ip << ":" << args.port << std::endl;
    }
    else
    {
        std::cout << "Running benchmark with " << args.threads << " threads using Unix domain socket" << std::endl;
    }

    // Create shared resources
    Stats stats;
    AccessPatternGenerator pattern_generator(args);
    GlobalRateLimiter rate_limiter(1000000); // Initial rate

    // Create barrier for synchronized start
    std::barrier sync_point(args.threads + 1);

    // Start time tracking
    stats.Start();

    // Create and start worker threads
    std::vector<std::thread> worker_threads;
    for (int i = 0; i < args.threads; i++)
    {
        worker_threads.emplace_back([&args, &stats, &pattern_generator,
                                     &rate_limiter, &sync_point, i]()
                                    {
            BenchmarkWorker worker(i, args, stats, pattern_generator, rate_limiter);
            
            // Wait for all threads to be ready
            sync_point.arrive_and_wait();
            
            // Run the benchmark
            worker.Run(); });
    }

    // Start status reporting thread
    std::atomic<bool> benchmark_running(true);
    std::thread status_thread(StatusReporter, std::ref(stats), std::ref(benchmark_running));

    // Start the benchmark
    sync_point.arrive_and_wait();

    // Wait for completion
    for (auto &t : worker_threads)
    {
        t.join();
    }

    // Stop time tracking
    stats.Stop();

    // Stop the status reporter
    benchmark_running = false;
    if (status_thread.joinable())
    {
        status_thread.join();
    }

    // Report final statistics
    stats.Report();

    return 0;
}