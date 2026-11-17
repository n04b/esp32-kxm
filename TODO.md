# TODO

## 1. Limit failed logins, persisted in NVS (done)

Implemented in `main/login_guard.c`. Previously login was blocked for 60 s after 5 wrong passwords, with the counter only in RAM, so a reboot (or a watchdog crash) reset the protection.

- [x] Store in NVS (a separate key in the `kvm` namespace, not in the `cfg` blob): the number of consecutive failed attempts and the lockout level.
- [x] Escalating delay: 5 failures → 1 min, next 5 → 5 min, then 15 min, then 1 h (cap). A successful login resets everything.
- [x] Measure lockout time with `esp_timer` since boot; if the board reboots during a lockout, restart the delay at the current level (there is no real-time clock before SNTP, so the "remaining time" cannot be restored exactly; restarting the countdown is the safer choice).
- [x] Write to NVS only when the counter changes, to avoid flash wear (brute-forcing is throttled by the delay anyway).
- [x] Apply the same logic to password change (`old` in the password change handler, around line 553) (previously it had no limit).
- [x] Response while locked: `429` with the remaining time in seconds; show a countdown on the page.
- [x] Log the client address and lockout moment to the UART log.
- [x] The BOOT reset (5 s) must erase this counter too (done: the `lockout` key is in the `kvm` namespace, which `config_factory_reset` erases with `nvs_erase_all`).
- [x] Open question: one global counter or per IP. Global is simpler and more robust against brute-forcing from different tailnet addresses, but lets an attacker lock out the owner. Implemented as global.
- [ ] Test on hardware: lockout and countdown in the browser, lockout surviving a reboot, BOOT reset clearing it.

## 2. MQTT support

Goal: integration with Home Assistant / Node-RED and control without a browser.

- [ ] `esp-mqtt` client from ESP-IDF, task on core 0 so it does not interfere with input on core 1.
- [ ] Settings in the web UI: broker address (via tailnet or LAN), port, username/password, TLS on/off, topic prefix (default `kvm/<device name>`).
- [ ] Status publishing (retain): `.../status` (online/offline via LWT), WiFi RSSI, Tailscale state, uptime, free memory, firmware version, active input source.
- [ ] Commands (subscribed):
  - [ ] `.../cmd/text`: type a string (US layout, with a delay between characters);
  - [ ] `.../cmd/key`: a key combination (`ctrl+alt+del`, `win+l`);
  - [ ] `.../cmd/consumer`: volume, play/pause, mute;
  - [ ] `.../cmd/wake`: wake the PC (mouse move or key press).
- [ ] MQTT input goes through `kvm_input` as a separate source with its own watchdog, so it does not conflict with the browser and UDP.
- [ ] Home Assistant MQTT Discovery: buttons for common combinations, status sensors.
- [ ] Security: MQTT is off by default; without TLS, only for a broker inside the tailnet. Store the broker password in NVS alongside the other keys.
- [ ] Check size: esp-mqtt + TLS add ~40–60 KB; the 4 MB OTA slot has headroom (49%), and so does the no-OTA variant.

## 3. Bluetooth and a mobile keyboard app

Goal: control the PC from a nearby phone without WiFi or the tailnet.

Firmware:

- [ ] ESP32-S3 supports BLE only (no Classic). Use the NimBLE stack (lighter than Bluedroid in RAM and flash).
- [ ] Custom GATT service: a write characteristic (write without response) for the same 16-byte packets used over UDP/WebSocket (`magic 0xCAFE`, `sequence`), and a notify characteristic for status.
- [ ] BLE input is another source in `kvm_input` with its own watchdog.
- [ ] Pairing only with confirmation: LE Secure Connections + passkey, with the passkey shown in the web UI / log; pairing mode is enabled from the UI or with a short BOOT press, not always on.
- [ ] Store bonds in NVS; list and remove paired devices in the web UI.
- [ ] WiFi coexistence: BLE and WiFi share one radio, check the impact on tailnet latency. Possibly make BLE optional in `menuconfig` and disable it by default in the 4 MB no-OTA variant.
- [ ] Estimate size: NimBLE is about 100–150 KB flash and ~40 KB RAM.

Mobile app:

- [ ] Platforms: iOS and Android. Candidates: Flutter (`flutter_blue_plus`) or React Native (`react-native-ble-plx`), or native SwiftUI + Kotlin.
- [ ] Screens: connect/pair, touchpad (motion, taps, two-finger scroll, drag), a full keyboard with modifiers and F-keys, media keys, macros (custom combinations and strings).
- [ ] Transports: BLE nearby, plus WebSocket over the tailnet (the same protocol as the browser) so the app also works remotely.
- [ ] Minimal latency: send mouse motion as packets with accumulated deltas, as in the browser.
- [ ] Separate repository or an `app/` directory: to decide.

## 4. Preset settings before flashing (`secrets.defaults`, like `.env`)

Goal: set WiFi, Tailscale and the UI password at build time so a freshly flashed board goes straight to normal mode, skipping the `kvm-setup-XXXX` access point.

- [ ] New Kconfig menu "Preset (first boot)" in `main/Kconfig.projbuild`, all empty by default:
  - [ ] `KVM_PRESET_WIFI_SSID`, `KVM_PRESET_WIFI_PASS`;
  - [ ] `KVM_PRESET_TS_AUTH_KEY`, `KVM_PRESET_TS_CTRL_HOST`, `KVM_PRESET_HOSTNAME`;
  - [ ] `KVM_PRESET_UI_PASSWORD`;
  - [ ] `KVM_PRESET_ALLOW`: comma-separated list of allowed sender IPs;
  - [ ] optionally `KVM_PRESET_UDP_PORT`, `KVM_PRESET_LAN_RX`, `KVM_PRESET_WEB_LAN`, `KVM_PRESET_WEB_TS`.
- [ ] `secrets.defaults` in the project root with these `CONFIG_KVM_PRESET_*` values; add it to `.gitignore`. Commit a `secrets.defaults.example` with placeholders.
- [ ] Build with `SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.supermini;secrets.defaults"`; document in README. Note: values land in the generated `sdkconfig` (already gitignored) and in the build folder.
- [ ] In `config_init()`: if the board is not provisioned and at least the SSID and UI password are preset, fill the settings, hash the password (PBKDF2, never store it in clear), parse the allowlist, set `provisioned`, write to NVS, and log that presets were applied (without secret values).
- [ ] Validate presets with the same rules as the web UI (WiFi password 8–64 or empty, UI password ≥ 6, hostname characters, valid IPs); on error, log it and fall back to the setup access point.
- [ ] After the BOOT factory reset, decide whether presets are applied again. Proposal: yes by default (handy for your own boards), with `KVM_PRESET_REAPPLY_AFTER_RESET` to disable it.
- [ ] Security note in README: preset secrets are stored in clear text inside the firmware `.bin`, including OTA images; do not share such a build. Without flash encryption, the NVS settings are also readable from flash.
- [ ] Optional: a build-time warning when a preset build is made, so it is not mistaken for a shareable image.
