#pragma once
#include <cstdint>
#include <array>
#include <span>
#include <bit>

namespace Libs::Graphics {

// Sample coordinates are signed 4-bit offsets in sixteenths of a pixel.
constexpr float DecodeSampleCoordinate(uint32_t nibble) noexcept {
	const int value = static_cast<int>(nibble & 15u);
	return 0.5f + static_cast<float>(value < 8 ? value : value - 16) / 16.0f;
}

// The PS_ITER_SAMPLES register is a base-2 exponent, while Vulkan takes a
// fraction of raster samples. Sample-qualified shader inputs still require all.
constexpr uint32_t PixelShadingSamples(uint32_t samples, uint32_t exponent,
                                      bool sample_qualified) noexcept {
	if (samples <= 1) return 1;
	if (sample_qualified || exponent >= 32) return samples;
	const uint32_t requested = 1u << exponent;
	return requested < samples ? requested : samples;
}

// Dynamic rendering requires identical color attachment counts. AMD mixed
// attachments permit depth/stencil to retain a higher sample count than color.
constexpr bool CanPreserveMixedSamples(bool supported, uint32_t raster, uint32_t depth,
                                       std::span<const uint32_t> colors) noexcept {
	if (!supported || colors.empty() || !std::has_single_bit(depth) ||
	    !std::has_single_bit(colors.front()) || depth > 64 || depth != raster ||
	    colors.front() >= depth) {
		return false;
	}
	for (const auto samples: colors) {
		if (samples != colors.front()) return false;
	}
	return true;
}

// Four-sample Vulkan standard pattern, encoded as signed guest 4-bit offsets.
inline constexpr uint32_t StandardFourSampleWord = 0x62'2a'e6'aeu;
using SampleLocationWords = std::array<uint32_t, 16>;

constexpr SampleLocationWords SelectDepthSampleWords(const SampleLocationWords& previous,
    const SampleLocationWords& requested, bool can_discard_depth) noexcept {
	return can_discard_depth ? requested : previous;
}

static_assert(PixelShadingSamples(4, 0, false) == 1);
static_assert(PixelShadingSamples(4, 1, false) == 2);
static_assert(PixelShadingSamples(4, 2, false) == 4);
static_assert(PixelShadingSamples(4, 0, true) == 4);
static_assert(DecodeSampleCoordinate(8) == 0.0f);
static_assert(DecodeSampleCoordinate(7) == 15.0f / 16.0f);
static_assert(CanPreserveMixedSamples(true, 4, 4, std::array<uint32_t, 2>{1, 1}));
static_assert(!CanPreserveMixedSamples(false, 4, 4, std::array<uint32_t, 2>{1, 1}));
static_assert(!CanPreserveMixedSamples(true, 4, 4, std::array<uint32_t, 2>{1, 2}));

} // namespace Libs::Graphics
