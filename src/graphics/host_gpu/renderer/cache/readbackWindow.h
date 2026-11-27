#pragma once

#include <algorithm>
#include <cstdint>

namespace Libs::Graphics {

// A bounded, forward-only readback window. Never omit requested bytes. A
// larger window is used only after a succession of adjacent faults in the
// same buffer; random/repeated accesses retain the original 512 KiB policy.
struct ReadbackWindow {
	static constexpr uint64_t BaseSize = 512 * 1024;
	static constexpr uint64_t MaxSize = 4 * BaseSize;
	struct Range { uint64_t begin, end; };

	uint64_t owner_begin = 0;
	uint64_t owner_end = 0;
	uint64_t previous_end = 0;
	uint64_t previous_tick = 0;
	uint32_t adjacent = 0;
	bool valid = false;

	// The caller guarantees a nonempty request fully contained in the buffer.
	Range Select(uint64_t base, uint64_t limit, uint64_t address, uint64_t size,
	             uint64_t tick, bool is_write) {
		const auto start = std::max(address & ~(BaseSize - 1), base);
		const bool follows = valid && owner_begin == base && owner_end == limit &&
		                     tick >= previous_tick && tick - previous_tick <= 32 &&
		                     start == previous_end && !is_write;
		adjacent = follows ? std::min(adjacent + 1u, 3u) : 0u;
		const uint64_t width = adjacent >= 3 ? MaxSize : adjacent >= 2 ? 2 * BaseSize : BaseSize;
		const auto end = std::max(start + std::min(width, limit - start), address + size);
		owner_begin = base;
		owner_end = limit;
		previous_end = end;
		previous_tick = tick;
		valid = !is_write;
		return {start, end};
	}
};

} // namespace Libs::Graphics
