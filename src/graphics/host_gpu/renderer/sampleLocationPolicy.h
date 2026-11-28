#pragma once
#include <cstdint>
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

} // namespace Libs::Graphics
