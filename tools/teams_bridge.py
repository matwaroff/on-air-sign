#!/usr/bin/env python3
"""Poll Microsoft Graph calendarView and update the ESP32 on-air sign."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import sys
import time
from pathlib import Path
from typing import Any

import msal
import requests


GRAPH_AUTHORITY_TEMPLATE = "https://login.microsoftonline.com/{tenant}"
GRAPH_CALENDAR_VIEW = "https://graph.microsoft.com/v1.0/me/calendarView"
SCOPES = ["User.Read", "Calendars.Read"]


def utc_now() -> dt.datetime:
    return dt.datetime.now(dt.timezone.utc)


def iso(value: dt.datetime) -> str:
    return value.isoformat().replace("+00:00", "Z")


def load_cache(path: Path) -> msal.SerializableTokenCache:
    cache = msal.SerializableTokenCache()
    if path.exists():
        cache.deserialize(path.read_text(encoding="utf-8"))
    return cache


def save_cache(cache: msal.SerializableTokenCache, path: Path) -> None:
    if cache.has_state_changed:
        path.write_text(cache.serialize(), encoding="utf-8")


def get_token(client_id: str, tenant: str, cache_path: Path) -> str:
    cache = load_cache(cache_path)
    app = msal.PublicClientApplication(
        client_id=client_id,
        authority=GRAPH_AUTHORITY_TEMPLATE.format(tenant=tenant),
        token_cache=cache,
    )

    accounts = app.get_accounts()
    result: dict[str, Any] | None = None
    if accounts:
        result = app.acquire_token_silent(SCOPES, account=accounts[0])

    if not result:
        flow = app.initiate_device_flow(scopes=SCOPES)
        if "user_code" not in flow:
            raise RuntimeError(f"Could not start device auth flow: {flow}")
        print(flow["message"], flush=True)
        result = app.acquire_token_by_device_flow(flow)

    save_cache(cache, cache_path)

    if "access_token" not in result:
        raise RuntimeError(f"Could not acquire Microsoft Graph token: {result}")

    return result["access_token"]


def parse_graph_time(value: str) -> dt.datetime:
    if value.endswith("Z"):
        value = value[:-1] + "+00:00"
    parsed = dt.datetime.fromisoformat(value)
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=dt.timezone.utc)
    return parsed.astimezone(dt.timezone.utc)


def fetch_events(token: str, lookahead_minutes: int) -> list[dict[str, Any]]:
    now = utc_now()
    end = now + dt.timedelta(minutes=lookahead_minutes)
    headers = {
        "Authorization": f"Bearer {token}",
        "Prefer": 'outlook.timezone="UTC"',
    }
    params = {
        "startDateTime": iso(now - dt.timedelta(minutes=2)),
        "endDateTime": iso(end),
        "$select": "subject,start,end,isCancelled,showAs,isOnlineMeeting,onlineMeetingProvider",
        "$orderby": "start/dateTime",
        "$top": "25",
    }
    response = requests.get(GRAPH_CALENDAR_VIEW, headers=headers, params=params, timeout=15)
    response.raise_for_status()
    return response.json().get("value", [])


def active_teams_event(events: list[dict[str, Any]]) -> dict[str, Any] | None:
    now = utc_now()

    for event in events:
        if event.get("isCancelled"):
            continue

        start_value = event.get("start", {}).get("dateTime")
        end_value = event.get("end", {}).get("dateTime")
        if not start_value or not end_value:
            continue

        start = parse_graph_time(start_value)
        end = parse_graph_time(end_value)
        if not (start <= now < end):
            continue

        show_as = event.get("showAs", "busy")
        provider = event.get("onlineMeetingProvider", "unknown")
        is_online = bool(event.get("isOnlineMeeting"))
        if show_as in {"free", "unknown"}:
            continue

        if is_online or provider not in {"unknown", "notOnline"}:
            return event

    return None


def update_device(device_url: str, active: bool, subject: str) -> None:
    url = device_url.rstrip("/") + "/api/teams"
    response = requests.post(
        url,
        json={"meetingActive": active, "subject": subject},
        timeout=8,
    )
    response.raise_for_status()
    print(json.dumps(response.json(), sort_keys=True), flush=True)


def main() -> int:
    parser = argparse.ArgumentParser(description="Bridge Microsoft Teams calendar presence to an ESP32 on-air sign.")
    parser.add_argument("--device", default=os.getenv("ON_AIR_SIGN_URL", "http://192.168.4.1"))
    parser.add_argument("--client-id", default=os.getenv("MS_GRAPH_CLIENT_ID"))
    parser.add_argument("--tenant", default=os.getenv("MS_GRAPH_TENANT", "common"))
    parser.add_argument("--interval", type=int, default=int(os.getenv("ON_AIR_POLL_SECONDS", "30")))
    parser.add_argument("--lookahead", type=int, default=int(os.getenv("ON_AIR_LOOKAHEAD_MINUTES", "10")))
    parser.add_argument("--cache", type=Path, default=Path(os.getenv("MS_GRAPH_TOKEN_CACHE", ".token_cache.bin")))
    args = parser.parse_args()

    if not args.client_id:
        print("Set MS_GRAPH_CLIENT_ID or pass --client-id for your Microsoft Entra app registration.", file=sys.stderr)
        return 2

    last_state: tuple[bool, str] | None = None

    while True:
        try:
            token = get_token(args.client_id, args.tenant, args.cache)
            event = active_teams_event(fetch_events(token, args.lookahead))
            active = event is not None
            subject = event.get("subject", "") if event else ""
            state = (active, subject)
            if state != last_state:
                update_device(args.device, active, subject)
                last_state = state
        except KeyboardInterrupt:
            return 0
        except Exception as exc:
            print(f"bridge error: {exc}", file=sys.stderr, flush=True)

        time.sleep(max(args.interval, 5))


if __name__ == "__main__":
    raise SystemExit(main())
