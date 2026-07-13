# Tools

Measurement and experiment helpers for CrestDB/OBASE. Build the C tools with
`gcc -O2 -o <name> <name>.c`.

| Tool | Purpose |
|------|---------|
| `trackRSS` | Sample a process's RSS (KiB, from `/proc/<pid>/smaps`) every N seconds: `./trackRSS <interval> <pid> <outfile>`. No sudo needed for same-user processes. |
| `memoryPressure` | Allocate and pin memory to create memory pressure for reclamation experiments: `./memoryPressure.x <GiB>`. |
| `monitor_tpp.sh` | Periodically sample TPP/NUMA promotion-demotion counters from vmstat: `./monitor_tpp.sh <interval>`. |
| `colocateExp` | Multi-tenant consolidation experiment driver (thesis E5): see `run_colocateExp.sh`. |
| `perf_crest.sh` / `perf_mem.sh` | Wrap `perf stat` around a running crest-server (CPU / memory event sets). Results land in `perf_results.txt`. |
| `perf_flame.sh` | Periodic `perf record` samples of a running crest-server for flame graphs. |
| `verify_migration.sh [names]` | Per-structure migration integrity check: builds `DS=<name>`, loads via YCSB, drives NEW→COLD→HOT migration cycles at a 5s scan interval, and re-reads known keys after each direction, failing if any byte changed. ~2 min per structure. |
| `verify_rss_workloadA.sh [names]` | Per-structure RSS-reduction check under YCSB-A (50/50 read/update, 1M records, 60s windows): requires bulk demotion, MADV_PAGEOUT rounds, an RSS drop, byte-exact re-reads of known keys, and zero runtime errors. ~11 min per structure. |
| `verify_pass2_e2e.sh [names]` | Pass 2 end-to-end check: for each raw-pointer, `OBASE_GUIDED`-annotated fixture in `crest/compiler/tests/e2e/` (`ht_conv`, plus de-converted evaluation structures `ht_chm_conv` and `sl_seq_conv`), verifies it compiles as plain C++, converts it with `guide-converter`, builds it as an extra structure through the full pipeline, and runs it through `verify_migration.sh`. |
| `verify_delete.py [threads] [secs]` | DELETE integrity stress against a running server: mixed SET/GET/DEL/SCAN from several connections, verifying GET-after-SET is byte-exact, GET-after-DEL is a miss, and a persistent key set survives. Run with migration active to stress the OC against deletions. |
| `verify_ycsb_matrix.sh` | Full YCSB coverage matrix with migration active: workloads A–F on bpt_mass, A–D,F on ht_chm, and a request-distribution sweep (uniform/zipfian/latest/hotspot/exponential/sequential). PASS requires zero errors, zero server-log errors, live server. ~25 min. |
| `traceReader/` | Real-trace workload drivers (build with `make` in that directory): `twitterZstLoader.x`/`twitterZstReplay.x` (Twitter production cache traces, streamed from `.sort.zst`), `kvTraceGenerator`/`kvTraceReplay`/`multiThreadedKvTraceReplay.x` (Meta CacheLib kvcache CSV traces), and `meta_bench.x` + `runMetaBench.sh` (Meta mixgraph synthetic capacity workload). Usage in `docs/BENCHMARKS.md`. |
