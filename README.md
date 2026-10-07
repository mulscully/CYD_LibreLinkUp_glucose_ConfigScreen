# CYD LibreLinkUp Glucose Display — v2 (Config Portal)

Displays live blood glucose readings from a LibreLinkUp-connected Libre 2 sensor
directly on a "Cheap Yellow Display" (CYD, ESP32-2432S028R) — no Home Assistant,
no hub, no phone app required. The device talks straight to LibreLinkUp's API
over WiFi.

This is **v2**: unlike [v1](../cyd-librelinkup-glucose), nothing is hardcoded into
the source code. On first boot (or whenever you hold the BOOT button for a few
seconds while it's already running), the device starts its own WiFi setup page
where you enter your WiFi, LibreLinkUp login, and glucose thresholds — all saved
to flash, so anyone can download this and configure it without touching a line
of code.

## Features

- Everything from v1 (color-coded status bar, trend arrow, auto re-auth, stale-data
  fallback, follower-account support)
- Web-based setup portal (via [WiFiManager](https://github.com/tzapu/WiFiManager)) —
  no code editing, no `secrets.h` file to fill in
- Settings persist across power loss (ESP32's built-in flash storage)
- Hold the BOOT button for ~3 seconds any time while it's running (not during
  power-on — see note below) to reopen the setup page and change settings,
  with your current values pre-filled

## Hardware

- ESP32-2432S028R "CYD" board (this specific pin configuration is for a Sunton/JCZN
  variant with an ILI9341 panel — see `platformio.ini` if you have a different
  revision and need to adjust pins/driver)

## Setup

1. Install [VS Code](https://code.visualstudio.com/) and the
   [PlatformIO extension](https://platformio.org/install/ide?install=vscode).
2. Clone this repo and open the folder in VS Code — PlatformIO will detect the
   project automatically and install dependencies (TFT_eSPI, ArduinoJson, WiFiManager).
3. Build and upload via PlatformIO (the arrow icon in the status bar). No config
   file to edit first — that all happens on the device itself next.
4. On first boot, the CYD's screen shows a WiFi network name to join (e.g.
   `CYD-Glucose-Setup`) and an address to visit (`192.168.4.1`).
5. On your phone or computer, join that WiFi network. A setup page should open
   automatically (or open that address manually).
6. Pick your home WiFi, enter your LibreLinkUp email/password and your low/high
   glucose thresholds, and submit.
7. The device reboots, connects to your WiFi, and starts showing glucose.

**To reconfigure later:** once the device has finished booting and is already
running normally (showing glucose, or "No data yet..."), press and hold the
BOOT button for about 3 seconds, then release. The setup page comes back up
with your existing settings pre-filled.

**Important:** don't hold BOOT *while plugging the device in*. GPIO0 (BOOT) is
a hardware "strapping pin" on the ESP32 — if it's held low at the exact instant
of power-on, the chip's own boot ROM enters its flashing/download mode instead
of running this firmware at all (you'd see a blank screen and no serial output).
Always let it boot normally first, then hold BOOT to trigger reconfiguration.

## A note on account types

This has been tested and confirmed working with LibreLinkUp **follower**
accounts (a separate account that watches someone else's readings). There's
also a fallback path intended for **patient** (self-monitoring) accounts, based
on how the API is documented to behave, but it hasn't been verified against a
real patient-role account — if you try it, consider opening an issue either way
so this note can be updated.

## A note on the LibreLinkUp API

This uses LibreLinkUp's unofficial, reverse-engineered API (the same one various
community projects and Home Assistant integrations rely on). Abbott has changed
required request headers before (e.g. an October 2025 update that changed the
expected `version`/`product` header values and added a mandatory `Account-Id`
header). If requests start failing after previously working, check
`addLLUHeaders()` in `src/main.cpp` first — that's almost certainly where an
update is needed.

## License

MIT — see LICENSE.
