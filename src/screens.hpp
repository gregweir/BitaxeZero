#ifndef BX_SCREENS_HPP
#define BX_SCREENS_HPP

// BitaxeZero LVGL screens and the small UI-side text helpers they share.
//
// Task 7 owns the main screen and this header's palette/formatting half;
// Task 8 (detail) and Task 9 (devices) add their screen classes here without
// touching the app shell in main.cpp again.

#include "bitaxe.hpp"
#include "config.hpp"
#include "discover.hpp"
#include "poller.hpp"

#include "lvgl.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace bx::ui {

// --- house palette (approved mockup main-screen-v3) ---
inline constexpr std::uint32_t kColBack   = 0x0d1b2a;
inline constexpr std::uint32_t kColCard   = 0x12263f;
inline constexpr std::uint32_t kColBorder = 0x2a4a6a;
inline constexpr std::uint32_t kColAccent = 0x00c8d7;
inline constexpr std::uint32_t kColGold   = 0xffd166;
inline constexpr std::uint32_t kColDim    = 0x7f95a8;
inline constexpr std::uint32_t kColText   = 0xe0e6ed;
inline constexpr std::uint32_t kColGreen  = 0x51cf66;
inline constexpr std::uint32_t kColYellow = 0xffe08a;
inline constexpr std::uint32_t kColRed    = 0xff6b6b;

// Temperature bands: ASIC green below 70 C, yellow 70 C to 75 C, red above
// 75 C; VR green below 80 C, yellow 80 C to 95 C, red above 95 C. Unknown
// values (-1) render the dim placeholder, never a colour.
inline constexpr double kAsicGreenBelow = 70.0;
inline constexpr double kAsicRedAbove = 75.0;
inline constexpr double kVrGreenBelow = 80.0;
inline constexpr double kVrRedAbove = 95.0;

std::uint32_t asic_temp_color(double celsius);
std::uint32_t vr_temp_color(double celsius);

// --- UI-side formatting (screen text, not the bx::format_* API) ---
//
// The montserrat fonts LVGL ships cover ASCII, the degree sign and the bullet
// dot, but not the em dash bx::format_* uses for "unknown" and not the
// mockup's middle dot. display_text() maps the em dash to a plain hyphen so an
// unknown value stays visible on the device; separators use the bullet.
std::string display_text(const std::string& text);

// "<n> W" with one decimal; "-" when unknown.
std::string format_watts(double watts);

// "<n> W/TH" with one decimal; "-" when unknown (-1).
std::string format_efficiency(double watts_per_th);

// One card's small readout line as plain text:
// "chip 50° • VR 57° • fan 8503 • 19.9 W • 12.6 W/TH".
std::string card_detail(const MinerInfo& info);

// The same line with LVGL recolor markup around the temperature values.
std::string card_detail_markup(const MinerInfo& info);

// Removes recolor markup ("#rrggbb ...#" spans) so logs and smoke assertions
// read plain text.
std::string strip_recolor(const std::string& markup);

// Wraps text in one LVGL recolor span so a label can mix the palette (used by
// the cards' right-hand values and the devices screen rows).
std::string recolor_span(std::uint32_t color, const std::string& text);

// Farm subline under the hero: "3 RIGS • 2 ONLINE • 60.7 W • 18.2 W/TH".
std::string farm_subline(const struct FarmView& farm);

// What the main screen renders, in one struct the poller (or the smoke
// harness) fills in. Numbers match MinerPoller's farm_* semantics.
struct FarmView {
    double hash_rate_gh = 0.0;
    double power = 0.0;        // farm watts; 0.0 when nothing online reports
                               // watts (a device's unknown -1 power is skipped)
    double efficiency = -1.0;  // W per TH; -1 renders "-"
    int rig_count = 0;         // every device on the list, online or not
    int online_count = 0;
    bool network_ok = true;
};

// One card: the saved device plus its poller state, in saved order. The
// saved hostname is the fallback name for a device that never answered.
struct DeviceRow {
    SavedDevice saved;
    MinerState state;
};

// The name a card (and the detail screen header) shows: the API hostname when
// the device ever answered one, the saved hostname otherwise, the address as
// the final safety net.
std::string device_name(const DeviceRow& row);

// The farm dashboard: slim header (built here), gold hero hash rate with the
// RIGS/ONLINE/W/W/TH subline, scrollable device cards. The "NO NETWORK" state
// swaps the hero for an offline hint. Cards are rebuilt only when the device
// count changes, so scroll position and label objects survive refreshes.
class MainScreen {
public:
    void create(lv_obj_t* parent);

    // Rows in saved-device order; now_epoch dates the offline cards'
    // "offline • <duration>" (the smoke harness pins it). Call from the LVGL
    // thread only. highlight (-1 for none) is the card index the app shell
    // keeps for Up/Down navigation; the highlighted card gets the accent
    // border and is scrolled into view.
    void update(const std::vector<DeviceRow>& rows, const FarmView& farm,
                std::int64_t now_epoch, int highlight = -1);

    // Plain-text views for the smoke harness: the hero label text, and per
    // card "title + hashrate + detail" with recolor markup stripped.
    std::string hero_text() const;
    std::vector<std::string> card_texts() const;

private:
    struct Card {
        lv_obj_t* panel = nullptr;
        lv_obj_t* name = nullptr;
        lv_obj_t* hashrate = nullptr;
        lv_obj_t* detail = nullptr;
        std::string title;
        std::string value;
        std::string plain;
    };
    void rebuild_cards(int count);

    lv_obj_t* hero_ = nullptr;
    lv_obj_t* subline_ = nullptr;
    lv_obj_t* cards_box_ = nullptr;  // scrollable card container
    std::vector<Card> cards_;
    int highlight_ = -1;             // last highlighted card (-1 = none)
};

// One device's read-only detail (Task 8): name header with the saved address,
// a large gold hash rate, the same card line the cards show, then the pool /
// shares / health read-outs. Rendering only: the caller passes the DeviceRow
// it wants shown, and owns open/close plus the poller's set_focus lifecycle,
// so Task 9's navigation reuses the screen without new wiring here. update()
// only re-sets label text, so the focused device's 2 s cadence never rebuilds
// objects.
class DetailScreen {
public:
    // Builds the (hidden) full-screen panel on parent; call once, from the
    // LVGL thread.
    void create(lv_obj_t* parent);

    // Shows one device; safe to call again on every refresh tick.
    void update(const DeviceRow& row);

    // Unhides and raises the panel over the main screen; hide() returns the
    // view to whatever is underneath.
    void show();
    void hide();

    // Plain-text views for the smoke harness: the title, and [hash rate,
    // card line, then the six section lines] with nothing but display text.
    std::string title_text() const;
    std::vector<std::string> line_texts() const;

private:
    lv_obj_t* root_ = nullptr;      // full-screen panel, hidden until show()
    lv_obj_t* name_ = nullptr;
    lv_obj_t* address_ = nullptr;
    lv_obj_t* hashrate_ = nullptr;
    lv_obj_t* cardline_ = nullptr;
    std::array<lv_obj_t*, 6> lines_{};
    std::string title_;
};

// The devices screen (Task 9, add / scan / remove): a "saved" row is the
// DeviceRow the app shell refreshes from the poller each shared tick (the
// hash-rate slot comes from the state, "offline • <duration>" from the
// last_seen pair); the screen itself keeps the row's plain text so the smoke
// harness and the app shell's key routing can assert on it. Plain texts drop
// the recolor markup the labels render from.
class DevicesScreen {
public:
    // Builds the (hidden) full-screen panel: header ("‹ Esc • Devices"),
    // scrollable SAVED / FOUND ON NETWORK sections and the bottom key hint.
    // The add-by-IP widgets (prompt, textarea, inline error) are created
    // hidden and shown by update() when add_mode is set.
    void create(lv_obj_t* parent);

    // Call from the LVGL thread on the shared 500 ms tick. highlight indexes
    // the combined row list (SAVED rows first, then found rows; -1 when the
    // screen has no rows at all) and is clamped here to what actually
    // renders. The app shell owns the add textarea's content: add_clear(),
    // add_char()/add_backspace() route the keyboard; add_text() fetches the
    // typed IP at submit time.
    void update(const std::vector<DeviceRow>& saved,
                const std::vector<DiscoveredDevice>& found,
                std::int64_t now_epoch, bool scanning, bool add_mode,
                const std::string& add_error, int highlight);

    void show();
    void hide();

    void add_clear();
    void add_char(char ch);
    void add_backspace();
    std::string add_text() const;

    // Plain-text views for the smoke harness: the "‹ Esc • Devices" header,
    // one line per SAVED row ("name | address | value | X remove?"), one line
    // per found row ("instance | address | asic • family") and the section
    // labels ("SAVED", "FOUND ON NETWORK ...", bottom hint line).
    std::string title_text() const;
    std::vector<std::string> saved_texts() const;
    std::vector<std::string> found_texts() const;
    std::vector<std::string> label_texts() const;

private:
    struct SavedRow {
        lv_obj_t* panel = nullptr;
        lv_obj_t* name = nullptr;
        lv_obj_t* addr = nullptr;
        lv_obj_t* right = nullptr;
        std::string plain;
    };
    struct FoundRow {
        lv_obj_t* panel = nullptr;
        lv_obj_t* name = nullptr;
        lv_obj_t* addr = nullptr;
        lv_obj_t* right = nullptr;
        std::string plain;
    };
    void rebuild_saved(std::size_t count);
    void rebuild_found(std::size_t count);
    void layout_rows(std::size_t saved_count, std::size_t found_count,
                     std::size_t saved_empty_h);

    lv_obj_t* root_ = nullptr;
    lv_obj_t* title_ = nullptr;
    lv_obj_t* box_ = nullptr;               // scrollable section container
    lv_obj_t* saved_label_ = nullptr;
    lv_obj_t* saved_empty_ = nullptr;
    lv_obj_t* found_label_ = nullptr;
    lv_obj_t* add_prompt_ = nullptr;        // add-mode widgets, hidden otherwise
    lv_obj_t* add_box_ = nullptr;
    lv_obj_t* add_error_ = nullptr;
    lv_obj_t* hint_ = nullptr;
    std::vector<SavedRow> saved_rows_;
    std::vector<FoundRow> found_rows_;
    std::string add_text_;
};

}  // namespace bx::ui

#endif /* BX_SCREENS_HPP */
