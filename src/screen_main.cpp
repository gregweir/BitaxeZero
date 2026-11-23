// BitaxeZero main screen: farm hero, RIGS/ONLINE/W/W/TH subline and the
// scrollable device cards, per the approved mockup main-screen-v3 scaled to
// the 320 x 170 display. Rendering only - farm numbers and card states come
// in through FarmView/DeviceRow from the poller (or the smoke harness).

#include "screens.hpp"

#include <ctime>
#include <string>
#include <vector>

namespace bx::ui {

namespace {

constexpr int kScreenW = 320;
constexpr int kScreenH = 170;
constexpr int kCardW = 312;      // screen minus the 4px container padding
constexpr int kCardH = 33;
constexpr int kCardStride = 35;  // 2px gap below each card
constexpr int kCardsY = 61;      // everything above is hero + subline

lv_obj_t* make_label(lv_obj_t* parent, const lv_font_t* font,
                     std::uint32_t color, int x, int y) {
    auto* item = lv_label_create(parent);
    lv_obj_set_pos(item, x, y);
    lv_obj_set_style_text_font(item, font, 0);
    lv_obj_set_style_text_color(item, lv_color_hex(color), 0);
    return item;
}

}  // namespace

void MainScreen::create(lv_obj_t* parent) {
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(parent, lv_color_hex(kColBack), 0);

    lv_label_set_text(make_label(parent, &lv_font_montserrat_12, kColDim, 8, 3),
                      "bitaxezero");
    auto* shortcut = make_label(parent, &lv_font_montserrat_12, kColDim, 8, 3);
    lv_obj_set_width(shortcut, kScreenW - 16);
    lv_obj_set_style_text_align(shortcut, LV_TEXT_ALIGN_RIGHT, 0);
#if BX_CARDPUTER_ZERO
    lv_label_set_text(shortcut, "Fn+1 devices • S rescan");
#else
    lv_label_set_text(shortcut, "F1 devices • S rescan");
#endif

    hero_ = make_label(parent, &lv_font_montserrat_20, kColGold, 0, 19);
    lv_obj_set_width(hero_, kScreenW);
    lv_obj_set_style_text_align(hero_, LV_TEXT_ALIGN_CENTER, 0);

    subline_ = make_label(parent, &lv_font_montserrat_12, kColDim, 0, 46);
    lv_obj_set_width(subline_, kScreenW);
    lv_obj_set_style_text_align(subline_, LV_TEXT_ALIGN_CENTER, 0);

    cards_box_ = lv_obj_create(parent);
    lv_obj_set_pos(cards_box_, 0, kCardsY);
    lv_obj_set_size(cards_box_, kScreenW, kScreenH - kCardsY);
    lv_obj_set_style_bg_opa(cards_box_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cards_box_, 0, 0);
    lv_obj_set_style_pad_top(cards_box_, 2, 0);
    lv_obj_set_style_pad_bottom(cards_box_, 2, 0);
    lv_obj_set_style_pad_left(cards_box_, 4, 0);
    lv_obj_set_style_pad_right(cards_box_, 4, 0);
    lv_obj_set_scrollbar_mode(cards_box_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(cards_box_, LV_DIR_VER);
}

void MainScreen::rebuild_cards(int count) {
    for (auto& card : cards_) {
        if (card.panel != nullptr) lv_obj_delete(card.panel);
    }
    cards_.clear();
    cards_.resize(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        Card& card = cards_[static_cast<std::size_t>(i)];
        card.panel = lv_obj_create(cards_box_);
        lv_obj_set_pos(card.panel, 0, i * kCardStride);
        lv_obj_set_size(card.panel, kCardW, kCardH);
        lv_obj_set_style_bg_color(card.panel, lv_color_hex(kColCard), 0);
        lv_obj_set_style_bg_opa(card.panel, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card.panel, 5, 0);
        lv_obj_set_style_border_width(card.panel, 1, 0);
        lv_obj_set_style_border_color(card.panel, lv_color_hex(kColBorder), 0);
        lv_obj_remove_flag(card.panel, LV_OBJ_FLAG_SCROLLABLE);
        // Keep the theme's default padding off the cards: the label offsets
        // below are tuned to the 312 x 33 card rectangle.
        lv_obj_set_style_pad_all(card.panel, 0, 0);
        card.name = make_label(card.panel, &lv_font_montserrat_12, kColText, 7, 2);
        card.hashrate =
            make_label(card.panel, &lv_font_montserrat_12, kColGold, 0, 2);
        lv_obj_set_width(card.hashrate, 150);
        lv_obj_set_pos(card.hashrate, kCardW - 150 - 7, 2);
        lv_obj_set_style_text_align(card.hashrate, LV_TEXT_ALIGN_RIGHT, 0);
        card.detail =
            make_label(card.panel, &lv_font_montserrat_10, kColDim, 7, 17);
        lv_obj_set_width(card.detail, kCardW - 14);
        lv_label_set_recolor(card.detail, true);
    }
}

void MainScreen::update(const std::vector<DeviceRow>& rows,
                        const FarmView& farm, std::int64_t now_epoch,
                        int highlight) {
    if (static_cast<int>(cards_.size()) != static_cast<int>(rows.size()))
        rebuild_cards(static_cast<int>(rows.size()));

    if (!farm.network_ok) {
        lv_label_set_text(hero_, "NO NETWORK");
        lv_obj_set_style_text_color(hero_, lv_color_hex(kColRed), 0);
        lv_label_set_text(subline_, "check the device's WiFi connection");
        lv_obj_set_style_text_color(subline_, lv_color_hex(kColDim), 0);
    } else {
        lv_label_set_text(
            hero_, display_text(bx::format_hashrate(farm.hash_rate_gh)).c_str());
        lv_obj_set_style_text_color(hero_, lv_color_hex(kColGold), 0);
        lv_label_set_text(subline_, display_text(farm_subline(farm)).c_str());
        lv_obj_set_style_text_color(subline_, lv_color_hex(kColDim), 0);
    }

    for (std::size_t i = 0; i < cards_.size(); ++i) {
        const DeviceRow& row = rows[i];
        Card& card = cards_[i];
        card.title = display_text(device_name(row));
        lv_label_set_text(card.name, card.title.c_str());
        // Up/Down navigation highlight from the app shell (the mockup's
        // "Up/Down scroll" hint): the selected card gets the accent border.
        const bool highlighted =
            static_cast<int>(i) == highlight && !rows.empty();
        lv_obj_set_style_border_color(card.panel, lv_color_hex(
            highlighted ? kColAccent : kColBorder), 0);

        const std::int64_t last_seen =
            row.state.last_seen_epoch != 0 ? row.state.last_seen_epoch
                                           : row.saved.last_seen;
        const std::int64_t elapsed =
            now_epoch > last_seen && last_seen > 0 ? now_epoch - last_seen : 0;

        if (row.state.online) {
            lv_obj_set_style_bg_opa(card.panel, LV_OPA_COVER, 0);
            lv_obj_set_style_text_color(card.name, lv_color_hex(kColText), 0);
            // The display value (10-minute average when reported, instant
            // otherwise) - the same field the poller sums into the hero, so
            // the hero always equals the exact sum of the cards.
            card.value = display_text(
                bx::format_hashrate(row.state.info.display_hashrate()));
            lv_label_set_text(card.hashrate, card.value.c_str());
            lv_obj_remove_flag(card.hashrate, LV_OBJ_FLAG_HIDDEN);
            const std::string markup = card_detail_markup(row.state.info);
            lv_label_set_text(card.detail, markup.c_str());
            card.plain = display_text(strip_recolor(markup));
        } else {
            lv_obj_set_style_bg_opa(card.panel, LV_OPA_40, 0);
            lv_obj_set_style_text_color(card.name, lv_color_hex(kColDim), 0);
            card.value.clear();
            lv_obj_add_flag(card.hashrate, LV_OBJ_FLAG_HIDDEN);
            const std::string offline =
                last_seen > 0
                    ? "offline • " +
                          display_text(bx::format_uptime(elapsed))
                    : "offline";
            lv_label_set_text(card.detail, offline.c_str());
            card.plain = offline;
        }
    }

    // Scroll the highlighted card into view, once per highlight change only
    // (lv_obj_scroll_to_view would fight every 500 ms repaint otherwise).
    if (highlight != highlight_) {
        highlight_ = highlight;
        if (highlight >= 0 && highlight < static_cast<int>(cards_.size())) {
            lv_obj_scroll_to_view(cards_[static_cast<std::size_t>(highlight)]
                                      .panel,
                                  LV_ANIM_OFF);
        }
    }
}

std::string MainScreen::hero_text() const {
    return hero_ == nullptr ? std::string() : std::string(lv_label_get_text(hero_));
}

std::vector<std::string> MainScreen::card_texts() const {
    std::vector<std::string> out;
    out.reserve(cards_.size());
    for (const Card& card : cards_) {
        out.push_back(card.title + " | " + card.value + " | " + card.plain);
    }
    return out;
}

}  // namespace bx::ui
