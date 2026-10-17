# OBASE progress

## Repository permission update — 2026-10-17

Updated `AGENTS.md` to require explicit approval before each Git commit or
push, including automated persistence actions, and to prohibit Codex author,
co-author, or contributor attribution. Aligned the continuity instructions
with these requirements. The user approved committing and pushing this
documentation update to `vp/obj` using
`vinita-pawar <vinitapawar2912@gmail.com>`. Its parent checkpoint is
`b7682bb0fdccba8c6c93bcce048d6dac40997bee`; use `git log -1 -- AGENTS.md`
to identify the instruction-update commit.
No experiments were run or artifacts created for this documentation change.
No persistence script is configured and no backup was performed.
Next action: review the separate local experiment-record updates before
requesting approval to commit or push them. Artifact backup remains deferred.

## Recovery checkpoint — 2026-10-16

The previous machine was lost. This session started from branch `vp/obj`,
commit `3fdab892ba77bbd6ac842117d1302688a494a3cb`, with a clean working tree.
Remote: `git@github.com:utah-scs/obase.git`. No previous experiment logs,
result tables, or continuity records were found in the checkout. The latest
completed research experiment cannot yet be established. Commit `1c945ff`
provides the throughput runner, but is not evidence that a run completed.

## Accomplished

- Added the requested continuity instructions to `AGENTS.md`.
- Documented reproducible dependency setup in `docs/OPERATIONS.md`.
- Installed Clang/LLVM 12.0.1, libclang development files, libevent,
  OpenJDK 17.0.20.1, Maven 3.6.3, allocator build tools, and numactl.
- Installed `libstdc++-12-dev` after Clang selected GCC 12 and failed to
  find C++ standard headers.
- Built jemalloc 5.3.0 with `--with-jemalloc-prefix=jem_` at
  `/home/cc/jemalloc`, source commit
  `54eaed1d8b56b1aa528be3bdd1877e59c56fa90c`. Verified exported
  `jem_malloc`, `jem_free`, and `jem_mallctl` symbols.
- Built all ten Crest data structures with validation/instrumentation enabled;
  linked the default `bpt_mass` server. Compiler checks: 16 passed, 0 failed.
- Built the Crest YCSB distribution; 41 tests passed, 0 failures/errors/skips.
- Set `vm.overcommit_memory=1` after server startup failed to reserve its
  virtual arenas with the original setting of 0; persisted it in
  `/etc/sysctl.conf`.
- A four-worker, 20-second integrity check with migration enabled reported
  zero integrity errors. See `EXPERIMENTS.md` for its limitations.
- On 2026-10-17, a 60-second post-reboot Masstree integrity check passed
  with zero errors (683,641 SETs, 1,708,982 GETs, 683,641 DELETEs).
  Approved migration/RSS script updates replace obsolete paths/commands,
  use built Java dependencies, restrict cleanup to owned processes, and
  measure RSS with `smaps_rollup` to avoid double-counting THP.

## Machine and pending setup

- Ubuntu 22.04.5; running kernel `5.15.0-185-generic` at session start.
- Four NUMA nodes; 224 logical CPUs; approximately 754 GiB RAM.
- THP mode: `madvise`. No swap at session start.
- User approved installing the Ubuntu 6.8 HWE kernel and a 30 GiB swap file.
  Kernel `6.8.0-138-generic` and its initramfs are installed. `/swapfile` is
  active (30 GiB) and registered in `/etc/fstab`. Following the authorized
  reboot, checks on 2026-10-16 (America/Denver) confirmed running kernel
  `6.8.0-138-generic`, active 30 GiB swap, `vm.overcommit_memory=1`, and THP
  `madvise`. Crest binaries, linked libraries, and setup logs remain available.
- User approved these two tracking files and selected their private laptop
  as the prospective artifact destination. Laptop OS, SSH connection details,
  and final destination path are still needed. The user deferred laptop backup
  setup; no artifact transfer is verified.

## Failures and unfinished work

- The integrity-check server did not exit after SIGTERM; its identified PID
  was stopped with SIGKILL. No source fix has been made or approved.
- The smoke-check wrapper returned 1 because an overly broad error search
  matched ordinary "reference counting" migration messages. A narrower
  inspection found no error/critical, allocation, purge, or invalid-heap errors.
- The migration and RSS scripts are being validated after approved updates.
  Other legacy helpers still need review for old paths, Python 2, CPU
  assumptions, and broad process cleanup before use.
- Full cold demotion/promotion, RSS reduction, and throughput experiments
  have not been validated on this machine. The short integrity check is not
  evidence for those results.
- No persistence script is configured. Adding one requires user approval.
- Prior research results remain unlocated and unvalidated.

## Exact next actions

1. Finish the running Masstree migration check, inspect its results, and run
   the RSS-reclamation check. Commands and current artifact locations are in
   `EXPERIMENTS.md` under POSTBOOT-2026-10-17. Inspect existing server/client
   processes before attempting another run.
2. Resume laptop backup setup when the user is ready. Obtain OS/SSH details,
   prepare artifact transfer commands, and verify an actual transfer.
3. Validate migration and RSS behavior before resuming the throughput matrix
   from `tools/run_obase_throughput.py`; choose CPU sets using this machine's
   NUMA topology. Locate old result artifacts before claiming continuity.

## Artifact and persistence status

Setup logs are collected at
`/home/cc/obase-artifacts/setup-2026-10-16/` (also available as
`/tmp/obase-*.log`). These are local ephemeral artifacts, not backups.
Pre-reboot checkpoint `1dddaf6099d44086ea5d6d62bca7681cbbe69a3e` was pushed to
`origin/vp/obj`, and `git ls-remote` confirmed the same remote commit. This
status update follows that checkpoint; use `git log -1 -- PROGRESS.md` for
the newest record. Laptop artifact transfer has not been verified, and no
persistence script exists yet. The combined Git-and-artifact backup is
therefore incomplete.
