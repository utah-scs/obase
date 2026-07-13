# Benchmarks and workloads

All drivers speak CrestKV's line-framed protocol over the UNIX socket
(`/tmp/server.sock`) or TCP. Start a server first (see
[OPERATIONS.md](OPERATIONS.md)); enable OBASE with
`bin/crest-cli 127.0.0.1 6363 "obase decay"` then `"obase migrate"`.

## YCSB

The binding lives in `YCSB/crest/` (rebuild with
`mvn -pl crest package -DskipTests` after editing). Each YCSB field is
stored as its own KV pair under the compound key `key:field`. All core
workloads A–F and all request distributions (uniform, zipfian, latest,
hotspot, exponential, sequential) are supported; workload E (scans)
requires an ordered structure.

```bash
cd YCSB
# load 1M records (10 fields x 1 KiB)
taskset -c 20-30 python2.7 bin/ycsb load crest -s -P workloads/workloadScatter_a \
  -p recordcount=1000000 -p operationcount=1000000 -p fieldlength=1024 \
  -threads 8 -jvm-args="-Xmx2g"
# skewed read-only run
taskset -c 20-30 python2.7 bin/ycsb run crest -s -P workloads/workloadScatter_c \
  -p recordcount=1000000 -p operationcount=500000000 -p requestdistribution=zipfian \
  -p readallfields=false -threads 8 -jvm-args="-Xmx2g"
```

Notes:
- `workloadScatter_c` reads a single field per operation, so exactly one of
  a record's ten field objects stays hot — the configuration used for the
  paper's page-utilization experiments.
- Stock `workloada`–`workloadf` work unmodified; workload D defaults to
  `requestdistribution=latest`.
- Pass `-p crest.host=<ip> -p crest.port=<port>` for TCP.
- Cap the client JVM (`-jvm-args="-Xmx2g"`); its default heap is large
  enough to distort memory experiments on small machines.

`tools/verify_ycsb_matrix.sh` runs the full sweep — workloads A–F, all six
distributions, on both an ordered and a hash structure — with migration
active, and reports PASS/FAIL for each combination (~25 minutes).

## Production traces (`tools/traceReader/`, build with `make`)

### Twitter cache traces

Replays the timestamp-sorted Twitter production cache traces
(`cluster{7,12,23,52}.sort.zst`), streamed with libzstd — no decompressed
copy on disk. Argument order is `<trace.zst> <ip> <port> [threads] [queue]`;
with `127.0.0.1` the UNIX socket is used and the port argument is ignored.
Threads default to all cores if omitted.

```bash
./twitterZstLoader.x /mnt/nvme/data/twitter/cluster7.sort.zst 127.0.0.1 6363 6   # load keyspace
./twitterZstReplay.x /mnt/nvme/data/twitter/cluster7.sort.zst 127.0.0.1 6363 6   # replay
```

### Meta workloads

Two kinds:

**Synthetic capacity (mixgraph)** — `meta_bench.x` generates Meta's
ZippyDB-style mixgraph traffic (key-range and power-law key distributions,
sine-modulated QPS) directly against the server. `runMetaBench.sh` wraps
the paper configurations:

```bash
./runMetaBench.sh fillrandom 0     # load
./runMetaBench.sh all_dist 0       # run; types: all_random, all_dist,
                                   # prefix_random, prefix_dist
                                   # second arg 1 = sine QPS throttling
```

**Trace replay** — replays the CacheLib kvcache traces (CSV: key,op,size):

```bash
./loadMetaTraces.sh                                            # load keyspace
./kvTraceReplay <trace.csv> 127.0.0.1 6363 [ops_per_second]    # single connection
./multiThreadedKvTraceReplay.x <folder> 127.0.0.1 6363 6       # threaded, whole folder
```

`kvTraceReplay` prints progress every 1M operations and full latency
statistics at completion.

## Measurement

- RSS over time: `tools/trackRSS 10 $(pgrep -x crest-server) out.dat`
  (KiB, sampled from `/proc/<pid>/smaps` every 10 s; no sudo for same-user
  processes).
- Overhead: `tools/overhead_scaling.sh` measures instrumented vs
  uninstrumented (`ENABLE_LLVM_PASS=0`) throughput across thread counts;
  `tools/overhead_cpu.sh` measures server CPU per operation, which resolves
  instrumentation cost even when throughput is bounded by client
  round-trips.
- `tools/perf_crest.sh`, `tools/perf_mem.sh`, `tools/perf_flame.sh` wrap
  `perf` around a running server.

## Verification scripts

Each script sets up a server, drives load, and checks results; all print
PASS/FAIL.

| Script | What it checks |
|--------|----------------|
| `tools/verify_migration.sh [names]` | Data survives relocation: forces objects through full NEW→COLD→HOT migration cycles under concurrent traffic and re-reads known keys after each move (~2 min per structure). |
| `tools/verify_rss_workloadA.sh [names]` | Memory actually falls: under a 50/50 read/update workload, cold objects are demoted, the kernel reclaims them, and resident memory drops — with values still byte-exact afterward. |
| `tools/verify_ycsb_matrix.sh` | Every YCSB workload and request distribution completes without errors while migration runs. |
| `tools/verify_delete.py` | Deletes are safe alongside migration: a read after a write returns the exact value, a read after a delete misses, and long-lived keys stay intact. |
| `tools/verify_pass2_e2e.sh` | The annotation path works end to end: plain-C++ sources with `OBASE_GUIDED` marks convert, build, and pass the migration check above. |
| `make test-compiler` (in `crest/`) | The converter's output matches expected files and compiles; the validator accepts correct patterns and rejects each misuse class. |
