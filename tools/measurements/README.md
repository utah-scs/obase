# Trace measurement

`tools/measure.py` is the shared entry point for new CrestDB experiments.
`collect.py` owns process/swap/perf collection, `parsers.py` owns event decoding,
`sweep.py` owns the configurable baseline/OBASE runner, and `report.py` supplies
the same normalized data to CSV reports and `plot.py`. Do not copy these files
into each experiment directory. Old `.trace-data/` scripts remain frozen
provenance for their historical runs; they are not maintained entry points.

## Requirements

Linux, Python 3.10+, perf with FIFO control and `perf mem` support, and access to
the target process's `/proc` files. Reporting uses only the Python standard
library. Plotting additionally needs `matplotlib` (and its NumPy dependency).
Trace sweeps need cgroup v2, systemd, `numactl`, `taskset`, zstd, the existing
CrestDB build dependencies, and noninteractive sudo for owned transient server
units and, when configured, perf. The runner does not change host perf policy,
swap configuration, NUMA balancing, or filesystem ownership.

```bash
make -C tools/traceReader twitterZstLoader.x twitterZstReplay.x
python3 -m unittest discover -s tools/measurements/tests -v
```

The protocol tests bind temporary UNIX sockets and require an environment that
permits local sockets. For plotting, use an environment with matplotlib, or
install it into a dedicated virtual environment:

```bash
python3 -m venv /tmp/crest-plots
/tmp/crest-plots/bin/pip install matplotlib
```

## Measure an existing server

```bash
python3 tools/measure.py capture --pid 12345 --out /tmp/crest-capture \
  --profiles cpu,samples,uncore --sudo \
  -- tools/traceReader/twitterZstReplay.x /path/to/replay.zst /tmp/server.sock 0 6
```

Use a new output directory. The client command runs in the foreground; its
startup and shutdown are included in the measurement window. This command does
not start or stop the target server. `--profiles none` collects process and
swap data without perf. `--cgroup /sys/fs/cgroup/...` additionally captures
cgroup memory, swap, pressure, and CPU state. `--interval` is in seconds;
`--sample-period` defaults to 4000 and `--ldlat` to 30 cycles. `--timeout`
defaults to 7200 seconds. A failed command returns nonzero and retains partial
metrics, logs, exit status, and collector diagnostics.

`perf_crest.sh` and `perf_mem.sh` are thin aliases for `capture --profiles cpu`
and `capture --profiles samples`. They accept the same remaining arguments.
Their old `start/stop` interface and shared `perf_pid.txt`/`perf_results.txt`
files have been retired. The flame-graph, TPP, overhead, multi-tenant, and legacy
RSS verification helpers serve separate workflows and are not invoked by the
trace collector.

## Run baseline/OBASE across DRAM limits

Build each mode into a separate fresh directory so switching the pass flag
cannot reuse stale objects. For example, with the build dependencies configured:

```bash
make -C crest all DS=bpt_mass ENABLE_LLVM_PASS=0 BIN_DIR=/tmp/crest-build-baseline
make -C crest all DS=bpt_mass ENABLE_LLVM_PASS=1 BIN_DIR=/tmp/crest-build-obase
```

Copy `example.json` to a local configuration and set the paths, trace counts,
CPU lists, NUMA node, and swap backend for the machine. Its example counts and
placement describe the earlier cluster-7 experiment, not universal defaults.
Paths are resolved relative to the invoking working directory. Use an absolute
`LD_LIBRARY_PATH` when the server needs a non-system jemalloc. Specify the
intended DS and binary build flags correctly: the runner checks the DS in the
server log and rejects identical binaries, but hashes alone cannot prove their
compiler settings. Both binaries should include the committed reclamation fixes.

```bash
python3 tools/measure.py sweep --config /path/to/sweep.json --out /path/to/new-run
```

The default order is 2, 4, 8 GiB, baseline then OBASE. Each case gets a fresh
server, a server-only cgroup `MemoryMax`, and its own preload. `MemoryMax`
includes charged cache/kernel memory as well as resident anonymous memory; it
is not a physical NUMA-node capacity setting. The client is outside that cap.
OBASE decay/migration starts after preload; baseline keeps both disabled.
There is one trial per configuration. For independent repetitions, run the
same configuration into separate directories.

Both server and client memory allocations use `memory_node`; CPU affinity is
set separately. PMEM swap placement follows the configured device and host
kernel. Binding allocations to a DRAM node does **not** bind swap traffic to a
PMEM NUMA node. Check `/proc/swaps` and device topology before interpreting
placement. The runner records `/proc/swaps` and server NUMA maps and can require
the configured `swap_backend` to be active; it does not reconfigure swap.

New sweeps use TCP loopback `127.0.0.2:6363` by default, configurable with
`address` and `port`. Crest reserves `127.0.0.1` for a fixed UNIX socket, so this
separate loopback endpoint avoids interfering with that socket. Earlier saved
experiments used UNIX sockets; account for this transport difference when
comparing historical throughput. Each server has a unique owned systemd unit,
bounded lifetime, and verified cleanup. An occupied endpoint fails preflight.

Eight immutable sentinel values are verified before, during, and after replay
(every five seconds by default). A mismatch stops the client and sweep. The
runner also checks full row/operation counts, preload completion, response
errors, OOM events, and runtime safety diagnostics. Sentinels supplement the
runtime checks; they cannot prove the absence of all memory corruption. Their
small server overhead is included in metrics but not trace operation counts.
Metric-quality warnings are recorded; operational or integrity failures stop
the remaining configurations.

The Twitter clients share `traceReader/crest_protocol.h` for full sends and
newline-framed replies. They return failure on connection/protocol errors,
report misses separately, and use fixed random seeds. Replay skips INCR, maps
the supported SET-family operations to unconditional SET, ignores TTL and
original timestamp pacing, and sleeps 100 microseconds per row per worker.
The loader keeps the first size for each key and has a 50-million-key ceiling;
the sweep rejects a preload that reaches that ceiling. Supply counts for the
actual bounded trace. Multi-thread replay does not preserve global trace order.

For detached execution and a five-minute health view:

```bash
nohup python3 -u tools/measure.py sweep --config /path/to/sweep.json \
  --out /path/to/new-run > /path/to/queue.log 2>&1 < /dev/null &
watch -n 300 python3 tools/measure.py status /path/to/new-run
```

`status` shows the recorded phase, runner PID presence, and latest memory
sample. It is an observation, not proof of progress or automated chat updates.
SIGINT/SIGTERM performs cleanup; SIGKILL cannot run cleanup, so server units also
have a maximum runtime. New runs refuse existing output directories, and failed
runs are not silently resumed or overwritten.

## Report and plot

Both new runs and the archived `setup.json`/`results.json` sweeps are supported:

```bash
python3 tools/measure.py summarize /path/to/run --out /path/to/new-summary
/tmp/crest-plots/bin/python tools/measure.py plot /path/to/run --out /path/to/new-plots
```

Reports contain throughput, swap-in/out pages, minor/major faults, DRAM/PMEM
sample counts, CPU counters/IPC, RSS/swap, latency, and optional uncore bandwidth.
Plots export PNG, PDF, SVG, comparison CSV, and per-configuration swap rate CSV.
Swap rates use actual adjacent sample intervals, with gaps for missing/reset
counters. Source results are not rewritten; relative case paths make new runs
relocatable, and archived absolute paths are resolved beside their results file.

Interpretation:

- Process faults and RSS belong to the server. Host swap counts include other
  workloads and readahead; they count base pages, not I/O requests. Page size is
  recorded. New swap-in/out totals include the final boundary snapshot. Legacy
  sampled swap-out/fault totals retain their original values and window labels.
- PEBS memory-source samples are userspace loads selected by a sampling period
  and latency threshold. Cache and unknown samples are also retained. In a
  PMEM-backed swap setup, a page is brought into DRAM before the userspace load;
  zero PMEM samples does not mean there was no PMEM traffic or imply a tier hit ratio.
- IMC counters cover the entire host and all sockets. Built-in raw encodings
  are gated to Cascade Lake (Intel family 6/model 85, stepping >= 5); use other
  profiles on unsupported hardware. Bandwidth uses 64 bytes per request.
- Perf running percentages, lost/throttled samples, raw files, FIFO-acknowledged
  window times, and errors are retained. Nested collector setup/teardown means
  their windows differ slightly. Below 99% running coverage is flagged.
- Missing measurements remain null, and plots label unavailable sample counts
  `N/A`. Failed trials never produce full-trace throughput comparisons. Sentinel
  checks and a successful client are not statistical evidence from repetitions.

`sha256.json`, `setup.json`, `machine.json`, service/command logs, and raw perf
artifacts provide provenance. Raw privileged perf files can require sudo to
read; the collector decodes them through the same privilege path and does not
change their ownership. Keep experiment data outside the source commit.
