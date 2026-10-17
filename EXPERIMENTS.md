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

## Post-reboot setup verification — 2026-10-16 (America/Denver)

**Hypothesis:** The authorized reboot activates the installed kernel and
preserves swap, overcommit, and the built artifacts.

**Configuration/commands:** Same machine, source checkpoint `1dddaf6`;
ran `uname -r`, `uptime -s`, `swapon --show`,
`sysctl vm.overcommit_memory`,
`cat /sys/kernel/mm/transparent_hugepage/enabled`,
`ldd crest/bin/crest-server`, and checked the binaries and setup-log directory.

**Results:** Running kernel `6.8.0-138-generic`; boot time
`2026-10-16 23:43:37 UTC`; `/swapfile` active at 30 GiB, unused;
overcommit 1; THP `madvise`. Server/client and prefixed jemalloc binaries
exist, all server libraries resolve, and the 352 KiB setup-log directory
remains present at `/home/cc/obase-artifacts/setup-2026-10-16/`.

**Conclusion:** Kernel and memory settings survived reboot as intended.
No workload was run in this check; post-reboot integrity, cold migration,
and RSS reclamation remain to be validated. Observations are recorded here;
no separate artifact file or remote artifact copy was created.

## POSTBOOT-2026-10-17 — Masstree integrity and reclamation

**Hypothesis:** The existing instrumented Masstree server retains correct
operation results on kernel 6.8; cold migration can reclaim anonymous memory
through the configured swap backend without corrupting sampled values.

**Configuration:** Source checkpoint `4097e09`; C++ unchanged. Kernel
`6.8.0-138-generic`, 30 GiB swap, overcommit 1, THP `madvise`. The user
approved edits to `tools/verify_migration.sh` and
`tools/verify_rss_workloadA.sh` for this machine. Their exact patch is captured
in each verification artifact directory. No new verification script was added.

**Integrity phase:** Started `crest/bin/crest-server 127.0.0.1 6363 1 4`
with `CREST_SCAN_INTERVAL_S=5`, pinned to CPUs `0,4,8,12,16`; enabled
`obase decay` and `obase migrate`. Ran
`taskset -c 20,24,28,32 python3 tools/verify_delete.py 4 60` with a 75-second
client timeout and 90-second server timeout (3-second kill grace).
Result: **PASS**, 683,641 SETs, 1,708,982 GETs, 683,641 DELETEs, 6,834 SCANs,
zero integrity errors; no Python exceptions or server error/critical messages.
The existing test checks byte-exact GETs, deletion misses, and persistent
keys; SCAN checks response format, not all returned payload bytes.

**Migration phase:** The first attempt below completed its workload checks
(50,419,450 successful reads, largest demotion 180,003 objects, largest
promotion 20,000 objects, sentinels unchanged), but the harness exited 2
after cleanup: an in-place edit during execution shifted the shell's input
and caused a trailing parse error. This is retained as an invalid harness
run, not the accepted verification result. The finalized script is being
rerun in `migration-final/` without editing it while running.

```bash
SCRATCH=/home/cc/obase-artifacts/postboot-2026-10-17/migration \
THREADS=8 SERVER_CPUS=0,4,8,12,16,20,24,28,32 \
CLIENT_CPUS=36,40,44,48,52,56,60,64 \
bash tools/verify_migration.sh bpt_mass
```

The migration check loads 20,000 records with ten 1,024-byte fields, uses a
5-second collector interval and 120-second read workload, and requires bulk
demotion, a subsequent promotion round, successful YCSB operations, and
byte-exact sentinel reads. Individual sentinel locations are not instrumented;
sentinel correctness across migration phases does not itself prove each
sentinel moved.

**Finalized migration rerun (in progress):** Same command above with
`SCRATCH=/home/cc/obase-artifacts/postboot-2026-10-17/migration-final`.

**RSS phase (queued after migration succeeds):**

```bash
SCRATCH=/home/cc/obase-artifacts/postboot-2026-10-17/rss \
THREADS=8 SERVER_CPUS=0,4,8,12,16,20,24,28,32 \
CLIENT_CPUS=36,40,44,48,52,56,60,64 \
RECORDS=100000 SCAN_INTERVAL=15 RUN_SECONDS=180 \
bash tools/verify_rss_workloadA.sh bpt_mass
```

This is a reduced setup check: 100,000 records, ten 1,024-byte fields,
50/50 reads/updates, Zipfian requests, 15-second collector interval,
180-second timed workload. It requires >=20% RSS reduction, positive swap
usage, >=100,000 demotions in a round, >=2 pageout rounds, successful YCSB
reads/updates, and byte-exact sentinels. RSS comes from `smaps_rollup`;
the legacy sampler incorrectly adds THP bytes already included in RSS.

**Artifacts:** `/home/cc/obase-artifacts/postboot-2026-10-17/` contains
`integrity-server.log`, `integrity-client.log`, and separate `migration/`,
`migration-final/`, and `rss/` directories (the last is created when queued
work starts). Both scripts passed `bash -n` and refused to interfere with
an existing server in explicit preflight checks.
These are local ephemeral files; laptop backup remains deferred.
