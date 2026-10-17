#!/bin/bash
# Validate bulk cold migration and sampled byte integrity on a fresh server.
# Requires a built YCSB distribution. Optional env: SCRATCH (must not exist),
# JEMALLOC_DIR, THREADS, SERVER_CPUS, CLIENT_CPUS.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
CREST=$ROOT/crest
YCSB=$ROOT/YCSB
JEMALLOC_DIR=${JEMALLOC_DIR:-$HOME/jemalloc}
export LD_LIBRARY_PATH=$JEMALLOC_DIR/lib:${LD_LIBRARY_PATH:-}
THREADS=${THREADS:-8}
SERVER_CPUS=${SERVER_CPUS:-$(awk '/Cpus_allowed_list:/ {print $2}' /proc/self/status)}
CLIENT_CPUS=${CLIENT_CPUS:-$SERVER_CPUS}
SCRATCH=$(realpath -m "${SCRATCH:-/tmp/obase-migration-$(date -u +%Y%m%dT%H%M%SZ)}")
SPID= CPID=
cleanup() {
  local pid
  for pid in "$CPID" "$SPID"; do
    [ -n "$pid" ] || continue
    if kill -0 "$pid" 2>/dev/null; then
      kill -TERM "$pid" 2>/dev/null || true
      for _ in {1..30}; do
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.1
      done
      if kill -0 "$pid" 2>/dev/null; then
        echo "Cleanup: PID $pid required SIGKILL"
        kill -KILL "$pid" 2>/dev/null || true
      fi
    fi
    wait "$pid" 2>/dev/null || true
  done
  SPID= CPID=
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
if pgrep -x crest-server >/dev/null; then
  echo "Existing Crest server; refusing to interfere" >&2
  exit 1
fi
# A fresh directory prevents silently replacing previous evidence.
mkdir "$SCRATCH"
exec > >(tee "$SCRATCH/summary.log") 2>&1
echo "Artifacts: $SCRATCH"
{
  git -C "$ROOT" rev-parse HEAD
  git -C "$ROOT" status --short
  uname -a
  swapon --show
  sysctl vm.overcommit_memory
  printf 'threads=%s server_cpus=%s client_cpus=%s\n' "$THREADS" "$SERVER_CPUS" "$CLIENT_CPUS"
} > "$SCRATCH/environment.txt"
git -C "$ROOT" diff > "$SCRATCH/source.patch"
# Use the built distribution, with no Python 2 launcher or Maven during tests.
shopt -s nullglob
archives=("$YCSB"/crest/target/ycsb-crest-binding-*.tar.gz)
[ "${#archives[@]}" -eq 1 ] || { echo "Build the Crest YCSB distribution first"; exit 1; }
mkdir "$SCRATCH/ycsb"
tar -xzf "${archives[0]}" -C "$SCRATCH/ycsb" --wildcards '*/lib/*.jar'
libs=("$SCRATCH"/ycsb/*/lib)
YCSB_CP=${libs[0]}/*
cli() { timeout 10s "$CREST/bin/crest-cli" 127.0.0.1 6363 "$1"; }
rss() { awk '/^Rss:/ {print $2}' "/proc/$SPID/smaps_rollup"; }
count() { grep -cE "$1" "$LOG" || true; }
wait_for() {
  local pattern=$1 wanted=$2 deadline=$((SECONDS + $3))
  while [ "$SECONDS" -lt "$deadline" ]; do
    kill -0 "$SPID" 2>/dev/null || return 1
    [ "$(count "$pattern")" -ge "$wanted" ] && return 0
    sleep 2
  done
  return 1
}
check_logs() {
  if grep -Ei '\[error\]|\[critical\]|saturated|invalid heap|Failed to allocate|purge failed|Reference count\b' "$LOG"; then
    return 1
  fi
}
metrics() {
  # Reuse the existing runner's checks for complete successful operations.
  PYTHONDONTWRITEBYTECODE=1 PYTHONPATH="$ROOT/tools" python3 - "$@" <<'PY'
import sys
from pathlib import Path
from run_obase_throughput import ycsb_metrics
print(ycsb_metrics(Path(sys.argv[1]), sys.argv[2],
                   int(sys.argv[3]) if len(sys.argv) > 3 else None))
PY
}
start() {
  local ds=$1 interval=$2
  make -C "$CREST" all DS="$ds" JEMALLOC_DIR="$JEMALLOC_DIR" -j8 > "$SCRATCH/$ds-build.log" 2>&1
  sha256sum "$CREST/bin/crest-server"
  LOG=$SCRATCH/$ds-server.log
  CREST_SCAN_INTERVAL_S=$interval taskset -c "$SERVER_CPUS" \
    "$CREST/bin/crest-server" 127.0.0.1 6363 1 "$THREADS" > "$LOG" 2>&1 &
  SPID=$!
  local ready=no
  for _ in {1..50}; do
    kill -0 "$SPID" 2>/dev/null || { cat "$LOG"; return 1; }
    if [ "$(cli 'GET verification-connectivity-check' 2>/dev/null || true)" = ERR ]; then
      ready=yes; break
    fi
    sleep 0.2
  done
  [ "$ready" = yes ]
  grep -q "Data structure: $ds" "$LOG"
}

SENT_VAL=migration-sentinel-payload-0123456789abcdefghijklmnopqrstuvwxyz
DS_LIST=${1:-"ht_harris ht_pugh ht_chm sl_seq sl_fraser sl_hierlihy bpt_seq bpt_occ bpt_mass trie_art"}
for ds in $DS_LIST; do
  start "$ds" 5
  for i in 1 2 3; do [ "$(cli "SET sent$i $SENT_VAL-$i")" = OK ]; done
  timeout -k 5s 300s taskset -c "$CLIENT_CPUS" java -Xmx1g -cp "$YCSB_CP" \
    site.ycsb.Client -db site.ycsb.db.CrestClient -load -s \
    -P "$YCSB/workloads/workloadScatter_a" -threads "$THREADS" \
    -p recordcount=20000 -p operationcount=20000 -p fieldlength=1024 \
    > "$SCRATCH/$ds-load.log" 2>&1
  metrics "$SCRATCH/$ds-load.log" INSERT 20000
  [ "$(cli 'obase decay')" = OK ]
  [ "$(cli 'obase migrate')" = OK ]
  taskset -c "$CLIENT_CPUS" java -Xmx1g -cp "$YCSB_CP" \
    site.ycsb.Client -db site.ycsb.db.CrestClient -t -s \
    -P "$YCSB/workloads/workloadScatter_c" -threads "$THREADS" \
    -p recordcount=20000 -p operationcount=100000000 \
    -p maxexecutiontime=120 -p readallfields=false \
    > "$SCRATCH/$ds-run.log" 2>&1 &
  CPID=$!
  wait_for 'Demoted [1-9][0-9]* objects' 1 90
  promotions=$(count 'Promoted [1-9][0-9]* objects')
  for i in 1 2 3; do [ "$(cli "GET sent$i")" = "$SENT_VAL-$i" ]; done
  # Require a NEW promotion round after touching the sentinels.
  wait_for 'Promoted [1-9][0-9]* objects' "$((promotions + 1))" 60
  for i in 1 2 3; do [ "$(cli "GET sent$i")" = "$SENT_VAL-$i" ]; done
  # Let YCSB finish naturally so final operation/error counts are available.
  timeout 150s tail --pid="$CPID" -f /dev/null
  wait "$CPID"
  CPID=
  metrics "$SCRATCH/$ds-run.log" READ
  check_logs
  kill -0 "$SPID"
  dmax=$(grep -oE 'Demoted [0-9]+' "$LOG" | awk 'BEGIN {m=0} $2>m {m=$2} END {print m}')
  pmax=$(grep -oE 'Promoted [0-9]+' "$LOG" | awk 'BEGIN {m=0} $2>m {m=$2} END {print m}')
  [ "$dmax" -ge 100000 ]
  echo "$ds RESULT=PASS bulk_demote_max=$dmax promote_max=$pmax sentinels=OK"
  echo "Sentinel bytes survived migration phases; individual sentinel locations are not instrumented."
  cleanup
done
