#!/bin/bash
set -e
cd "$(dirname "$0")/.."

if [ ! -f config.yml ]; then
    echo "Error: config.yml not found"
    echo "Copy config.yml.example to config.yml and add your Lichess API token"
    exit 1
fi

# Set up venv if it doesn't exist
if [ ! -d lichess-bot/.venv ]; then
    echo "Setting up Python virtual environment..."
    python3 -m venv lichess-bot/.venv
    lichess-bot/.venv/bin/pip install -r lichess-bot/requirements.txt
fi

exec lichess-bot/.venv/bin/python3 lichess-bot/lichess-bot.py
