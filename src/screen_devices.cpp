// BitaxeZero devices screen (add / scan / remove devices, Task 9): the
// "‹ Esc • Devices" header, the scrollable SAVED section (name, address,
// live hash rate or "offline • <duration>", "X remove" hint on the
// highlighted row), the FOUND ON NETWORK section (discovery results; Enter
// adds) and the bottom key hint. Rendering only: the rows come in from the
// app shell each shared tick, the add-by-IP textarea content is owned by the
// app shell, and the network work (discover_baxes, the probe) stays there.

#include "screens.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace bx::ui {

namespace {

constexpr int kScreenW = 320;
constexpr int kScreenH = 170;

constexpr int kBoxY = 26;          // below the header
constexpr int kBoxH = kScreenH - kBoxY - 14;  // above the bottom hint line
constexpr int kRowStride = 20;     // 17px panel + 3px gap
constexpr int kRowH = 17;
constexpr int kSavedLabelY = 4;    // inside the scroll box
constexpr int kSavedRowsY = 18;    // first saved row's panel top
constexpr int kFoundGapY = 6;      // above the found section label

// Row columns: the device name, the saved address, then the right-hand slot
// (the hash rate or "offline • <duration>" with the highlighted row's
// "X remove" hint appended, or the found row's ASIC/family summary). One
// single-colour label per slot: no recolor markup here, so the ellipsis of a
// too-long name can never cut a "#rrggbb ...#" span in half.
constexpr int kNameX = 7;
constexpr int kNameW = 114;
constexpr int kAddrX = 124;
constexpr int kAddrW = 68;
constexpr int kRightX = 196;
constexpr int kRightW = 109;

// A highlighted row: brighter card and the accent border, like the mockup's
// dashed found rows.
inline constexpr std::uint32_t kColHighlight = 0x1b3d5f;

lv_obj_t* make_label(lv_obj_t* parent, const lv_font_t* font,
                     std::uint32_t color, int x, int y) {
    auto* item = lv_label_create(parent);
    lv_obj_set_pos(item, x, y);
    lv_obj_set_style_text_font(item, font, 0);
    lv_obj_set_style_text_color(item, lv_color_hex(color), 0);
    return item;
}

// One row's middle label: the saved address, ellipsized when too long.
void make_addr_label(lv_obj_t*& label, lv_obj_t* panel) {
    label = lv_label_create(panel);
    lv_obj_set_pos(label, kAddrX, 4);
    lv_obj_set_width(label, kAddrW);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(kColDim), 0);
}

// One row's right-hand slot: right-aligned, ellipsized, small.
void make_right_label(lv_obj_t*& label, lv_obj_t* panel) {
    label = lv_label_create(panel);
    lv_obj_set_pos(label, kRightX, 4);
    lv_obj_set_width(label, kRightW);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_10, 0);
}

// The right-hand summary of a found row: "BM1370 • Gamma" from the mDNS TXT
// records, "Bitaxe" when the device announced nothing about itself.
std::string found_summary(const DiscoveredDevice& device) {
    const std::string asic = [&] {
        const auto it = device.txt.find("asic");
        return it == device.txt.end() ? std::string() : it->second;
    }();
    const std::string family = [&] {
        const auto it = device.txt.find("family");
        return it == device.txt.end() ? std::string() : it->second;
    }();
    std::string out = asic.empty() ? "Bitaxe" : asic;
    if (!family.empty()) out += " • " + family;
    return out;
}

// The offline row's duration slot: "offline • 1h12m" from the fresher of the
// poller state and the saved file, plain "offline" for one that never
// answered (same wording the main screen's cards use).
std::string offline_slot(const DeviceRow& row, std::int64_t now_epoch) {
    const std::int64_t last_seen = row.state.last_seen_epoch != 0
                                       ? row.state.last_seen_epoch
                                       : row.saved.last_seen;
    if (last_seen <= 0) return "offline";
    const std::int64_t elapsed =
        now_epoch > last_seen ? now_epoch - last_seen : 0;
    return "offline • " + display_text(bx::format_uptime(elapsed));
}

}  // namespace

void DevicesScreen::create(lv_obj_t* parent) {
    root_ = lv_obj_create(parent);
    lv_obj_set_pos(root_, 0, 0);
    lv_obj_set_size(root_, kScreenW, kScreenH);
    lv_obj_set_style_bg_color(root_, lv_color_hex(kColBack), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root_, 0, 0);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);

    title_ = make_label(root_, &lv_font_montserrat_14, kColText, 8, 3);
    lv_label_set_recolor(title_, true);
    // Plain ASCII "<" instead of the mockup's "‹": one glyph, no codepoint
    // the built-in fonts may miss. The header is the only recolor label on
    // this screen and never elides.
    lv_label_set_text(title_, (recolor_span(kColDim, "< Esc") + " • " +
                               recolor_span(kColAccent, "Devices"))
                                  .c_str());
    auto* note = make_label(root_, &lv_font_montserrat_10, kColDim, 0, 6);
    lv_obj_set_width(note, kScreenW - 16);
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(note, "saved to config");

    box_ = lv_obj_create(root_);
    lv_obj_set_pos(box_, 0, kBoxY);
    lv_obj_set_size(box_, kScreenW, kBoxH);
    lv_obj_set_style_bg_opa(box_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box_, 0, 0);
    lv_obj_set_style_pad_top(box_, 0, 0);
    lv_obj_set_style_pad_bottom(box_, 4, 0);
    lv_obj_set_style_pad_left(box_, 4, 0);
    lv_obj_set_style_pad_right(box_, 4, 0);
    lv_obj_set_scrollbar_mode(box_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(box_, LV_DIR_VER);

    saved_label_ = make_label(box_, &lv_font_montserrat_10, kColDim,
                              4, kSavedLabelY);
    lv_label_set_text(saved_label_, "SAVED");
    saved_empty_ = make_label(box_, &lv_font_montserrat_10, kColDim,
                              4, kSavedRowsY);
    lv_label_set_text(saved_empty_, "no saved devices yet - press A to add");
    found_label_ = make_label(box_, &lv_font_montserrat_10, kColDim, 4, 0);

    add_prompt_ = make_label(root_, &lv_font_montserrat_12, kColAccent, 8, 27);
    lv_label_set_text(add_prompt_, "add by IP - enter saves, esc cancels");
    add_box_ = lv_textarea_create(root_);
    lv_obj_set_pos(add_box_, 8, 43);
    lv_obj_set_size(add_box_, kScreenW - 16, 32);
    lv_textarea_set_one_line(add_box_, true);
    lv_textarea_set_max_length(add_box_, 63);
    lv_obj_set_style_bg_color(add_box_, lv_color_hex(kColCard), 0);
    lv_obj_set_style_bg_opa(add_box_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(add_box_, lv_color_hex(kColAccent), 0);
    lv_obj_set_style_border_width(add_box_, 1, 0);
    lv_obj_set_style_radius(add_box_, 5, 0);
    lv_obj_set_style_text_color(add_box_, lv_color_hex(kColText), 0);
    lv_obj_set_style_text_font(add_box_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_pad_all(add_box_, 6, 0);
    add_error_ = make_label(root_, &lv_font_montserrat_10, kColRed, 8, 79);
    lv_obj_set_width(add_error_, kScreenW - 16);
    lv_obj_add_flag(add_box_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(add_prompt_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(add_error_, LV_OBJ_FLAG_HIDDEN);

    hint_ = make_label(root_, &lv_font_montserrat_10, kColDim, 8, kScreenH - 12);
    lv_label_set_text(hint_, "S rescan • A add by IP • X remove");
}

void DevicesScreen::rebuild_saved(std::size_t count) {
    for (auto& row : saved_rows_) {
        if (row.panel != nullptr) lv_obj_delete(row.panel);
    }
    saved_rows_.clear();
    saved_rows_.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        SavedRow& row = saved_rows_[i];
        row.panel = lv_obj_create(box_);
        lv_obj_set_pos(row.panel, 4, kSavedRowsY +
                           static_cast<int>(i) * kRowStride);
        lv_obj_set_size(row.panel, kScreenW - 8, kRowH);
        lv_obj_set_style_bg_color(row.panel, lv_color_hex(kColCard), 0);
        lv_obj_set_style_bg_opa(row.panel, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row.panel, 5, 0);
        lv_obj_set_style_border_width(row.panel, 1, 0);
        lv_obj_set_style_pad_all(row.panel, 0, 0);
        lv_obj_remove_flag(row.panel, LV_OBJ_FLAG_SCROLLABLE);
        row.name = make_label(row.panel, &lv_font_montserrat_12, kColText,
                              kNameX, 3);
        lv_obj_set_width(row.name, kNameW);
        lv_label_set_long_mode(row.name, LV_LABEL_LONG_DOT);
        make_addr_label(row.addr, row.panel);
        make_right_label(row.right, row.panel);
        lv_label_set_recolor(row.right, true);
    }
}

void DevicesScreen::rebuild_found(std::size_t count) {
    for (auto& row : found_rows_) {
        if (row.panel != nullptr) lv_obj_delete(row.panel);
    }
    found_rows_.clear();
    found_rows_.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        FoundRow& row = found_rows_[i];
        row.panel = lv_obj_create(box_);
        lv_obj_set_size(row.panel, kScreenW - 8, kRowH);
        lv_obj_set_style_bg_opa(row.panel, LV_OPA_TRANSP, 0);
        lv_obj_set_style_radius(row.panel, 5, 0);
        lv_obj_set_style_border_width(row.panel, 1, 0);
        lv_obj_set_style_border_color(row.panel, lv_color_hex(kColAccent), 0);
        lv_obj_set_style_pad_all(row.panel, 0, 0);
        lv_obj_remove_flag(row.panel, LV_OBJ_FLAG_SCROLLABLE);
        row.name = make_label(row.panel, &lv_font_montserrat_12, kColText,
                              kNameX, 3);
        lv_obj_set_width(row.name, kNameW);
        lv_label_set_long_mode(row.name, LV_LABEL_LONG_DOT);
        make_addr_label(row.addr, row.panel);
        make_right_label(row.right, row.panel);
        lv_obj_set_style_text_color(row.right, lv_color_hex(kColDim), 0);
    }
}

void DevicesScreen::layout_rows(std::size_t saved_count,
                                std::size_t found_count,
                                std::size_t saved_empty_h) {
    // The saved section's height depends on its rows, or its empty hint; the
    // found section starts after it and carries its own label.
    lv_obj_set_pos(saved_empty_, 4, kSavedRowsY);
    lv_obj_add_flag(saved_empty_, LV_OBJ_FLAG_HIDDEN);
    if (saved_count == 0 && saved_empty_h > 0) {
        lv_obj_remove_flag(saved_empty_, LV_OBJ_FLAG_HIDDEN);
    }
    const int found_label_y = kSavedRowsY + static_cast<int>(
        saved_count > 0 ? saved_count * kRowStride : saved_empty_h);
    lv_obj_set_pos(found_label_, 4, found_label_y + kFoundGapY);
    for (std::size_t i = 0; i < saved_count; ++i) {
        lv_obj_set_pos(saved_rows_[i].panel, 4,
                       kSavedRowsY + static_cast<int>(i) * kRowStride);
    }
    const int found_rows_y = found_label_y + kFoundGapY + 14;
    for (std::size_t i = 0; i < found_count; ++i) {
        lv_obj_set_pos(found_rows_[i].panel, 4,
                       found_rows_y + static_cast<int>(i) * kRowStride);
    }
}

void DevicesScreen::update(const std::vector<DeviceRow>& saved,
                           const std::vector<DiscoveredDevice>& found,
                           std::int64_t now_epoch, bool scanning,
                           bool add_mode, const std::string& add_error,
                           int highlight) {
    if (root_ == nullptr) return;

    // Highlight indexes the combined list; clamp to what actually renders.
    const long total = static_cast<long>(saved.size() + found.size());
    if (highlight >= total) highlight = static_cast<int>(total) - 1;

    if (saved_rows_.size() != saved.size()) rebuild_saved(saved.size());
    if (found_rows_.size() != found.size()) rebuild_found(found.size());

    // --- SAVED rows ---
    for (std::size_t i = 0; i < saved_rows_.size(); ++i) {
        const DeviceRow& row = saved[i];
        SavedRow& view = saved_rows_[i];
        const bool highlighted = static_cast<int>(i) == highlight && !add_mode;

        lv_obj_set_style_bg_color(view.panel, lv_color_hex(
            highlighted ? kColHighlight : kColCard), 0);
        lv_obj_set_style_border_color(view.panel, lv_color_hex(
            highlighted ? kColAccent : kColBorder), 0);

        const std::string name = display_text(device_name(row));
        const std::string address = row.saved.address;
        lv_label_set_text(view.name, name.c_str());
        lv_label_set_text(view.addr, address.c_str());

        const std::string slot = row.state.online
            // The display value (10-minute average when reported), the same
            // field the poller sums into the hero.
            ? display_text(
                  bx::format_hashrate(row.state.info.display_hashrate()))
            : offline_slot(row, now_epoch);
        // The highlighted row carries the "X remove" hint next to its value.
        const std::string right_text =
            highlighted ? slot + "  X remove" : slot;
        lv_label_set_text(view.right, right_text.c_str());
        lv_obj_set_style_text_color(view.right, lv_color_hex(
            row.state.online ? kColGold : kColDim), 0);
        view.plain = name + " | " + address + " | " + right_text;
    }

    // --- FOUND rows ---
    for (std::size_t i = 0; i < found_rows_.size(); ++i) {
        const DiscoveredDevice& device = found[i];
        FoundRow& view = found_rows_[i];
        const bool highlighted =
            static_cast<int>(static_cast<long>(saved.size()) + i) ==
                highlight && !add_mode;

        lv_obj_set_style_bg_opa(
            view.panel, highlighted ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        if (highlighted) {
            lv_obj_set_style_bg_color(view.panel,
                                      lv_color_hex(kColHighlight), 0);
        }

        const std::string instance = display_text(device.instance);
        lv_label_set_text(view.name, instance.c_str());
        lv_label_set_text(view.addr, device.address.c_str());
        const std::string summary = found_summary(device);
        lv_label_set_text(view.right, summary.c_str());
        view.plain = instance + " | " + device.address + " | " + summary;
    }

    // --- section chrome ---
    if (scanning) {
        lv_label_set_text(found_label_, "FOUND ON NETWORK • scanning");
    } else if (found.empty()) {
        lv_label_set_text(found_label_, "FOUND ON NETWORK • S rescan");
    } else {
        lv_label_set_text(found_label_, "FOUND ON NETWORK • ENTER TO ADD");
    }
    const std::size_t saved_empty_h =
        saved.empty() ? 12 : 0; // the "no saved devices yet" hint line
    layout_rows(saved.size(), found.size(), saved_empty_h);

    // --- add mode chrome ---
    if (add_mode) {
        lv_obj_remove_flag(add_prompt_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(add_box_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(box_, LV_OBJ_FLAG_HIDDEN);
        if (add_error.empty()) {
            lv_obj_add_flag(add_error_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_label_set_text(add_error_, add_error.c_str());
            lv_obj_remove_flag(add_error_, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        lv_obj_add_flag(add_prompt_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(add_box_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(add_error_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(box_, LV_OBJ_FLAG_HIDDEN);
        if (highlight >= 0) {
            lv_obj_scroll_to_view(highlight < static_cast<int>(saved.size())
                ? saved_rows_[static_cast<std::size_t>(highlight)].panel
                : found_rows_[static_cast<std::size_t>(
                      highlight - static_cast<int>(saved.size()))].panel,
                LV_ANIM_OFF);
        }
    }
}

void DevicesScreen::show() {
    if (root_ == nullptr) return;
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(root_);
}

void DevicesScreen::hide() {
    if (root_ != nullptr) lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
}

void DevicesScreen::add_clear() {
    add_text_.clear();
    if (add_box_ != nullptr) lv_textarea_set_text(add_box_, "");
}

void DevicesScreen::add_char(char ch) {
    // Printable ASCII only, capped at the textarea's own max length: this is
    // an IP address (or, worst case, a resolver name), never free text.
    if (static_cast<unsigned char>(ch) < 0x20 || ch > 0x7e) return;
    if (add_text_.size() >= 63) return;
    add_text_.push_back(ch);
    if (add_box_ != nullptr) {
        lv_textarea_set_text(add_box_, add_text_.c_str());
        lv_textarea_set_cursor_pos(add_box_, LV_TEXTAREA_CURSOR_LAST);
    }
}

void DevicesScreen::add_backspace() {
    if (add_text_.empty()) return;
    add_text_.pop_back();
    if (add_box_ != nullptr) {
        lv_textarea_set_text(add_box_, add_text_.c_str());
        lv_textarea_set_cursor_pos(add_box_, LV_TEXTAREA_CURSOR_LAST);
    }
}

std::string DevicesScreen::add_text() const { return add_text_; }

std::string DevicesScreen::title_text() const { return title_ == nullptr
    ? std::string() : std::string(lv_label_get_text(title_)); }

std::vector<std::string> DevicesScreen::saved_texts() const {
    std::vector<std::string> out;
    out.reserve(saved_rows_.size());
    for (const SavedRow& row : saved_rows_) out.push_back(row.plain);
    return out;
}

std::vector<std::string> DevicesScreen::found_texts() const {
    std::vector<std::string> out;
    out.reserve(found_rows_.size());
    for (const FoundRow& row : found_rows_) out.push_back(row.plain);
    return out;
}

std::vector<std::string> DevicesScreen::label_texts() const {
    std::vector<std::string> out;
    if (root_ == nullptr) return out;
    out.emplace_back(lv_label_get_text(saved_label_));
    out.emplace_back(lv_label_get_text(found_label_));
    out.emplace_back(lv_label_get_text(hint_));
    return out;
}

}  // namespace bx::ui
