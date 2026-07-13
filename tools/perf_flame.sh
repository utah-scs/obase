#!/bin/bash

OUTPUT_DIR="perf_samples"
INTERVAL=30  # 60 seconds for each sample period
SAMPLE_DURATION=10  # How long each sample should last in seconds

mkdir -p $OUTPUT_DIR

start_sampling() {
    PID=$(pgrep -f "crest-server 127.0.0.1 6363 1 1")
    if [ -z "$PID" ]; then
        echo "Process not found. Please start the process first."
        exit 1
    fi

    echo "Starting sampling for PID: $PID every $INTERVAL seconds"
    echo "Samples will be saved in $OUTPUT_DIR"
    echo "Press Ctrl+C to stop monitoring"

    for (( i=1; i<=10; i++ )); do
        echo "Sampling iteration $i..."
        # Sample with perf record for SAMPLE_DURATION seconds
        sudo perf record -F 99 -p $PID -g -- sleep $SAMPLE_DURATION

        # Move the perf.data file to a uniquely named output for each interval
        mv perf.data "$OUTPUT_DIR/perf_sample_$i.data"
        
        # Wait until the next interval before starting the next sample
        sleep $((INTERVAL - SAMPLE_DURATION))
    done

    echo "Sampling complete. Results are saved in $OUTPUT_DIR"
}

process_samples() {
    echo "Processing samples to generate flame graphs..."

    # Ensure the FlameGraph scripts are available in the directory
    FLAMEGRAPH_DIR="/home/vin/FlameGraph"
    if [ ! -d "$FLAMEGRAPH_DIR" ]; then
        echo "FlameGraph directory not found. Please clone it from https://github.com/brendangregg/FlameGraph"
        exit 1
    fi

    for file in $OUTPUT_DIR/*.data; do
        echo "Processing $file..."
        
        # Generate out.perf for each sample
        sudo perf script -i "$file" > "$file.perf"

        # Collapse stacks
        $FLAMEGRAPH_DIR/stackcollapse-perf.pl "$file.perf" > "$file.folded"

        # Generate flame graph
        $FLAMEGRAPH_DIR/flamegraph.pl "$file.folded" > "${file%.data}.svg"
        echo "Flame graph saved to ${file%.data}.svg"
    done
}

case "$1" in
    start)
        start_sampling
        ;;
    process)
        process_samples
        ;;
    *)
        echo "Usage: $0 {start|process}"
        exit 1
esac
