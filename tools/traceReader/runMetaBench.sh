#!/bin/bash

# A shell script to run meta_bench. Takes two arguments, where on that indicates 
# the workload type. The workload type can be one of the following:
# 1. fillrandom -- loads the db
# 2. all_random -- any key from 50M selected randomly with equal probability
# 3. all_dist -- Follows power law distribution over all the keys
# 4. prefix_random -- Certain ranges of keys are selected often, but the keys 
# within the range are selected randomly
# 5. prefix_dist -- Certain ranges of keys are selected often, but the keys within
# the range are selected according to power law distribution

# The second argument is a flag to enable qps throttling. If the flag is set to 1,
# then the qps throttling is enabled.

# sample (prefix_dist w/ qps throttling): 
# ./meta_bench \
#   -benchmarks "mixgraph" \
#   -keyrange_dist_a 14.18 \
#   -keyrange_dist_b -2.917 \
#   -keyrange_dist_c 0.0164 \
#   -keyrange_dist_d -0.08082 \
#   -keyrange_num 30 \
#   -key_dist_a 0.002312 \
#   -key_dist_b 0.3467 \
#   -value_k 0.2615 \
#   -value_sigma 25.45 \
#   -iter_k 2.517 \
#   -iter_sigma 14.236 \
#   -mix_get_ratio 0.86 \
#   -mix_put_ratio 0.14 \
#   -mix_seek_ratio 0.00 \
#   -sine_mix_rate_interval_ms 5000 \
#   -sine_a 1000 \
#   -sine_b 0.000073 \
#   -sine_d 4500 \
#   -reads 420000000 \
#   -num 50000000 \
#   -key_size 48

# Check if correct number of arguments are provided
if [ "$#" -ne 2 ]; then
    echo "Usage: $0 <workload_type> <qps_throttle>"
    echo "Workload types: fillrandom, all_random, all_dist, prefix_random, prefix_dist"
    echo "QPS throttle: 0 (disabled) or 1 (enabled)"
    exit 1
fi

# Assign arguments to variables
WORKLOAD_TYPE=$1
QPS_THROTTLE=$2

# g++ -std=c++11 -O2 -pthread -o meta_bench meta_bench.cc
# Base command
# BASE_CMD="./meta_bench.x -reads 500000000 -num 5000000 -key_size 48 "
# BASE_CMD="./meta_bench.x -reads 420000000 -num 50000000 -key_size 48"
# BASE_CMD="./meta_bench.x -reads 420000000 -num 50000000 -key_size 48 -t 6"
BASE_CMD="./meta_bench.x -reads 5000000000 -num 50000000 -key_size 48 -t 6"
# BASE_CMD="./meta_bench.x -reads 50000000 -num 50000000 -key_size 48 -t 6"

# Set workload-specific parameters
case $WORKLOAD_TYPE in
    fillrandom)
        # PARAMS="-benchmarks fillrandom -value_size 43 "
        PARAMS="-benchmarks fillrandom -value_size 235 "
        # PARAMS="-benchmarks fillrandom -value_size 100 "
        # PARAMS="-benchmarks fillrandom -value_size 1024 "
        ;;
    all_random)
        PARAMS="-benchmarks mixgraph -keyrange_num 1 "
        ;;
    all_dist)
        PARAMS="-benchmarks mixgraph -keyrange_num 1 -key_dist_a 0.002312 -key_dist_b 0.3467 "
        ;;
    prefix_random)
        PARAMS="-benchmarks mixgraph -keyrange_dist_a 14.18 -keyrange_dist_b -3.917 -keyrange_dist_c 0.0364 -keyrange_dist_d -0.08082 -keyrange_num 30 "
        # PARAMS="-benchmarks mixgraph -keyrange_dist_a 14.18 -keyrange_dist_b -2.917 -keyrange_dist_c 0.0164 -keyrange_dist_d -0.08082 -keyrange_num 30 "
        ;;
    prefix_dist)
        PARAMS="-benchmarks mixgraph -keyrange_dist_a 14.18 -keyrange_dist_b -2.917 -keyrange_dist_c 0.0164 -keyrange_dist_d -0.08082 -keyrange_num 30 -key_dist_a 0.002312 -key_dist_b 0.3467 "
        ;;
    *)
        echo "Invalid workload type. Please choose from: fillrandom, all_random, all_dist, prefix_random, prefix_dist"
        exit 1
        ;;
esac

# Add common parameters
COMMON_PARAMS="-value_k 0.2615 -value_sigma 25.45 -iter_k 2.517 -iter_sigma 14.236 -mix_get_ratio 0.86 -mix_put_ratio 0.14 -mix_seek_ratio 0.00 "

# Add QPS throttling if enabled
if [ "$QPS_THROTTLE" -eq 1 ]; then
    # QPS_PARAMS="-sine_mix_rate_interval_ms 5000 -sine_a 1000 -sine_b 0.000073 -sine_d 4500"
    QPS_PARAMS="-sine_mix_rate_interval_ms 5000 -sine_a 20000 -sine_b 0.1047 -sine_d 40000"
else
    QPS_PARAMS=""
fi

# Construct and execute the final command
FINAL_CMD="$BASE_CMD $PARAMS $COMMON_PARAMS $QPS_PARAMS"
echo "Executing command: $FINAL_CMD"
eval $FINAL_CMD