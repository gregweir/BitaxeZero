# Cardputer Zero device and package review

Only the M5Stack Cardputer Zero Linux ARM64 target is in scope. Do not flash an image or change the OS or its permissions for this app, and do not change a miner's firmware or settings: BitaxeZero is a read-only monitor.

## Build a candidate locally

Use the versioned [release recipe](RELEASING.md) for the pinned BSP, exact package icon and approved maintainer.

1. Obtain a project-isolated GNU `aarch64-linux-gnu-gcc/g++` toolchain and the official [v0.0.4 BSP sysroot](https://github.com/CardputerZero/launcher/releases/tag/v0.0.4). The published BSP is 435,042,027 bytes compressed with SHA-256 `e51b6eb803ed08f450e459efbfe62dd0341440846f3be9d01da861fe6cfdebb0`; verify it before extracting. On a host without a cross compiler, use an isolated Debian 13 builder with GNU ARM64 cross tools, CMake/Ninja and dpkg tooling instead of modifying the host OS. The CMake toolchain defaults to no automatic SDK download; pass the existing verified sysroot. The stock Launcher requires an existing user session with framebuffer and keyboard access, plus your local WiFi network for the miner polls; this program does not add group permissions or a systemd service.
2. Configure with `-DCMAKE_TOOLCHAIN_FILE=cmake/cardputerzero-aarch64.cmake -DCM0_SDK_ROOT=/path/to/bsp -DBX_CARDPUTER_ZERO=ON -DCMAKE_BUILD_TYPE=Release`, using a fresh `build/cardputerzero` directory. Put the approved cross tools on this command's PATH, not in global configuration.
3. To generate a .deb, first make a local 256 × 256 PNG from the source icon, for example `mkdir -p build/package && convert -background none packaging/icon.svg build/package/bitaxezero.png`. Configure with the same options plus `-DBX_ENABLE_DEB=ON -DBX_APP_ICON="$PWD/build/package/bitaxezero.png" -DBX_DEB_MAINTAINER="$BX_MAINTAINER"`. Supply the approved public maintainer name and email (`Name <address>`) in that shell variable. The project leaves the maintainer unset; do not use a sample value or put the contact into Git without approval. Build then use `cpack --config build/cardputerzero/CPackConfig.cmake -B build/package -G DEB`.
4. Before installation, inspect the .deb with `dpkg-deb -I` and `dpkg-deb -c` and verify ELF AArch64 architecture/library dependencies, launcher target/icon and checksums. The only shared paths are package-specific entries `/usr/share/APPLaunch/applications/bitaxezero.desktop` and `/usr/share/APPLaunch/share/images/bitaxezero.png`; docs are under `/usr/share/doc/bitaxezero`; no postinst, services, credentials or global settings. Check `dpkg-deb -f package.deb Maintainer` displays the approved public contact, not a placeholder. Verify the actual package contains `LICENSE`, `THIRD-PARTY-NOTICES.md` and the five license files under `licenses/` (CardputerZero Template, LVGL, Montserrat, nlohmann/json, stb) in its own documentation directory; the CMake install entries alone are not release proof.

## Physical acceptance

With the owner's permission, open the candidate through APPLaunch on a stock Cardputer Zero on the same WiFi network as the Bitaxe miners. Verify an intact 320 × 170 view and short one-handed control path; Up/Down card scroll, Enter detail open, Esc back, and the F1/S/A/X shortcut rows. The program also accepts `--keyboard /dev/input/eventN` for local diagnosis; do not run the whole app as root to work around file access.

Check each of these against a real farm and record the result:

1. The app opens at 320 × 170 with no clipped hero or card text.
2. Each live rig's card hash rate matches that miner's own web UI, and the gold hero matches the sum across the two devices.
3. A power cycled-off miner dims to an offline card, and the offline duration counts up between polls.
4. With WiFi switched off, the hero reads **NO NETWORK** with the connection hint and the cards keep their last-rendered values; switching WiFi back on restores the hash rate hero.
5. **S rescan** finds the miners that announce mDNS on this network. Expectation from the reviewed devices: at least one announcing miner appears under FOUND ON NETWORK (firmware v2.15.3), while the v2.12 unit does **not** announce mDNS on the reviewed network, so it must be added with **A add by IP**. Note the scan/add split you observed in the review record rather than treating a one-item scan as a failure.
6. Add-by-IP reaches a miner that does not announce: its probe fills the hostname from `/api/system/info`, the row saves and starts polling.
7. **X remove** deletes a row and it stays deleted after leaving and reopening the app; the saved list and the last-seen times restart correctly.
8. The detail page of a live miner shows the same pool, shares and uptime its web UI reports, with no blank or `0.00` placeholder fields.
9. Opening the app with no reachable network renders the NO NETWORK state immediately; nothing hangs or retries visibly.

## First-release demo (not yet recorded)

The [packages publishing lifecycle](https://github.com/CardputerZero/packages/blob/main/docs/PUBLISHING.md) lists a human review and device demo for first releases through either the CLI or web portal. The [AppBuilder schema](https://github.com/CardputerZero/AppBuilder/blob/main/docs/APP_BUILDER_JSON.md) specifies a short video of the app on real hardware, posted in the submission PR comments after the PR opens. A desktop simulator clip does not meet that requirement. The official docs do not specify an exact duration or shot list; roughly 30–60 seconds is enough to cover the steps below.

Record one short sequence on the physical Cardputer Zero after the exact candidate passes the checks above: open **BitaxeZero** in APPLaunch, let the farm hero fill in with two real miners shown, scroll to an offline card, open a live miner's detail page, Esc back, then switch WiFi off to show the NO NETWORK hero and back on. Keep screen and keys visible, show only demo miner names rather than identifying pool or worker details, and keep the clip out of source control. Save the clip beside the exact package version and checksum. Sharing the clip and submitting a release remain separate approval steps.

## Later submission

Separate approval is required for creating or publishing a public GitHub repo, uploading to the store, or installing on a device. Review all source via `scripts/check-source-export.sh` against the exact Git index, then inspect archive contents. Keep the reviewed ARM64 package, source revision and package SHA. Confirm the unique package name/share code, approved maintainer and public identity, app icon, the four 320 × 170 screenshots and the Home & Hobby / Tools categories in the [official App Builder](https://dev.cardputer.cc/). Suggested summary: "Bitaxe miner farm monitor for Cardputer Zero." The manifest enables the network permission only; no app background service, microphone, camera or IMU is needed. Do not use the template's deployment script or unreviewed publishing workflow.

## Physical acceptance — 2026-11-23, package 0.1.0-1

Device: M5Stack Cardputer Zero (CPZ, Raspberry Pi CM0, stock launcher
kernel), launcher-session app. Farm: two Bitaxe miners on the local network -
one on esp-miner v2.15.3 that announces mDNS, and one on esp-miner v2.12 that
does not announce mDNS on this network.

Results, all on the final device-acceptance package:

- Install: `dpkg -i`, opens through APPLaunch, intact 320×170 view.
- Main screen: hero farm total = sum of both rigs' 10-minute averages and stays
  at the sum of the cards; instant hash-rate fields proved too jittery to read
  (fixed during acceptance: display prefers `hashRate_10m`; the detail screen
  keeps the instantaneous rate). Chip/VR temps on device matched the rigs' own
  web UIs, no false cold/hot readings at boot.
- Devices screen: scan found the announcing v2.15.3 rig (Enter to add); manual
  add-by-IP required the `.` key (Sym+B on this keyboard — fixed during
  acceptance by porting the official FactoryTest TCA8418 symbol table; the
  controller reports its own raw symbol codes that shadow standard KEY_* codes,
  e.g. code 91 = `.`). Note: on-device mDNS scans are announcement-timing
  sensitive and may return empty; rescan again or add manually — add-by-IP is
  the reliable path for non-announcing and unreachable devices alike.
- Devices-screen selection: fixed during acceptance — the screen now opens
  with the first row highlighted and Enter/X act on the displayed row;
  found-row selection indexes the displayed (unsaved) list.
- Detail screen: pool, worker, accepted shares, best
  diffs, uptime, firmware line all correct and live (2 s cadence while open);
  no live pool host, worker or wallet identifiers are recorded here.
- Offline: powering off one live rig dimmed its card with a persisted
  `offline · <duration>` count within ~10 s; the hero dropped to the
  online-only sum; the app stopped hammering the dead IP (back-off) and the
  rig rejoined within one poll cycle when powered back on.
- NO NETWORK: `nmcli device disconnect wlan0` (owner-run) → red `NO NETWORK`
  hero + hint, distinct from all-rigs-offline; config intact; recovery to the
  farm view after reconnect.
- Restart persistence: quit and reopen through APPLaunch — both saved devices
  and their durations survive.
- Store risks resolved on device: a launcher-session app has outbound LAN
  socket access (verified live: both rigs polled and persisted while the
  manifest carries `network: true`); UDP 5353 binding works on-device, but
  group-addressed queries do not reliably return responses — passive
  announcements plus manual IP entry cover the gap.
- Config stability: the saved-device file stayed correct across installs,
  offline sessions and network outages during the whole acceptance session.

First-release demo: to be recorded on this device by the author (kept beside
the package checksum, out of source control, not yet recorded).
