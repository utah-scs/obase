#!/bin/bash
# OBASE instrumentation cost in server CPU time per operation.
#
# Companion to overhead_scaling.sh: end-to-end throughput with a lockstep
# client is round-trip-dominated (~50us/op), so single-digit-percent CPU
# overhead does not move it. This driver samples the server's cumulative
# CPU time (/proc/<pid>/stat utime+stime, all threads) around each timed
# run and reports CPU-microseconds per operation, which resolves the
# instrumentation cost directly and converts to the throughput overhead a
# CPU-bound deployment would see.
#
# Same workload as overhead_scaling.sh (read-only zipfian, 1M x 10KB).
# Output: data/overhead/cpu_per_op.csv (build,ds,threads,cpu_us_per_op,ops_sec)

set -u
CREST=/home/vin/evolve/tidal/crest
YCSB=/home/vin/evolve/tidal/YCSB
OUT=/home/vin/evolve/tidal/data/overhead
LOGS=${TMPDIR:-/tmp}/overhead-cpu-logs
RECORDS=1000000
RUNSECS=60
DS_LIST="ht_pugh sl_fraser bpt_mass"
THREADS="8 32"
export LD_LIBRARY_PATH=/home/vin/jemalloc/lib:${LD_LIBRARY_PATH:-}

mkdir -p $OUT $LOGS
CSV=$OUT/cpu_per_op.csv
echo "build,ds,threads,cpu_us_per_op,ops_sec" > $CSV

stop_server() { pkill -x crest-server 2>/dev/null; sleep 1; }

server_cpu_ticks() { awk '{print $14+$15}' /proc/$1/stat; }

for build in baseline instrumented; do
  [ $build = baseline ] && PASS=0 || PASS=1
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
      SPID=$(pgrep -x crest-server)

      ( cd $YCSB && taskset -c $CCPU python2.7 bin/ycsb load crest -s -P workloads/workloadScatter_a \
          -p recordcount=$RECORDS -p operationcount=$RECORDS -p fieldlength=1024 \
          -threads 8 -jvm-args="-Xmx2g" > $LOGS/load-$build-$ds-$N.log 2>&1 )
      grep -q "\[INSERT\], Return=OK, $RECORDS" $LOGS/load-$build-$ds-$N.log \
        || { echo "LOAD FAIL $build $ds $N"; stop_server; continue; }

      # warm-up run (JVM + page cache), not measured
      ( cd $YCSB && taskset -c $CCPU python2.7 bin/ycsb run crest -s -P workloads/workloadScatter_c \
          -p recordcount=$RECORDS -p operationcount=500000000 -p fieldlength=1024 \
          -p requestdistribution=zipfian -p readallfields=false \
          -p maxexecutiontime=20 -threads $N -jvm-args="-Xmx2g" >/dev/null 2>&1 )

      t0=$(server_cpu_ticks $SPID)
      RLOG=$LOGS/run-$build-$ds-$N.log
      ( cd $YCSB && taskset -c $CCPU python2.7 bin/ycsb run crest -s -P workloads/workloadScatter_c \
          -p recordcount=$RECORDS -p operationcount=500000000 -p fieldlength=1024 \
          -p requestdistribution=zipfian -p readallfields=false \
          -p maxexecutiontime=$RUNSECS -threads $N -jvm-args="-Xmx2g" > $RLOG 2>&1 )
      t1=$(server_cpu_ticks $SPID)

      thpt=$(grep "^\[OVERALL\], Throughput" $RLOG | awk -F', ' '{print $3}')
      ms=$(grep "^\[OVERALL\], RunTime" $RLOG | awk -F', ' '{print $3}')
      ops=$(awk -v t=$thpt -v m=$ms 'BEGIN {printf "%.0f", t*m/1000}')
      # ticks are 1/100 s; us/op = dticks*10000/ops
      us=$(awk -v a=$t0 -v b=$t1 -v o=$ops 'BEGIN {printf "%.3f", (b-a)*10000/o}')
      echo "$build,$ds,$N,$us,${thpt:-NA}" >> $CSV
      echo "  $build $ds N=$N: $us us/op (${thpt} ops/s)"
      stop_server
    done
  done
done

echo "DONE -> $CSV"
