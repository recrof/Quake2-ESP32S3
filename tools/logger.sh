#!/bin/sh
# Starts the serial logger in the background: [PORT=...] tools/logger.sh <workdir>
# (console in <workdir>/console.log, commands appended to <workdir>/commands)
cd "$(dirname "$0")/.."
. tools/idf_env.sh
pkill -f serial_daemon.py
nohup python tools/serial_daemon.py "$1" ${PORT:+--port "$PORT"} > /dev/null 2>&1 &
