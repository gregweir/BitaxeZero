cmake_minimum_required(VERSION 3.25)
project(bitaxezero VERSION 0.1.0 LANGUAGES C CXX)
include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(BX_BUILD_UI "Build the LVGL app" ON)
option(BX_CARDPUTER_ZERO "Build for Cardputer Zero Linux aarch64 (not the ESP32 Cardputer)" OFF)
set(BX_LVGL_SOURCE "" CACHE PATH "Optional pre-fetched LVGL v9.5.0")
set(BX_JSON_SOURCE "" CACHE PATH "Optional pre-fetched nlohmann/json")
set(BX_STB_SOURCE "" CACHE PATH
  "Optional pre-fetched nothings/stb (smoke-test PNG writer)")

include(FetchContent)
find_package(Threads REQUIRED)
FetchContent_Declare(json GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG 55f93686c01528224f448c19128836e7df245f72)
if(BX_JSON_SOURCE)
  set(FETCHCONTENT_SOURCE_DIR_JSON "${BX_JSON_SOURCE}")
endif()
FetchContent_MakeAvailable(json)

add_library(bx-core STATIC src/config.hpp src/config.cpp
  src/bitaxe.hpp src/bitaxe.cpp
  src/http.hpp src/http.cpp
  src/poller.hpp src/poller.cpp
  src/discover.hpp src/discover.cpp)
# bx-core compiles its sources (config, and later task modules); the explicit
# linker language is kept from the Task 1 header-only skeleton, where a static
# target with no compiled sources cannot infer one.
set_target_properties(bx-core PROPERTIES LINKER_LANGUAGE CXX)
target_include_directories(bx-core PUBLIC src)
target_compile_options(bx-core PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(bx-core PUBLIC nlohmann_json::nlohmann_json Threads::Threads)

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
  add_executable(bx-tests tests/test_core.cpp)
  target_link_libraries(bx-tests PRIVATE bx-core)
  target_compile_options(bx-tests PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME bx-core COMMAND bx-tests)
  # The bx-core checks read esp-miner fixtures from the source tree.
  set_tests_properties(bx-core PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endif()

if(BX_BUILD_UI)
  set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
  set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
  set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/src/lv_conf.h" CACHE FILEPATH "" FORCE)
  if(BX_LVGL_SOURCE)
    set(FETCHCONTENT_SOURCE_DIR_LVGL "${BX_LVGL_SOURCE}")
  endif()
  FetchContent_Declare(lvgl GIT_REPOSITORY https://github.com/lvgl/lvgl.git
    GIT_TAG 85aa60d18b3d5e5588d7b247abf90198f07c8a63)
  # Header-only library for the smoke test's PNG writer. stb has no CMake
  # build, so it is only populated; the include path goes on the app below.
  # Pinned at stb master 6b5799bae8a1fe3792691ef4b72ae26434c3e799:
  # stb_image_write.h v1.19.
  FetchContent_Declare(stb GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG 6b5799bae8a1fe3792691ef4b72ae26434c3e799)
  FetchContent_MakeAvailable(lvgl)
  if(BX_STB_SOURCE)
    set(FETCHCONTENT_SOURCE_DIR_STB "${BX_STB_SOURCE}")
  endif()
  FetchContent_GetProperties(stb)
  if(NOT stb_POPULATED)
    FetchContent_Populate(stb)
  endif()
  target_compile_definitions(lvgl PUBLIC BX_CARDPUTER_ZERO=$<BOOL:${BX_CARDPUTER_ZERO}>)

  add_executable(bitaxezero
    src/main.cpp
    src/screens.hpp
    src/screen_main.cpp
    src/screen_detail.cpp
    src/screen_devices.cpp
    src/ui_text.cpp)
  target_compile_options(bitaxezero PRIVATE -Wall -Wextra -Wpedantic)
  target_include_directories(bitaxezero PRIVATE "${stb_SOURCE_DIR}")
  target_link_libraries(bitaxezero PRIVATE bx-core lvgl::lvgl ${CMAKE_DL_LIBS} m)
  if(BX_CARDPUTER_ZERO)
    if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
      message(FATAL_ERROR "BX_CARDPUTER_ZERO requires an ARM64 Linux target or the official BSP cross-toolchain.")
    endif()
    include(cmake/cardputer-package.cmake)
  else()
    find_package(SDL2 CONFIG REQUIRED)
    target_link_libraries(bitaxezero PRIVATE SDL2::SDL2)
    if(BUILD_TESTING)
      add_test(NAME bx-smoke COMMAND bitaxezero --smoke-test
        "${CMAKE_BINARY_DIR}/smoke" "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
      set_tests_properties(bx-smoke PROPERTIES ENVIRONMENT "SDL_VIDEODRIVER=dummy" TIMEOUT 75)
    endif()
  endif()
endif()
