# o-platform — all-in-one firmware for the FoloToy AI Passport

<p align="right">
  <strong>English</strong> · <a href="README.zh_CN.md">简体中文</a>
</p>

**Project: `o-platform`.** All-in-one firmware for the **FoloToy AI Passport** badge (ESP32-C3). It
currently ships two Bluetooth features — **PPT remote** (BLE HID keyboard) and **voice input**
(on-device capture → PC streaming ASR → typed transcript) — **coexisting on one shared stack**.

**Vision:** `o-platform` aims to be a **plugin-style platform** — so developers can conveniently
**port community / third-party features** in, re-tiering them into this project's layers and sharing
the same BLE / Wi-Fi / memory budget, instead of each shipping an incompatible fork. The
"Porting sources" and "Memory / stack engineering" sections below are written to lower that barrier.

---

## 移植参考 / Porting sources

`o-platform` is not written from scratch — it composes a base platform plus several ported
features, per our porting pipeline
([`docs/development/porting-pipeline.zh_CN.md`](docs/development/porting-pipeline.zh_CN.md)).
Donor code is **re-tiered into this project's layers, not copied verbatim**, and keeps an origin
comment in its file headers.

| Part | Upstream / donor | License | Landed as |
|---|---|---|---|
| **Base platform** | [`rvaim/ai-passport`](https://github.com/rvaim/ai-passport) (plugin platform: `.pap` packages, BLE install, `passport_core`/`ui`/`runtime`) | — | `o-platform/` baseline |
| **UI style reference** | [`FoloToy/ai-passport`](https://github.com/FoloToy/ai-passport) (official firmware, pixel-art language) | — | visual style only |
| **Wi-Fi provisioning** | **final: 小智's hotspot provisioning** — [`78/esp-wifi-connect`](https://github.com/78/esp-wifi-connect) (softAP + captive portal) | MIT | `components/passport_wifi_ap/` (the earlier BLE provisioning in `main/ble_prov.c` was reworked into hotspot) |
| **Voice input** | [`zhaohuaxiaoy/folo-ai-passport-voice`](https://github.com/zhaohuaxiaoy/folo-ai-passport-voice) | MIT | `components/passport_voice/` + PC `companion/` |
| **PPT remote** | [`YeatsLiao/ai-passport-ppt`](https://github.com/YeatsLiao/ai-passport-ppt) | MIT | `components/passport_ppt/` via official `esp_hid` |
| **Home avatar (animated sprite)** | [`WhiteMagic2014/ai-passport`](https://github.com/WhiteMagic2014/ai-passport) — its `pet` sprite engine, re-tiered & renamed `human_display` (`pet_*`→`human_*`) | — | `components/human_display/`; frames compiled into firmware, generated from LPC sheets via `tools/lpc2pet.py` + `tools/prep_pet.py` |

---

## Hardware & build

- **ESP32-C3** (single-core RISC-V, ~400 KB SRAM, **no PSRAM**), 8 MB flash; 240×320 SPI + LVGL;
  3 ADC buttons; ES8311 audio + I2S; CW2017 fuel gauge; flash/log over USB-Serial/JTAG.
- ESP-IDF **v5.5.3**. Windows: `tools/build.ps1` (E-drive ccache + file-lock retry):
  ```powershell
  .\tools\build.ps1          # build
  .\tools\build.ps1 -Flash   # build + flash COM6
  ```
  Details: [`docs/development/build-and-test.zh_CN.md`](docs/development/build-and-test.zh_CN.md).

---

## Features — how to use

**PPT remote (BLE HID keyboard):** open the **PPT 遥控** page → pair in OS Bluetooth settings →
UP = previous slide (←), DOWN = next slide (→), **OK short** = start slideshow (F5 + macOS combos)
and start the timer, **DOWN long** = exit (Esc) + reset timer.

**Voice input:** open the **语音输入** page → run the PC companion (`companion/`) → hold to speak;
the transcript is typed into the focused desktop window and shown on screen.

**Electronic badge:** home page shows badge fields + avatar; edit via the on-device transfer page
(`main/transfer_page.c`).

---

## Memory / stack engineering — making voice + PPT coexist

This is the heart of `o-platform`. The ESP32-C3 has no PSRAM and everything runs in a few tens of
KB of heap, yet provisioning, **voice** and **PPT/HID** all need Bluetooth at once, and Wi-Fi wants
the same memory. Four things make coexistence work:

### 1. One shared NimBLE stack
Provisioning, voice and HID run on a **single NimBLE stack** whose lifecycle is owned centrally by
`main/ble_prov.c`. Donor components must not start their own stack — two stacks would double the
controller/host footprint and blow the budget. (`ROLE_OBSERVER`/`HOST_BASED_PRIVACY` are disabled to
save RAM; `ROLE_CENTRAL` stays on because the peripheral must initiate the ATT MTU exchange the voice
audio needs.)

### 2. Identity isolation per page — so Windows treats voice and the keyboard separately
Windows decides "this is my keyboard" from **both** the advertising content (appearance + the HID
`0x1812` UUID) **and the BLE address**. So the two features are isolated on every axis:

| | PPT page | Voice page |
|---|---|---|
| Name | `AI Passport` | `AI Passport Voice` |
| Adv 16-bit UUIDs | `0xA2B0` + `0x1812` | `0xA2B0` only |
| Appearance | `0x03C1` (keyboard) | `0x0000` (generic) |
| Address | public | a **derived static-random address** (stable per boot) |

The random voice address is the crucial part: advertising a different address means Windows — which
bonded the public address as a keyboard — **does not recognize / hijack the voice link**, so the voice
companion gets a clean GATT connection. (Along the way: `ble_gap_conn_active()` was fixed — it returns
"am I a connecting master (0/1)", not a connection count — replaced with a self-maintained count;
`adv_restart` now stops before re-starting so the new identity/address actually takes effect.)

### 3. Two connection slots + re-pairing handled
`CONFIG_BT_NIMBLE_MAX_CONNECTIONS=2` (controller `BLE_MAX_ACT=6`) lets the host's auto-reconnecting
HID keyboard and the voice companion each hold their own slot. `gap_event_cb` handles
`BLE_GAP_EVENT_REPEAT_PAIRING` (delete the stale bond, return `RETRY`) so a re-pair after the host
drops its bond doesn't get silently discarded by NimBLE.

### 4. Wi-Fi is never resident + zero-heap buffers
- **Wi-Fi → heap for BLE.** Wi-Fi only comes up at boot for SNTP time-sync, then is torn down on
  sync/timeout (**~99 KB freed**); `show_voice` / `show_ppt` also `wifi_sta_stop()` before starting
  BLE. If Wi-Fi stayed resident, `NimBLE + esp_hid + the voice GATT service` would fail to start for
  want of heap → advertising fails → the desktop can't scan.
- **Zero-heap runtime.** The voice event downlink queue (4×512 B), the CTRL scratch (2 KB), the audio
  static ring, and the `event_worker` **task stack live in `.bss`** — because `nimble_port_init`
  (controller) already consumes ~44.7 KB of heap, and allocating another task stack at runtime starves
  `ble_hs_start` and triggers reboot loops. (The worker stack is sized 4096; 5120 was measured to break
  BLE start.)

---

## Companion tools (PC side)

`companion/` (ported from the voice donor): `relay.py` (BLE↔ASR relay; `语音中转-relay.bat` /
`语音助手-GUI.bat`), `probe.py`, `asr_client.py` (Volcano streaming ASR).

```bash
cd companion
python -m venv .venv && .venv/Scripts/pip install -r requirements.txt
# put your Volcano ASR key in config.local.json (shape: config.example.json)
```

> **Secrets:** the ASR key lives only in `companion/config.local.json`, which is **git-ignored**.
> Never commit real keys.

## Changing the on-screen avatar

The home-page avatar is an animated sprite **compiled into the firmware** (not uploaded at
runtime). The engine shows **one** character — the one registered in
`components/human_display/human/human_manifest.h`. Two ships with source PNGs (`humans/mage`,
`humans/cowboy`); pick which one appears:

```bash
python tools/prep_pet.py --src humans/cowboy     # rewrite the manifest to this character
powershell .\tools\build.ps1 -Flash              # rebuild + flash
```

To use your own: download a sprite sheet from the
[Universal LPC character generator](https://sanderfrenken.github.io/Universal-LPC-Spritesheet-Character-Generator/),
slice it, regenerate, flash:

```bash
python tools/lpc2pet.py <sheet>.png <name> --only stand,walk,walkfront
python tools/prep_pet.py --src humans/<name>
powershell .\tools\build.ps1 -Flash
```

Full walkthrough (naming contract, motion presets, pitfalls):
[`docs/development/human-slicing-guide.zh_CN.md`](docs/development/human-slicing-guide.zh_CN.md).

## Status

Development / demo branch. Voice input and PPT control verified on real hardware. Not an official release.
