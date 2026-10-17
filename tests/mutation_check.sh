#!/bin/bash
# Mutation check for the output stage: breaks OutputStage.h on purpose, in four different ways, builds tests/level_test.cpp against each broken copy
# and requires the test to FAIL every time. If a broken copy passes, this script fails (the test would not notice that bug).
# Run from the repository root:  bash tests/mutation_check.sh
set -u
ROOT="$(pwd)"
WORK="$(mktemp -d)"
D=third_party/clouds/dsp
SOURCES="src/CloudsEngine.cpp $D/correlator.cc $D/granular_processor.cc $D/mu_law.cc $D/pvoc/frame_transformation.cc $D/pvoc/phase_vocoder.cc $D/pvoc/stft.cc third_party/clouds/resources.cc third_party/stmlib/utils/random.cc third_party/stmlib/dsp/units.cc third_party/stmlib/dsp/atan.cc"
FLAGS="-std=c++17 -O2 -DTEST=1 -w -include cstdio -include cstdlib -include cmath -include cstring -Wno-narrowing"

bad=0
# name | text to find in src/OutputStage.h | text to put instead
MUTANTS=(
"gain removed (level always 0 dB)|target = dbToGain (std::clamp (db, kMinLevelDb, kMaxLevelDb));|target = 1.0f;"
"default level wrong (8.0 dB instead of 9.03 dB)|kDefaultLevelDb = 9.03f;|kDefaultLevelDb = 8.0f;"
"dB converted with the wrong factor|std::pow (10.0f, db * 0.05f)|std::pow (10.0f, db * 0.1f)"
"limiter removed (passes everything)|if (a <= kThreshold) return x;|return x;"
)
# first: the unbroken header must pass
mkdir -p "$WORK/orig"
cp src/OutputStage.h "$WORK/orig/OutputStage.h"
c++ $FLAGS -I"$WORK/orig" -Isrc -Ithird_party tests/level_test.cpp $SOURCES -o "$WORK/level_orig" || { echo "build of the unbroken test failed"; exit 2; }
if "$WORK/level_orig" --fast > "$WORK/orig.txt" 2>&1; then echo "unbroken code: test passes (as it must)"; else echo "unbroken code: test FAILS - fix that first"; tail -5 "$WORK/orig.txt"; exit 2; fi

i=0
for m in "${MUTANTS[@]}"; do
  i=$((i+1))
  name="${m%%|*}"; rest="${m#*|}"; from="${rest%%|*}"; to="${rest#*|}"
  mkdir -p "$WORK/m$i"
  python3 - "$from" "$to" "$WORK/m$i/OutputStage.h" <<'PY'
import sys
src=open('src/OutputStage.h').read()
frm,to,out=sys.argv[1:4]
if frm not in src:
    print("MUTATION TEXT NOT FOUND:", frm); sys.exit(3)
open(out,'w').write(src.replace(frm,to,1))
PY
  [ $? -eq 0 ] || { echo "mutant $i ($name): could not be applied"; bad=1; continue; }
  if ! c++ $FLAGS -I"$WORK/m$i" -Isrc -Ithird_party tests/level_test.cpp $SOURCES -o "$WORK/level_m$i" 2> "$WORK/build$i.txt"; then
    echo "mutant $i ($name): did not compile"; bad=1; continue
  fi
  if "$WORK/level_m$i" --fast > "$WORK/m$i.txt" 2>&1; then
    echo "mutant $i ($name): test PASSED  <-- the test missed this bug"; bad=1
  else
    echo "mutant $i ($name): test fails, as it should ($(grep -c FAIL "$WORK/m$i.txt") failed checks)"
  fi
done
rm -rf "$WORK"
if [ $bad -eq 0 ]; then echo "MUTATION CHECK OK: all $i broken copies were caught"; exit 0; else echo "MUTATION CHECK FAILED"; exit 1; fi
