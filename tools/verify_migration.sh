#!/bin/bash
# Per-data-structure migration verification.
# For each DS: build, boot (8 workers), plant sentinels, YCSB-load 20k records,
# enable migration with a 5s scan interval, run a concurrent read workload,
# then byte-verify sentinels after demotion (START->COLD) and after
# re-promotion (COLD->HOT). Scan the log for runtime errors.

set -u
CREST=/home/vin/evolve/tidal/crest
YCSB=/home/vin/evolve/tidal/YCSB
SCRATCH=/tmp/claude-1000/-home-vin-evolve-tidal/36c4f7b3-ef1d-4da2-9054-6c254f0db481/scratchpad
export LD_LIBRARY_PATH=/home/vin/jemalloc/lib:${LD_LIBRARY_PATH:-}

DS_LIST=${1:-"ht_harris ht_pugh ht_chm sl_seq sl_fraser sl_hierlihy bpt_seq bpt_occ bpt_mass trie_art"}
SENT_VAL="migration-sentinel-payload-0123456789abcdefghijklmnopqrstuvwxyz"

cleanup() {
  pkill -f "ycsb run" 2>/dev/null
  pkill -x crest-server 2>/dev/null
  sleep 1
}

cli() { $CREST/bin/crest-cli 127.0.0.1 6363 "$1" 2>/dev/null | head -1; }

# wait_for <regex> <count> <timeout_s> <log>
wait_for() {
  local pat=$1 cnt=$2 tmo=$3 log=$4 t=0
  while [ $t -lt $tmo ]; do
    [ "$(grep -cE "$pat" $log 2>/dev/null)" -ge "$cnt" ] && return 0
    sleep 2; t=$((t+2))
  done
  return 1
}

for ds in $DS_LIST; do
  cleanup
  LOG=$SCRATCH/verify-$ds.log
  RES="FAIL"
  echo "=== $ds ==="

  ( cd $CREST && make all DS=$ds -j8 >/dev/null 2>&1 ) || { echo "$ds BUILD_FAIL"; continue; }

  ( cd $CREST && CREST_SCAN_INTERVAL_S=5 taskset -c 0-8 bin/crest-server 127.0.0.1 6363 1 8 > $LOG 2>&1 & )
  sleep 2
  grep -q "Data structure: $ds" $LOG || { echo "$ds WRONG_DS"; cleanup; continue; }

  # sentinels (planted before the bulk load)
  for i in 1 2 3; do cli "SET sent$i $SENT_VAL-$i" >/dev/null; done

  # bulk load
  ( cd $YCSB && taskset -c 9-16 python2.7 bin/ycsb load crest -s -P workloads/workloadScatter_a \
      -p recordcount=20000 -p operationcount=20000 -p fieldlength=1024 -threads 8 \
      -p requestdistribution=zipfian -jvm-args="-Xmx1g" > $SCRATCH/verify-$ds-load.log 2>&1 )
  loaded=$(grep -c "INSERT], Return=OK, 20000" $SCRATCH/verify-$ds-load.log)

  # migration on, concurrent reads on
  cli "hades decay" >/dev/null; cli "hades migrate" >/dev/null
  ( cd $YCSB && taskset -c 9-16 python2.7 bin/ycsb run crest -s -P workloads/workloadScatter_c \
      -p recordcount=20000 -p operationcount=100000000 -p readallfields=false -threads 8 \
      -p requestdistribution=zipfian -jvm-args="-Xmx1g" > $SCRATCH/verify-$ds-run.log 2>&1 & )

  # wait for a demote round with actual demotions (one round drains the
  # whole cold set here since it is far below the per-round cap)
  wait_for "Demoted [1-9][0-9]* objects" 1 90 $LOG; demoted_ok=$?

  # verify sentinels after demotion (byte-exact through the COLD copy)
  post_demote=OK
  for i in 1 2 3; do
    [ "$(cli "GET sent$i")" = "$SENT_VAL-$i" ] || post_demote=BAD
  done

  # the GETs above re-touched them -> wait for a promote round, then verify again
  wait_for "Promoted [1-9][0-9]* objects" 1 60 $LOG; promoted_ok=$?
  sleep 12   # two more scan windows so the promotion definitely lands
  post_promote=OK
  for i in 1 2 3; do
    [ "$(cli "GET sent$i")" = "$SENT_VAL-$i" ] || post_promote=BAD
  done

  errors=$(grep -cE "saturated|invalid heap|Failed to allocate|purge failed|Reference count" $LOG)
  alive=$(pgrep -x crest-server >/dev/null && echo yes || echo no)
  dmax=$(grep -oE "Demoted [0-9]+" $LOG | awk '{print $2}' | sort -n | tail -1)
  pmax=$(grep -oE "Promoted [0-9]+" $LOG | awk '{print $2}' | sort -n | tail -1)

  # bulk demotion must have moved the cold mass, not just the sentinels
  bulk_ok=no; [ "${dmax:-0}" -ge 100000 ] && bulk_ok=yes
  if [ "$loaded" = "1" ] && [ $demoted_ok -eq 0 ] && [ $promoted_ok -eq 0 ] && [ "$bulk_ok" = "yes" ] && \
     [ "$post_demote" = "OK" ] && [ "$post_promote" = "OK" ] && [ "$errors" = "0" ] && [ "$alive" = "yes" ]; then
    RES="PASS"
  fi
  echo "$ds RESULT=$RES load=$loaded bulk_demote=$bulk_ok(max=$dmax) promote_max=$pmax post_demote=$post_demote post_promote=$post_promote errors=$errors alive=$alive"
  cleanup
done
echo "ALL DONE"
