#!/bin/bash
# YCSB-A reclamation: >=20% RSS reduction, bulk demotion, pageout, and integrity.
# Defaults preserve the 1M-record, 60s-window workload. For a bounded smoke test:
# RECORDS=100000 SCAN_INTERVAL=15 RUN_SECONDS=180 bash tools/verify_rss_workloadA.sh bpt_mass
# SCRATCH must not exist. CPU sets, THREADS, and JEMALLOC_DIR are configurable.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
CREST=$ROOT/crest
YCSB=$ROOT/YCSB
JEMALLOC_DIR=${JEMALLOC_DIR:-$HOME/jemalloc}
export LD_LIBRARY_PATH=$JEMALLOC_DIR/lib:${LD_LIBRARY_PATH:-}
THREADS=${THREADS:-8}
SERVER_CPUS=${SERVER_CPUS:-$(awk '/Cpus_allowed_list:/ {print $2}' /proc/self/status)}
CLIENT_CPUS=${CLIENT_CPUS:-$SERVER_CPUS}
SCRATCH=$(realpath -m "${SCRATCH:-/tmp/obase-rss-$(date -u +%Y%m%dT%H%M%SZ)}")
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

RECORDS=${RECORDS:-1000000}
SCAN_INTERVAL=${SCAN_INTERVAL:-60}
RUN_SECONDS=${RUN_SECONDS:-660}
printf 'records=%s fieldlength=1024 scan_interval=%s run_seconds=%s\n' \
  "$RECORDS" "$SCAN_INTERVAL" "$RUN_SECONDS" >> "$SCRATCH/environment.txt"
SENT_VAL=wa-sentinel-payload-0123456789abcdefghijklmnopqrstuvwxyz
DS_LIST=${1:-"ht_harris ht_pugh ht_chm sl_seq sl_fraser sl_hierlihy bpt_seq bpt_occ bpt_mass trie_art"}
for ds in $DS_LIST; do
  start "$ds" "$SCAN_INTERVAL"
  for i in 1 2 3; do [ "$(cli "SET wsent$i $SENT_VAL-$i")" = OK ]; done
  timeout -k 5s 1800s taskset -c "$CLIENT_CPUS" java -Xmx2g -cp "$YCSB_CP" \
    site.ycsb.Client -db site.ycsb.db.CrestClient -load -s \
    -P "$YCSB/workloads/workloadScatter_a" -threads "$THREADS" \
    -p recordcount="$RECORDS" -p operationcount="$RECORDS" -p fieldlength=1024 \
    > "$SCRATCH/$ds-load.log" 2>&1
  metrics "$SCRATCH/$ds-load.log" INSERT "$RECORDS"
  sleep 5
  base=$(rss)
  [ "$base" -gt 0 ]
  printf 'elapsed_s rss_kib swap_kib\n' > "$SCRATCH/$ds-memory.tsv"
  [ "$(cli 'obase decay')" = OK ]
  [ "$(cli 'obase migrate')" = OK ]
  taskset -c "$CLIENT_CPUS" java -Xmx2g -cp "$YCSB_CP" \
    site.ycsb.Client -db site.ycsb.db.CrestClient -t -s \
    -P "$YCSB/workloads/workloadScatter_a" -threads "$THREADS" \
    -p recordcount="$RECORDS" -p operationcount=500000000 -p fieldlength=1024 \
    -p readallfields=false -p maxexecutiontime="$RUN_SECONDS" \
    > "$SCRATCH/$ds-run.log" 2>&1 &
  CPID=$!
  started=$SECONDS
  deadline=$((SECONDS + RUN_SECONDS + 60))
  while kill -0 "$CPID" 2>/dev/null; do
    kill -0 "$SPID"
    [ "$SECONDS" -lt "$deadline" ] || { echo "YCSB timed out"; exit 1; }
    # smaps_rollup counts THP once; the legacy trackRSS tool double-counts it.
    awk -v elapsed="$((SECONDS - started))" \
      '/^Rss:/ {r=$2} /^Swap:/ {s=$2} END {print elapsed, r, s}' \
      "/proc/$SPID/smaps_rollup" >> "$SCRATCH/$ds-memory.tsv"
    sleep 5
  done
  wait "$CPID"
  CPID=
  metrics "$SCRATCH/$ds-run.log" READ
  metrics "$SCRATCH/$ds-run.log" UPDATE
  end=$(rss)
  swap=$(awk '/^Swap:/ {print $2}' "/proc/$SPID/smaps_rollup")
  for i in 1 2 3; do [ "$(cli "GET wsent$i")" = "$SENT_VAL-$i" ]; done
  check_logs
  kill -0 "$SPID"
  dmax=$(grep -oE 'Demoted [0-9]+' "$LOG" | awk 'BEGIN {m=0} $2>m {m=$2} END {print m}')
  pageouts=$(count 'Target promotion rate reached: Paging out')
  drop=$(( (base - end) * 100 / base ))
  echo "$ds base_kib=$base end_kib=$end swap_kib=$swap drop_pct=$drop pageouts=$pageouts demote_max=$dmax sentinels=OK"
  [ "$dmax" -ge "$RECORDS" ]
  [ "$pageouts" -ge 2 ]
  [ "$drop" -ge 20 ]
  [ "$swap" -gt 0 ]
  echo "$ds RESULT=PASS"
  cleanup
done
