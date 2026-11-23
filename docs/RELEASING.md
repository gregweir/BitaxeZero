# BitaxeZero release preparation

Developed for Tartanleaf.com by Greg Weir VA1GW

This procedure prepares an ARM64 Debian package for the Linux-based **M5Stack
Cardputer Zero**. It does not publish, submit, sign, install, or change the
device or a Bitaxe miner. The application is not ESP32 Cardputer firmware and
not Bitaxe firmware.

## Prepare and build

Start in a clean checkout of the exact release source. Run
`scripts/check-source-export.sh` and record the revision. Follow the simulator
build and CTest checks in the README. For the ARM64 build, use a disposable
Debian 13 builder (or equivalent native toolchain environment) with CMake 3.25+,
Ninja, Git, GNU ARM64 GCC/G++, and dpkg packaging tools. The CMake recipe pins
LVGL, nlohmann/json and stb; the first configure needs Git access unless the
exact pre-fetched trees are supplied with `BX_LVGL_SOURCE`, `BX_JSON_SOURCE`
and `BX_STB_SOURCE`.

Obtain the official [v0.0.4 BSP](https://github.com/CardputerZero/launcher/releases/download/v0.0.4/sdk_bsp.tar.gz);
that launcher-repo URL is the canonical source, and the CMake `CM0_SDK_URL`
default in `cmake/cardputerzero-aarch64.cmake` mirrors the same v0.0.4 archive
from a secondary host path.
Confirm the downloaded file is 435,042,027 bytes. Verify its SHA-256 and extract
it into the toolchain's default project-local path so CMake's nested compiler
checks find it too. Use a fresh checkout without a pre-existing sysroot:

```sh
printf '%s  %s\n' \
  e51b6eb803ed08f450e459efbfe62dd0341440846f3be9d01da861fe6cfdebb0 \
  /path/to/sdk_bsp.tar.gz | sha256sum -c - || exit 1
mkdir -p .cache/sdk_bsp-src
tar -xzf /path/to/sdk_bsp.tar.gz -C .cache/sdk_bsp-src

cmake -S . -B build/cardputerzero -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/cardputerzero-aarch64.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DCM0_ALLOW_FETCH_DEPS=OFF \
  -DBX_CARDPUTER_ZERO=ON -DBX_ENABLE_DEB=ON -DBUILD_TESTING=OFF \
  -DBX_APP_ICON="$PWD/packaging/icon.png" \
  '-DBX_DEB_MAINTAINER=Greg Weir VA1GW <gweir@tartanleaf.com>'
cmake --build build/cardputerzero --parallel
cpack --config build/cardputerzero/CPackConfig.cmake -B build/package -G DEB
```

The maintainer contact above is for the official Tartanleaf.com release only.
Forks supply their own approved contact with the same flag; CMake leaves the
default unset. The committed PNG matches the reviewed launcher icon embedded in
the package; the editable SVG is beside it. Do not expect undocumented toolchain
versions or native build paths to produce identical archive bytes.

## Verify and prepare Store material

Set `deb` to the one newly built package, then review and record:

```sh
dpkg-deb -f "$deb" Package Version Architecture Maintainer Depends
dpkg-deb -c "$deb"
sha256sum "$deb"
```

Check package/executable/launcher names, ARM64 ELF and dependencies against the
target BSP, the package-specific icon and launcher paths, and the included MIT
and third-party notices. Do not add package binaries, SDK files, build output,
logs, local settings, credentials, device footage, or screenshots of your own
miner farm beyond the four committed store captures to Git.

[app-builder.json](../app-builder.json) holds Store metadata and uses the
[official AppBuilder schema](https://github.com/CardputerZero/AppBuilder/blob/main/docs/APP_BUILDER_JSON.md).
BitaxeZero is the first CardputerZero app here that asks for the **network**
permission, so that flag stays `true` and every other permission stays `false`
in the manifest; double-check this at submission. The four checked-in 320×170
PNGs are native-size **desktop simulator captures**. They are not
physical-device evidence:

1. [Farm dashboard](../screenshots/main.png): the hero total and per-miner cards.
2. [Read-only detail](../screenshots/detail.png): pool, shares, difficulty, uptime.
3. [Devices screen](../screenshots/devices.png): saved miners and mDNS scan hits.
4. [NO NETWORK](../screenshots/offline.png): the hero hint shown with WiFi off.

To refresh them intentionally, run the desktop simulator workflow test; it
creates `smoke-main.png`, `smoke-detail.png`, `smoke-devices.png`, and
`smoke-network.png` in the desktop build directory's `smoke/` subdirectory.
The four screenshots are 320×170 PNGs produced by the smoke harness. Rename
them to the manifest names (no resizing needed), inspect all images, and
include only the intended Store images in the change.

Recheck Store title and share-code availability at submission; the draft code
is not a reservation. Keep the intended source URL in step with the actual
public repository. [Device acceptance](DEVICE-TEST.md) and the official
[publishing lifecycle](https://github.com/CardputerZero/packages/blob/main/docs/PUBLISHING.md)
require a short physical-device demo for a first-time Store submission. A
simulator capture is not that demo. Keep it separate from Git and submit it only
through the approved Store workflow.
