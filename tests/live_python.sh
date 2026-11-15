#!/usr/bin/env bash
# Live Python walkthrough against a running hub: the flow docs/guide.html shows.
# Import, browse, link tests, verify (pytest), notes and context, push, clone,
# shared locks, edit/update/diff, the verify push gate, impact testing, rollback,
# and rendering back to files that pytest passes on.
# Usage: tests/live_python.sh <hs.exe> <work dir> [hub url] [project]
# Each run uses a new hub project (calc-<time>) unless one is given.
# Needs: python with pytest; a hub (hshub serve) at the URL.
set -u
HS="$1"; W="$2"; URL="${3:-http://127.0.0.1:7420}"; PROJECT="${4:-calc-$(date +%s)}"
SRC="$(cd "$(dirname "$0")/fixtures/sample_project" && pwd)"
run() { echo; echo "\$ hs $*"; "$HS" "$@"; echo "(exit $?)"; }
fails=0
expect() { if ! eval "$1"; then echo "!!! FAILED: $2"; fails=$((fails + 1)); fi; }

cd "$W" || exit 1
rm -rf calc-alice calc-bob calc-out
curl -s -X POST "$URL/api/p/$PROJECT/create" -d '{}'; echo

mkdir calc-alice && cd calc-alice
export HS_AGENT=alice
run init "$PROJECT"
run import "$SRC"
run ls
run graph callers add
run tests missing
run commit -m "Import the calc sample"
out=$("$HS" verify); echo "$out"
expect '[[ "$out" == *"Verification passed (4 tests run)"* ]]' "first verify runs all 4 tests"
run note calc/ops.py:mul 2-4 "O(b) loop; fine for a demo, not for big numbers"
run why calc/ops.py:mul "Multiply by repeated addition so the sample has a call graph" --kind origin
run remote add origin "$URL/$PROJECT"
out=$("$HS" push 2>&1); echo "$out"
expect '[[ "$out" == Pushed* ]]' "push after verify"

echo; echo "=== bob clones and locks"
cd "$W"; export HS_AGENT=bob
run clone "$URL/$PROJECT" calc-bob
(cd calc-bob && "$HS" lock calc/ops.py:Calculator.clear)
cd "$W/calc-alice"; export HS_AGENT=alice
out=$("$HS" lock calc/ops.py:Calculator.clear 2>&1); echo "$out"
expect '[[ "$out" == *"locked on the hub by bob"* ]]' "hub lock refuses alice"

echo; echo "=== alice edits mul"
"$HS" edit calc/ops.py:mul > /dev/null
python - .hs/scratch/edit.py <<'EOF'
import sys
p = sys.argv[1]
s = open(p, encoding="utf-8").read()
old = "    total = 0\n"
new = '    """Multiply a by a non-negative integer b."""\n    if b < 0:\n        raise ValueError("b must be non-negative")\n    total = 0\n'
assert old in s
open(p, "w", encoding="utf-8").write(s.replace(old, new, 1))
EOF
run update -m "Reject negative b instead of silently returning 0"
run diff
run commit -m "mul: reject negative multipliers"
out=$("$HS" push 2>&1); echo "$out"
expect '[[ "$out" == *"not verified"* ]]' "push refused before verify"
out=$("$HS" verify); echo "$out"
expect '[[ "$out" == *"Verification passed (1 test run)"* ]]' "verify runs only test_mul"
run push
out=$("$HS" context calc/ops.py:mul); echo "$out"
expect '[[ "$out" == *"lines 5-7"* ]]' "note moved with the code"

echo; echo "=== reviews: the AI checks its commit, then a person approves it"
out=$("$HS" show head); echo "$out"
expect '[[ "$out" == *"calc/ops.py:mul   [1/1 passed]"* && "$out" == *'"'"'+    if b < 0:'"'"'* ]]' "show has line diffs and the test summary"
HS_REVIEWER=ai "$HS" review head approve -m "diff matches the message; test_mul passes"
out=$("$HS" reviews); echo "$out"
expect '[[ "$out" == *"AI reviewed"* ]]' "flag: AI reviewed"
HS_REVIEWER=human "$HS" --as a-person review head approve
out=$("$HS" reviews); echo "$out"
expect '[[ "$out" == *"human approved"* ]]' "flag: human approved"
page=$(curl -s "$URL/ui/p/$PROJECT")
expect '[[ "$page" == *"human approved"* ]]' "the hub's review pages show the approval"

echo; echo "=== bob pulls"
cd "$W/calc-bob"; export HS_AGENT=bob
out=$("$HS" pull 2>&1); echo "$out"
expect '[[ "$out" == fast-forward* ]]' "bob pulls"
"$HS" unlock calc/ops.py:Calculator.clear

echo; echo "=== a broken add is caught through its caller, then rolled back"
cd "$W/calc-alice"; export HS_AGENT=alice
"$HS" edit calc/ops.py:add > /dev/null
python - .hs/scratch/edit.py <<'EOF'
import sys
p = sys.argv[1]
s = open(p, encoding="utf-8").read()
assert "return a + b" in s
open(p, "w", encoding="utf-8").write(s.replace("return a + b", "return a - b"))
EOF
run update -m "Experiment: subtract instead"
run commit -m "Experiment"
out=$("$HS" verify); echo "$out"
expect '[[ "$out" == *"also testing callers: calc/ops.py:mul"* && "$out" == *"FAIL  tests/test_ops.py:test_mul"* ]]' "impact testing runs test_mul"
out=$("$HS" push 2>&1); echo "$out"
expect '[[ "$out" == *"failing verification"* ]]' "push refused on failing tests"
before=$("$HS" history --json | python -c "import json,sys; print([e for e in json.load(sys.stdin) if e['step']=='update'][0]['id'] - 1)")
run rollback "@$before"
out=$("$HS" view calc/ops.py:add); echo "$out"
expect '[[ "$out" == *"return a + b"* ]]' "rollback restores add"

echo; echo "=== render and run pytest on plain files"
run render --out ../calc-out
out=$(cd ../calc-out && python -m pytest -q 2>&1 | tail -1); echo "$out"
expect '[[ "$out" == *"4 passed"* ]]' "rendered files pass pytest"

echo
if [ $fails -eq 0 ]; then echo "live_python: all checks passed"; else echo "live_python: $fails check(s) FAILED"; exit 1; fi
