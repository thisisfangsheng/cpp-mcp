#!/usr/bin/env bash
# 本地验收脚本：覆盖 spec.md 的 A-1 ~ A-8。
# 用法： ./scripts/smoke_test.sh [可执行文件路径]
set -uo pipefail

BIN="${1:-build/mcp_server}"
PORT="${PORT:-18080}"
TOKEN="${TOKEN:-test-token-123}"
BASE="http://127.0.0.1:${PORT}"

if [ ! -x "$BIN" ]; then
  echo "FATAL: $BIN not found (先执行 cmake --build build)" >&2
  exit 1
fi

MCP_HOST=127.0.0.1 MCP_PORT="$PORT" MCP_AUTH_TOKEN="$TOKEN" "$BIN" >/tmp/mcp-smoke.out 2>/tmp/mcp-smoke.err &
PID=$!
trap 'kill -TERM "$PID" 2>/dev/null; wait "$PID" 2>/dev/null' EXIT

for _ in $(seq 1 50); do
  if curl -s -o /dev/null "$BASE/health"; then break; fi
  sleep 0.1
done

PASS=0; FAIL=0
check() { # check <name> <expected> <actual>
  if [ "$2" = "$3" ]; then echo "PASS  $1"; PASS=$((PASS+1));
  else echo "FAIL  $1"; echo "        expected: $2"; echo "        actual:   $3"; FAIL=$((FAIL+1)); fi
}

post() { # post <body> [token]
  local body="$1" tok="${2-$TOKEN}"
  if [ -z "$tok" ]; then
    curl -s -o /tmp/mcp-body.txt -w '%{http_code}' -X POST "$BASE/mcp" \
      -H 'Content-Type: application/json' --data-binary "$body"
  else
    curl -s -o /tmp/mcp-body.txt -w '%{http_code}' -X POST "$BASE/mcp" \
      -H 'Content-Type: application/json' -H "Authorization: Bearer $tok" --data-binary "$body"
  fi
}

# A-1 initialize -------------------------------------------------------------
CODE=$(post '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2026-07-28","capabilities":{},"clientInfo":{"name":"smoke","version":"0"}}}')
BODY=$(cat /tmp/mcp-body.txt)
check "A-1 initialize status" "200" "$CODE"
check "A-1 protocolVersion" "2026-07-28" "$(echo "$BODY" | jq -r '.result.protocolVersion')"
check "A-1 capabilities.tools.listChanged" "false" "$(echo "$BODY" | jq -r '.result.capabilities.tools.listChanged')"
check "A-1 serverInfo.name" "true" "$(echo "$BODY" | jq -r '.result.serverInfo | has("name")')"
CODE=$(post '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"1999-01-01"}}')
check "A-1 version fallback" "2026-07-28" "$(cat /tmp/mcp-body.txt | jq -r '.result.protocolVersion')"
CODE=$(post '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25"}}')
check "A-1 downgrade 2025-11-25" "2025-11-25" "$(cat /tmp/mcp-body.txt | jq -r '.result.protocolVersion')"

# A-2 notification -> 202 + empty body ---------------------------------------
CODE=$(post '{"jsonrpc":"2.0","method":"notifications/initialized"}')
check "A-2 notification status" "202" "$CODE"
check "A-2 notification body" "0" "$(wc -c </tmp/mcp-body.txt | tr -d ' ')"

# A-3 tools/list -------------------------------------------------------------
CODE=$(post '{"jsonrpc":"2.0","id":2,"method":"tools/list"}')
BODY=$(cat /tmp/mcp-body.txt)
check "A-3 tools/list status" "200" "$CODE"
check "A-3 tool names" "add echo" "$(echo "$BODY" | jq -r '.result.tools | map(.name) | sort | join(" ")')"
check "A-3 add inputSchema" "object" "$(echo "$BODY" | jq -r '.result.tools[] | select(.name=="add") | .inputSchema.type')"

# A-4 tools/call -------------------------------------------------------------
CODE=$(post '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"add","arguments":{"a":40,"b":2}}}')
check "A-4 add result" "42" "$(cat /tmp/mcp-body.txt | jq -r '.result.content[0].text')"
check "A-4 add isError" "false" "$(cat /tmp/mcp-body.txt | jq -r '.result.isError')"
CODE=$(post '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"echo","arguments":{"text":"hello mcp"}}}')
check "A-4 echo result" "hello mcp" "$(cat /tmp/mcp-body.txt | jq -r '.result.content[0].text')"
CODE=$(post '{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"add","arguments":{"a":"x","b":1}}}')
check "A-4 handler error isError" "true" "$(cat /tmp/mcp-body.txt | jq -r '.result.isError')"

# A-5 error paths ------------------------------------------------------------
CODE=$(post '{"jsonrpc":"2.0","id":6,"method":"nope"}')
check "A-5 unknown method status" "200" "$CODE"
check "A-5 unknown method code" "-32601" "$(cat /tmp/mcp-body.txt | jq -r '.error.code')"
CODE=$(post '{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"ghost"}}')
check "A-5 unknown tool code" "-32602" "$(cat /tmp/mcp-body.txt | jq -r '.error.code')"
CODE=$(post '{"jsonrpc":"2.0","id":8,"method":"ping"}')
check "A-5 ping result" "{}" "$(cat /tmp/mcp-body.txt | jq -c '.result')"
CODE=$(post '{not json')
check "A-5 bad json status" "400" "$CODE"
check "A-5 bad json code" "-32700" "$(cat /tmp/mcp-body.txt | jq -r '.error.code')"
CODE=$(post '[{"jsonrpc":"2.0","id":9,"method":"ping"}]')
check "A-5 batch status" "400" "$CODE"
check "A-5 batch code" "-32600" "$(cat /tmp/mcp-body.txt | jq -r '.error.code')"

# A-6 health + method not allowed -------------------------------------------
CODE=$(curl -s -o /tmp/mcp-body.txt -w '%{http_code}' "$BASE/health")
check "A-6 health status" "200" "$CODE"
check "A-6 health body" "ok" "$(cat /tmp/mcp-body.txt)"
CODE=$(post '{"jsonrpc":"2.0","id":10,"method":"ping"}' "")
check "A-6 unauthenticated health" "200" "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/health")"
CODE=$(curl -s -o /dev/null -w '%{http_code}' -X GET "$BASE/mcp")
check "A-6 GET /mcp" "405" "$CODE"
CODE=$(curl -s -o /dev/null -w '%{http_code}' -X DELETE "$BASE/mcp")
check "A-6 DELETE /mcp" "405" "$CODE"

# A-7 auth -------------------------------------------------------------------
CODE=$(post '{"jsonrpc":"2.0","id":11,"method":"ping"}' "")
check "A-7 no token" "401" "$CODE"
CODE=$(post '{"jsonrpc":"2.0","id":12,"method":"ping"}' "wrong-token")
check "A-7 wrong token" "401" "$CODE"
CODE=$(post '{"jsonrpc":"2.0","id":13,"method":"ping"}')
check "A-7 valid token" "200" "$CODE"

# FR-9 CORS 预检 -------------------------------------------------------------
CODE=$(curl -s -o /tmp/mcp-body.txt -w '%{http_code}' -X OPTIONS "$BASE/mcp" \
  -H 'Origin: https://example.com' -H 'Access-Control-Request-Method: POST')
check "FR-9 OPTIONS status" "204" "$CODE"
check "FR-9 allow-methods" "POST, OPTIONS" "$(curl -s -D - -o /dev/null -X OPTIONS "$BASE/mcp" | tr -d '\r' | awk -F': ' '/[Aa]ccess-[Cc]ontrol-[Aa]llow-[Mm]ethods/{print $2}')"
check "FR-9 allow-origin" "https://example.com" "$(curl -s -D - -o /dev/null -X OPTIONS "$BASE/mcp" -H 'Origin: https://example.com' | tr -d '\r' | awk -F': ' '/[Aa]ccess-[Cc]ontrol-[Aa]llow-[Oo]rigin/{print $2}')"

# FR-12 keep-alive 连接复用（同一 curl 进程内两次 POST，应只建 1 条连接）
KA=$(curl -s -o /dev/null -o /dev/null -w '%{http_code}/%{num_connects} ' "$BASE/mcp" "$BASE/mcp" \
  -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  --data-binary '{"jsonrpc":"2.0","id":20,"method":"ping"}')
check "FR-12 keep-alive reuse" "200/1 200/0 " "$KA"

# A-8 graceful shutdown ------------------------------------------------------
kill -TERM "$PID" 2>/dev/null
wait "$PID" 2>/dev/null
RC=$?
check "A-8 SIGTERM exit code" "0" "$RC"
if grep -q "mcp server stopped" /tmp/mcp-smoke.out; then
  check "A-8 shutdown log" "found" "found"
else
  check "A-8 shutdown log" "found" "$(tail -2 /tmp/mcp-smoke.err)"
fi

echo "-----------------------------"
echo "passed: $PASS   failed: $FAIL"
[ "$FAIL" -eq 0 ]
