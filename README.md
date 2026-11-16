# ESP32-S3 K¯\\_(ツ)_/¯M over Tailscale

Firmware for the ESP32-S3 SuperMini (ESP32-S3FH4R2). The board plugs into the target PC's USB as a keyboard and mouse, joins the tailnet itself, and takes input from its own web UI in a browser or over UDP from the [esp32-kvm-ip](https://github.com/KMChris/esp32-kvm-ip) server. No drivers are needed on the target PC.

## Building

Requires ESP-IDF v5.3 (tested on v5.3.2). The component manager downloads the `esp_tinyusb` and `led_strip` dependencies on the first build.

SuperMini, 4 MB flash, with OTA (main variant):

```bash
idf.py -B build -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.supermini" set-target esp32s3 build
```

SuperMini without OTA (fallback, if the firmware outgrows 1920 KB):

```bash
idf.py -B build-noota -D SDKCONFIG=build-noota/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.supermini;sdkconfig.defaults.supermini_noota" set-target esp32s3 build
```

N16R8 (16 MB flash, 8 MB octal PSRAM), same code:

```bash
idf.py -B build-n16r8 -D SDKCONFIG=build-n16r8/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.n16r8" set-target esp32s3 build
```

Firmware size as of v0.1.0: about 986 KB, leaving 49% of the 1920 KB OTA slot free.

## Flashing over the cable

Native USB is taken by HID, so to flash over the cable, hold BOOT while plugging the board in:

```bash
idf.py -B build -p /dev/cu.usbmodemXXXX flash
```

Logs go to UART0: TX = GPIO43, RX = GPIO44, 115200 baud. Connect a USB-UART adapter. The pins can be changed in `menuconfig` → Component config → ESP System Settings → Channel for console output.

## First boot

1. An unconfigured board starts the `kvm-setup-XXXX` access point. The default password is `kvmsetup` (set in `menuconfig` → ESP32 KVM → Network); it is also printed to the UART log.
2. Open `http://192.168.4.1/` and set WiFi, the Tailscale auth key, the device name, the tailnet address of the controlling PC, and the UI password.
3. The board reboots, connects to WiFi and joins the tailnet. The UI is available at `http://<tailnet-address>/` and at the local address.

Holding BOOT for 5 seconds in normal mode erases the settings and node keys and returns the board to initial setup mode.

## LED (WS2812 on GPIO48)

| State | Indication |
|---|---|
| Initial setup | blue, blinking at 1 Hz |
| No WiFi | red, blinking fast |
| WiFi up, Tailscale down | yellow, blinking slowly |
| All connected | green |
| Input in progress | flickering cyan |
| BOOT held | blinking white |

For a board with a plain LED: `menuconfig` → ESP32 KVM → Board, disable WS2812 and set the GPIO.

## Firmware structure

| Module | What it does | Core |
|---|---|---|
| `usb_hid.c` | TinyUSB, ITF0 keyboard (boot protocol), ITF1 mouse and consumer, 1 ms polling | 1 |
| `kvm_input.c` | Input state, link watchdog, single source, ordering filter, counters | 1 |
| `kvm_rx.c` | UDP from the tailnet (MicroLink socket) and, if enabled, from LAN | 1 |
| `net_wifi.c` | STA without Modem Sleep, reconnects with up to 10 s backoff, setup AP | 0 |
| `ts_client.c` | MicroLink: registration, WireGuard, DISCO, DERP | 0 |
| `web_ui.c` | HTTP, REST, WebSocket, sessions, OTA | 0 |
| `app_config.c` | Settings in NVS, password stored as PBKDF2-SHA256 | — |
| `login_guard.c` | Failed-login lockout (1 min → 5 min → 15 min → 1 h), state kept in NVS | 0 |
| `status.c` | LED and BOOT button | 0 |

MicroLink lives in `components/microlink` (MIT, commit in `UPSTREAM_COMMIT`). Differences from upstream:

- the `ml_coord` and `ml_wg_mgr` tasks are moved to core 0 (`include/microlink_internal.h`) so WireGuard handshakes do not interfere with input on core 1;
- `wireguard_lwip` is moved out to `components/wireguard_lwip` so the build can find it;
- MicroLink's own web server is disabled (`CONFIG_ML_ENABLE_CONFIG_HTTPD=n`), and the H2 and JSON buffers are reduced to 256 KB via Kconfig.

## Protocol and input behavior

The esp32-kvm-ip UDP protocol is unchanged: 16 bytes, `magic 0xCAFE`, a 32-bit `sequence`, and an event type (mouse, keyboard, consumer). The browser sends the same 16-byte packets over WebSocket.

- **Packet order.** Each packet carries a `sequence` counter; the firmware drops old and duplicate packets the same way the original project does.
- **Mouse motion.** Mouse packets carry deltas, not state, so instead of a length-1 overwrite queue the firmware accumulates deltas until the next USB poll. No motion is lost.
- **Link watchdog.** Separate timeouts for the browser and for UDP, 200 ms by default. While anything is held, the browser repeats the state every 50 ms. The esp32-kvm-ip server only sends packets on changes, so with a 200 ms UDP watchdog a key held longer than that without other input will be released. If that is a problem, set the UDP watchdog to 0 in the KVM section.
- **Allowlist** applies to UDP (tailnet and LAN). The browser is protected by the UI password and is not restricted by the list.
- **Password lockout.** Every 5 consecutive wrong passwords (at login or password change) lock password checks for 1 min, then 5 min, 15 min and 1 h. The counter is global and stored in NVS, so a reboot restarts the current lockout instead of clearing it. A correct password resets it; so does the BOOT factory reset.
- **BIOS/UEFI.** The keyboard is a separate boot-protocol interface. The mouse also understands the boot protocol (3 bytes), but is not guaranteed to work in BIOS.

## Tested and untested

Tested: builds of all three variants on ESP-IDF v5.3.2, a host-side unit test of the `kvm_input` logic, and the page layout against a stub API.

Not tested on hardware: everything else, including USB enumeration on Windows, macOS and Linux, operation in BIOS, tailnet registration, incoming TCP connections to the web UI on the tailnet address, and latency and memory usage over 8 hours. These are the acceptance stages from the spec.
