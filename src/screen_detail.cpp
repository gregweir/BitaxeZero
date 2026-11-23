// BitaxeZero detail screen (read-only per-device view, Task 8): the focused
// rig's name header with its saved address, a large gold hash rate, the same
// card line the cards show, then the pool / shares / health read-outs.
// Rendering only - the app shell owns open/close and the poller's set_focus
// lifecycle; update() only re-sets label text so the 2 s focus-cadence
// refresh never rebuilds objects. Unknown fields (-1) render the mapped
// "unknown" dash from bx::format_*, never 0 (blank-not-zero rule).

#include "screens.hpp"

#include <array>
#include <cstdio>
#include <string>
#include <vector>

namespace bx::ui {

namespace {

constexpr int kScreenW = 320;
constexpr int kScreenH = 170;
constexpr int kSectionY = 66;   // below hero + card line
constexpr int kSectionLineH = 16;
constexpr int kSectionLines = 6;

lv_obj_t* make_label(lv_obj_t* parent, const lv_font_t* font,
                     std::uint32_t color, int x, int y) {
    auto* item = lv_label_create(parent);
    lv_obj_set_pos(item, x, y);
    lv_obj_set_style_text_font(item, font, 0);
    lv_obj_set_style_text_color(item, lv_color_hex(color), 0);
    return item;
}

// "-" for an absent string field; numeric unknowns (-1) come back "-" from
// the same bx::format_* the cards use, and display_text() maps the em dash.
std::string or_dash(const std::string& value) {
    return value.empty() ? "-" : value;
}

std::string shares_text(std::int64_t count) {
    return count < 0 ? "-" : std::to_string(count);
}

std::string pct_text(double pct) {
    if (pct < 0.0) return "-";
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.1f%%", pct);
    return buf;
}

// Pool latency (responseTime) in whole milliseconds.
std::string latency_text(double ms) {
    if (ms < 0.0) return "-";
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.0f ms", ms);
    return buf;
}

// "pool <host> • <port>", the card line's bullet separator; with no pool
// URL the line reads just "pool -".
std::string pool_line(const MinerInfo& info) {
    if (info.pool_url.empty()) return "pool -";
    std::string out = "pool " + info.pool_url;
    if (info.pool_port > 0) out += " • " + std::to_string(info.pool_port);
    return out;
}

void section_lines(const MinerInfo& info,
                   std::array<std::string, kSectionLines>& out) {
    out[0] = pool_line(info);
    out[1] = "worker " + or_dash(info.pool_user);
    out[2] = "shares " + shares_text(info.shares_accepted) + " ok • " +
             shares_text(info.shares_rejected) + " rej • err " +
             pct_text(info.error_pct);
    out[3] = "best " + bx::format_diff(info.best_diff) + " • session " +
             bx::format_diff(info.best_session_diff);
    out[4] = "latency " + latency_text(info.pool_latency_ms) + " • uptime " +
             bx::format_uptime(info.uptime_s);
    out[5] = or_dash(info.fw_version) + " • board " +
             or_dash(info.board_version) + " • " + or_dash(info.asic);
}

}  // namespace

void DetailScreen::create(lv_obj_t* parent) {
    root_ = lv_obj_create(parent);
    lv_obj_set_pos(root_, 0, 0);
    lv_obj_set_size(root_, kScreenW, kScreenH);
    lv_obj_set_style_bg_color(root_, lv_color_hex(kColBack), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root_, 0, 0);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);

    name_ = make_label(root_, &lv_font_montserrat_14, kColText, 8, 4);
    address_ = make_label(root_, &lv_font_montserrat_12, kColDim, 0, 7);
    lv_obj_set_width(address_, kScreenW - 16);
    lv_obj_set_style_text_align(address_, LV_TEXT_ALIGN_RIGHT, 0);

    hashrate_ = make_label(root_, &lv_font_montserrat_24, kColGold, 0, 21);
    lv_obj_set_width(hashrate_, kScreenW);
    lv_obj_set_style_text_align(hashrate_, LV_TEXT_ALIGN_CENTER, 0);

    cardline_ = make_label(root_, &lv_font_montserrat_10, kColDim, 7, 53);
    lv_obj_set_width(cardline_, kScreenW - 14);

    for (int i = 0; i < kSectionLines; ++i) {
        lines_[static_cast<std::size_t>(i)] = make_label(
            root_, &lv_font_montserrat_12, kColText, 8,
            kSectionY + i * kSectionLineH);
    }
}

void DetailScreen::update(const DeviceRow& row) {
    if (root_ == nullptr) return;
    const MinerInfo& info = row.state.info;

    title_ = display_text(device_name(row));
    lv_label_set_text(name_, title_.c_str());
    lv_label_set_text(address_, row.saved.address.c_str());

    lv_label_set_text(
        hashrate_,
        display_text(bx::format_hashrate(info.hash_rate_gh)).c_str());
    // The card line comes through bx::ui::card_detail, so its rendering (one
    // degree sign, bullet separators, unknown dashes) is pinned by the smoke
    // detail step.
    lv_label_set_text(cardline_, display_text(card_detail(info)).c_str());

    std::array<std::string, kSectionLines> texts;
    section_lines(info, texts);
    for (int i = 0; i < kSectionLines; ++i) {
        lv_label_set_text(lines_[static_cast<std::size_t>(i)],
                          display_text(texts[static_cast<std::size_t>(i)])
                              .c_str());
    }
}

void DetailScreen::show() {
    if (root_ == nullptr) return;
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(root_);
}

void DetailScreen::hide() {
    if (root_ != nullptr) lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
}

std::string DetailScreen::title_text() const { return title_; }

std::vector<std::string> DetailScreen::line_texts() const {
    std::vector<std::string> out;
    if (root_ == nullptr) return out;
    out.push_back(lv_label_get_text(hashrate_));
    out.push_back(lv_label_get_text(cardline_));
    for (const auto* line : lines_) out.emplace_back(lv_label_get_text(line));
    return out;
}

}  // namespace bx::ui
