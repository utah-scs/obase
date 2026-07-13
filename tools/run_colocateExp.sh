#!/bin/bash

# sudo ./run_colocateExp.sh 10000000 256 20000000
# 0.002 (0.2%): 4.88 MiB - Well within L3 cache and TLB coverage
# 0.005 (0.5%): 12.21 MiB - Within L3 cache, exceeds TLB coverage
# 0.01 (1%): 24.41 MiB - Just under L3 cache size
# 0.02 (2%): 48.83 MiB - Exceeds L3 cache size
# 0.05 (5%): 122.07 MiB - Significantly larger than L3 cache
# 0.1 (10%): 244.14 MiB - Very large hot set, much larger than L3 cache


# Check if correct number of arguments are provided
if [ "$#" -ne 3 ]; then
    echo "Usage: $0 <num_objects> <object_size> <num_operations>"
    exit 1
fi

NUM_OBJECTS=$1
OBJECT_SIZE=$2
NUM_OPERATIONS=$3

# Array of hot percentages to test
HOT_PERCENTAGES=(0.002 0.005 0.01 0.02 0.05 0.1 0.2 0.3)

# Page sizes
SMALL_PAGE_SIZE=4096
# HUGE_PAGE_SIZE=$((2 * 1024 * 1024))  # 2 MiB
HUGE_PAGE_SIZE=$((1 * 1024 * 1024 * 1024))  # 1 GiB

# Output file
OUTPUT_FILE="colocateExp_results.txt"

# Clear the output file if it exists
> $OUTPUT_FILE

# Function to run experiment and save results
run_experiment() {
    local hot_percentage=$1
    local clustered=$2
    local page_size=$3
    
    echo "Running experiment: Hot Percentage = $hot_percentage, Clustered = $clustered, Page Size = $page_size" >> $OUTPUT_FILE
    ./colocateExp $NUM_OBJECTS $OBJECT_SIZE $hot_percentage $NUM_OPERATIONS $clustered 1 $page_size >> $OUTPUT_FILE
    echo "" >> $OUTPUT_FILE
}

# Run experiments for each hot percentage
for hot_pct in "${HOT_PERCENTAGES[@]}"; do
    hot_size=$(echo "$NUM_OBJECTS * $OBJECT_SIZE * $hot_pct / (1024*1024)" | bc -l)
    echo "Hot Set Size: $hot_size MiB" >> $OUTPUT_FILE
    echo "" >> $OUTPUT_FILE
    
    # Run spread experiment with 4 KiB pages
    run_experiment $hot_pct 0 $SMALL_PAGE_SIZE
    
    # Run clustered experiment with 2 MiB pages
    run_experiment $hot_pct 1 $HUGE_PAGE_SIZE
    
    echo "----------------------------------------" >> $OUTPUT_FILE
    echo "" >> $OUTPUT_FILE
done

echo "Experiments completed. Results saved in $OUTPUT_FILE"