#!/bin/bash

OUTPUT_FILE="perf_results.txt"

start_monitoring() {
    local interval=$1
    PID=$(pgrep -f "crest-server 127.0.0.1 6363 1 1")
    if [ -z "$PID" ]; then
        echo "Process not found. Please start the process first."
        exit 1
    fi
    
    echo "Starting monitoring for PID: $PID with interval of $interval seconds"
    echo "Results will be written to $OUTPUT_FILE"
    echo "Press Ctrl+C to stop monitoring"

    # Using more commonly available events
    perf stat -e "\
cache-references,cache-misses,\
LLC-loads,LLC-load-misses,\
LLC-stores,LLC-store-misses,\
memory_bw_reads,memory_bw_writes,\
mem_load_retired.l3_miss,mem_inst_retired.all_loads,\
mem_inst_retired.stlb_miss_loads,mem_inst_retired.stlb_miss_stores,\
mem_inst_retired.all_stores" \
        -I $interval -o $OUTPUT_FILE -p $PID -t \* &
    
    PERF_PID=$!
    echo $PERF_PID > perf_pid.txt
}

stop_monitoring() {
    if [ -f perf_pid.txt ]; then
        PERF_PID=$(cat perf_pid.txt)
        kill $PERF_PID
        rm perf_pid.txt
        echo "Monitoring stopped. Results saved in $OUTPUT_FILE"
    else
        echo "No active monitoring found."
    fi
}

case "$1" in
    start)
        if [ -z "$2" ]; then
            echo "Error: Interval not specified"
            echo "Usage: $0 start <interval_in_ms>"
            exit 1
        fi
        start_monitoring $2
        ;;
    stop)
        stop_monitoring
        ;;
    *)
        echo "Usage: $0 {start <interval_in_ms>|stop}"
        exit 1
esac