# OBASE experiment record

## Recovered history

No completed research experiment can be identified from saved raw results in
this checkout. Starting source: `3fdab892ba77bbd6ac842117d1302688a494a3cb`
on `vp/obj`; throughput runner introduced in `1c945ff`. README/paper figures
are published claims, not recovered or revalidated experiment artifacts.

## SETUP-2026-10-16 — replacement-machine validation

**Hypothesis:** The existing source can build and serve correct basic
operations on the replacement machine after installing its documented
dependencies. This is a setup check, not a performance measurement.

**Configuration:** Ubuntu 22.04.5, kernel `5.15.0-185-generic`, four NUMA
nodes/224 logical CPUs/~754 GiB RAM, no swap during testing, THP `madvise`.
Clang/LLVM 12.0.1, OpenJDK 17.0.20.1, Maven 3.6.3, jemalloc 5.3.0
(`54eaed1d8b56b1aa528be3bdd1877e59c56fa90c`, `jem_` function prefix).
Crest source unchanged from `3fdab89`; documentation edits only.
All ten structures compiled; runtime check used `bpt_mass`, four server
workers and four test threads, no CPU affinity, 5-second collector interval,
20-second test, decay and migration enabled.

**Commands:** Dependency installation is recorded in `docs/OPERATIONS.md`.
From `/home/cc/obase`:

```bash
make -C crest all test-compiler JEMALLOC_DIR=/home/cc/jemalloc -j8
(cd YCSB && mvn -B -pl site.ycsb:crest-binding -am clean package -Dcheckstyle.skip)
sudo sysctl -w vm.overcommit_memory=1
CREST_SCAN_INTERVAL_S=5 crest/bin/crest-server 127.0.0.1 6363 1 4
```

With that server running, from another terminal:

```bash
crest/bin/crest-cli 127.0.0.1 6363 'SET setup-check recovered-machine'
crest/bin/crest-cli 127.0.0.1 6363 'GET setup-check'
crest/bin/crest-cli 127.0.0.1 6363 'obase decay'
crest/bin/crest-cli 127.0.0.1 6363 'obase migrate'
python3 tools/verify_delete.py 4 20
```

**Results:** All ten structures compiled. Compiler tests: 16 passed, 0 failed.
YCSB tests: 41 passed, 0 failures/errors/skips; distribution packaged.
SET returned `OK`, GET returned the exact value, and both OBASE commands
returned `OK`. Integrity check: 192,630 SETs, 481,510 GETs, 192,630 DELs,
1,925 SCANs; zero reported integrity errors. No Python traceback or server
error/critical messages were found. Migration logs showed promotion, but the
cold heap was empty in the inspected rounds; bulk demotion and reclaim were
not established.

**Failures encountered:** Initial compilation failed on missing C++ headers;
installing `libstdc++-12-dev` resolved it. Initial server startup aborted with
`Could not map memory for arena`; setting overcommit to 1 resolved it. The
smoke wrapper's broad `Reference count` search matched normal `reference
counting` log messages and returned 1, despite the integrity check reporting
zero errors. A narrower log inspection found no corresponding runtime error.
SIGTERM did not finish shutting down the test server, requiring SIGKILL of
that server only. No application code or test scripts were changed.

**Conclusion:** Build prerequisites and basic concurrent integrity are
validated on the original kernel. Kernel/swap setup, cold relocation,
reclamation, clean shutdown, and performance validation remain outstanding.
Do not compare these untimed smoke-test operation counts to paper throughput.

**Artifacts:** `/home/cc/obase-artifacts/setup-2026-10-16/` contains
`obase-crest-build.log`, `obase-ycsb-build.log`,
`obase-jemalloc-configure.log`, `obase-jemalloc-build.log`,
`obase-server-smoke.log`, and `obase-integrity-smoke.log`.
The server log contains the successful second startup; the initial failure
is recorded above. No remote artifact copy has been verified.
