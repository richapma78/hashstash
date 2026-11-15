#!/usr/bin/env bash
# Live test against a running hub: import a project, verify, push, clone,
# shared locks, MCP over HTTP, and publishing to git.
# Usage: tests/live_hub.sh <hs.exe> <hshub.exe> <work dir> <source project> [hub url]
set -u
HS="$1"; HUBEXE="$2"; WORK="$3"; SRC="$4"; URL="${5:-http://127.0.0.1:7420}"
PROJECT=hashstash
run() { echo "\$ hs $*"; "$HS" "$@"; echo "(exit $?)"; }

cd "$WORK" || exit 1
rm -rf alice bob gitmirror.git checkout publish-wc
echo "=== hub is up"
curl -s "$URL/" ; echo
curl -s -X POST "$URL/api/p/$PROJECT/create" -d '{}'; echo

echo "=== alice imports the project and pushes"
mkdir alice && cd alice
export HS_AGENT=alice
run init $PROJECT
run remote add origin "$URL/$PROJECT"
run import "$SRC"
run commit -m "Import hashstash into hashstash"
run push
run verify
run push
cd ..

echo "=== bob clones it"
export HS_AGENT=bob
run clone "$URL/$PROJECT" bob
cd bob
run status
run ls src/ops/sync.cpp
run graph callers hs.push
cd ..

echo "=== shared locks: alice locks, bob is refused"
(cd alice && HS_AGENT=alice "$HS" lock src/ops/sync.cpp:hs.push)
(cd bob && HS_AGENT=bob "$HS" lock src/ops/sync.cpp:hs.push; echo "(exit $?)")
curl -s -X POST "$URL/api/p/$PROJECT/locks" -d '{}' ; echo

echo "=== alice edits, verifies, pushes; bob pulls"
cd alice
export HS_AGENT=alice
"$HS" edit src/core/hash.cpp:hs.short_hash > /dev/null
python - .hs/scratch/edit.cpp <<'EOF'
import sys
p = sys.argv[1]
s = open(p, encoding="utf-8").read()
s = s.replace("std::string short_hash(const Hash& h, size_t n) { return h.substr(0, n); }",
              "std::string short_hash(const Hash& h, size_t n) {\n  return n >= h.size() ? h : h.substr(0, n);\n}")
open(p, "w", encoding="utf-8").write(s)
EOF
run update -m "short_hash: don't copy when n covers the whole hash"
run diff
run commit -m "Tidy short_hash"
run verify
run push
cd ../bob
export HS_AGENT=bob
run pull
run view src/core/hash.cpp:hs.short_hash
run log -n 3
cd ..

echo "=== MCP over HTTP on the hub"
python - "$URL/mcp/$PROJECT" <<'EOF'
import json, sys, urllib.request
url = sys.argv[1]
sid = None
def rpc(method, params=None, i=[0]):
    global sid
    i[0] += 1
    msg = {"jsonrpc": "2.0", "id": i[0], "method": method, "params": params or {}}
    req = urllib.request.Request(url, json.dumps(msg).encode(), {"Content-Type": "application/json"})
    if sid: req.add_header("Mcp-Session-Id", sid)
    with urllib.request.urlopen(req) as r:
        sid = r.headers.get("Mcp-Session-Id") or sid
        return json.loads(r.read())
init = rpc("initialize", {"protocolVersion": "2025-06-18", "clientInfo": {"name": "web-agent"}})
print("server:", init["result"]["serverInfo"])
print("tools:", len(rpc("tools/list")["result"]["tools"]))
res = rpc("tools/call", {"name": "hs_graph", "arguments": {"query": "impact", "name": "hs.short_hash", "depth": 1}})
print(res["result"]["content"][0]["text"][:600])
EOF

echo "=== publish the hub's main branch to a git repository"
git init -q --bare gitmirror.git
CFG="hub/$PROJECT/.hs/config.toml"
printf '\n[publish]\ngit_dir = "../publish-wc"\nremote = "%s"\nbranch = "main"\non_push = true\n' "$(cd gitmirror.git && pwd -W 2>/dev/null || pwd)" >> "$CFG"
"$HUBEXE" publish $PROJECT --root hub
git -C gitmirror.git log --oneline -3 main
rm -rf checkout && git -c core.autocrlf=false clone -q -b main gitmirror.git checkout
diff -r checkout "$SRC" -x .git -x build -x secret -x out -x .claude -x README.md -q; echo "(only the short_hash edit should differ above)"
