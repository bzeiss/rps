#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace rps::coordinator {

// ---------------------------------------------------------------------------
// Named speaker layouts with unique sequential IDs.
// Channel count is NOT encoded in the enum value — use getChannelCount().
// ---------------------------------------------------------------------------

enum class ChannelFormat : uint32_t {
    Unknown        = 0,

    // Standard / Legacy
    Mono           = 1,
    Stereo         = 2,
    Stereo_2_1     = 3,       // L, R, LFE
    LCR            = 4,       // L, C, R
    LCRS           = 5,       // L, C, R, Cs (Dolby Pro Logic)
    Quad           = 6,       // L, R, Ls, Rs

    // Standard Surround
    Surround_5_0   = 7,       // L, C, R, Ls, Rs
    Surround_5_1   = 8,       // L, C, R, LFE, Ls, Rs
    Surround_6_1   = 9,       // L, C, R, LFE, Ls, Rs, Cs
    Surround_7_1   = 10,      // L, C, R, LFE, Ls, Rs, Lss, Rss

    // Immersive / Dolby Atmos (Base.LFE.Height)
    Atmos_5_1_2    = 11,      // 5.1 + 2 Top (Ltf, Rtf)
    Atmos_5_1_4    = 12,      // 5.1 + 4 Top (Ltf, Rtf, Ltr, Rtr)
    Atmos_7_1_2    = 13,      // 7.1 + 2 Top
    Atmos_7_1_4    = 14,      // 7.1 + 4 Top
    Atmos_7_1_6    = 15,      // 7.1 + 6 Top (Front, Middle, Rear)
    Atmos_9_1_4    = 16,      // 7.1 + Wides (Lw, Rw) + 4 Top
    Atmos_9_1_6    = 17,      // 9.1 + 6 Top

    // Auro-3D Formats
    Auro_9_1       = 18,      // 5.1 + 4 Height
    Auro_11_1      = 19,      // 5.1 + 5 Height + 1 VOG (Voice of God)
    Auro_13_1      = 20,      // 7.1 + 5 Height + 1 VOG

    // Ambisonics (Spherical Harmonics)
    Ambisonics_FOA = 21,      // 1st Order (4 channels: W, Y, Z, X)
    Ambisonics_SOA = 22,      // 2nd Order (9 channels)
    Ambisonics_TOA = 23,      // 3rd Order (16 channels)

    // Custom / Fallback
    Custom         = 0xFF,    // Arbitrary channel count (specified in ChannelLayout)
};

/// Resolve a named ChannelFormat to its channel count.
/// Returns 0 for Unknown and Custom (use ChannelLayout::channelCount for Custom).
constexpr uint32_t getChannelCount(ChannelFormat format) {
    switch (format) {
        case ChannelFormat::Unknown:        return 0;
        case ChannelFormat::Mono:           return 1;
        case ChannelFormat::Stereo:         return 2;
        case ChannelFormat::Stereo_2_1:     return 3;
        case ChannelFormat::LCR:            return 3;
        case ChannelFormat::LCRS:           return 4;
        case ChannelFormat::Quad:           return 4;
        case ChannelFormat::Surround_5_0:   return 5;
        case ChannelFormat::Surround_5_1:   return 6;
        case ChannelFormat::Surround_6_1:   return 7;
        case ChannelFormat::Surround_7_1:   return 8;
        case ChannelFormat::Atmos_5_1_2:    return 8;
        case ChannelFormat::Atmos_5_1_4:    return 10;
        case ChannelFormat::Atmos_7_1_2:    return 10;
        case ChannelFormat::Atmos_7_1_4:    return 12;
        case ChannelFormat::Atmos_7_1_6:    return 14;
        case ChannelFormat::Atmos_9_1_4:    return 14;
        case ChannelFormat::Atmos_9_1_6:    return 16;
        case ChannelFormat::Auro_9_1:       return 10;
        case ChannelFormat::Auro_11_1:      return 12;
        case ChannelFormat::Auro_13_1:      return 14;
        case ChannelFormat::Ambisonics_FOA: return 4;
        case ChannelFormat::Ambisonics_SOA: return 9;
        case ChannelFormat::Ambisonics_TOA: return 16;
        case ChannelFormat::Custom:         return 0;
        default:                            return 0;
    }
}

/// Aggregated channel layout: format + explicit channel count.
struct ChannelLayout {
    ChannelFormat format = ChannelFormat::Unknown;
    uint32_t channelCount = 0;

    /// Resolve the effective channel count (prefers named format, falls back to explicit count).
    [[nodiscard]] constexpr uint32_t effectiveChannelCount() const {
        uint32_t n = getChannelCount(format);
        return (n > 0) ? n : channelCount;
    }
};

/// Convert a ChannelFormat enum to its string name.
std::string_view channelFormatToString(ChannelFormat format);

/// Parse a string name to a ChannelFormat. Returns Unknown on failure.
ChannelFormat channelFormatFromString(std::string_view name);

/// Check if two channel layouts are compatible for direct connection
/// (same effective channel count, or one is a superset for routing).
bool areLayoutsCompatible(const ChannelLayout& source, const ChannelLayout& dest);

} // namespace rps::coordinator
