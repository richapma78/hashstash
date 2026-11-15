#!/usr/bin/env bash
# End-to-end smoke test of the hs CLI on the sample project.
# Usage: tests/smoke.sh <path to hs executable> <work dir>
set -u
HS="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
WORK="$2"
HERE="$(cd "$(dirname "$0")" && pwd)"
rm -rf "$WORK" && mkdir -p "$WORK" && cd "$WORK" || exit 1
export HS_AGENT=agent-a

step() { echo; echo "=== $*"; }
run() { echo "\$ hs $*"; "$HS" "$@"; echo "(exit $?)"; }

step init and import
run init demo
run import "$HERE/fixtures/sample_project"
run ls
run tests missing

step graph
run graph callers add
run graph callees calc.ops.mul
run graph impact add
run graph entry

step context and notes
run why add "Requested in design doc: basic arithmetic for the calculator" --kind origin --source docs/DESIGN.md
run note mul 3-4 "repeated addition; slow for large b"
run context mul

step commit requires a message
run commit
run commit -m "Import calculator"
run verify --all

step edit through a scratch file
run edit mul
SCRATCH=.hs/scratch/edit.py
cat "$SCRATCH"
# Replace the body of mul with a direct multiplication plus a helper.
python - "$SCRATCH" <<'EOF'
import sys
p = sys.argv[1]
s = open(p, encoding="utf-8").read()
start = s.index("def mul(a, b):")
s = s[:start] + "def mul(a, b):\n    total = 0\n    total = scale(a, b)\n    return total\n\n\ndef scale(a, b):\n    return a * b\n"
open(p, "w", encoding="utf-8").write(s)
EOF
run --as agent-b edit mul
run update -m "Use real multiplication; split out scale()"
run context mul
run diff
run status

step verify and commit
run verify
run commit -m "Faster mul"
run log
run log mul

step rollback keeps the latest
run history
FIRST=$("$HS" history --json | python -c "import json,sys; h=json.load(sys.stdin); print([e['id'] for e in h if e['step']=='import'][0])")
run rollback "@$FIRST"
run diff --from head --to work --stat
run history -n 5

step render matches the original files
run rollback "@$("$HS" history --json | python -c "import json,sys; h=json.load(sys.stdin); print([e['id'] for e in h if e['step']=='commit'][0])")"
run render --out out
(cd out && python -m pytest -q 2>&1 | tail -2)
