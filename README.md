# AI Passport — Voice Input + PPT Remote demo

<p align="right">
  <strong>English</strong> · <a href="README.zh_CN.md">简体中文</a>
</p>

A firmware demo branch for the **FoloToy AI Passport** (ESP32-C3 badge). On top of the
base platform it adds two Bluetooth features that work at the same time:

- **PPT remote** — the badge advertises as a Bluetooth HID **keyboard** (standard `esp_hid`
  / NimBLE backend) so Windows / macOS / Linux drive a slideshow: previous / next slide,
  start show (F5 and macOS key combos), exit show (Esc), with an on-screen presentation timer.
- **Voice input** — a custom BLE GATT service (`0xA2B0`: CTRL / EVENT / AUDIO) streams on-device
  captured audio (IMA ADPCM) to a PC companion that runs speech recognition and returns transcripts.

The two features are kept from colliding by **per-page advertising identity isolation**: the PPT page
advertises `AI Passport` with the HID UUID + keyboard appearance (and its own public address), while
the voice page advertises `AI Passport Voice` on a derived random address with no HID UUID, so the host
does not mistake the voice link for a keyboard and grab the connection.

## Hardware / build

- Target: **ESP32-C3**, 8 MB flash, **no PSRAM**, ESP-IDF **v5.5.3**, LVGL UI on a 240×320 SPI display.
- Build & flash: see [`docs/development/build-and-test.md`](docs/development/build-and-test.md)
  (`tools/build.ps1` on Windows wraps the ESP-IDF 5.5.3 environment with retry).
- Repo conventions for contributors and AI agents: [`AGENTS.md`](AGENTS.md).

## Highlights of this branch

- BLE lifecycle owned centrally by `main/ble_prov.c` (single NimBLE stack shared by
  provisioning, voice and HID), with dual connection slots so the host's auto-reconnecting
  keyboard and the voice companion can stay connected at once.
- `components/passport_ppt` — HID keyboard via the official `esp_hid` component
  (hand-written NimBLE GATT HID is not recognized as a keyboard by Windows; see the porting notes).
- `components/passport_voice` — audio capture, ADPCM streaming and the `0xA2B0` GATT service.
- Electronic badge (name / school / etc.) with WiFi-based transfer, and BLE/hotspot provisioning.

## Status

Demo/development branch. Functional on-device for voice input and PPT control; not an official
release. The PC companion for voice input lives in a separate project.
