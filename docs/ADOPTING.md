# Adopting OBASE in your data structure

How to make a pointer-based C/C++ data structure OBASE-managed. The worked
example is in this repo: `crest/compiler/tests/e2e/ht_conv.{h,cc}` is a
plain chained hash table with two annotations, and
`tools/verify_pass2_e2e.sh` converts it, builds it, and checks that every
value survives migration byte-for-byte under concurrent load.

## The rules

1. **Annotate the data, not the structure.** Mark the pointers to keys and
   values that a record owns. Never mark traversal pointers (child/next
   links, interior separator keys) — every lookup touches those, so they
   would always look hot and the cold set would never settle.
2. **Allocate managed objects with jemalloc** (`jem_malloc`/`jem_calloc`).
   OBASE frees relocated objects through jemalloc and asks jemalloc for
   their sizes.
3. **Don't let raw pointers outlive an operation.** A pointer read through
   a managed field is valid until the public operation that read it
   returns; after that the object may have moved. Copy data out before
   returning — never cache such a pointer in a field or hand it to a caller.

One structural requirement: each managed object is reached through exactly
one pointer (like `std::unique_ptr`). Structures that alias a node through
multiple pointers — shared graphs, doubly-linked lists — are not supported.
CrestKV's structures satisfy this by deep-copying keys and values on insert.

## Step 1 — annotate

```cpp
#include "GuideAnnotations.h"

struct Node {
    OBASE_GUIDED void *key;
    OBASE_GUIDED void *val;
    size_t key_len, val_len;
    Node *next;              // traversal pointer: stays raw
};
```

Write the rest as ordinary C++. The idioms you would use anyway have the
right meaning after conversion:

```cpp
// update: read the old pointer, publish the replacement, free the old
void *old = n->val;
n->val = jem_malloc(len);
memcpy(n->val, src, len);
jem_free(old);

// teardown: free, then null out (the null tells OBASE the slot is gone)
jem_free(n->key);
n->key = nullptr;
```

Across CrestKV's ten structures, each managed type needed one to three
annotations.

## Step 2 — convert

```bash
cd crest && make converter
bin/guide-converter path/to/yours.cc -- <your cflags> -Iruntime
```

The converter (a Clang rewriter) turns each annotated declaration into a
`Guide` — the tagged pointer type that makes the object relocatable — and
repairs the few casts that stop compiling, e.g. `(char*)n->val` becomes
`(char*)static_cast<void*>(n->val)`. Everything else compiles unchanged
through operator overloads; many files need no repairs at all. Output is
written next to your sources as `*.converted` (`-inplace` overwrites).

## Step 3 — build

Compile the converted source through CrestKV's pipeline (it needs
clang/LLVM-12; the Makefile drives everything). Two things happen on every
build:

- a **validator** rejects code the runtime cannot support — pointer
  arithmetic on a managed object's address, casting a managed field's
  address to a raw pointer type — with a file/line diagnostic, and
- an **instrumentation pass** inserts the access tracking automatically, so
  you never call OBASE APIs by hand.

To run your structure inside CrestKV: implement the same five operations
the built-in structures do (`insert`, `search`, `searchCopy`, `remove`,
`scanCopy` — the last returns −1 for unordered structures) in a namespace
matching the file name, drop the files into `crest/datastructures/`, and

```bash
make all DS=yours EXTRA_DS=yours
```

`searchCopy` exists because of rule 3: the GET path copies the value into
the response while the operation is still running.

## Bringing the runtime into your own application

The steps above run inside CrestKV's tree, but nothing in the runtime
depends on the store. `crest/runtime/` plus `crest/globalConfig.*` is the
complete set to take:

| Files | Role |
|-------|------|
| `Guide.hpp`, `GuideVoid.cc` | the relocatable pointer type your annotated fields become |
| `GuideAnnotations.h` | the `OBASE_GUIDED` marker |
| `TagRuntime.cc` | the per-operation tracking hooks the compiler inserts calls to |
| `soda.{h,cc}` | a bitmap of live managed fields — how the collector finds objects without ever asking your structure |
| `Sama.{h,cc}` | the segregated heaps: NEW (default arenas), HOT (huge-page-backed), COLD (released with `MADV_PAGEOUT`) |
| `ObjectCollector.{h,cc}` | the background thread that classifies objects each window and migrates them |
| `globalConfig.{h,cc}` | process-wide bookkeeping (address ranges, per-thread slots) |

Integration is three things:

1. **Build.** Compile the runtime files into your application and link a
   prefix-built jemalloc (`--with-jemalloc-prefix=jem_`, so the managed
   heap coexists with glibc malloc). Compile each annotated translation
   unit through the pipeline — validator, IR emission, instrumentation —
   using `crest/Makefile`'s data-structure rules as the recipe.

2. **Initialize, in `main()`.** Managed fields must never be created
   during static initialization (the bookkeeping they register with may
   not exist yet). After startup:

   ```cpp
   Sama sama({MemType::DRAM, MemType::DRAM_2MB_THP});  // COLD, HOT regions
   g_sama = &sama;
   auto *structure = new YourStructure();               // after globals, never static
   std::atomic<uint8_t> mode{0};
   ObjectCollector::initializeIfNeeded(
       /*window seconds*/ 120, /*cold after N windows*/ 3,
       structure, &sama, &mode);
   ```

3. **Control.** The mode atomic is the whole interface: `1` starts access
   tracking, `2` enables migration and pageout, `0` stops both. CrestKV
   wires it to the `obase decay|migrate|none` commands; wire it to
   whatever fits your application. The collector discovers objects through
   the bitmap, so there is no per-structure integration beyond the
   annotations themselves.

One process-level constraint: one `Sama` instance (one set of managed
regions) per process.

## Step 4 — check it

```bash
tools/verify_migration.sh yours
```

loads the structure, forces objects through full NEW → COLD → HOT
migration cycles under concurrent traffic, and re-reads known keys after
each move, failing if any byte changed (about two minutes).