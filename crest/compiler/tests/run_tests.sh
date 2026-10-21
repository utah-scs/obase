#!/bin/bash
# Unit tests for the OBASE compiler passes.
#
#   converter : golden-file diff + the converted output must compile
#   validator : legal patterns accepted, each violation class rejected
#               with its rule tag, and all real data structures clean
#
# Run from crest/ (or via `make test-compiler`). The end-to-end conversion
# test (raw hash table -> converter -> migration harness) is separate:
# tools/verify_pass2_e2e.sh.

set -u
cd "$(dirname "$0")/../.."   # crest/

CONVERTER=bin/guide-converter
VALIDATOR=bin/guide-validator
CFLAGS="-Wall -std=c++17 -I. -Idatastructures -Iruntime -I/home/vin/jemalloc/include -fPIC -march=native -O2"
TESTS=compiler/tests
PASS=0
FAIL=0

ok()   { PASS=$((PASS+1)); echo "PASS: $1"; }
bad()  { FAIL=$((FAIL+1)); echo "FAIL: $1"; }

[ -x $CONVERTER ] || { echo "missing $CONVERTER (make converter)"; exit 1; }
[ -x $VALIDATOR ] || { echo "missing $VALIDATOR"; exit 1; }

# --- converter: golden diff ---------------------------------------------
rm -f $TESTS/convert/*.converted
$CONVERTER $TESTS/convert/basic.cc -- $CFLAGS >/dev/null 2>&1
if diff -q $TESTS/convert/basic.h.converted $TESTS/convert/basic.h.expected >/dev/null 2>&1 \
&& diff -q $TESTS/convert/basic.cc.converted $TESTS/convert/basic.cc.expected >/dev/null 2>&1; then
  ok "convert/basic golden diff"
else
  bad "convert/basic golden diff"
  diff $TESTS/convert/basic.h.converted $TESTS/convert/basic.h.expected 2>&1 | head -10
  diff $TESTS/convert/basic.cc.converted $TESTS/convert/basic.cc.expected 2>&1 | head -10
fi

# --- converter: output compiles ------------------------------------------
TMP=$(mktemp -d)
cp $TESTS/convert/basic.h.converted $TMP/basic.h
cp $TESTS/convert/basic.cc.converted $TMP/basic.cc
if clang++-12 -fsyntax-only $CFLAGS -I$TMP $TMP/basic.cc 2>$TMP/err; then
  ok "convert/basic output compiles"
else
  bad "convert/basic output compiles"
  head -10 $TMP/err
fi
rm -rf $TMP $TESTS/convert/*.converted

# --- validator: legal patterns -------------------------------------------
if $VALIDATOR $TESTS/validate/good.cc -- $CFLAGS >/dev/null 2>&1; then
  ok "validate/good accepted"
else
  bad "validate/good accepted"
fi

# --- validator: violations rejected with the right rule ------------------
check_bad() {
  local file=$1 rule=$2 expected=$3
  local out
  out=$($VALIDATOR $TESTS/validate/$file -- $CFLAGS 2>&1)
  local rc=$?
  local n
  n=$(echo "$out" | grep -c "obase-validate:$rule")
  if [ $rc -ne 0 ] && [ "$n" -eq "$expected" ]; then
    ok "validate/$file rejected ($n x $rule)"
  else
    bad "validate/$file (rc=$rc, $rule matches=$n, expected $expected)"
    echo "$out" | head -5
  fi
}
check_bad bad_arith.cc      arithmetic 1
check_bad bad_subscript.cc  arithmetic 1
check_bad bad_slot_alias.cc slot-alias 2

# --- validator: every real data structure must be clean ------------------
for ds in ht_harris ht_pugh ht_chm sl_seq sl_fraser sl_hierlihy bpt_seq bpt_occ bpt_mass trie_art; do
  if $VALIDATOR datastructures/$ds.cc -- $CFLAGS -DCREST_DS=$ds >/dev/null 2>&1; then
    ok "validate/$ds clean"
  else
    bad "validate/$ds clean"
    $VALIDATOR datastructures/$ds.cc -- $CFLAGS -DCREST_DS=$ds 2>&1 | grep obase-validate | head -5
  fi
done

# --- reclamation callbacks must not retain TAG slots past operator delete -
RECLAIM_TMP=$(mktemp -d)
if bin/visibility-extractor -o "$RECLAIM_TMP/reclamation_delete.cc.visibility" \
     "$TESTS/reclamation_delete.cc" -- $CFLAGS >"$RECLAIM_TMP/visibility.log" 2>&1 \
&& clang++-12 $CFLAGS -S -emit-llvm "$TESTS/reclamation_delete.cc" -o "$RECLAIM_TMP/input.ll" \
&& VISIBILITY_DATA_DIR="$RECLAIM_TMP" opt-12 -load bin/llvm/libGuideInstrumentation.so \
     -guide-instrumentation -S "$RECLAIM_TMP/input.ll" -o "$RECLAIM_TMP/output.ll" \
     >"$RECLAIM_TMP/pass.log" 2>&1 \
&& grep -q '11reclamation12retireDelete' "$RECLAIM_TMP/input.ll" \
&& grep -q 'Created scope guard.*read_reclaimed_value' "$RECLAIM_TMP/pass.log" \
&& ! grep -q 'Created scope guard.*11reclamation' "$RECLAIM_TMP/pass.log"; then
  ok "instrument/reclamation callbacks skip TAG cleanup after delete"
else
  bad "instrument/reclamation callbacks"
  tail -15 "$RECLAIM_TMP/pass.log" 2>/dev/null
fi
rm -rf "$RECLAIM_TMP"

echo "----------------------------------------"
echo "compiler pass tests: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
