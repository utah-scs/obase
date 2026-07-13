#!/bin/bash

# Check if interval argument is provided
if [ -z "$1" ]; then
    echo "Usage: $0 <interval_seconds>"
    exit 1
fi

INTERVAL=$1

# Print CSV Header
echo "timestamp,pgpromote_success,pgdemote_kswapd,pgdemote_direct,pgpromote_lru_inactive"

# Loop forever
while true; do
    # Capture current timestamp
    TS=$(date +%s)

    # Read the specific keys from /proc/vmstat into a variable to ensure consistent timing
    # We use awk to create an associative array for the specific keys we need
    # This ensures the columns are always in the correct order regardless of how they appear in the file
    vals=$(awk '
    BEGIN {
        # Initialize to 0 to handle missing keys gracefully
        stats["pgpromote_success"] = 0
        stats["pgdemote_kswapd"] = 0
        stats["pgdemote_direct"] = 0
        stats["pgpromote_lru_inactive"] = 0
    }
    /pgpromote_success|pgdemote_kswapd|pgdemote_direct|pgpromote_lru_inactive/ {
        stats[$1] = $2
    }
    END {
        print stats["pgpromote_success"] "," stats["pgdemote_kswapd"] "," stats["pgdemote_direct"] "," stats["pgpromote_lru_inactive"]
    }
    ' /proc/vmstat)

    # Output to stdout
    echo "$TS,$vals"

    # Wait for N seconds
    sleep "$INTERVAL"
done