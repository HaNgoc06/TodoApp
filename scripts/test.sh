#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
make -s
TMP=$(mktemp -d); trap 'if [[ -n ${PID:-} ]]; then kill "$PID" 2>/dev/null || true; fi; rm -rf "$TMP"' EXIT
cp -R public src task-manager "$TMP/"; mkdir "$TMP/data"
printf '[{"id":1,"title":"Legacy","completed":true}]\n' > "$TMP/data/tasks.json"
cd "$TMP"
for v in 1 2 3; do
 ./task-manager --version "$v" --port 18080 >server.log 2>&1 & PID=$!
 for i in {1..30}; do if curl -sf http://localhost:18080/api/version >/dev/null; then break; fi; sleep .1; done
 curl -sf http://localhost:18080/api/tasks >before.json
 python3 -c 'import json; a=json.load(open("before.json")); assert any(x["title"]=="Legacy" and x["completed"] for x in a)'
 curl -sf -X POST http://localhost:18080/api/tasks -H 'Content-Type: application/json' -d "{\"title\":\"Added V$v\"}" >created.json
 ID=$(python3 -c 'import json;print(json.load(open("created.json"))["id"])')
 curl -sf -X PATCH "http://localhost:18080/api/tasks/$ID" -H 'Content-Type: application/json' -d '{"completed":true}' >/dev/null
 curl -sf http://localhost:18080/api/tasks >after.json
 python3 -c 'import json,sys; a=json.load(open("after.json")); assert any(x["id"]==int(sys.argv[1]) and x["completed"] for x in a)' "$ID"
 curl -sf -X DELETE "http://localhost:18080/api/tasks/$ID" >/dev/null
 curl -sf http://localhost:18080/api/tasks >after_delete.json
 python3 -c 'import json,sys; assert all(x["id"]!=int(sys.argv[1]) for x in json.load(open("after_delete.json")))' "$ID"
 kill "$PID"; wait "$PID" 2>/dev/null || true; unset PID
 echo "V$v: CRUD + persistence source OK"
done
python3 - <<'PY'
import sqlite3
c=sqlite3.connect('data/tasks.db')
assert c.execute('select title,completed from tasks where id=1').fetchone()==('Legacy',1)
print('V3: JSON -> SQLite migration OK')
PY
