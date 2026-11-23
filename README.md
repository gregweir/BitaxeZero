# BitaxeZero

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Latest GitHub release](https://img.shields.io/github/v/release/gregweir/BitaxeZero)](https://github.com/gregweir/BitaxeZero/releases/latest)

Developed for Tartanleaf.com by Greg Weir VA1GW

Bitaxe miner farm monitor for **M5Stack Cardputer Zero** (Linux/aarch64, Raspberry Pi CM0). This is not firmware for the ESP32 Cardputer or Cardputer ADV, and it is not firmware for the Bitaxe either: BitaxeZero only reads what the miners publish and never writes settings to a miner.

## Features

- Farm dashboard at 320 × 170: the gold hero carries the farm-wide hash rate, and every card carries its miner's own rate with chip and VR temperatures, fan speed, power draw and efficiency.
- Scan for Bitaxe devices that announce themselves over mDNS, or add one by IP address when your firmware version stays quiet. No cloud service, account or analytics. BitaxeZero discovers miners on your local network and makes read-only requests to addresses you add. Use it on a trusted network.
- Read-only detail page per miner: pool (stratum host and port), shares accepted/rejected with the error rate, best session difficulty, uptime, worker, and firmware/board/ASIC version.
- Miners that stop answering dim to an offline card that remembers how long it has been dark.
- The saved device list and last-seen times persist on the device in `$XDG_CONFIG_HOME/bitaxezero/devices-v1` (`~/.config/bitaxezero/devices-v1`), surviving restarts.

## Supported devices

Any [Bitaxe](https://bitaxe.org/) (Ultra, Supra, Gamma, Mega or a clone) running [esp-miner](https://github.com/bitaxeorg/ESP-Miner) with its read-only `/api/system/info` JSON API. Verified against firmware **v2.12** and **v2.15.3**. Firmware v2.12 may not announce mDNS on every network: use **A add by IP** for those miners, and the scan for everything else.

## Install on Cardputer Zero

**From the Cardputer Zero Store, once listed:** Open **Store** on your device, press **5** for **Share Code**, enter the code shown in the listing, then press **Enter**. Install **BitaxeZero** and open it from APPLaunch.

**Manual install:** Once a release is published, download the ARM64 `.deb` from [GitHub Releases](https://github.com/gregweir/BitaxeZero/releases/latest). Check it against the supplied `SHA256SUMS`, install the package, then open **BitaxeZero** in APPLaunch. No new system image or reflash is needed.

See the [Cardputer Zero device guide](https://docs.m5stack.com/en/CardputerZero) or the [build instructions](docs/RELEASING.md) if you want to build the package yourself.

## Controls

| Key | Action |
| --- | --- |
| Up / Down (Cardputer Zero: **Fn+F** / **Fn+X**) | Move the card (or device row) highlight |
| Enter | Open the highlighted miner, or save the typed address in add-by-IP mode |
| F1 (Fn+1 on Cardputer Zero) | Devices screen: saved miners, mDNS scan, manual add |
| S | Rescan (from the dashboard this opens the Devices screen first) |
| A / X (without Fn) | Add by IP / remove the highlighted row on the Devices screen |
| Keys (printable) | Type the address in add-by-IP mode (an IP, or worst case a resolver name) |
| Backspace | Edit the typed address |
| Esc | Back a screen; quits from the dashboard |

## Build and run the Linux simulator

Dependencies: C++17, CMake 3.25 or newer, SDL2 development files and a CMake build generator. The LVGL, nlohmann/json and stb source commits are pinned by CMake. The first configure fetches them unless given their matching pre-fetched source trees with `-DBX_LVGL_SOURCE`, `-DBX_JSON_SOURCE` and `-DBX_STB_SOURCE`. Running the app needs no downloads.

```sh
cmake -S . -B build/desktop -DCMAKE_BUILD_TYPE=Release
cmake --build build/desktop --parallel
ctest --test-dir build/desktop --output-on-failure
./build/desktop/bitaxezero
```

If needed, set `-DSDL2_DIR=/path/to/SDL2/cmake`. Use a Linux graphical session for the visible simulator. CTest can run without a screen: the simulator smoke test uses SDL's dummy video driver, renders the fixed-state dashboard against the committed esp-miner fixtures, and exercises the save/remove/poll chain against a loopback-only canned miner server. The four 320 × 170 store screenshots are those same renders; to refresh them intentionally:

```sh
./build/desktop/bitaxezero --smoke-test build/desktop/smoke tests/fixtures
```

That writes `build/desktop/smoke/smoke-main.png`, `smoke-detail.png`, `smoke-devices.png` and `smoke-network.png`, and asserts the labels behind every screenshot. Convert or rename to the `screenshots/` names without resizing when a deliberate screenshot refresh is intended, following [RELEASING.md](docs/RELEASING.md). The core and its tests also build without SDL/LVGL with `-DBX_BUILD_UI=OFF`.

Project source is MIT-licensed; preserve the notices when sharing or adapting it. See [LICENSE](LICENSE) and [third-party notices](THIRD-PARTY-NOTICES.md).

## Repository layout

| Path | Contents |
| --- | --- |
| `src/` | `bx-core` (config store, esp-miner JSON parsing, HTTP, poller, mDNS discovery), the LVGL screens, and the Cardputer Zero + simulator app shell |
| `tests/` | Core test binary and committed esp-miner/mDNS fixtures |
| `cmake/` | The BSP aarch64 cross toolchain and Debian/APPLaunch packaging |
| `packaging/` | Launcher icon (SVG and generated PNG) and the .desktop entry |
| `screenshots/` | The four 320 × 170 store captures |
| `licenses/` | Copy of each third-party license |
| `docs/` | [Release recipe](docs/RELEASING.md) and [device acceptance checklist](docs/DEVICE-TEST.md) |
