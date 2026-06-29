#!/usr/bin/env python3
"""Read macOS Calendar.app locally and update the ESP32 on-air sign."""

from __future__ import annotations

import argparse
import json
import os
import platform
import subprocess
import sys
import time
import urllib.error
import urllib.request
from typing import Any


DEFAULT_DEVICE_URL = "http://onair.local"

JXA_SCRIPT = r"""
function readString(item, propertyName) {
    try {
        var value = item[propertyName]();
        if (value === null || value === undefined) {
            return "";
        }
        return String(value);
    } catch (error) {
        return "";
    }
}

function readBool(item, propertyName) {
    try {
        return Boolean(item[propertyName]());
    } catch (error) {
        return false;
    }
}

function eventLooksLikeTeams(item) {
    var text = [
        readString(item, "summary"),
        readString(item, "location"),
        readString(item, "description"),
        readString(item, "url")
    ].join("\n");

    return /teams\.microsoft\.com|microsoft teams|skype teams|join microsoft teams/i.test(text);
}

function eventStatus(item) {
    try {
        return String(item.status());
    } catch (error) {
        return "";
    }
}

function run(argv) {
    var options = JSON.parse(argv[0]);
    var calendarApp = Application("Calendar");
    var calendars = calendarApp.calendars();

    if (options.listCalendars) {
        var names = calendars.map(function(calendar) {
            return calendar.name();
        });
        return JSON.stringify({ calendars: names });
    }

    var now = new Date();
    var windowStart = new Date(now.getTime() - 2 * 60 * 1000);
    var windowEnd = new Date(now.getTime() + Math.max(options.lookaheadMinutes, 1) * 60 * 1000);
    var selectedCalendars = options.calendars || [];
    var events = [];

    calendars.forEach(function(calendar) {
        var calendarName = calendar.name();
        if (selectedCalendars.length > 0 && selectedCalendars.indexOf(calendarName) === -1) {
            return;
        }

        var calendarEvents = [];
        try {
            calendarEvents = calendar.events.whose({
                _and: [
                    { endDate: { _ge: windowStart } },
                    { startDate: { _le: windowEnd } }
                ]
            })();
        } catch (error) {
            calendarEvents = calendar.events();
        }

        calendarEvents.forEach(function(item) {
            try {
                var start = item.startDate();
                var end = item.endDate();

                if (!(start <= now && now < end)) {
                    return;
                }

                var allDay = readBool(item, "alldayEvent");
                if (!options.includeAllDay && allDay) {
                    return;
                }

                if (/cancel/i.test(eventStatus(item))) {
                    return;
                }

                var teams = eventLooksLikeTeams(item);
                if (options.requireTeamsLink && !teams) {
                    return;
                }

                events.push({
                    calendar: calendarName,
                    subject: readString(item, "summary"),
                    start: start.toISOString(),
                    end: end.toISOString(),
                    allDay: allDay,
                    teams: teams
                });
            } catch (error) {
            }
        });
    });

    events.sort(function(left, right) {
        return new Date(left.start) - new Date(right.start);
    });

    return JSON.stringify({
        now: now.toISOString(),
        active: events.length > 0,
        event: events.length > 0 ? events[0] : null,
        eventCount: events.length
    });
}
"""


def env_int(name: str, default: int) -> int:
    value = os.getenv(name, "")
    try:
        parsed = int(value)
    except ValueError:
        return default
    return parsed if parsed > 0 else default


def run_calendar_query(options: dict[str, Any]) -> dict[str, Any]:
    result = subprocess.run(
        ["osascript", "-l", "JavaScript", "-e", JXA_SCRIPT, json.dumps(options)],
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        stderr = result.stderr.strip() or "osascript failed"
        raise RuntimeError(stderr)

    try:
        return json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"Calendar query returned invalid JSON: {result.stdout!r}") from exc


def post_device_state(device_url: str, active: bool, subject: str) -> dict[str, Any]:
    url = device_url.rstrip("/") + "/api/teams"
    body = json.dumps({"meetingActive": active, "subject": subject}).encode("utf-8")
    request = urllib.request.Request(
        url,
        data=body,
        headers={"Content-Type": "application/json"},
        method="POST",
    )

    with urllib.request.urlopen(request, timeout=8) as response:
        response_body = response.read().decode("utf-8")

    try:
        return json.loads(response_body)
    except json.JSONDecodeError:
        return {"response": response_body}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Bridge the local macOS Calendar app to an ESP32 on-air sign."
    )
    parser.add_argument("--device", default=os.getenv("ON_AIR_SIGN_URL", DEFAULT_DEVICE_URL))
    parser.add_argument("--interval", type=int, default=env_int("ON_AIR_POLL_SECONDS", 30))
    parser.add_argument("--lookahead", type=int, default=env_int("ON_AIR_LOOKAHEAD_MINUTES", 10))
    parser.add_argument(
        "--calendar",
        action="append",
        default=[],
        help="Calendar name to include. Repeat for multiple calendars. Defaults to all calendars.",
    )
    parser.add_argument("--list-calendars", action="store_true")
    parser.add_argument("--require-teams-link", action="store_true")
    parser.add_argument("--include-all-day", action="store_true")
    parser.add_argument("--hide-subject", action="store_true")
    parser.add_argument("--once", action="store_true")
    parser.add_argument("--verbose", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    if platform.system() != "Darwin":
        print("This local bridge is macOS-only because it reads Calendar.app.", file=sys.stderr)
        return 2

    query_options = {
        "calendars": args.calendar,
        "includeAllDay": args.include_all_day,
        "listCalendars": args.list_calendars,
        "lookaheadMinutes": args.lookahead,
        "requireTeamsLink": args.require_teams_link,
    }

    if args.list_calendars:
        data = run_calendar_query(query_options)
        for name in data.get("calendars", []):
            print(name)
        return 0

    device = args.device.rstrip("/")
    interval = max(args.interval, 5)
    last_state: tuple[bool, str] | None = None

    print("macOS Calendar bridge running", flush=True)
    print(f"  Device: {device}", flush=True)
    print(f"  Poll seconds: {interval}", flush=True)
    print(f"  Calendars: {', '.join(args.calendar) if args.calendar else 'all'}", flush=True)
    print(f"  Require Teams link: {args.require_teams_link}", flush=True)

    while True:
        try:
            data = run_calendar_query(query_options)
            event = data.get("event")
            active = bool(data.get("active"))
            subject = ""

            if event:
                subject = "Meeting" if args.hide_subject else str(event.get("subject", ""))

            state = (active, subject)
            if args.verbose or state != last_state:
                response = post_device_state(device, active, subject)
                timestamp = time.strftime("%H:%M:%S")
                event_count = data.get("eventCount", 0)
                print(
                    f'{timestamp} active={active} subject="{subject}" '
                    f"events={event_count} response={json.dumps(response, sort_keys=True)}",
                    flush=True,
                )
                last_state = state
        except KeyboardInterrupt:
            return 0
        except (RuntimeError, urllib.error.URLError, TimeoutError) as exc:
            print(f"bridge error: {exc}", file=sys.stderr, flush=True)

        if args.once:
            return 0

        time.sleep(interval)


if __name__ == "__main__":
    raise SystemExit(main())
