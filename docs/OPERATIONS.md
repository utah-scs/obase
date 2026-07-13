# Building and operating CrestKV

## Prerequisites

- Linux 6.1+ (6.11+ for the batched-pageout optimization below)
- libevent (`apt install libevent-dev`)
- clang/LLVM 12 (`apt install llvm-12* clang-12`) — the compiler pipeline
  is pinned to this version
- jemalloc built with a function prefix, so CrestKV's managed heap coexists
  with glibc malloc:

  ```bash
  ./autogen.sh --with-jemalloc-prefix=jem_ && make
  # in the shell that runs crest-server:
  export LD_LIBRARY_PATH=/path/to/jemalloc/lib:$LD_LIBRARY_PATH
  ```

  The jemalloc include/lib paths are set at the top of `crest/Makefile`.
- `echo 1 > /proc/sys/vm/overcommit_memory` if the large virtual
  reservations are refused (the HOT/COLD regions are reserved virtually and
  faulted on demand).

## Build

```bash
cd crest
make all              # every structure through the pipeline; links the default (bpt_mass)
make all DS=ht_pugh   # build and link one structure
```

Useful variants: `make all ENABLE_LLVM_PASS=0` builds without OBASE
instrumentation (baseline comparisons); `make converter` builds the
annotation-conversion tool used in [ADOPTING.md](ADOPTING.md).

## Run

```bash
taskset -c 0-8 bin/crest-server 127.0.0.1 6363 1 8
#                               ip        port loglevel workers
```

With ip `127.0.0.1` the server listens on the UNIX socket
`/tmp/server.sock`; any other address uses TCP. The server logs
`Data structure: <name>` at startup. Give it a few seconds to boot — the
HOT/COLD reservations are large.

Client:

```bash
bin/crest-cli 127.0.0.1 6363                 # interactive
bin/crest-cli 127.0.0.1 6363 "obase decay"   # one-shot command
```

OBASE is off until enabled:

| Command | Effect |
|---------|--------|
| `obase decay`   | start access tracking (decay scans each window) |
| `obase migrate` | enable migration + pageout |
| `obase none`    | stop tracking and migration |

`CREST_SCAN_INTERVAL_S=<seconds>` overrides the default 120 s scan window
(useful for tests; the verification scripts use 5–15 s).

## Wire protocol

Line-framed text; one `\n`-terminated response per command. Values may
contain spaces but never `\n`.

| Command | Response |
|---------|----------|
| `SET <key> <value...>` | `OK` (value is the rest of the line) |
| `GET <key>` | the raw value, or `ERR` on miss |
| `DEL <key>` | `OK` / `ERR` |
| `SCAN <start> <n>` | `OK <count> <klen> <vlen> <key> <value> ...` — parse by lengths, not tokens; `ERR unsupported` on hash structures |
| `obase decay\|migrate\|none` | `OK` |

Unterminated input is capped at 1 MiB per connection.

## Memory environment

OBASE's frontend needs a backend to reclaim what it segregates. The
simplest is a swap file (cold pages go to swap after `MADV_PAGEOUT`):

```bash
dd if=/dev/zero of=/swapfile bs=1M count=30720
mkswap /swapfile && chmod 600 /swapfile && swapon /swapfile
# on pmem, mount /dev/pmem0 (ext4, 4K blocks) and place the swapfile there
```

Huge pages for the HOT region: the default configuration uses transparent
huge pages (`MemType::DRAM_2MB_THP`), which needs no setup. Alternatives
selected in `ProcessCommand.h`:

- preallocated 2 MiB pages: `sysctl -w vm.nr_hugepages=N` and
  `MemType::DRAM_2MB`
- 1 GiB pages: `default_hugepagesz=1G hugepagesz=1G hugepages=N` on the
  kernel command line and `MemType::DRAM_1GB`

Optional kernel-side reclaim batching (Linux 6.11+, if the patch is
present): `echo 1 > /sys/module/vmscan/parameters/use_batch_pageout`.

## Running under a memory limit (cgroup)

```bash
sudo cgexec -g memory:crest bash -c \
  'export LD_LIBRARY_PATH=...; taskset -c 0-8 bin/crest-server 127.0.0.1 6363 1 8'
echo 8G > /sys/fs/cgroup/crest/memory.high
chown $USER /tmp/server.sock   # so unprivileged clients can connect
```

`memory.high` pressure plus OBASE segregation is the intended deployment
shape: the kernel reclaims the COLD region first and the hot set stays
resident.
