# On Air Sign

ESP32-C6 firmware for a USB-C powered on-air sign. The first milestone controls an LED controller board from a Seeed Studio XIAO ESP32-C6 by electronically pressing the controller's `SW1`/`SW2` switch pads. The LED controller cycles through three modes: `On -> Glowing Pulse -> Off -> On`. The firmware also accepts a normally-open physical switch, exposes a Wi-Fi dashboard, and provides a local API endpoint for a Microsoft Teams calendar bridge.

Battery power and deep sleep are intentionally left for the next hardware pass.

## Hardware

Default pins are set in `platformio.ini`:

| Function | XIAO label | ESP32-C6 GPIO | Notes |
| --- | --- | ---: | --- |
| Controller switch press | D3 | GPIO21 | Drives an optocoupler, relay, or switch IC input to advance the LED controller mode |
| Switch input | D2 | GPIO2 | Active-low, internal pull-up enabled |

The LED board already has its own controller. The ESP32 should simulate a momentary press across the controller's `SW1` and `SW2` pads; it should not power the LED array directly.

Measured `SW1`/`SW2` voltage is about 3.5-4.7 V, which is above the ESP32-C6 GPIO safe input range. Do not connect either switch pad directly to the XIAO GPIO pins.

Recommended wiring is to use an isolating or dry-contact part:

```text
XIAO GPIO21/D3 -> 220-330 ohm -> optocoupler input LED -> XIAO GND

Optocoupler transistor, PhotoMOS, or relay contacts:
  one side -> LED controller SW1 pad
  other side -> LED controller SW2 pad
```

Best first choices are a small PhotoMOS relay or reed relay because they behave like a floating button contact and do not care which `SW` pad is high. A basic optocoupler can also work if its output transistor polarity matches the controller pad polarity. If you use a plain transistor or MOSFET instead of an isolator/relay, first identify which pad is ground and confirm the circuit never exposes the ESP32 to more than 3.3 V.

### SRD-05VDC-SL-C Relay Driver

For the mechanical relay you have, drive the coil with a transistor. Do not power the relay coil directly from an ESP32 GPIO.

```text
ESP32 / XIAO side, shared 5 V and GND

                  +5 V from XIAO VBUS/5V
                            |
                            +---- relay coil ----+---- NPN collector
                            |                    |
                            |               flyback diode
                            |               cathode/stripe at +5 V
                            +--------------------+
                                             anode at NPN collector

GPIO21 / D3 ---- 1k to 4.7k ---- NPN base

ESP32 / XIAO GND ---------------- NPN emitter

Optional: add a 10k pulldown from NPN base to GND.

Relay contact side, isolated from the ESP32

LED controller SW1 pad ---- relay COM
LED controller SW2 pad ---- relay NO
```

The flyback diode is required. Put the diode directly across the relay coil with the cathode/stripe on the +5 V side and the anode on the transistor collector side. Good NPN transistor choices are small signal parts such as `2N2222`, `PN2222`, `S8050`, or `BC337`. A `1k` base resistor is a good starting value for a 5 V relay coil; use `2.2k` or `4.7k` only if the relay pulls in reliably.

For the external normally-open user switch, wire one side to `D2` and the other side to `GND`; the firmware uses the internal pull-up. If this is an illuminated switch, the lamp/LED side can use 5 V separately, while the contact side should still switch the ESP32 GPIO to ground.

Avoid wiring a separate physical switch directly across the LED board's `SW1`/`SW2` pads unless you accept occasional state drift. The most reliable setup is: physical switch -> ESP32 input, ESP32 output -> electronic press across `SW1`/`SW2`.

## Firmware Setup

Install PlatformIO, then copy the Wi-Fi template:

```powershell
Copy-Item include\secrets.example.h include\secrets.h
```

Edit `include/secrets.h` with your Wi-Fi SSID and password. Then build and upload:

```powershell
pio run
pio run --target upload
pio device monitor
```

If Wi-Fi credentials are missing or the board cannot connect, it starts a fallback access point named `OnAirSign-xxxx`. Connect to it with password `onair1234` and open `http://192.168.4.1`.

The firmware advertises an mDNS hostname, so on clients that support `.local` names you can open:

```text
http://onair.local/
```

If `.local` does not resolve on a particular computer or phone, use the IP address printed in the serial monitor.

## Dashboard and API

Once connected, open `http://onair.local/` or the IP address printed in the serial monitor. The dashboard supports manual `On`, `Glow`, and `Off` modes plus a `Follow Teams schedule` mode.

Useful endpoints:

```text
GET  /api/status
POST /api/on
POST /api/glow
POST /api/pulse
POST /api/off
POST /api/toggle
POST /api/next
POST /api/sync?mode=off
POST /api/auto?enabled=1
POST /api/teams?active=1&subject=Daily%20standup
POST /api/teams
```

JSON body for `/api/teams`:

```json
{
  "meetingActive": true,
  "subject": "Daily standup"
}
```

When auto mode is enabled, `/api/teams` selects solid `On` while a meeting is active and `Off` when it is idle. Manual dashboard actions and the physical switch turn auto mode off until you re-enable it.

Because the LED board exposes only switch pads, the ESP32 tracks an assumed three-mode state. `/api/on`, `/api/glow`, and `/api/off` send 0, 1, or 2 presses based on that assumed mode. `/api/toggle` and `/api/next` always send one press and advance the assumed mode.

For deterministic direct mode control, start with the LED controller actually in `Off` when the ESP32 boots. If the assumed mode drifts from the actual LED mode, put the LED controller in a known visible mode and call `/api/sync?mode=on`, `/api/sync?mode=glow`, or `/api/sync?mode=off`; sync updates only the firmware's assumption and does not press `SW1`/`SW2`.

## Teams Bridge

The ESP32 does not do Microsoft OAuth directly. The included bridge script runs on a computer, polls Microsoft Graph calendar events, and posts the active meeting state to the ESP32.

### Local Outlook Bridge, No App Registration

If you cannot create Microsoft Entra app registrations, use the local Outlook bridge instead. It reads the default calendar from the Outlook desktop profile already signed in on this Windows computer, so it does not need your Exchange password, a Graph client ID, or tenant admin access.

Open Outlook first, then run:

```powershell
$env:ON_AIR_SIGN_URL = "http://onair.local"
powershell -ExecutionPolicy Bypass -File tools\outlook_bridge.ps1
```

By default this treats any active non-free calendar event as a meeting. To only trigger for events that look like Teams meetings, use:

```powershell
powershell -ExecutionPolicy Bypass -File tools\outlook_bridge.ps1 -RequireTeamsLink
```

Useful options:

```text
-Device http://onair.local     ESP32 dashboard/API base URL
-Interval 30                   Poll interval in seconds
-RequireTeamsLink              Only trigger when the event has a Teams/online-meeting marker
-HideSubject                   Send "Meeting" instead of the real calendar subject
-IncludeAllDay                 Allow all-day busy events to turn the sign on
```

Keep the PowerShell window open while you want calendar sync running.

### Local macOS Calendar Bridge, No App Registration

On macOS, use the local Calendar bridge. It reads events from the built-in Calendar app, so your Exchange or Microsoft 365 calendar must already be synced into macOS Calendar. This does not use Microsoft Graph, does not need an app registration, and does not store your Exchange password.

First confirm your work calendar appears in the Calendar app. Then run:

```bash
cd /path/to/on_air_sign
export ON_AIR_SIGN_URL="http://onair.local"
python3 tools/macos_calendar_bridge.py
```

The first run may trigger a macOS privacy prompt for Terminal or Python to access Calendar. Allow it. If you need to fix permissions later, check System Settings > Privacy & Security > Calendars and Automation.

List available macOS calendar names:

```bash
python3 tools/macos_calendar_bridge.py --list-calendars
```

Restrict the bridge to one calendar and only trigger for events that look like Teams meetings:

```bash
python3 tools/macos_calendar_bridge.py --calendar "Calendar" --require-teams-link --hide-subject
```

Useful options:

```text
--device http://onair.local    ESP32 dashboard/API base URL
--interval 30                  Poll interval in seconds
--calendar "Calendar"          Calendar name to include; repeat for multiple calendars
--require-teams-link           Only trigger when the event has a Teams/online-meeting marker
--hide-subject                 Send "Meeting" instead of the real calendar subject
--include-all-day              Allow all-day events to turn the sign on
--once                         Send one update and exit
```

By default this treats any active timed calendar event as a meeting. That is more reliable than Teams-link detection because some synced Exchange calendars hide the meeting body from macOS Calendar.

To run the macOS bridge at login and keep it in the background, install it as a per-user LaunchAgent:

```bash
python3 tools/macos_calendar_bridge.py --once --hide-subject
sh tools/install_macos_launch_agent.sh --device "http://onair.local"
```

Run the installer with `sh`, not `sudo`. If you get `permission denied` from `./tools/install_macos_launch_agent.sh`, that only means the script file is not executable; use the `sh tools/install_macos_launch_agent.sh ...` form above.

The installer hides the real meeting subject by default so the background logs and dashboard show `Meeting`. To send the real subject, add `--show-subject`.

Example with one calendar and Teams-looking events only:

```bash
sh tools/install_macos_launch_agent.sh \
  --device "http://onair.local" \
  --calendar "Calendar" \
  --require-teams-link
```

Check status and logs:

```bash
sh tools/install_macos_launch_agent.sh --status
tail -f ~/Library/Logs/on_air_sign/macos_calendar_bridge.out.log
tail -f ~/Library/Logs/on_air_sign/macos_calendar_bridge.err.log
```

Stop and remove the background service:

```bash
sh tools/install_macos_launch_agent.sh --uninstall
```

If the background service logs a macOS privacy error, run the `--once` command from Terminal again and confirm Calendar and Local Network permissions in System Settings > Privacy & Security.

If you accidentally ran the installer with `sudo`, clean up any root-owned files and reinstall as your normal user:

```bash
sudo rm -f "$HOME/Library/LaunchAgents/com.onair.sign.calendar-bridge.plist"
sudo chown "$USER":staff "$HOME/Library/LaunchAgents"
sudo chown -R "$USER":staff "$HOME/Library/Logs/on_air_sign" 2>/dev/null || true
sh tools/install_macos_launch_agent.sh --device "http://onair.local"
```

### Microsoft Graph Bridge

The Graph bridge is a cleaner API-based option when your Microsoft tenant allows app registrations.

Create a Microsoft Entra app registration that allows public-client device-code auth and delegated `User.Read` plus `Calendars.Read` permissions. Then:

```powershell
python -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install -r tools\requirements.txt
$env:MS_GRAPH_CLIENT_ID = "your-client-id"
$env:ON_AIR_SIGN_URL = "http://the-esp32-ip-address"
python tools\teams_bridge.py
```

On the first run, the script prints a Microsoft device-code login prompt. After login, it caches the token in `.token_cache.bin`.

## Later Battery Pass

For 3 AAA operation, the next pass should revisit:

- LED array current budget and whether the array can realistically run from AAA cells.
- Power path from battery to the XIAO and LED controller board.
- External pull resistors and leakage for the switch circuit.
- Deep-sleep wake source pin choice.
- How often the Teams bridge state should be checked, since Wi-Fi wakeups dominate battery use.

## References

- Seeed Studio XIAO ESP32-C6 getting started and pinout: https://wiki.seeedstudio.com/xiao_esp32c6_getting_started/
- PlatformIO Seeed XIAO ESP32-C6 board page: https://docs.platformio.org/en/latest/boards/espressif32/seeed_xiao_esp32c6.html
- Microsoft Graph calendarView API: https://learn.microsoft.com/en-us/graph/api/user-list-calendarview
