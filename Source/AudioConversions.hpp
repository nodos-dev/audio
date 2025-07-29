// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#pragma once

#include <cstdint>
#include <algorithm>

namespace nos::audio
{

/**
 * @brief Converts a shifted 24-bit integer sample to float
 * @param sample 32-bit integer with 24-bit sample stored in MSB (shifted left by 8 bits)
 * @return Float value normalized to [-1.0, 1.0] range
 */
constexpr float ShiftedInt24ToFloat(int32_t sample)
{
    int32_t sampleShifted = sample >> 8; // Convert 32-bit to 24-bit by shifting right
    return static_cast<float>(sampleShifted) / 8388607.0f; // Normalize to [-1.0, 1.0]
}

/**
 * @brief Converts a float sample to shifted 24-bit integer format
 * @param sample Float value in [-1.0, 1.0] range
 * @return 32-bit integer with 24-bit sample stored in MSB (shifted left by 8 bits)
 */
constexpr int32_t FloatToShiftedInt24(float sample)
{
    int32_t sample24bit = static_cast<int32_t>(sample * 8388607.0f);
    sample24bit = std::max(-8388608, std::min(8388607, sample24bit));
    return sample24bit << 8; // Shift to store as 32-bit with 24-bit sample in MSB
}

/**
 * @brief Converts a float sample to 24-bit integer format (not shifted)
 * @param sample Float value in [-1.0, 1.0] range
 * @return 24-bit integer value clamped to valid range
 */
constexpr int32_t FloatToInt24(float sample)
{
    int32_t sample24bit = static_cast<int32_t>(sample * 8388607.0f);
    return std::max(-8388608, std::min(8388607, sample24bit));
}

// Constants for 24-bit audio
namespace AudioConstants
{
    constexpr int32_t INT24_MAX = 8388607;      // 2^23 - 1
    constexpr int32_t INT24_MIN = -8388608;     // -2^23
    constexpr float INT24_MAX_FLOAT = 8388607.0f;
}

} // namespace nos::audio
