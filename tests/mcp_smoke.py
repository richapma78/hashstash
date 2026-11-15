"""Drives `hs mcp` over stdio like an MCP client would.
Usage: python tests/mcp_smoke.py <hs executable> <project dir>"""
import json
import subprocess
import sys

hs, project = sys.argv[1], sys.argv[2]
proc = subprocess.Popen([hs, "mcp"], cwd=project, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                        text=True, encoding="utf-8")
next_id = 0


def rpc(method, params=None, notify=False):
    global next_id
    msg = {"jsonrpc": "2.0", "method": method}
    if params is not None:
        msg["params"] = params
    if not notify:
        next_id += 1
        msg["id"] = next_id
    proc.stdin.write(json.dumps(msg) + "\n")
    proc.stdin.flush()
    if notify:
        return None
    return json.loads(proc.stdout.readline())


def call(tool, **args):
    r = rpc("tools/call", {"name": tool, "arguments": args})["result"]
    text = r["content"][0]["text"]
    print(f"--- {tool} {'(error)' if r['isError'] else ''}")
    print(text[:1500])
    return r


init = rpc("initialize", {"protocolVersion": "2025-06-18", "clientInfo": {"name": "smoke"}, "capabilities": {}})
print("server:", init["result"]["serverInfo"], "protocol", init["result"]["protocolVersion"])
rpc("notifications/initialized", notify=True)
tools = rpc("tools/list")["result"]["tools"]
print(len(tools), "tools:", ", ".join(t["name"] for t in tools))

call("hs_find", query="calculator apply")
call("hs_context", name="add")
call("hs_edit_def", name="add", code="def add(a, b):\n    # integers and floats\n    return a + b\n",
     reason="document the accepted types")
call("hs_add_def", file="calc/ops.py", code="def sub(a, b):\n    return a - b\n", reason="calculator needs subtraction")
call("hs_add_def", file="tests/test_ops.py", code="def test_sub():\n    from calc.ops import sub\n    assert sub(5, 3) == 2\n",
     reason="tests for sub")
call("hs_link_tests", name="sub", tests=["test_sub"])
call("hs_graph", query="impact", name="add")
call("hs_diff")
call("hs_verify")
call("hs_commit", message="")
call("hs_commit", message="Add sub() with tests; document add()")
call("hs_history", limit=4)
proc.stdin.close()
proc.wait()
