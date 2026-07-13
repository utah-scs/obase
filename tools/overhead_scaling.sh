#!/bin/bash
# OBASE overhead and thread scalability (paper Fig 11(b) methodology).
#
# Measures YCSB read-only (workloadScatter_c, zipfian, 1M x 10KB records)
# throughput on three structures spanning synchronization designs --
# ht_pugh (fine-grained locks), sl_fraser (lock-free), bpt_mass (OCC) --
# at 2..32 server worker threads, for two builds:
#   baseline     make all ENABLE_LLVM_PASS=0   (guides present, no TAG/ATC hooks)
#   instrumented make all                      (full OBASE instrumentation)
# No decay or migration is enabled (obase mode stays "none"): this is the
# steady-state instrumentation overhead, normalized per-cell as
# instrumented/baseline.
#
# Client threads = server threads (CrestClient is lockstep, one op per
# round-trip). Server pinned to CPUs 0-15, client to 16-31; at N=32 both
# share all CPUs (oversubscribed, but symmetrically for both builds).
# Each cell: boot, load once, two 60s timed runs (rep1 warms the JVM and
# page cache; report the mean).
#
# Output: data/overhead/fig11b.csv  (build,ds,threads,rep,ops_sec)

set -u
CREST=/home/vin/evolve/tidal/crest
YCSB=/home/vin/evolve/tidal/YCSB
OUT=/home/vin/evolve/tidal/data/overhead
LOGS=${TMPDIR:-/tmp}/overhead-logs
RECORDS=1000000
RUNSECS=60
DS_LIST="ht_pugh sl_fraser bpt_mass"
THREADS="2 4 8 16 32"
export LD_LIBRARY_PATH=/home/vin/jemalloc/lib:${LD_LIBRARY_PATH:-}

mkdir -p $OUT $LOGS
CSV=$OUT/fig11b.csv
echo "build,ds,threads,rep,ops_sec" > $CSV

stop_server() { pkill -x crest-server 2>/dev/null; sleep 1; }

# baseline first; the repo ends on the instrumented default build
for build in baseline instrumented; do
  [ $build = baseline ] && PASS=0 || PASS=1

  # ENABLE_LLVM_PASS is not tracked by any stamp: a full clean is the only
  # safe way to switch build modes.
  ( cd $CREST && make clean >/dev/null && make all DS=ht_pugh ENABLE_LLVM_PASS=$PASS -j16 >/dev/null 2>&1 ) \
    || { echo "BUILD FAIL $build"; exit 1; }

  for ds in $DS_LIST; do
    ( cd $CREST && make all DS=$ds ENABLE_LLVM_PASS=$PASS -j16 >/dev/null 2>&1 ) \
      || { echo "BUILD FAIL $build $ds"; exit 1; }

    for N in $THREADS; do
      if [ $N -eq 32 ]; then SCPU=0-31; CCPU=0-31; else SCPU=0-15; CCPU=16-31; fi
      stop_server
      SLOG=$LOGS/server-$build-$ds-$N.log
      ( cd $CREST && taskset -c $SCPU bin/crest-server 127.0.0.1 6363 1 $N > $SLOG 2>&1 & )
      sleep 3
      grep -q "Data structure: $ds" $SLOG || { echo "BOOT FAIL $build $ds $N"; stop_server; continue; }

      LLOG=$LOGS/load-$build-$ds-$N.log
      ( cd $YCSB && taskset -c $CCPU python2.7 bin/ycsb load crest -s -P workloads/workloadScatter_a \
          -p recordcount=$RECORDS -p operationcount=$RECORDS -p fieldlength=1024 \
          -threads 8 -jvm-args="-Xmx2g" > $LLOG 2>&1 )
      grep -q "\[INSERT\], Return=OK, $RECORDS" $LLOG || { echo "LOAD FAIL $build $ds $N"; stop_server; continue; }

      for rep in 1 2; do
        RLOG=$LOGS/run-$build-$ds-$N-$rep.log
        ( cd $YCSB && taskset -c $CCPU python2.7 bin/ycsb run crest -s -P workloads/workloadScatter_c \
            -p recordcount=$RECORDS -p operationcount=500000000 -p fieldlength=1024 \
            -p requestdistribution=zipfian -p readallfields=false \
            -p maxexecutiontime=$RUNSECS -threads $N -jvm-args="-Xmx2g" > $RLOG 2>&1 )
        thpt=$(grep "^\[OVERALL\], Throughput" $RLOG | awk -F', ' '{print $3}')
        echo "$build,$ds,$N,$rep,${thpt:-NA}" >> $CSV
        echo "  $build $ds N=$N rep$rep: ${thpt:-NA} ops/s"
      done
      stop_server
    done
  done
done

echo "DONE -> $CSV"
