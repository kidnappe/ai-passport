# AI Passport — Voice Input + PPT Remote

<p align="right">
  <strong>English</strong> · <a href="README.zh_CN.md">简体中文</a>
</p>

Firmware demo branch for the **FoloToy AI Passport** badge (ESP32-C3). On top of the base
platform it ships two Bluetooth features that coexist on one shared stack:

- **PPT remote** — the badge presents as a Bluetooth HID **keyboard**.
- **Voice input** — on-device capture streams audio over a custom BLE service to a PC
  companion that runs streaming ASR and types the transcript back into the desktop.

---

## What this is built on (base + porting sources)

This project is not written from scratch — it is a fork/derivative that composes a base
platform plus several ported features. Per our porting pipeline
([`docs/development/porting-pipeline.zh_CN.md`](docs/development/porting-pipeline.zh_CN.md)):

| Part | Upstream / donor | License | Landed as |
|---|---|---|---|
| **Base platform** | [`rvaim/ai-passport`](https://github.com/rvaim/ai-passport) (plugin platform: `.pap` packages, BLE install, `passport_core`/`ui`/`runtime`) | — | the `o-platform/` baseline |
| **UI style reference** | [`FoloToy/ai-passport`](https://github.com/FoloToy/ai-passport) (official firmware, pixel-art language) | — | visual style only, not code |
| **Wi-Fi provisioning** | [`killhello/ai-pass-port-wifi`](https://github.com/killhello/ai-pass-port-wifi) | — | rewritten in-house: `main/ble_prov.c` + `components/passport_wifi_ap/` |
| **Voice input** | [`zhaohuaxiaoy/folo-ai-passport-voice`](https://github.com/zhaohuaxiaoy/folo-ai-passport-voice) | MIT | re-tiered into `components/passport_voice/` + the PC `companion/` |
| **PPT remote** | [`YeatsLiao/ai-passport-ppt`](https://github.com/YeatsLiao/ai-passport-ppt) | MIT | `components/passport_ppt/` via the official `esp_hid` component |

Ported code keeps an origin comment in its file header (e.g. `voice_ble.c`,
`components/passport_ppt/src/passport_ppt.c`).

---

## Hardware

- **ESP32-C3** (RISC-V, single core, ~400 KB SRAM, **no PSRAM**), 8 MB flash.
- 240×320 SPI display (LVGL), 3 ADC buttons (UP / DOWN / OK), ES8311 audio codec + I2S,
  CW2017 fuel gauge, USB-Serial/JTAG for flashing/logs.

## Build & flash

ESP-IDF **v5.5.3**. See [`docs/development/build-and-test.md`](docs/development/build-and-test.md).
On Windows `tools/build.ps1` wraps the toolchain (E-drive ccache + Defender file-lock retry):

```powershell
.\tools\build.ps1          # incremental build
.\tools\build.ps1 -Flash   # build then flash COM6
```

---

## Features — how to use

### 1. Electronic badge
Home page shows the badge fields (nickname / school / major / student id) and an avatar.
Edit them from the PC with the transfer page on-device (`main/transfer_page.c` starts a
small HTTP server; upload from the companion transfer tool). Changes apply on returning home.

### 2. PPT remote (Bluetooth HID keyboard)
1. Open the **PPT 遥控** page (this starts BLE advertising `AI Passport` with keyboard appearance).
2. Pair it in your OS Bluetooth settings → it becomes a normal keyboard (Windows / macOS / Linux).
3. Buttons:
   - **UP** — previous slide (←)
   - **DOWN** — next slide (→)
   - **OK (short)** — start slideshow (F5; also macOS combos) + start the presentation timer
   - **DOWN (long)** — exit slideshow (Esc) + reset the timer

### 3. Voice input
1. Open the **语音输入** page (advertising `AI Passport Voice` on its own random address,
   so the desktop keyboard link is not grabbed).
2. Run the PC companion (see below), hold the PTT key and speak; the transcript is typed
   into whatever is focused and echoed on-screen.

---

## Companion tools (PC side)

The voice companion lives in **[`companion/`](companion/)** (ported from
`zhaohuaxiaoy/folo-ai-passport-voice`):

- `relay.py` — the BLE↔ASR relay; `语音中转-relay.bat` / `语音助手-GUI.bat` launch it.
- `probe.py` — quick scan/connect helper.
- `asr_client.py` — streaming ASR client (Volcano/火山 engine).

Setup:
```bash
cd companion
python -m venv .venv && .venv/Scripts/pip install -r requirements.txt
# put your Volcano ASR key in config.local.json (see config.example.json)
```

> **Secrets:** the ASR API key goes in `companion/config.local.json`, which is **git-ignored**.
> Never commit real keys — `config.example.json` shows the shape.

---

## Memory / stack engineering for feature coexistence

ESP32-C3 has no PSRAM and the whole thing runs in a few tens of KB of heap. To keep
provisioning + voice + HID working at the same time:

- **BLE and Wi-Fi contend for the same heap → Wi-Fi is not resident.** At boot Wi-Fi only
  comes up for SNTP time-sync and is torn down as soon as sync completes or times out
  (~99 KB freed). `show_voice` / `show_ppt` also stop Wi-Fi before starting the NimBLE stack.
- **One shared NimBLE stack.** Provisioning, voice and HID all run on a single stack whose
  lifecycle is owned centrally by `main/ble_prov.c` (donor code must not start a second one).
- **Zero-heap buffers.** The voice event downlink queue (4×512 B), the CTRL scratch
  (2 KB), the audio static ring, and the `event_worker` task stack are all **static (.bss)**,
  because `nimble_port_init` (controller) already consumes ~44.7 KB of heap — allocating
  those at runtime would starve BLE start and cause reboot loops.
- **Dual connection slots.** `CONFIG_BT_NIMBLE_MAX_CONNECTIONS=2` (controller
  `BLE_MAX_ACT=6`) so the host's auto-reconnecting HID keyboard and the voice companion can
  each hold a slot without stealing the other.

## Status

Development / demo branch. Functional on-device for voice input and PPT control.
Not an official release.
