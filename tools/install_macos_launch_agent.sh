#!/bin/sh
set -eu

LABEL="${LABEL:-com.onair.sign.calendar-bridge}"
DEVICE="${ON_AIR_SIGN_URL:-http://onair.local}"
INTERVAL="${ON_AIR_POLL_SECONDS:-30}"
LOOKAHEAD="${ON_AIR_LOOKAHEAD_MINUTES:-10}"
HIDE_SUBJECT=1
REQUIRE_TEAMS_LINK=0
INCLUDE_ALL_DAY=0
CALENDARS=""
ACTION="install"

usage() {
    cat <<'EOF'
Usage:
  sh tools/install_macos_launch_agent.sh [options]

Options:
  --device URL             ESP32 base URL. Default: ON_AIR_SIGN_URL or http://onair.local
  --interval SECONDS       Poll interval. Default: ON_AIR_POLL_SECONDS or 30
  --lookahead MINUTES      Calendar query lookahead. Default: ON_AIR_LOOKAHEAD_MINUTES or 10
  --calendar NAME          Calendar name to include. Repeat for multiple calendars.
  --require-teams-link     Only trigger on events that look like Teams meetings.
  --include-all-day        Allow all-day events to turn the sign on.
  --show-subject           Send/log the real meeting subject. Default sends "Meeting".
  --label LABEL            launchd label. Default: com.onair.sign.calendar-bridge
  --status                 Show launchd status and log paths.
  --uninstall              Stop and remove the LaunchAgent plist.
  -h, --help               Show this help.
EOF
}

need_value() {
    if [ "${2:-}" = "" ]; then
        echo "Missing value for $1" >&2
        exit 2
    fi
}

append_calendar() {
    if [ "$CALENDARS" = "" ]; then
        CALENDARS=$1
    else
        CALENDARS="${CALENDARS}
$1"
    fi
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --device)
            need_value "$1" "${2:-}"
            DEVICE=$2
            shift 2
            ;;
        --interval)
            need_value "$1" "${2:-}"
            INTERVAL=$2
            shift 2
            ;;
        --lookahead)
            need_value "$1" "${2:-}"
            LOOKAHEAD=$2
            shift 2
            ;;
        --calendar)
            need_value "$1" "${2:-}"
            append_calendar "$2"
            shift 2
            ;;
        --require-teams-link)
            REQUIRE_TEAMS_LINK=1
            shift
            ;;
        --include-all-day)
            INCLUDE_ALL_DAY=1
            shift
            ;;
        --show-subject)
            HIDE_SUBJECT=0
            shift
            ;;
        --label)
            need_value "$1" "${2:-}"
            LABEL=$2
            shift 2
            ;;
        --status)
            ACTION="status"
            shift
            ;;
        --uninstall)
            ACTION="uninstall"
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [ "$(uname -s)" != "Darwin" ]; then
    echo "This installer is macOS-only." >&2
    exit 2
fi

SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd -P)
REPO_DIR=$(CDPATH= cd "$SCRIPT_DIR/.." && pwd -P)
BRIDGE_SCRIPT="$REPO_DIR/tools/macos_calendar_bridge.py"
PYTHON_BIN="${PYTHON_BIN:-$(command -v python3 || true)}"
UID_VALUE=$(id -u)
PLIST_DIR="$HOME/Library/LaunchAgents"
PLIST_PATH="$PLIST_DIR/$LABEL.plist"
LOG_DIR="$HOME/Library/Logs/on_air_sign"
OUT_LOG="$LOG_DIR/macos_calendar_bridge.out.log"
ERR_LOG="$LOG_DIR/macos_calendar_bridge.err.log"

if [ "$ACTION" = "status" ]; then
    echo "Label: $LABEL"
    echo "Plist: $PLIST_PATH"
    echo "Stdout log: $OUT_LOG"
    echo "Stderr log: $ERR_LOG"
    launchctl print "gui/$UID_VALUE/$LABEL" || true
    exit 0
fi

if [ "$ACTION" = "uninstall" ]; then
    launchctl bootout "gui/$UID_VALUE/$LABEL" 2>/dev/null || true
    launchctl bootout "gui/$UID_VALUE" "$PLIST_PATH" 2>/dev/null || true
    rm -f "$PLIST_PATH"
    echo "Removed $PLIST_PATH"
    exit 0
fi

if [ ! -f "$BRIDGE_SCRIPT" ]; then
    echo "Cannot find $BRIDGE_SCRIPT" >&2
    exit 2
fi

if [ "$PYTHON_BIN" = "" ]; then
    echo "python3 was not found. Install Python 3 first, then rerun this script." >&2
    exit 2
fi

mkdir -p "$PLIST_DIR" "$LOG_DIR"

export LABEL DEVICE INTERVAL LOOKAHEAD HIDE_SUBJECT REQUIRE_TEAMS_LINK INCLUDE_ALL_DAY
export CALENDARS PYTHON_BIN BRIDGE_SCRIPT REPO_DIR OUT_LOG ERR_LOG PLIST_PATH

"$PYTHON_BIN" - <<'PY'
import os
import plistlib
from pathlib import Path

program_args = [
    os.environ["PYTHON_BIN"],
    os.environ["BRIDGE_SCRIPT"],
    "--device",
    os.environ["DEVICE"],
    "--interval",
    os.environ["INTERVAL"],
    "--lookahead",
    os.environ["LOOKAHEAD"],
]

for calendar in os.environ.get("CALENDARS", "").splitlines():
    if calendar:
        program_args.extend(["--calendar", calendar])

if os.environ["REQUIRE_TEAMS_LINK"] == "1":
    program_args.append("--require-teams-link")

if os.environ["INCLUDE_ALL_DAY"] == "1":
    program_args.append("--include-all-day")

if os.environ["HIDE_SUBJECT"] == "1":
    program_args.append("--hide-subject")

plist = {
    "Label": os.environ["LABEL"],
    "ProgramArguments": program_args,
    "RunAtLoad": True,
    "KeepAlive": True,
    "ThrottleInterval": 30,
    "WorkingDirectory": os.environ["REPO_DIR"],
    "StandardOutPath": os.environ["OUT_LOG"],
    "StandardErrorPath": os.environ["ERR_LOG"],
    "EnvironmentVariables": {
        "PYTHONUNBUFFERED": "1",
    },
}

with Path(os.environ["PLIST_PATH"]).open("wb") as file:
    plistlib.dump(plist, file)
PY

plutil -lint "$PLIST_PATH"

launchctl bootout "gui/$UID_VALUE/$LABEL" 2>/dev/null || true
launchctl bootout "gui/$UID_VALUE" "$PLIST_PATH" 2>/dev/null || true
launchctl bootstrap "gui/$UID_VALUE" "$PLIST_PATH"
launchctl enable "gui/$UID_VALUE/$LABEL"
launchctl kickstart -k "gui/$UID_VALUE/$LABEL"

echo "Installed and started $LABEL"
echo "Plist: $PLIST_PATH"
echo "Stdout log: $OUT_LOG"
echo "Stderr log: $ERR_LOG"
