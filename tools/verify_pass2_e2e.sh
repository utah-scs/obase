#!/bin/bash
# Pass 2 end-to-end verification.
#
# For each raw-pointer fixture in crest/compiler/tests/e2e (plain C++,
# OBASE_GUIDED annotations, no Guide types anywhere):
#   1. compile the raw source as plain C++ (proves it really is plain C++)
#   2. convert it with guide-converter
#   3. drop the converted sources into datastructures/ as an extra
#      structure and build through the full pipeline (validator,
#      visibility extraction, instrumentation pass)
#   4. run the migration integrity harness: byte-verified sentinels
#      through NEW -> COLD -> HOT migration under 8-thread load
#
# Fixtures: ht_conv (minimal chained hash table), plus de-converted
# versions of two evaluation structures -- ht_chm_conv (segmented
# concurrent hash table, lock striping) and sl_seq_conv (coarse-lock
# skip list). PASS means annotation + compiler passes alone turned
# plain C++ into a migration-correct OBASE structure.

set -u
CREST=/home/vin/evolve/tidal/crest
E2E=$CREST/compiler/tests/e2e
FIXTURES=${1:-"ht_conv ht_chm_conv sl_seq_conv"}
CFLAGS="-Wall -std=c++17 -I. -Idatastructures -Iruntime -I/home/vin/jemalloc/include -fPIC -march=native -O2"
cd $CREST

make converter >/dev/null 2>&1 || { echo "E2E RESULT=FAIL (converter build)"; exit 1; }

overall=0
for name in $FIXTURES; do
  echo "=== Pass 2 e2e: $name ==="

  cleanup_files() {
    rm -f datastructures/$name.h datastructures/$name.cc \
          bin/obj/datastructures/$name.o bin/llvm/$name* \
          bin/visibility_data/$name.cc.visibility bin/.ds-$name.stamp \
          $E2E/$name.h.converted $E2E/$name.cc.converted
  }

  # The raw sources must contain no Guide types and compile as plain C++.
  if grep -q "Guide<" $E2E/$name.h $E2E/$name.cc; then
    echo "$name E2E RESULT=FAIL (raw fixture already contains guides)"; overall=1; continue
  fi
  if ! clang++-12 -fsyntax-only $CFLAGS -I$E2E $E2E/$name.cc 2>/tmp/pass2-raw-$name.err; then
    echo "$name E2E RESULT=FAIL (raw fixture does not compile as plain C++)"
    head -5 /tmp/pass2-raw-$name.err; overall=1; continue
  fi

  rm -f $E2E/$name.h.converted $E2E/$name.cc.converted
  bin/guide-converter $E2E/$name.cc -- $CFLAGS >/dev/null 2>&1
  # The converter writes only the files it changed; a source whose guide
  # usage is entirely implicit conversions needs no .cc rewrite.
  if [ ! -f $E2E/$name.h.converted ]; then
    echo "$name E2E RESULT=FAIL (conversion)"; cleanup_files; overall=1; continue
  fi
  grep -q "Guide<void>" $E2E/$name.h.converted || { echo "$name E2E RESULT=FAIL (no guides in output)"; cleanup_files; overall=1; continue; }

  cp $E2E/$name.h.converted datastructures/$name.h
  if [ -f $E2E/$name.cc.converted ]; then
    cp $E2E/$name.cc.converted datastructures/$name.cc
  else
    cp $E2E/$name.cc datastructures/$name.cc
  fi

  EXTRA_DS=$name /home/vin/evolve/tidal/tools/verify_migration.sh $name | tee /tmp/pass2-e2e-$name.txt

  if grep -q "$name RESULT=PASS" /tmp/pass2-e2e-$name.txt; then
    echo "$name E2E RESULT=PASS"
  else
    echo "$name E2E RESULT=FAIL (harness)"
    overall=1
  fi
  cleanup_files
done

[ $overall -eq 0 ] && echo "E2E RESULT=PASS (all fixtures)" || echo "E2E RESULT=FAIL"
exit $overall
