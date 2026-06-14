# Changelog

All notable changes to ESP Nest. Format follows
[Keep a Changelog](https://keepachangelog.com/), versioning follows
[SemVer](https://semver.org/).

## [Unreleased]

## [2.0.1] - 2026-06-14
### Changed
- External MQTT topic names (lights, fan, stereo, blinds) and the two
  on-screen blinds labels are now Kconfig options (`TOPIC_*`) with generic
  defaults instead of hardcoded values — override them in `secrets.defaults`
  (or `idf.py menuconfig`). Removes personal home-topology details from the
  public source and the released binaries; existing devices keep working once
  their real topics are set in `secrets.defaults` before the next build/flash.

## [2.0.0] - 2026-06-12

First public release. Major bump for the UI redesign: the menu grew from two
pages to four and gained two new detail screens.

### Added
- Menu redesigned to 4 color-coded pages (Lights / Fiiuuu / Blinds / Sound)
- Sound detail screen — relative stereo volume over MQTT (bridged to IR by a
  Home Assistant automation)
- Blinds detail screen wired to the CC1101 ESPHome bridge: per-group position
  control with read-back, group broadcast for full open/close
- Long press toggles light/fan directly from menu pages and the fan detail
- Firmware version shown on the status line and in HA discovery (`version.txt`)
- Public-release scaffolding: MIT license, README, `secrets.defaults.example`,
  CI release pipeline (tag → OTA + factory images + checksums)

### Changed
- Light synchronization rewritten as the `light_sync` authority-window state
  machine (pure logic, host-testable, regression-tested) — replaces the
  two-phase echo-suppression heuristics that caused brightness jump-backs

## [1.0.0] - 2026-02-28

Initial stable firmware; in daily living-room use since February 2026.
No prebuilt binaries for this historic release — build from source.

### Added
- ST7701S round 480×480 display bring-up (9-bit SPI init → 16-bit RGB) with
  LVGL, software 180° rotation, backlight PWM with idle dimming
- Rotary-encoder menu navigation with animated page transitions, brightness
  and fan detail screens, WS2812 ring breathing in the page color
- WiFi STA with endless reconnect + backoff, periodic status monitoring
- MQTT control of zigbee2mqtt lights and fan, Home Assistant MQTT discovery
  incl. diagnostic sensors
- HTTP OTA updates (`/` form, `POST /update`) on port 80
