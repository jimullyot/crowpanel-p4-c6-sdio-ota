# CrowPanel 7" ESP32-P4: Upgrade C6 Co-processor Firmware via SDIO

Standalone ESP-IDF application that upgrades the ESP32-C6 co-processor on [Elecrow CrowPanel 7" ESP32-P4](https://www.elecrow.com/crowpanel-advanced-7-0-inch-esp32-p4-display.html) boards to esp\_hosted v2.12.11 — over the existing SDIO bus, without soldering. Factory firmware is v2.3.0 on V1.0 panels and v2.12.3 on the V1.1 panels currently being sold; both are upgraded.

> **Why v2.12.11?** It is the ESP-Hosted *host* version baked into Arduino ESP32 core 3.3.11. A slave older than the host is the version mismatch the driver warns about on every boot, so this target moves whenever that core does. Check the core you build against: `ESP_HOSTED_VERSION_*` in `esp_hosted_host_fw_ver.h`.

## The Problem

The CrowPanel 7" ships with esp\_hosted **v2.3.0** on the ESP32-C6 WiFi co-processor. This version has SDIO transport bugs that cause WiFi to fail after about four minutes. Stable operation requires SDIO transport failure detection and automatic reinitialization, available in **v2.9.4 or later** on both host and slave.

USB updates reach the host (ESP32-P4) easily. The C6 has no exposed UART or USB port — its only external data connection is the SDIO bus shared with the P4. OTA through frameworks that start WiFi fails because WiFi triggers the transport failure before OTA completes.

**This tool runs OTA without WiFi.** A dedicated IDF application on the P4 initializes the SDIO transport, transfers the new C6 firmware from a LittleFS partition, and activates it. WiFi never starts, so the SDIO bus stays stable for the full transfer.

Two things worth knowing before you start, both of which show up on newer boards:

- **Not every board still ships with v2.3.0.** A V1.1 panel bought in 2026 arrived with C6 v2.12.3. That is still older than the v2.12.11 here, so it does get upgraded — but only just, and the jump is small enough that the win is version parity with the host rather than any fix. (While this tool targeted v2.11.6, that same board was correctly *skipped* as already newer.) The app compares versions and refuses to downgrade, so "it did nothing" is a valid outcome whenever the C6 is at or past the target.
- **V1.1 panels are wired differently.** The SDIO data lines and the C6 reset line moved, so a build hard-wired for V1.0 cannot reach the C6 at all. The app works out which revision it is on and remembers it — see [Board revisions](#board-revisions).

## How It Works

```
┌───────────────────┐    SDIO OTA RPC    ┌──────────────────┐
│    ESP32-P4       │───────────────────▶│    ESP32-C6      │
│   (this app)      │   1-bit, 10 MHz    │  (co-processor)  │
│                   │                    │                  │
│  LittleFS part.   │   1.5 KB chunks    │  Writes to       │
│  holds C6 binary  │───────────────────▶│  inactive OTA    │
│  (~1.1 MB)        │   ~15 seconds      │  partition       │
└───────────────────┘                    └──────────────────┘
```

1. Flash this app to the P4 (replaces your normal firmware temporarily)
2. App initializes SDIO transport — **no WiFi** (avoids bus contention with v2.3.0)
3. Reads C6 firmware binary from LittleFS partition on P4 flash
4. Transfers binary to C6 via esp\_hosted OTA RPC in 1.5 KB chunks
5. Activates new firmware on C6; C6 reboots into v2.12.11
6. Flash your normal firmware back to the P4

The C6 upgrade persists across P4 reflashes — the C6 has its own flash.

## Tested On

| Component | Version |
|-----------|---------|
| Board | CrowPanel Advanced 7" ESP32-P4 HMI (1024x600), PCB V1.0 and V1.1 |
| Factory C6 firmware | esp\_hosted v2.3.0 (V1.0) / v2.12.3 (V1.1, 2026 boards) |
| Target C6 firmware | esp\_hosted v2.12.11 |
| ESP-IDF | v5.5.1 (esp\_hosted v2.12.11 requires >= 5.3) |
| esp\_hosted component | v2.12.11 ([ESP Component Registry](https://components.espressif.com/components/espressif/esp_hosted/versions/2.12.11)) |

**V1.0:** Succeeded on first attempt, 2026-02-16. Zero solder. C6 upgraded from v2.3.0 to the then-target v2.11.6 in about 15 seconds.

**V1.1:** Verified 2026-08-01. The app worked out the revision on its own and connected in 1.9 s. That board shipped with C6 v2.12.3, which was newer than the v2.11.6 targeted at the time, so it reported the version and exited without writing anything. It cost a reboot to get there then, because V1.0 was tried first; V1.1 now leads, so it connects on the first attempt. See [Board revisions](#board-revisions).

**Not yet re-verified against the v2.12.11 target.** The V1.1 runs above ended in the skip path, so the transfer itself has only ever been exercised from v2.3.0. A v2.12.3 → v2.12.11 upgrade on V1.1 is the untested case.

## Prerequisites

### Hardware

- **CrowPanel 7" ESP32-P4** (PCB V1.0 and V1.1 both tested; the app detects which) connected via USB-C cable to the **upper** USB-C port (CH341 UART bridge)
- **CH341 USB-UART driver** — Linux: built into kernel. macOS: [download from WCH](http://www.wch-ic.com/downloads/CH341SER_MAC_ZIP.html) if `ls /dev/cu.wchusbserial*` shows nothing when the board is connected.

### Software

- **ESP-IDF v5.5.1** — [installation guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/get-started/index.html). Other v5.5.x versions may work but are untested. The installer handles Python, cmake, and esptool.py.
- **just** task runner — `brew install just` (macOS) or [other install methods](https://github.com/casey/just#installation)
- **curl** — for downloading the pre-built firmware binary. Standard on macOS and most Linux distributions.
- **git** — for ESP-IDF component manager and (optionally) building firmware from source.
- **Unix shell** — the justfile requires Bash and standard utilities (find, ls, wc). macOS and Linux work out of the box. Windows users need WSL.

> **Before every session:** source the ESP-IDF environment in your terminal:
> ```bash
> source $IDF_PATH/export.sh
> ```

### Firmware

- **C6 firmware binary** — `network_adapter.bin` for ESP32-C6, esp\_hosted v2.12.11 (see below)

### Obtaining the C6 Firmware Binary

**Option 1: Download pre-built binary (recommended)**

```bash
curl -L -o components/ota_littlefs/slave_fw_bin/network_adapter.bin \
    https://espressif.github.io/arduino-esp32/hosted/esp32c6-v2.12.11.bin
```

Expected SHA256: `5b05e1530e122881ffeeea3ce2e930963cef6fcc79f930cf0994bf9cb9d709cb`

Verify after download:

```bash
shasum -a 256 components/ota_littlefs/slave_fw_bin/network_adapter.bin
```

**Option 2: Build from source**

```bash
git clone https://github.com/espressif/esp-hosted-mcu.git
cd esp-hosted-mcu/slave
idf.py set-target esp32c6
idf.py build
cp build/network_adapter.bin /path/to/c6-firmware-upgrade/components/ota_littlefs/slave_fw_bin/
```

**Verify the binary (either option):**

```bash
esptool image-info components/ota_littlefs/slave_fw_bin/network_adapter.bin
```

Check:
- `Chip ID: 13 (ESP32-C6)` — correct target
- `Flash mode: DIO` — **avoid QIO** (QIO causes OTA failures on the C6)
- `App version:` — should show `2.12.11` (or whichever version you built)

## Quick Start

A `justfile` automates the full workflow. Verify your environment first:

```bash
just status                # verify serial port, IDF, firmware binary
```

### 1. Place the C6 firmware binary

```bash
# If you haven't downloaded it yet:
curl -L -o components/ota_littlefs/slave_fw_bin/network_adapter.bin \
    https://espressif.github.io/arduino-esp32/hosted/esp32c6-v2.12.11.bin

just check-fw              # confirm binary found
just verify-fw             # inspect chip ID, flash mode, version
```

The build system automatically picks up any `.bin` file in this directory and packages it into the LittleFS partition image. Place **one** `.bin` file only.

### 2. Build

```bash
just set-target            # downloads managed components (esp_hosted v2.12.11, littlefs)
just build                 # checks firmware binary, then builds
```

#### Two builds, because the C6 can be in two states

The SDIO receive mode is fixed when the host is compiled, and the two states a C6 can be in answer to different ones:

| C6 state | Answers in | Build with |
|---|---|---|
| Factory (esp\_hosted v2.3.0, predates the option) | packet | `just build` |
| Already upgraded (any 2.12.x — the slave defaults to streaming) | streaming | `just build-streaming` |

Streaming is actually esp\_hosted's default for the host; `sdkconfig.defaults` overrides it to packet so a board straight out of its box works with a stock build. `sdkconfig.streaming` is an overlay that puts it back.

**Always try `just flash` first.** The two mismatches are not equally diagnosable, and that asymmetry is the whole reason for the ordering:

- **Packet host, streaming slave** — aborts in `process_init_event`, loudly, and *after* the slave has identified itself. The next boot recognises it and prints `C6 NOT UPDATED - this build cannot reach this C6`, which is the signal to run `just flash-streaming`.
- **Streaming host, factory slave** — goes quiet. Indistinguishable from the wrong data-line map, so it looks like a wiring fault and sends you probing pins that were never wrong.

The startup banner names which build is running, because they differ only in a config value:

```
Host SDIO RX: PACKET  (expects a factory C6)
```

What the abort proves is only that the C6 **is not factory** — never which version it carries, because `phase_query_version()` runs after a handshake that build cannot complete. A C6 on 2.12.3 aborts exactly like one on 2.12.11. Completing the handshake with the streaming build is what makes the version readable, which is why that build is the answer rather than a workaround.

> **There is no USB or UART route to the C6 on a stock board** (see [The Problem](#the-problem)), so "flash the C6 directly with esptool" is not an alternative to this — it needs [soldered wires](#uart-flash-requires-soldering-3-wires). The streaming build is the supported way to reach an already-upgraded C6.

> **Note:** The `ota_littlefs` component and `Kconfig.projbuild` are included in this repository. Do **not** run `just fix-components` — it overwrites these files with upstream defaults that lack project-specific configuration. The `fix-components` recipe exists only for development against a fresh esp\_hosted checkout without this repository's tracked components.

### 3. Backup (optional)

```bash
just backup                # reads 16 MB P4 flash to timestamped file
```

The backup is best-effort (esptool `read_flash` can be unreliable on CrowPanel). If you can rebuild your P4 firmware from source, you can skip this.

### 4. Flash and monitor

```bash
just flash                 # builds (if needed), then flashes to P4 and opens serial monitor
```

This **overwrites your current P4 firmware.** The OTA app runs once, upgrades the C6, then halts.

Alternatively, use `just flash-only` to flash without opening the monitor.

### 5. Read the serial output

Watch for these log tags:

| Tag | Meaning |
|-----|---------|
| `[PHASE]` | Progress through the five phases (0–5) |
| `[PASS]` | Success — phase completed |
| `[FAIL]` | Failure — includes error code and diagnostic hints |
| `[DIAG]` | Timing data, versions, transfer sizes |

**Success looks like this** (upgrading from the factory **v2.3.0** firmware):

```
I c6-sdio-ota: [PASS] Connected to C6 slave in 1884ms
I c6-sdio-ota: [DIAG] C6 firmware: 2.3.0
I c6-sdio-ota: [PASS] OTA transfer completed in 13085ms
I c6-sdio-ota: [DIAG] Immediate-activate RPC not supported by the running C6 firmware (expected on v2.3.0)
I c6-sdio-ota: [PASS] New image already marked bootable during transfer — it starts after a power cycle
I c6-sdio-ota: [PASS] New firmware is staged and bootable — it takes effect after the power cycle below
W c6-sdio-ota:   RESULT: OTA TRANSFER SUCCEEDED
W c6-sdio-ota:   C6 UPDATE COMPLETE — firmware upgraded successfully.
```

> **This is the normal, successful path.** The factory v2.3.0 firmware does not
> support the immediate-activate (in-place reboot) RPC, so the upgrade does not
> take effect until you power-cycle the board. The version query in this run
> still reports **2.3.0** — that is expected, *not* a failure. Power-cycle, and
> on the next boot the C6 reports **2.12.11**.

If the C6 firmware already supports immediate activation, you'll instead see
`[PASS] Activate succeeded` followed by
`[PASS] *** C6 UPGRADED TO v2.12.11 — SUCCESS ***`, and no power cycle is needed.
A V1.1 board coming from v2.12.3 is the likely candidate for this path rather
than the staged one, but that has not been observed yet — either outcome is fine.

If you see `[PASS] C6 is at v2.12.11, at or past the v2.12.11 this tool carries — OTA not needed`, it was already upgraded. Nothing more to do.

### 6. Restore your normal firmware

Flash your regular firmware back to the P4. The C6 retains v2.12.11 independently.

If you use **ESPHome**: reflash your ESPHome firmware via USB. No configuration changes needed — ESPHome detects the new C6 version automatically during boot.

If you use **plain ESP-IDF**: rebuild and flash your application with `idf.py flash`.

If you need to restore **Elecrow factory firmware**: download it from the [Elecrow CrowPanel GitHub repo](https://github.com/Elecrow-RD/CrowPanel-Advanced-7inch-ESP32-P4-HMI-AI-Display-1024x600-IPS-Touch-Screen) and flash with esptool.

### 7. Verify the upgrade

After your normal firmware boots, check the serial output for the esp\_hosted version handshake. Look for a line containing the C6 firmware version:

```
I esp_hosted: Slave chip_id [13] ESP32-C6
I esp_hosted: Slave FW version [2.12.11]
```

If your host firmware is built with Arduino ESP32 core 3.3.11, its host version is also 2.12.11, so the boot log should report the versions matching rather than warning that one side is newer.

**WiFi stability test:** Connect to your network and confirm WiFi remains connected beyond four minutes. The v2.3.0 failure mode was a hard disconnect at ~4 minutes with no recovery. With v2.12.11, WiFi should remain connected indefinitely.

## Build Troubleshooting

### `ota_littlefs.h` not found

The `ota_littlefs` component lives inside the esp\_hosted example directory, not as a standalone component. Run:

```bash
just fix-components
just build
```

Or manually copy from the downloaded managed component:

```bash
HOSTED=$(find managed_components -name "espressif__esp_hosted" -type d | head -1)
cp -r "$HOSTED/examples/host_performs_slave_ota/components/ota_littlefs" components/
cp "$HOSTED/examples/host_performs_slave_ota/main/Kconfig.projbuild" main/
idf.py build
```

### `esp_app_desc.h` not found

Add `esp_app_format` to `PRIV_REQUIRES` in `main/CMakeLists.txt`:

```cmake
PRIV_REQUIRES esp_app_format ota_littlefs
```

### API function not found

The esp\_hosted public API changes between versions. After `idf.py set-target`, verify the expected functions exist:

```bash
grep -r "esp_hosted_connect_to_slave\|esp_hosted_slave_ota_begin" managed_components/
```

If a function is missing, check the actual header names and update `main.c`.

## Interpreting Failures

| Serial output | Cause | Next step |
|---|---|---|
| `[FAIL] esp_hosted_init` | SDIO transport broken at init | Verify serial port, check that the C6 EN pin is reachable (GPIO32 on V1.0, GPIO54 on V1.1) |
| `[FAIL] esp_hosted_connect_to_slave`, once | Wrong pin map for this board revision | None — the app reboots and tries the other revision by itself. V1.1 is tried first, so on a V1.0 board watch for `Trying CrowPanel V1.0 wiring` on the next boot |
| `C6 NOT UPDATED - this build cannot reach this C6` (after one panic and backtrace) | C6 is not factory, so it answers in streaming mode and the packet build aborts by design | Run `just flash-streaming`. This proves nothing about *which* version it carries — only that it isn't factory |
| `[WARN] Tried every known wiring` | C6 answered on neither revision | Check C6 power (P37 test pad should read 3.3V) — this is the chip or the board, not the pin map. If this is the **streaming** build, a factory C6 produces the same silence: try `just flash` instead |
| `[FAIL] OTA transfer failed` + duration < 5s | Transport or OTA begin failed | SDIO fundamentally broken — skip to UART flash (see [Alternative Paths](#alternative-flash-paths)) |
| `[FAIL] OTA transfer failed` + duration 5–30s | Transport died mid-transfer | Try lower SDIO clock or smaller chunks (see [Tuning](#tuning-parameters)) |
| `[FAIL] OTA transfer failed` + duration > 30s | Timeout | C6 hung — power-cycle and retry |
| `[DIAG] Immediate-activate RPC not supported` | Expected on factory v2.3.0 — the image was already marked bootable by the transfer | None — power-cycle; the C6 boots the new firmware |
| `[WARN] Immediate-activate returned ...` | Unexpected activate code, but the image is still staged | Power-cycle and verify; if the new version doesn't appear, retry from scratch |

## Tuning Parameters

If OTA fails mid-transfer, try these adjustments one at a time:

| Parameter | Default | Try | File |
|-----------|---------|-----|------|
| SDIO clock | 10 MHz | 5 MHz | `sdkconfig.defaults` → `CONFIG_ESP_HOSTED_SDIO_CLOCK_FREQ_KHZ=5000` |
| Chunk size | 1500 B | 1400, 512, 256 | `components/ota_littlefs/ota_littlefs.c` → `CHUNK_SIZE` |
| Bus width | 1-bit | 4-bit | `sdkconfig.defaults` → remove `CONFIG_ESP_HOSTED_SDIO_1_BIT_BUS`, set `BUS_WIDTH=4` |

Rebuild with `idf.py build` after each change.

## Alternative Flash Paths

If SDIO OTA fails entirely, the C6 can still be flashed through test pads on the PCB. See [HARDWARE.md](HARDWARE.md) for test pad locations and wiring.

### UART flash (requires soldering 3 wires)

| Test Pad | C6 Pin | Connect to |
|----------|--------|------------|
| P25 | GPIO16 (TX) | USB-UART adapter RX |
| P21 | GPIO17 (RX) | USB-UART adapter TX |
| P36 | GPIO9 (BOOT) | GND during reset |

Hold P36 to GND, reset the C6 (power-cycle or toggle GPIO32), then flash:

```bash
esptool.py --chip esp32c6 --port /dev/ttyUSB0 --baud 115200 \
    write_flash --flash_mode dio \
    0x0     bootloader.bin \
    0x8000  partition-table.bin \
    0x10000 network_adapter.bin
```

> **Note:** The ESP32-C6 bootloader address is **0x0** (RISC-V architecture), not 0x1000 as on Xtensa-based ESP32 chips. Using the wrong address bricks the bootloader — repeat the UART flash to recover.

The UART approach requires `bootloader.bin` and `partition-table.bin` in addition to the app binary. Build them from source:

```bash
git clone https://github.com/espressif/esp-hosted-mcu.git
cd esp-hosted-mcu/slave
idf.py set-target esp32c6
idf.py build
# Binaries are at:
#   build/bootloader/bootloader.bin
#   build/partition_table/partition-table.bin
#   build/network_adapter.bin
```

## Safety

**P4 (main processor):** You cannot brick it. The CH341 USB-UART bridge connects permanently. `esptool.py` can always reflash the P4.

**C6 (co-processor):** OTA writes to the **inactive** partition slot only. A failed or interrupted transfer leaves the active slot (current firmware) intact. The only bricking scenario is a binary that passes validation but crashes at runtime — and even then, [UART flash](#alternative-flash-paths) recovers it.

Running this tool a second time is safe. If the C6 already runs v2.12.11 (or anything newer), the app detects it and halts without writing anything.

## Hardware Reference

| Parameter | Value |
|-----------|-------|
| SDIO bus | 1-bit mode, D0 = GPIO14 (V1.0) or GPIO17 (V1.1) |
| SDIO clock | 10 MHz (configurable) |
| SDIO CLK | P4 GPIO18 → C6 IO19 |
| SDIO CMD | P4 GPIO19 → C6 IO18 |
| C6 reset | P4 GPIO32 (V1.0) or GPIO54 (V1.1) → C6 EN (active high) |
| C6 flash | 4 MB, dual OTA partitions (1920 KB each) |
| Serial port | CH341 UART, upper USB-C |

Series resistors with pullups to 3.3V terminate all six SDIO lines (D0–D3, CLK, CMD). Four-bit SDIO mode works electrically but remains untested for OTA.

### Board revisions

V1.1 panels reversed the SDIO data lines (IO14,15,16,17 became IO17,16,15,14) and moved the C6 reset line, so a build wired for V1.0 cannot talk to a V1.1 board. Nothing readable on the board says which revision it is; the only marking is the silkscreen (`7.0V1.0` / `7.0V1.1`).

The app works this out for itself. It tries one pin map, and because a wrong map makes the transport reboot the host rather than return an error, it writes the guess to NVS *before* trying it and clears it only once the C6 answers — so a reboot is the evidence the guess was wrong, and the next boot tries the other map. Once a map works it is remembered, and later runs go straight to it. A board that answers on neither says so plainly instead of flipping forever.

**V1.1 is tried first**, since that is what is currently being sold. Whichever revision goes first is the one that never pays the reboot, so V1.0 boards now take the single extra reset that V1.1 boards used to.

The catch this hides is that a wrong map does not look like a wiring fault: the SDIO card enumerates and reports its block sizes over CMD52 on the CMD line alone, and only the CMD53 data transfers ever touch the data pins. What you see is a healthy bus that goes quiet — indistinguishable from a C6 that is simply not ready.

For the full hardware analysis — pin map, test pad locations, connector inventory — see [HARDWARE.md](HARDWARE.md).

## P4 Flash Layout

This project uses a simplified partition table on the P4 (the OTA app does not need dual OTA):

```
Offset     Name         Size           Purpose
0x009000   nvs           16 KB         Non-volatile storage
0x00D000   otadata        8 KB         OTA state
0x00F000   phy_init       4 KB         PHY calibration
0x010000   ota_0          3 MiB        This application
0x310000   storage      1920 KB        LittleFS — holds the C6 firmware binary (~1.1 MB)
```

The build system automatically generates the LittleFS image from `components/ota_littlefs/slave_fw_bin/` and flashes it to the `storage` partition.

## Project Structure

```
├── justfile                            Task runner (just build, just flash, ...)
├── CMakeLists.txt                      Root build definition
├── sdkconfig.defaults                  CrowPanel SDIO config, OTA settings
├── partitions.csv                      P4 flash layout (app + LittleFS)
├── HARDWARE.md                         C6 pin map, test pads, SDIO bus details
├── main/
│   ├── main.c                          5-phase OTA workflow
│   ├── idf_component.yml               esp_hosted ==2.12.11, littlefs
│   └── Kconfig.projbuild               OTA method selection menu
└── components/
    ├── ota_littlefs/
    │   ├── ota_littlefs.c              LittleFS mount, binary validation, chunked transfer
    │   ├── ota_littlefs.h              Public API
    │   └── slave_fw_bin/               Place C6 .bin file here before build
    └── common_ota_scripts/
        ├── find_newest_firmware.cmake  Selects newest .bin at build time
        └── flash_selected_firmware.cmake
```

## Platform Notes

### macOS

Serial port auto-detection (`/dev/cu.wchusbserial*`) works out of the box.

### Linux

The justfile auto-detects macOS serial ports only. Override the port manually:

```bash
just flash serial_port=/dev/ttyUSB0
```

Or edit line 8 of the `justfile` to match your system:

```
serial_port := `ls /dev/ttyUSB* 2>/dev/null | head -1 || echo ""`
```

CH341 drivers are built into most Linux kernels. The device appears as `/dev/ttyUSB0`.

## Known Limitations

- **Tested on PCB V1.0 and V1.1.** Other CrowPanel revisions may have different GPIO assignments or C6 module variants.
- **Target firmware: v2.12.11.** Other esp\_hosted versions are untested. The download URL above is specific to v2.12.11, and the transfer itself has so far only been exercised against the v2.11.6 target (from a v2.3.0 board).
- **4-bit SDIO mode untested.** The wiring supports it, but OTA reliability in 4-bit mode is unverified.
- **Version query may fail with v2.3.0.** The factory C6 firmware often times out on RPC version queries. The OTA proceeds anyway (version checks are disabled by default).
- **LittleFS partition is overwritten** when you flash new firmware to the P4. The C6 binary must be re-embedded each time you rebuild this app.
- **No automatic rollback on C6.** If the new firmware crashes, the C6 enters a reboot loop. Recovery requires running this tool again with a known-good binary, or UART flash via test pads.
- **`just fix-components` overwrites local edits.** It copies `ota_littlefs` and `Kconfig.projbuild` from the managed esp\_hosted component, replacing any local customizations. Back up modified files before running it.

## FAQ

**Will this brick my board?**
No. The P4 can always be reflashed via USB. The C6 OTA writes to the inactive partition — a failed transfer leaves the current firmware intact. See [Safety](#safety).

**Do I need to solder anything?**
No. The SDIO OTA path requires zero soldering. Soldering is only needed for the [UART fallback](#alternative-flash-paths) if SDIO OTA fails entirely.

**Can I run this tool twice?**
Yes. If the C6 already runs v2.12.11, the app detects it and halts without writing. Running it again is safe.

**Can I upgrade to a different version (not v2.12.11)?**
In principle, yes — place any esp\_hosted `network_adapter.bin` for ESP32-C6 in `slave_fw_bin/` and change `kTargetFwMajor/Minor/Patch` in `main/main.c` plus the pin in `main/idf_component.yml` to match. Verify the binary with `esptool image-info` first. Pick the version to match the ESP-Hosted host version in whatever core your P4 firmware is built with, not simply the newest available.

**What if I don't have the `just` tool?**
Run the ESP-IDF commands directly:

```bash
# Download C6 firmware (if not already present)
curl -L -o components/ota_littlefs/slave_fw_bin/network_adapter.bin \
    https://espressif.github.io/arduino-esp32/hosted/esp32c6-v2.12.11.bin

# Build and flash
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/cu.wchusbserial110 flash monitor
```

## Related Resources

- [Elecrow CrowPanel P4 repo](https://github.com/Elecrow-RD/CrowPanel-Advanced-7inch-ESP32-P4-HMI-AI-Display-1024x600-IPS-Touch-Screen) — schematics, factory code
- [Elecrow issue #5](https://github.com/Elecrow-RD/CrowPanel-Advanced-7inch-ESP32-P4-HMI-AI-Display-1024x600-IPS-Touch-Screen/issues/5) — community discussion on C6 flash access
- [esp-hosted-mcu](https://github.com/espressif/esp-hosted-mcu) — slave firmware source, OTA examples
- [esp\_hosted v2.12.11 on Component Registry](https://components.espressif.com/components/espressif/esp_hosted/versions/2.12.11) — managed component used by this project
- [tymorton/esp32-p4-c6-espnow-enabler](https://github.com/tymorton/esp32-p4-c6-espnow-enabler) — independent C6 OTA tool (v2.6.7 target, LittleFS method)
- [esp-serial-flasher](https://github.com/espressif/esp-serial-flasher) — alternative: SDIO download mode flash (requires 1 wire to GPIO9 test pad)

## License

Based on Espressif's `host_performs_slave_ota` example. [Apache-2.0](LICENSE).
