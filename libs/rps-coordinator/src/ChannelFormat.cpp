#include <rps/coordinator/ChannelFormat.hpp>

#include <array>
#include <algorithm>

namespace rps::coordinator {

namespace {

struct FormatEntry {
    ChannelFormat format;
    std::string_view name;
};

constexpr std::array kFormatTable = {
    FormatEntry{ChannelFormat::Unknown,        "unknown"},
    FormatEntry{ChannelFormat::Mono,           "mono"},
    FormatEntry{ChannelFormat::Stereo,         "stereo"},
    FormatEntry{ChannelFormat::Stereo_2_1,     "2.1"},
    FormatEntry{ChannelFormat::LCR,            "lcr"},
    FormatEntry{ChannelFormat::LCRS,           "lcrs"},
    FormatEntry{ChannelFormat::Quad,           "quad"},
    FormatEntry{ChannelFormat::Surround_5_0,   "5.0"},
    FormatEntry{ChannelFormat::Surround_5_1,   "5.1"},
    FormatEntry{ChannelFormat::Surround_6_1,   "6.1"},
    FormatEntry{ChannelFormat::Surround_7_1,   "7.1"},
    FormatEntry{ChannelFormat::Atmos_5_1_2,    "5.1.2"},
    FormatEntry{ChannelFormat::Atmos_5_1_4,    "5.1.4"},
    FormatEntry{ChannelFormat::Atmos_7_1_2,    "7.1.2"},
    FormatEntry{ChannelFormat::Atmos_7_1_4,    "7.1.4"},
    FormatEntry{ChannelFormat::Atmos_7_1_6,    "7.1.6"},
    FormatEntry{ChannelFormat::Atmos_9_1_4,    "9.1.4"},
    FormatEntry{ChannelFormat::Atmos_9_1_6,    "9.1.6"},
    FormatEntry{ChannelFormat::Auro_9_1,       "auro_9.1"},
    FormatEntry{ChannelFormat::Auro_11_1,      "auro_11.1"},
    FormatEntry{ChannelFormat::Auro_13_1,      "auro_13.1"},
    FormatEntry{ChannelFormat::Ambisonics_FOA, "ambi_foa"},
    FormatEntry{ChannelFormat::Ambisonics_SOA, "ambi_soa"},
    FormatEntry{ChannelFormat::Ambisonics_TOA, "ambi_toa"},
    FormatEntry{ChannelFormat::Custom,         "custom"},
};

} // anonymous namespace

std::string_view channelFormatToString(ChannelFormat format) {
    for (const auto& entry : kFormatTable) {
        if (entry.format == format) return entry.name;
    }
    return "unknown";
}

ChannelFormat channelFormatFromString(std::string_view name) {
    for (const auto& entry : kFormatTable) {
        if (entry.name == name) return entry.format;
    }
    return ChannelFormat::Unknown;
}

bool areLayoutsCompatible(const ChannelLayout& source, const ChannelLayout& dest) {
    // Direct connection requires matching effective channel counts.
    // For routing mismatches, an explicit ChannelRouterNode or DownmixNode is needed.
    return source.effectiveChannelCount() == dest.effectiveChannelCount();
}

} // namespace rps::coordinator
