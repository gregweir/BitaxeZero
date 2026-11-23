// BitaxeZero - esp-miner HTTP API mapping (pure module, no I/O).
//
// Parses the JSON returned by GET /api/system/info on esp-miner firmware and
// renders hash rate / uptime / difficulty strings for the LVGL UI.
//
// Convention: any negative (by convention -1) numeric field means
// "unknown/absent" and must never be rendered as 0; the UI shows "—".

#ifndef BX_BITAXE_HPP
#define BX_BITAXE_HPP

#include <cstdint>
#include <string>

namespace bx {

// Snapshot of one Bitaxe miner as reported by esp-miner. Defaults mark every
// field unknown so a sparse or exotic firmware response still parses.
struct MinerInfo {
    std::string hostname;      // falls back to "Bitaxe <ip>" if API omits it
    std::string asic;          // ASICModel
    std::string board_version; // boardVersion
    std::string fw_version;    // version
    std::string pool_url;      // pools[0].stratumURL, else flat stratumURL
    std::string pool_user;     // pools[0].stratumUser, else flat stratumUser

    int pool_port = 0;
    bool using_fallback = false;

    double hash_rate_gh = -1;     // hashRate (rolling, GH/s)
    double hash_rate_10m = -1;    // hashRate_10m (GH/s)
    double expected_gh = -1;      // expectedHashrate (GH/s)
    double power = -1;            // watts
    double chip_temp = -1;        // temp (C); -1 also when firmware reports -1
    double chip2_temp = -1;       // temp2 (C); -1 means "no second chip"
    double vr_temp = -1;          // vrTemp (C)
    double voltage_mv = -1;       // mV as given by the firmware
    double error_pct = -1;        // errorPercentage
    double pool_latency_ms = -1;  // responseTime

    int fan_rpm = -1;
    int fan_pct = -1;             // fanspeed
    int frequency_mhz = -1;       // actualFrequency, fallback frequency

    std::int64_t shares_accepted = -1;
    std::int64_t shares_rejected = -1;
    std::int64_t best_diff = -1;
    std::int64_t best_session_diff = -1;
    std::int64_t uptime_s = -1;

    // The hash rate the farm UI displays: the 10-minute rolling average when
    // the firmware reports one (percent-scale jitter on the instantaneous
    // hashRate makes the hero total wander), the instantaneous field as the
    // fallback when no average was sent (older firmware), -1 when the parser
    // set neither. The detail screen keeps the instantaneous value so both
    // sources stay inspectable.
    double display_hashrate() const;
};

// Parses one /api/system/info document. Returns false on unparseable or empty
// input (out is then left untouched); tolerant of both firmware layouts
// (pools[] array on v2.13+, flat stratum fields on v2.12).
bool parse_miner_info(const std::string& json, MinerInfo& out);

// Hash rate formatting, two decimals, unit switches at 1000 GH/s:
// 1400.4205322 -> "1.40 TH/s", 999.5 -> "999.50 GH/s", unknown (-1) -> "—".
std::string format_hashrate(double gh);

// Uptime formatting: "<1m" below 60 s, "12m", "1h12m", "3d04h".
std::string format_uptime(std::int64_t seconds);

// Difficulty formatting from the raw field value: 9792755360 -> "9.79G",
// 208002877.9 -> "208.00M", unknown (-1) -> "—".
std::string format_diff(double best);

} // namespace bx

#endif // BX_BITAXE_HPP
