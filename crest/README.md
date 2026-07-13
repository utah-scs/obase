<div align="center">

<img src="../docs/img/crestkv-logo.svg" width="420" alt="CrestKV">

**A memory-efficient key-value store, built on
[address space engineering](../README.md)**

</div>

CrestKV keeps throughput within a few percent of an unmanaged baseline
while running in up to 70% less resident memory under skewed workloads.
Its ten highly concurrent data structures — three hash tables (Harris, Pugh, Java-CHM-style),
three skip lists (sequential, Fraser, Herlihy), three B+ trees (coarse,
OCC, Masstree), and an adaptive radix trie — are all OBASE-managed: cold
objects continuously migrate onto pages the kernel can reclaim, while the
hot set stays clustered on huge pages. CrestKV appears as *CrestDB* in the
[papers](../README.md#research).

## Run it

Prerequisites and full setup are in
[../docs/OPERATIONS.md](../docs/OPERATIONS.md) (Linux, clang/LLVM-12,
libevent, prefix-built jemalloc, a swap device for the cold tier).

```bash
make all                                   # default structure: Masstree
taskset -c 0-8 bin/crest-server 127.0.0.1 6363 1 8

# in a second terminal: load data, enable OBASE
cd ../YCSB && python2.7 bin/ycsb load crest -s -P workloads/workloadScatter_a \
  -p recordcount=1000000 -p fieldlength=1024 -threads 8 -jvm-args="-Xmx2g"
cd ../crest
bin/crest-cli 127.0.0.1 6363 "obase decay"
bin/crest-cli 127.0.0.1 6363 "obase migrate"

# watch resident memory fall while a skewed workload runs
../tools/trackRSS 10 $(pgrep -x crest-server) rss.dat
```

The server speaks a line-framed text protocol — `SET`/`GET`/`DEL`/`SCAN`
plus `obase decay|migrate|none` to control tracking and migration
(reference in [../docs/OPERATIONS.md](../docs/OPERATIONS.md)). All YCSB
core workloads (A–F) and request distributions run against it, as do
Twitter and Meta production trace replays:
[../docs/BENCHMARKS.md](../docs/BENCHMARKS.md).

## Choosing a data structure

The active structure is selected at build time:

```bash
make all DS=ht_pugh    # any of: ht_harris ht_pugh ht_chm sl_seq sl_fraser
                       #         sl_hierlihy bpt_seq bpt_occ bpt_mass trie_art
```

Every structure lives in a namespace named after its file and exposes the
same five operations, so nothing else changes (`dsconfig.h`). A bare
`make all` compiles every structure through the pipeline; `DS=` on the
command line builds just that one. `EXTRA_DS=<name>` adds a structure
without editing the Makefile — see [../docs/ADOPTING.md](../docs/ADOPTING.md)
for bringing your own. The server logs `Data structure: <name>` at startup.

Other build knobs: `ENABLE_LLVM_PASS=0` builds an uninstrumented baseline
for comparisons; `ENABLE_VALIDATION=0` skips the build-time usage checks
(debugging only). `make test-compiler` runs the compiler tests;
per-structure instrumentation reports land in `bin/llvm/*_report.txt`.

## Source layout

| Path | Contents |
|------|----------|
| `datastructures/` | The ten OBASE-managed structures. Each `.cc` is checked by the validator, compiled to IR, instrumented, then compiled to an object. |
| `runtime/` | The OBASE runtime: guides (`Guide.hpp`, `GuideVoid.cc`), developer annotations (`GuideAnnotations.h`), operation tracking (`TagRuntime.cc`), the live-object bitmap (`soda.*`), the segregated arenas (`Sama.*`), and the collector that classifies and migrates objects (`ObjectCollector.*`). |
| `compiler/` | The passes: visibility extraction, annotation conversion, usage validation, IR instrumentation — and their tests (`compiler/tests/`). |
| root | The server (`crest_server.*`, `ProcessCommand.*`), CLI client (`crest_client.cc`), globals (`globalConfig.*`), structure selection (`dsconfig.h`). |

## The operation contract

Every structure implements `insert` / `search` / `searchCopy` / `remove` /
`scanCopy` in its own namespace:

- GETs go through `searchCopy()`, which copies the value out while the
  operation is still running. Raw pointers returned by `search()` must
  never be dereferenced after the operation returns since the object may have
  moved. A new structure defines this wrapper in its own `.cc` so the
  instrumentation covers it.
- `scanCopy(start, len, n, out)` returns up to n key/value pairs from the
  first key ≥ start; unordered structures return −1 and the server answers
  `ERR unsupported`. (`bpt_mass` scans in Masstree's internal slice order
  rather than byte order; keys sharing prefixes still come out in
  contiguous runs.)

Three invariants hold everywhere (rationale in the
[paper](https://www.usenix.org/conference/osdi26/presentation/banakar)):
managed objects are jemalloc-allocated; only per-record data is managed,
never traversal pointers; each managed object is reached through exactly
one pointer.

## Scope

Single node, in-memory, no persistence or replication — a memcached-like
deployment model. GET/SET/DEL/SCAN; scans need an ordered structure.
`CREST_SCAN_INTERVAL_S=<seconds>` overrides the default 120 s
classification window (used by the verification scripts, which run at
5–15 s).
