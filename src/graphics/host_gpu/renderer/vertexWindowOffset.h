#pragma once

namespace Libs::Graphics {

// PA_SU_SC_MODE_CNTL enables a signed post-transform raster window translation.
// Scissor registers have independent offset enables and are not changed here.
constexpr void ApplyVertexWindowOffset(float& x, float& y, bool enabled,
                                       int offset_x, int offset_y) noexcept {
	if (enabled) {
		x += static_cast<float>(offset_x);
		y += static_cast<float>(offset_y);
	}
}

} // namespace Libs::Graphics
