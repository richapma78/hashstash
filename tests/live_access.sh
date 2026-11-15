#!/usr/bin/env bash
# Live test of access keys against a running hub: a person is added on the hub
# machine, signs in to the pages, creates an orchestrator key; the orchestrator
# issues a worker key; changes without a key are refused (CLI and MCP over
# HTTP), a worker acts as its work item inside its scope, and a replacement
# agent with a new key carries on.
# Usage: tests/live_access.sh <hs.exe> <hshub.exe> <work dir> <hub root> [hub url]
# Run it after the other live scripts: it adds a person to the hub, and from
# then on the hub's pages need signing in.
set -u
HS="$1"; HUBEXE="$2"; W="$3"; ROOT="$4"; URL="${5:-http://127.0.0.1:7420}"
PROJECT="keys-$(date +%s)"
SRC="$(cd "$(dirname "$0")/fixtures/sample_project" && pwd)"
fails=0
expect() { if ! eval "$1"; then echo "!!! FAILED: $2"; fails=$((fails + 1)); fi; }
cd "$W" || exit 1
rm -rf acc-alice acc-w1 acc-w2 cookies.txt
unset HS_WORK_KEY HS_ORCH_KEY

echo "=== a project with code, before keys"
curl -s -X POST "$URL/api/p/$PROJECT/create" -d '{}'; echo
mkdir acc-alice && cd acc-alice
export HS_AGENT=alice
"$HS" init "$PROJECT" > /dev/null && "$HS" remote add origin "$URL/$PROJECT" > /dev/null
"$HS" import "$SRC" > /dev/null && "$HS" commit -m "import" > /dev/null && "$HS" verify > /dev/null
out=$("$HS" push 2>&1); echo "$out"
expect '[[ "$out" == Pushed* ]]' "push before keys"
cd "$W"

echo; echo "=== a person is added on the hub machine and signs in"
NAME="person-$(date +%s)"
"$HUBEXE" user add "$NAME" --password "correct horse battery" --root "$ROOT"
page=$(curl -s "$URL/ui")
expect '[[ "$page" == *"Sign in"* ]]' "pages ask to sign in"
code=$(curl -s -o /dev/null -w '%{http_code}' -c cookies.txt -d "name=$NAME&password=wrong-password" "$URL/ui/login")
expect '[[ "$code" == 401 ]]' "wrong password refused"
curl -s -o /dev/null -c cookies.txt -d "name=$NAME&password=correct+horse+battery" "$URL/ui/login"
page=$(curl -s -b cookies.txt "$URL/ui/p/$PROJECT/access")
expect '[[ "$page" == *"Agent access"* && "$page" == *"Signed in as"* ]]' "signed in, access page shows"

echo; echo "=== the person creates an orchestrator key for 7 days"
page=$(curl -s -b cookies.txt -d "label=ork-1&days=7" "$URL/ui/p/$PROJECT/access/orchestrator")
ORCH=$(echo "$page" | grep -o 'hsk_[0-9a-f]*_[0-9a-f]*' | head -1)
echo "orchestrator key: ${ORCH:0:12}..."
expect '[[ -n "$ORCH" ]]' "orchestrator key shown once"
again=$(curl -s -b cookies.txt "$URL/ui/p/$PROJECT/access")
expect '[[ "$again" != *"$ORCH"* && "$again" == *"ork-1"* ]]' "the key is not shown again; it is listed"

echo; echo "=== without a key, changes are refused"
cd "$W/acc-alice"
out=$("$HS" lock calc/ops.py:mul 2>&1); echo "$out"
expect '[[ "$out" == *"needs an access key"* ]]' "lock without a key refused"
cd "$W"

echo; echo "=== the orchestrator issues a worker key for FEAT-1 (calc/ops.py only)"
cd "$W/acc-alice"
HS_ORCH_KEY="$ORCH" "$HS" key issue FEAT-1 --title "Faster arithmetic" --scope "calc/ops.py" --hours 4 --out ../w1.env
WKEY=$(sed 's/^HS_WORK_KEY=//' ../w1.env)
out=$(HS_ORCH_KEY="$ORCH" "$HS" key list); echo "$out"
expect '[[ "$out" == *"worker  FEAT-1  calc/ops.py"* ]]' "orchestrator lists keys"
out=$(HS_WORK_KEY="$WKEY" "$HS" key issue FEAT-2 --title x 2>&1); echo "$out"
expect '[[ "$out" == *"needs an orchestrator key"* ]]' "a worker can't issue keys"
cd "$W"

echo; echo "=== a worker with the key acts as FEAT-1"
export HS_WORK_KEY="$WKEY"; export HS_AGENT=some-agent
"$HS" clone "$URL/$PROJECT" acc-w1 > /dev/null
cd acc-w1
out=$("$HS" key); echo "$out"
expect '[[ "$out" == "worker key"*"for FEAT-1"* ]]' "whoami"
out=$("$HS" lock calc/ops.py:mul); echo "$out"
expect '[[ "$out" == Locked* ]]' "worker locks inside its scope"
out=$("$HS" lock tests/test_ops.py:test_mul 2>&1); echo "$out"
expect '[[ "$out" == *"outside your key"* ]]' "outside the scope refused"
locks=$(curl -s -X POST "$URL/api/p/$PROJECT/locks" -d '{}')
expect '[[ "$locks" == *"\"holder\":\"FEAT-1\""* ]]' "the lock is held by the work item"
cd "$W"

echo; echo "=== MCP over HTTP: reading is open, changing needs the key"
out=$(python - "$URL/mcp/$PROJECT" "$WKEY" <<'EOF'
import json, sys, urllib.request
url, key = sys.argv[1], sys.argv[2]
def session(headers):
    sid = [None]
    def rpc(method, params=None):
        msg = {"jsonrpc": "2.0", "id": 1, "method": method, "params": params or {}}
        req = urllib.request.Request(url, json.dumps(msg).encode(), dict({"Content-Type": "application/json"}, **headers))
        if sid[0]: req.add_header("Mcp-Session-Id", sid[0])
        with urllib.request.urlopen(req) as r:
            sid[0] = r.headers.get("Mcp-Session-Id") or sid[0]
            return json.loads(r.read())
    rpc("initialize", {"protocolVersion": "2025-06-18", "clientInfo": {"name": "web"}})
    return rpc
anon = session({})
r = anon("tools/call", {"name": "hs_view", "arguments": {"name": "calc/ops.py:add"}})
print("anon read:", not r["result"]["isError"])
r = anon("tools/call", {"name": "hs_lock", "arguments": {"names": ["calc/ops.py:add"]}})
print("anon lock:", r["result"]["content"][0]["text"][:60])
keyed = session({"X-HS-Key": key})
r = keyed("tools/call", {"name": "hs_lock", "arguments": {"names": ["calc/ops.py:add"]}})
print("keyed lock:", not r["result"]["isError"])
r = keyed("tools/call", {"name": "hs_lock", "arguments": {"names": ["tests/test_ops.py:test_add"]}})
print("keyed out of scope:", r["result"]["content"][0]["text"][:60])
r = keyed("tools/call", {"name": "hs_unlock", "arguments": {"names": ["calc/ops.py:add"]}})
EOF
); echo "$out"
expect '[[ "$out" == *"anon read: True"* && "$out" == *"anon lock: project"*"needs an access key"* ]]' "MCP: anonymous reads only"
expect '[[ "$out" == *"keyed lock: True"* && "$out" == *"keyed out of scope: tests/test_ops.py:test_add is outside"* ]]' "MCP: key works within scope"

echo; echo "=== the worker is replaced; a new key for FEAT-1 carries on"
cd "$W/acc-alice"
HS_ORCH_KEY="$ORCH" "$HS" key issue FEAT-1 --scope "calc/**" --out ../w2.env > /dev/null
export HS_WORK_KEY=$(sed 's/^HS_WORK_KEY=//' ../w2.env); export HS_AGENT=fresh-agent
cd "$W" && "$HS" clone "$URL/$PROJECT" acc-w2 > /dev/null && cd acc-w2
"$HS" edit calc/ops.py:mul > /dev/null
out=$?
expect '[[ $out == 0 ]]' "the new agent can open FEAT-1's locked function"
python - .hs/scratch/edit.py <<'EOF'
import sys
p = sys.argv[1]
s = open(p, encoding="utf-8").read()
old = "    total = 0\n    for _ in range(b):\n        total = add(total, a)\n    return total\n"
assert old in s, s
open(p, "w", encoding="utf-8").write(s.replace(old, "    return a * b\n", 1))
EOF
"$HS" update -m "simpler" > /dev/null && "$HS" commit -m "simpler mul" > /dev/null && "$HS" verify > /dev/null
out=$("$HS" push 2>&1); echo "$out"
expect '[[ "$out" == Pushed* ]]' "the replacement pushes FEAT-1's work"

echo; echo "=== revoking the orchestrator key stops its workers"
cd "$W"
KID=$(echo "$ORCH" | cut -d_ -f2)
curl -s -o /dev/null -b cookies.txt -d "id=$KID" "$URL/ui/p/$PROJECT/access/revoke"
cd acc-w2
out=$("$HS" key 2>&1); echo "$out"
expect '[[ "$out" == *"no longer valid"* ]]' "worker key dead after revoke"
unset HS_WORK_KEY

echo
if [ $fails -eq 0 ]; then echo "live_access: all checks passed"; else echo "live_access: $fails check(s) FAILED"; exit 1; fi
