#!/usr/bin/env bash
# Teaching Deck interactive lab: server.py + robot_agent.py + lab.py
# After starting, open http://127.0.0.1:8080 in your browser.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HTTP=8080
CMD_PORT=9090        # server.py: reads PSP deck commands, relays to robot
ROBOT_CMD_PORT=9092  # robot listens here (server --forward-to)
CAM_PORT=9091        # robot streams cameras here (lab serves them to the browser)
TEL_PORT=9093        # robot telemetry -> lab

LOG=/tmp
cleanup() { echo; echo "stopping lab..."; kill -9 "$WEB" "$ROB" "$SRV" 2>/dev/null; }
trap cleanup INT TERM EXIT

echo "[lab] starting test program (server.py: command reader + relay)..."
python3 "$ROOT/server/server.py" --port "$CMD_PORT" \
    --forward-to 127.0.0.1:"$ROBOT_CMD_PORT" >"$LOG/lab-server.log" 2>&1 &
SRV=$!

echo "[lab] starting robot agent (cam streams -> $CAM_PORT, telemetry -> $TEL_PORT)..."
python3 "$ROOT/robot/robot_agent.py" --receive --cmd-port "$ROBOT_CMD_PORT" \
    --sink 127.0.0.1 --sink-port "$CAM_PORT" \
    --telemetry-to 127.0.0.1:"$TEL_PORT" >"$LOG/lab-robot.log" 2>&1 &
ROB=$!

echo "[lab] starting interactive web panel..."
python3 "$ROOT/test/lab.py" --http "$HTTP" \
    --video-port "$CAM_PORT" --telemetry-port "$TEL_PORT" \
    --cmd-port "$CMD_PORT" >"$LOG/lab-web.log" 2>&1 &
WEB=$!
for i in $(seq 1 30); do
    curl -s -o /dev/null "http://127.0.0.1:$HTTP/state" && break
    sleep 0.2
done

echo
echo "  lab ready -> http://127.0.0.1:$HTTP"
echo "  logs:      $LOG/lab-{server,robot,web}.log"
echo "  Ctrl-C to stop everything."
wait "$WEB"
trap - INT TERM EXIT