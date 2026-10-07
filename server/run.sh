#!/bin/sh
# Start the laptop server. Web page: http://localhost:8080
cd "$(dirname "$0")"
exec ../.venv/bin/python server.py "$@"
