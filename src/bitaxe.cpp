// BitaxeZero - esp-miner HTTP API mapping. Pure functions only: no I/O, no
// locale, no library calls that read the current C locale (all number
// formatting is done by hand so the UI renders identically everywhere).

#include "bitaxe.hpp"

#include <cmath>
#include <limits>
#include <utility>

#include <nlohmann/json.hpp>

namespace bx {
namespace {

using nlohmann::json;

// Numeric or boolean field; anything else (absent, null, string, -1
// sentinel for temperatures) falls through to the caller's default.
bool read_number(const json& doc, const char* key, double& out) {
    const auto it = doc.find(key);
    if (it == doc.end() || !it->is_number()) return false;
    out = it->get<double>();
    return true;
}

std::int64_t int_or(const json& doc, const char* key, std::int64_t fallback) {
    const auto it = doc.find(key);
    if (it == doc.end() || !it->is_number()) return fallback;
    // Exact integer path first: difficulties exceed 2^53, so a double
    // round-trip would lose precision.
    if (it->is_number_unsigned()) {
        const auto value = it->get<std::uint64_t>();
        return value <=
                       static_cast<std::uint64_t>(
                           std::numeric_limits<std::int64_t>::max())
                   ? static_cast<std::int64_t>(value)
                   : fallback;
    }
    if (it->is_number_integer()) return it->get<std::int64_t>();
    return static_cast<std::int64_t>(it->get<double>());
}

std::string string_or(const json& doc, const char* key,
                      std::string fallback = {}) {
    const auto it = doc.find(key);
    if (it == doc.end() || !it->is_string()) return fallback;
    return it->get<std::string>();
}

// isUsingFallbackStratum is a number on v2.12/v2.15.3 and a boolean on some
// forks; useFallbackStratum is the legacy alias.
bool fallback_flag(const json& doc) {
    for (const char* key : {"isUsingFallbackStratum", "useFallbackStratum"}) {
        const auto it = doc.find(key);
        if (it == doc.end()) continue;
        if (it->is_boolean()) return it->get<bool>();
        if (it->is_number()) return it->get<double>() != 0.0;
    }
    return false;
}

// Pool coordinates: pools[0] when the firmware exposes the array, otherwise
// the flat stratum fields (v2.12).
void read_pool(const json& doc, MinerInfo& out) {
    const auto pools = doc.find("pools");
    const json* pool = nullptr;
    if (pools != doc.end() && pools->is_array() && !pools->empty() &&
        (*pools)[0].is_object()) {
        pool = &(*pools)[0];
    }
    const json& source = pool ? *pool : doc;
    out.pool_url = string_or(source, "stratumURL");
    out.pool_port = static_cast<int>(int_or(source, "stratumPort", 0));
    out.pool_user = string_or(source, "stratumUser");
}

// Chip/VR temperatures outside (0, 120] C are sensor garbage, not readings:
// they count as "unknown". Only temperatures are clamped like this; hash
// rate, fan, power and friends pass through untouched.
constexpr double kMaxPlausibleTemp = 120.0;

void read_temp(const json& doc, const char* key, double& out) {
    double value = 0.0;
    if (read_number(doc, key, value) && value > 0.0 &&
        value <= kMaxPlausibleTemp) {
        out = value;
    }
}

// Non-negative, locale-independent rounding to two decimal places at the
// current unit's scale (no printf: printf honors the active locale).
unsigned long long hundredths(double value) {
    return static_cast<unsigned long long>(std::floor(value * 100.0 + 0.5));
}

std::string two_decimals(double value) {
    const unsigned long long scaled = hundredths(value);
    const unsigned long long whole = scaled / 100ULL;
    const unsigned long long frac = scaled % 100ULL;
    std::string text = std::to_string(whole);
    text += '.';
    if (frac < 10) text += '0';
    text += std::to_string(frac);
    return text;
}

const char* kUnknown = "\xE2\x80\x94"; // em dash, the UI's "unknown" glyph

// Ascending SI units; the value is placed in the largest tier it reaches.
struct Unit { double scale; const char* suffix; };

const Unit kDiffUnits[] = {{1e3, "K"}, {1e6, "M"}, {1e9, "G"}, {1e12, "T"}};

std::string render_si(double value, const Unit* units, std::size_t count,
                      std::size_t index) {
    // Rounding can carry the numeral into the next unit ("999.995" at the
    // K scale reads "1000.00"); re-check the tier so numeral and unit agree.
    while (index + 1 < count &&
           hundredths(value / units[index].scale) / 100ULL >= 1000ULL) {
        ++index;
    }
    return two_decimals(value / units[index].scale) + units[index].suffix;
}

} // namespace

bool parse_miner_info(const std::string& json_text, MinerInfo& out) {
    if (json_text.empty()) return false;

    json doc;
    try {
        doc = json::parse(json_text);
    } catch (const json::exception&) {
        return false;
    }
    if (!doc.is_object()) return false;

    MinerInfo info;

    info.hostname = string_or(doc, "hostname");
    if (info.hostname.empty()) {
        // Fallback only when the API omits the hostname entirely.
        const std::string ip = string_or(doc, "ipv4");
        info.hostname = ip.empty() ? "Bitaxe" : "Bitaxe " + ip;
    }
    info.asic = string_or(doc, "ASICModel");
    info.board_version = string_or(doc, "boardVersion");
    info.fw_version = string_or(doc, "version");

    // Temperatures: the firmware uses -1 for "not present"; -1, an absent
    // field and an implausible reading all map to the "unknown" sentinel,
    // so temp2 = -1 still means "no second chip".
    read_temp(doc, "temp", info.chip_temp);
    read_temp(doc, "temp2", info.chip2_temp);
    read_temp(doc, "vrTemp", info.vr_temp);

    double value = 0.0;
    if (read_number(doc, "hashRate", value)) info.hash_rate_gh = value;
    if (read_number(doc, "hashRate_10m", value)) info.hash_rate_10m = value;
    if (read_number(doc, "expectedHashrate", value)) info.expected_gh = value;
    if (read_number(doc, "power", value)) info.power = value;
    if (read_number(doc, "voltage", value)) info.voltage_mv = value;
    if (read_number(doc, "errorPercentage", value)) info.error_pct = value;
    if (read_number(doc, "responseTime", value)) info.pool_latency_ms = value;

    if (read_number(doc, "fanspeed", value)) info.fan_pct = static_cast<int>(value);
    if (read_number(doc, "fanrpm", value)) info.fan_rpm = static_cast<int>(value);

    // Requested frequency lives under "actualFrequency" on newer firmware;
    // "frequency" (the setting) is the fallback.
    double frequency = 0.0;
    if (read_number(doc, "actualFrequency", frequency) ||
        read_number(doc, "frequency", frequency)) {
        info.frequency_mhz = static_cast<int>(std::llround(frequency));
    }

    info.shares_accepted = int_or(doc, "sharesAccepted", -1);
    info.shares_rejected = int_or(doc, "sharesRejected", -1);
    info.best_diff = int_or(doc, "bestDiff", -1);
    info.best_session_diff = int_or(doc, "bestSessionDiff", -1);
    info.uptime_s = int_or(doc, "uptimeSeconds", -1);

    info.using_fallback = fallback_flag(doc);
    read_pool(doc, info);

    out = std::move(info); // written only after a successful parse
    return true;
}

double MinerInfo::display_hashrate() const {
    // 10-minute average first, instantaneous field as the fallback; -1 when
    // the parser set neither (the UI shows the unknown dash, never 0).
    if (hash_rate_10m >= 0) return hash_rate_10m;
    return hash_rate_gh;
}

std::string format_hashrate(double gh) {
    if (gh < 0.0) return kUnknown;
    if (gh >= 1000.0) return two_decimals(gh / 1000.0) + " TH/s";
    static const Unit kHashUnits[] = {{1.0, " GH/s"}, {1000.0, " TH/s"}};
    return render_si(gh, kHashUnits, 2, 0);
}

std::string format_uptime(std::int64_t seconds) {
    if (seconds < 0) return kUnknown;
    if (seconds < 60) return "<1m";
    const auto minutes = static_cast<unsigned long long>(seconds) / 60ULL;
    if (minutes < 60ULL) return std::to_string(minutes) + "m";
    const auto hours = minutes / 60ULL;
    if (hours < 24ULL) {
        const std::string mins = std::to_string(minutes % 60ULL);
        return std::to_string(hours) + "h" +
               (minutes % 60ULL < 10ULL ? "0" + mins : mins) + "m";
    }
    const auto days = hours / 24ULL;
    const auto rest = hours % 24ULL;
    const std::string hrs = std::to_string(rest);
    return std::to_string(days) + "d" +
           (rest < 10ULL ? "0" + hrs : hrs) + "h";
}

std::string format_diff(double best) {
    if (best < 0.0) return kUnknown;
    if (best < 1e3) {
        // Small difficulties render as plain integers.
        return std::to_string(
            static_cast<unsigned long long>(std::floor(best + 0.5)));
    }
    std::size_t index = 0;
    for (std::size_t i = 1; i < 4; ++i) {
        if (best >= kDiffUnits[i].scale) index = i;
    }
    return render_si(best, kDiffUnits, 4, index);
}

} // namespace bx
