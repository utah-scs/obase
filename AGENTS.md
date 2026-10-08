# Repository Guidelines

## Agent Permissions & Communication

- Modify code only when strictly necessary. Explain why and obtain explicit user confirmation before making any code changes, including script edits.
- Ask for and receive explicit user confirmation before adding any new files or scripts.
- Prefer existing tools and commands. If new scripts are necessary, propose the minimum number needed and wait for approval before creating them.
- Keep approved code concise; avoid unnecessary abstractions and boilerplate.
- Keep conversation messages short and concise.
- These permission requirements apply to all workflows below.

## Project Structure & Module Organization

OBASE is a Linux research prototype for object relocation; CrestKV demonstrates it in a key-value server.

- `crest/`: C++17 server/client, `runtime/`, managed `datastructures/`, and LLVM tooling in `compiler/`. Build artifacts go to `crest/bin/`.
- `crest/compiler/tests/`: conversion fixtures, expected outputs, validation cases, and end-to-end fixtures.
- `YCSB/`: Maven benchmark suite; Crest bindings live in `crest/src/main/java/`, core tests in `core/src/test/java/`, and workloads in `workloads/`.
- `tools/`: measurement, integrity checks, and trace drivers.
- `docs/`: adoption, operations, benchmarks, and image assets in `img/`.

## Build, Test, and Development Commands

Install clang/LLVM 12, libevent, and prefix-built jemalloc as described in `docs/OPERATIONS.md`. The Crest YCSB binding requires JDK 16+.

Run from the repository root:

- `make -C crest all JEMALLOC_DIR=/path/to/jemalloc`: compile all structures and build the server/client; Masstree is the default.
- `make -C crest all DS=ht_pugh`: build and select only Pugh's hash table.
- `crest/bin/crest-server 127.0.0.1 6363 1 8`: start a local server.
- `make -C crest test-compiler`: run conversion and validation checks.
- `cd YCSB && mvn -pl site.ycsb:crest-binding -am clean package -Dcheckstyle.skip`: package the binding and dependencies, including tests, with Checkstyle skipped as documented by the binding.

## Coding Style & Naming Conventions

Match neighboring C++ code: typically four-space indentation, braces on separate lines, and `.cc`/`.h` files. Structure filenames and namespaces match, such as `ht_pugh`. No repository-wide C++ formatter is configured. YCSB uses two-space Java/XML indentation through `.editorconfig` and Maven Checkstyle; preserve license headers and follow `YCSB/CONTRIBUTING.md`.

## Testing Guidelines

Compiler tests use shell-driven golden comparisons (`*.expected`) and rejection fixtures (`bad_*.cc`). YCSB core uses TestNG with `Test*.java` or `*Test.java`; run `cd YCSB && mvn -pl core -am test`. No numeric coverage threshold is configured.

For relocation changes, use `tools/verify_migration.sh` and relevant integrity scripts. First adapt their hard-coded paths and CPU assignments; some terminate benchmark processes. Record workload, structure, and results.

## Commit & Pull Request Guidelines

History uses short descriptive subjects, such as “Make jemalloc installation path configurable”; no enforced prefix convention is evident. Use imperative subjects and focused commits. PRs should explain behavior changes, link relevant issues, list validation commands/results, and include reproducible throughput/RSS comparisons for performance changes.

## Runtime Invariants

Managed objects must be jemalloc-allocated and singly owned. Keep traversal pointers unmanaged, and never dereference returned raw pointers after an operation ends. Keep build-time validation enabled.
