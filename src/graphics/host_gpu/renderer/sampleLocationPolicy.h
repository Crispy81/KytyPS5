#pragma once
#include <cstdint>

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

} // namespace Libs::Graphics
