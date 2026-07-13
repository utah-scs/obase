#!/bin/bash
# YCSB workload/distribution coverage matrix for CrestDB, with OBASE
# decay+migration ACTIVE throughout (15s scan windows).
#
#   - workloads A-F on bpt_mass (E = scans; needs an ordered structure)
#   - workloads A-D,F on ht_chm (hash: scans unsupported by design)
#   - request-distribution sweep on workload B (bpt_mass):
#     uniform zipfian latest hotspot exponential sequential
#
# Each cell: fresh server, YCSB load, timed run (45s). PASS requires at
# least one Return=OK, zero Return=ERROR / UNEXPECTED_STATE, zero server
# log errors, and the server alive at the end. NOT_FOUND is tolerated
# (warned) only on workload D, where reads chase concurrent inserts.
#
# Usage: verify_ycsb_matrix.sh   (~25 min)

set -u
CREST=/home/vin/evolve/tidal/crest
YCSB=/home/vin/evolve/tidal/YCSB
LOGS=${TMPDIR:-/tmp}/ycsb-matrix
RECORDS=200000
RUNSECS=45
export LD_LIBRARY_PATH=/home/vin/jemalloc/lib:${LD_LIBRARY_PATH:-}
mkdir -p $LOGS
cd $CREST

PASS=0; FAIL=0

run_cell() {
  local ds=$1 workload=$2 dist=$3   # dist="-" means workload default
  local tag="$ds-$workload-$dist"
  local extra=""
  [ "$dist" != "-" ] && extra="-p requestdistribution=$dist"

  pkill -x crest-server 2>/dev/null; sleep 1
  ( make all DS=$ds -j16 >/dev/null 2>&1 ) || { echo "FAIL $tag (build)"; FAIL=$((FAIL+1)); return; }
  ( CREST_SCAN_INTERVAL_S=15 taskset -c 0-15 bin/crest-server 127.0.0.1 6363 1 8 > $LOGS/server-$tag.log 2>&1 & )
  sleep 3
  grep -q "Data structure: $ds" $LOGS/server-$tag.log || { echo "FAIL $tag (boot)"; FAIL=$((FAIL+1)); return; }

  ( cd $YCSB && taskset -c 16-31 python2.7 bin/ycsb load crest -s -P workloads/$workload \
      -p recordcount=$RECORDS -p operationcount=$RECORDS -threads 8 \
      -jvm-args="-Xmx1g" > $LOGS/load-$tag.log 2>&1 )
  grep -q "\[INSERT\], Return=OK, $RECORDS" $LOGS/load-$tag.log || { echo "FAIL $tag (load)"; FAIL=$((FAIL+1)); return; }

  bin/crest-cli 127.0.0.1 6363 "obase decay" >/dev/null 2>&1
  bin/crest-cli 127.0.0.1 6363 "obase migrate" >/dev/null 2>&1

  ( cd $YCSB && taskset -c 16-31 python2.7 bin/ycsb run crest -s -P workloads/$workload \
      -p recordcount=$RECORDS -p operationcount=500000000 -p maxexecutiontime=$RUNSECS \
      $extra -threads 8 -jvm-args="-Xmx1g" > $LOGS/run-$tag.log 2>&1 )

  local ok err nf thpt srverr alive
  ok=$(grep -c "Return=OK" $LOGS/run-$tag.log)
  err=$(grep -cE "Return=(ERROR|UNEXPECTED_STATE|NOT_IMPLEMENTED)" $LOGS/run-$tag.log)
  nf=$(grep "Return=NOT_FOUND" $LOGS/run-$tag.log | awk -F', ' '{s+=$3} END {print s+0}')
  thpt=$(grep "^\[OVERALL\], Throughput" $LOGS/run-$tag.log | awk -F', ' '{printf "%.0f", $3}')
  srverr=$(grep -icE "error|saturat|corrupt" $LOGS/server-$tag.log)
  alive=$(pgrep -x crest-server >/dev/null && echo yes || echo no)

  local verdict="PASS"
  [ "$ok" -ge 1 ] || verdict="FAIL(no-ok)"
  [ "$err" -eq 0 ] || verdict="FAIL(errors)"
  [ "$srverr" -eq 0 ] || verdict="FAIL(server-log)"
  [ "$alive" = "yes" ] || verdict="FAIL(dead)"
  if [ "$nf" -gt 0 ] && [ "$workload" != "workloadd" ]; then verdict="FAIL(not-found=$nf)"; fi

  local note=""
  [ "$nf" -gt 0 ] && [ "$workload" = "workloadd" ] && note=" (nf=$nf tolerated)"
  echo "$verdict $tag thpt=${thpt:-NA}$note"
  [ "$verdict" = "PASS" ] && PASS=$((PASS+1)) || FAIL=$((FAIL+1))
}

# workloads A-F on the ordered default structure
for w in workloada workloadb workloadc workloadd workloade workloadf; do
  run_cell bpt_mass $w -
done

# A-D,F on a hash structure (E unsupported by design)
for w in workloada workloadb workloadc workloadd workloadf; do
  run_cell ht_chm $w -
done

# distribution sweep (workload B; zipfian covered above as its default)
for dist in uniform latest hotspot exponential sequential; do
  run_cell bpt_mass workloadb $dist
done

pkill -x crest-server 2>/dev/null
echo "----------------------------------------"
echo "matrix: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
