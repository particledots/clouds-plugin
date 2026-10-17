#!/bin/bash
# Wiring mutation check: removes the Output Level stage from PluginProcessor::processBlock (and, separately, the parameter's default), rebuilds the
# whole-plug-in test, and requires it to FAIL each time. The source is put back afterwards.
# Run from the repository root with the CMake build directory as the argument (it must already contain a built proc_level_test):
#   bash tests/wiring_mutation_check.sh build
set -u
BUILD="${1:-build}"
FILE=src/PluginProcessor.cpp
cp "$FILE" /tmp/PluginProcessor.cpp.orig
restore() { cp /tmp/PluginProcessor.cpp.orig "$FILE"; }
trap restore EXIT
bad=0; n=0
MUTANTS=(
"stage not called in processBlock|outputStage.process (buffer.getArrayOfWritePointers(), juce::jmin (chans, 2), n);|(void) chans;"
"level parameter not read (stage stays at 0 dB)|outputStage.setLevelDb (v (\"output_level\"));|outputStage.setLevelDb (0.0f);"
"parameter default wrong (0 dB)|juce::NormalisableRange<float> (OutputStage::kMinLevelDb, OutputStage::kMaxLevelDb, 0.01f), OutputStage::kDefaultLevelDb,|juce::NormalisableRange<float> (OutputStage::kMinLevelDb, OutputStage::kMaxLevelDb, 0.01f), 0.0f,"
)
for m in "${MUTANTS[@]}"; do
  n=$((n+1)); name="${m%%|*}"; rest="${m#*|}"; from="${rest%%|*}"; to="${rest#*|}"
  restore
  python3 - "$from" "$to" "$FILE" <<'PY' || { echo "mutant $n ($name): could not be applied"; bad=1; continue; }
import sys
frm,to,path=sys.argv[1:4]
s=open(path).read()
if frm not in s: print("MUTATION TEXT NOT FOUND:", frm); sys.exit(3)
open(path,'w').write(s.replace(frm,to,1))
PY
  if ! cmake --build "$BUILD" --target proc_level_test -j4 > /tmp/wm_build.txt 2>&1; then echo "mutant $n ($name): did not compile"; tail -5 /tmp/wm_build.txt; bad=1; continue; fi
  if "$BUILD/proc_level_test" > /tmp/wm_run.txt 2>&1; then echo "mutant $n ($name): test PASSED  <-- the test missed this bug"; bad=1
  else echo "mutant $n ($name): test fails, as it should ($(grep -c FAIL /tmp/wm_run.txt) failed checks)"; fi
done
restore
cmake --build "$BUILD" --target proc_level_test -j4 > /dev/null 2>&1
if "$BUILD/proc_level_test" > /dev/null 2>&1; then echo "restored source: whole-plug-in test passes again"; else echo "restored source: test FAILS"; bad=1; fi
if [ $bad -eq 0 ]; then echo "WIRING MUTATION CHECK OK: all $n broken copies were caught"; exit 0; else echo "WIRING MUTATION CHECK FAILED"; exit 1; fi
