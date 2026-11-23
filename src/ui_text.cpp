// BitaxeZero UI-side text helpers shared by the screens (Task 7): palette
// temperature bands and the strings the main screen's labels carry. Pure
// formatting - no LVGL calls, no I/O - so tests can assert on exact text.

#include "screens.hpp"

#include <cmath>
#include <cstdio>

namespace bx::ui {

std::uint32_t asic_temp_color(double celsius) {
    if (celsius < kAsicGreenBelow) return kColGreen;
    if (celsius <= kAsicRedAbove) return kColYellow;
    return kColRed;
}

std::uint32_t vr_temp_color(double celsius) {
    if (celsius < kVrGreenBelow) return kColGreen;
    if (celsius <= kVrRedAbove) return kColYellow;
    return kColRed;
}

std::string display_text(const std::string& text) {
    // The montserrat fonts LVGL builds have no glyph for the em dash that
    // bx::format_* returns for unknown values; a plain hyphen reads the same.
    const std::string em_dash = "\xE2\x80\x94";
    std::string out = text;
    std::size_t pos = 0;
    while ((pos = out.find(em_dash, pos)) != std::string::npos) {
        out.replace(pos, em_dash.size(), "-");
        pos += 1;
    }
    return out;
}

std::string format_watts(double watts) {
    if (watts < 0.0) return "-";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f W", watts);
    return buf;
}

std::string format_efficiency(double watts_per_th) {
    if (watts_per_th < 0.0) return "-";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f W/TH", watts_per_th);
    return buf;
}

namespace {

// Degrees rounded for the card line, "-" when unknown.
std::string temp_text(double celsius) {
    if (celsius < 0.0) return "-";
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d°",
                  static_cast<int>(std::llround(celsius)));
    return buf;
}

// A rig's W per TH from its own raw fields; unknown when either side is.
double rig_efficiency(const MinerInfo& info) {
    if (info.power < 0.0 || info.hash_rate_gh <= 0.0) return -1.0;
    return info.power / (info.hash_rate_gh / 1000.0);
}

std::string fan_text(const MinerInfo& info) {
    return info.fan_rpm < 0 ? "-" : std::to_string(info.fan_rpm);
}

void append_colored(std::string& out, double celsius,
                    std::uint32_t (*pick)(double)) {
    if (celsius < 0.0) {
        out += "-";
        return;
    }
    char hex[16];
    std::snprintf(hex, sizeof(hex), "#%06x ", pick(celsius));
    out += hex;
    out += temp_text(celsius);
    out += "#";
}

}  // namespace

std::string card_detail(const MinerInfo& info) {
    // temp_text() already appends the degree sign; adding one here rendered
    // "°°" on every card.
    return "chip " + temp_text(info.chip_temp) + " • VR " +
           temp_text(info.vr_temp) + " • fan " + fan_text(info) + " • " +
           format_watts(info.power) + " • " +
           format_efficiency(rig_efficiency(info));
}

std::string card_detail_markup(const MinerInfo& info) {
    std::string out = "chip ";
    append_colored(out, info.chip_temp, &asic_temp_color);
    out += " • VR ";
    append_colored(out, info.vr_temp, &vr_temp_color);
    out += " • fan " + fan_text(info) + " • " + format_watts(info.power) +
           " • " + format_efficiency(rig_efficiency(info));
    return out;
}

std::string strip_recolor(const std::string& markup) {
    std::string out;
    out.reserve(markup.size());
    std::size_t i = 0;
    while (i < markup.size()) {
        // "#rrggbb text#" spans: drop the code, keep the text. The recolor
        // parser needs the space after the six hex digits, which this
        // matcher also anchors on.
        if (markup[i] == '#' && i + 7 < markup.size() &&
            markup[i + 7] == ' ') {
            const std::size_t close = markup.find('#', i + 8);
            if (close != std::string::npos) {
                out += markup.substr(i + 8, close - (i + 8));
                i = close + 1;
                continue;
            }
        }
        out += markup[i++];
    }
    return out;
}

std::string recolor_span(std::uint32_t color, const std::string& text) {
    char hex[12];
    std::snprintf(hex, sizeof(hex), "#%06x",
                  static_cast<unsigned int>(color & 0xffffffu));
    return std::string(hex) + " " + text + "#";
}

// The name a card (and the detail screen header) shows: the API hostname when
// the device ever answered one, the saved hostname otherwise, the address as
// the final safety net.
std::string device_name(const DeviceRow& row) {
    if (!row.state.info.hostname.empty()) return row.state.info.hostname;
    if (!row.saved.hostname.empty()) return row.saved.hostname;
    return row.saved.address;
}

std::string farm_subline(const FarmView& farm) {
    return std::to_string(farm.rig_count) + " RIGS • " +
           std::to_string(farm.online_count) + " ONLINE • " +
           format_watts(farm.power) + " • " +
           format_efficiency(farm.efficiency);
}

}  // namespace bx::ui
