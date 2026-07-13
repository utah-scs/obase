#!/bin/bash
# Workload-A RSS-reduction verification, per data structure.
# 1M records (10M objects, ~10GB), 60s scan interval, threshold 3 windows.
# PASS requires: load OK, bulk demotion, >=1 MADV_PAGEOUT round, RSS drop
# >=20% from the post-load baseline, byte-exact sentinels after demotion,
# zero runtime errors, server alive.

set -u
CREST=/home/vin/evolve/tidal/crest
YCSB=/home/vin/evolve/tidal/YCSB
SCRATCH=/tmp/claude-1000/-home-vin-evolve-tidal/36c4f7b3-ef1d-4da2-9054-6c254f0db481/scratchpad
export LD_LIBRARY_PATH=/home/vin/jemalloc/lib:${LD_LIBRARY_PATH:-}

DS_LIST=${1:-"ht_harris ht_pugh ht_chm sl_seq sl_fraser sl_hierlihy bpt_seq bpt_occ bpt_mass trie_art"}
SENT_VAL="wa-sentinel-payload-0123456789abcdefghijklmnopqrstuvwxyz"

cleanup() {
  pkill -9 -f "site.ycsb" 2>/dev/null
  pkill -9 -f "bin/ycsb" 2>/dev/null
  pkill -9 -x trackRSS 2>/dev/null
  pkill -9 -x crest-server 2>/dev/null
  sleep 2
}

cli() { $CREST/bin/crest-cli 127.0.0.1 6363 "$1" 2>/dev/null | head -1; }

last_rss() { grep -vE '^0$|^$' "$1" 2>/dev/null | tail -1; }

# wait_for <regex> <count> <timeout_s> <log>
wait_for() {
  local pat=$1 cnt=$2 tmo=$3 log=$4 t=0
  while [ $t -lt $tmo ]; do
    [ "$(grep -cE "$pat" $log 2>/dev/null)" -ge "$cnt" ] && return 0
    sleep 5; t=$((t+5))
  done
  return 1
}

for ds in $DS_LIST; do
  cleanup
  LOG=$SCRATCH/wa-$ds.log
  RSSF=$SCRATCH/wa-$ds-rss.dat
  rm -f $RSSF
  RES="FAIL"

  ( cd $CREST && make all DS=$ds -j8 >/dev/null 2>&1 ) || { echo "$ds RESULT=BUILD_FAIL"; continue; }

  ( cd $CREST && CREST_SCAN_INTERVAL_S=60 taskset -c 0-8 bin/crest-server 127.0.0.1 6363 1 8 > $LOG 2>&1 & )
  sleep 2
  SPID=$(pgrep -x crest-server | head -1)
  [ -n "$SPID" ] || { echo "$ds RESULT=BOOT_FAIL"; cleanup; continue; }
  /home/vin/evolve/tidal/tools/trackRSS 10 $SPID $RSSF >/dev/null 2>&1 &

  for i in 1 2 3; do cli "SET wsent$i $SENT_VAL-$i" >/dev/null; done

  ( cd $YCSB && taskset -c 9-16 python2.7 bin/ycsb load crest -s -P workloads/workloadScatter_a \
      -p recordcount=1000000 -p operationcount=1000000 -p fieldlength=1024 -threads 8 \
      -p requestdistribution=zipfian -jvm-args="-Xmx2g" > $SCRATCH/wa-$ds-load.log 2>&1 )
  loaded=$(grep -c "INSERT], Return=OK, 1000000" $SCRATCH/wa-$ds-load.log)

  sleep 15   # let trackRSS capture the post-load plateau
  base=$(last_rss $RSSF)

  cli "hades decay" >/dev/null; cli "hades migrate" >/dev/null
  ( cd $YCSB && taskset -c 9-16 python2.7 bin/ycsb run crest -s -P workloads/workloadScatter_a \
      -p recordcount=1000000 -p operationcount=500000000 -p readallfields=false -threads 8 \
      -p requestdistribution=zipfian -jvm-args="-Xmx2g" > $SCRATCH/wa-$ds-run.log 2>&1 & )

  # init 3x60s windows, then demote rounds; wait for two pageout rounds
  wait_for "Paging out" 2 660 $LOG; paged_ok=$?
  sleep 45   # let the pageout finish and samples accumulate

  end=$(last_rss $RSSF)
  drop="n/a"; drop_ok=no
  if [ -n "$base" ] && [ -n "$end" ] && [ "$base" -gt 0 ]; then
    drop=$(( (base - end) * 100 / base ))
    [ "$drop" -ge 20 ] && drop_ok=yes
  fi

  sent=OK
  for i in 1 2 3; do
    [ "$(cli "GET wsent$i")" = "$SENT_VAL-$i" ] || sent=BAD
  done

  dmax=$(grep -oE "Demoted [0-9]+" $LOG | awk '{print $2}' | sort -n | tail -1)
  bulk_ok=no; [ "${dmax:-0}" -ge 1000000 ] && bulk_ok=yes
  pageouts=$(grep -c "Paging out" $LOG)
  errors=$(grep -cE "saturated|invalid heap|Failed to allocate|purge failed|Reference count" $LOG)
  alive=$(pgrep -x crest-server >/dev/null && echo yes || echo no)
  ycsb_alive=$(pgrep -f "site.ycsb" >/dev/null && echo yes || echo no)

  if [ "$loaded" = "1" ] && [ $paged_ok -eq 0 ] && [ "$bulk_ok" = "yes" ] && [ "$drop_ok" = "yes" ] && \
     [ "$sent" = "OK" ] && [ "$errors" = "0" ] && [ "$alive" = "yes" ]; then
    RES="PASS"
  fi
  echo "$ds RESULT=$RES base=$((${base:-0}/1024))MB end=$((${end:-0}/1024))MB drop=${drop}% pageouts=$pageouts demote_max=$dmax sentinels=$sent errors=$errors alive=$alive ycsb=$ycsb_alive"
  cleanup
done
echo "ALL DONE"
