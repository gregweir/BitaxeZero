#!/usr/bin/env bash
set -euo pipefail
cd -- "$(git rev-parse --show-toplevel)"
expected="$(printf '%s\n' \
  .gitignore CMakeLists.txt LICENSE README.md THIRD-PARTY-NOTICES.md app-builder.json \
  cmake/cardputer-package.cmake cmake/cardputerzero-aarch64.cmake docs/DEVICE-TEST.md docs/RELEASING.md \
  licenses/CardputerZero-Template-MIT.txt licenses/factorytest.txt licenses/LVGL-MIT.txt \
  licenses/Montserrat-OFL.txt licenses/nlohmann-json-MIT.txt licenses/stb-LICENSE.txt \
  packaging/bitaxezero.desktop packaging/icon.svg packaging/icon.png \
  scripts/capture-mdns.py scripts/check-source-export.sh \
  screenshots/detail.png screenshots/devices.png screenshots/main.png screenshots/offline.png \
  src/bitaxe.cpp src/bitaxe.hpp src/config.cpp src/config.hpp src/discover.cpp src/discover.hpp \
  src/http.cpp src/http.hpp src/lv_conf.h src/main.cpp src/poller.cpp src/poller.hpp \
  src/screen_detail.cpp src/screen_devices.cpp src/screen_main.cpp src/screens.hpp src/ui_text.cpp \
  tests/fixtures/device-v2.12.json tests/fixtures/device-v2.15.3.json tests/fixtures/mdns-response.bin \
  tests/test_core.cpp \
  | LC_ALL=C sort)"
actual="$(git ls-files --cached | LC_ALL=C sort)"
diff -u <(printf '%s\n' "$expected") <(printf '%s\n' "$actual")
# Keep the upstream Montserrat notice byte-for-byte, including its trailing space.
git diff --cached --check -- . ':(exclude)licenses/Montserrat-OFL.txt'
count="$(printf '%s\n' "$expected" | grep -c .)"
printf '%s\n' "Exact ${count}-path source allowlist verified. Review file content and intended public destinations before publishing."
