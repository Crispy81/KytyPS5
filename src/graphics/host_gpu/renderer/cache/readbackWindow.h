#pragma once

#include <algorithm>
#include <cstdint>

namespace Libs::Graphics {

// Bounded readback prediction. Preserve the forward-stream policy and also
// recognize faults scattered within one local 2 MiB region of the same buffer.
// Only dirty GPU bytes are downloaded by the caller. Never omit requested bytes.
struct ReadbackWindow {
	static constexpr uint64_t BaseSize = 512 * 1024;
	static constexpr uint64_t MaxSize = 4 * BaseSize;
	static constexpr uint64_t HotSize = 4 * MaxSize;
	struct Range { uint64_t begin, end; };

	uint64_t owner_begin = 0;
	uint64_t owner_end = 0;
	uint64_t previous_end = 0;
	uint64_t previous_tick = 0;
	uint32_t adjacent = 0;
	uint64_t previous_address = 0;
	uint64_t local_tick = 0;
	uint32_t local_hits = 0;
	bool local_valid = false;
	uint32_t hot_hits = 0;
	bool valid = false;

	// The caller guarantees a nonempty request fully contained in the buffer.
	Range Select(uint64_t base, uint64_t limit, uint64_t address, uint64_t size,
	             uint64_t tick, bool is_write) {
		const auto start = std::max(address & ~(BaseSize - 1), base);
		const auto group = address & ~(MaxSize - 1);
		const bool nearby = local_valid && owner_begin == base && owner_end == limit &&
		                    tick >= local_tick && tick - local_tick <= 32 &&
		                    (previous_address & ~(MaxSize - 1)) == group &&
		                    (previous_address & ~(BaseSize - 1)) != (address & ~(BaseSize - 1));
		local_hits = nearby ? std::min(local_hits + 1u, 2u) : 0u;
		// Larger allocations can be consumed in scattered 2 MiB regions. Promote
		// only after repeated cross-region faults inside the same 8 MiB group.
		// Repeated faults within the same region imply GPU re-dirtying, not useful
		// spatial prefetch, so decay the prediction rather than widen it.
		const auto hot_group = address & ~(HotSize - 1);
		const bool hot_recent = local_valid && owner_begin == base && owner_end == limit &&
		                        limit - base >= HotSize && tick >= local_tick &&
		                        tick - local_tick <= 128 &&
		                        (previous_address & ~(HotSize - 1)) == hot_group;
		if (!hot_recent) {
			hot_hits = 0;
		} else if ((previous_address & ~(MaxSize - 1)) != group) {
			hot_hits = std::min(hot_hits + 1u, 3u);
		} else if (hot_hits != 0) {
			--hot_hits;
		}
		previous_address = address;
		local_tick = tick;
		local_valid = true;
		const bool follows = valid && owner_begin == base && owner_end == limit &&
		                     tick >= previous_tick && tick - previous_tick <= 32 &&
		                     start == previous_end && !is_write;
		adjacent = follows ? std::min(adjacent + 1u, 3u) : 0u;
		const uint64_t width = adjacent >= 3 ? MaxSize : adjacent >= 2 ? 2 * BaseSize : BaseSize;
		// Include preceding dirty ranges as well as following ones once a second
		// nearby window faults. Writes still publish GPU data before CPU ownership.
		const bool hot = hot_hits >= 2;
		const auto begin = hot ? std::max(hot_group, base) :
		                   local_hits != 0 ? std::max(group, base) : start;
		const auto local_end = group + std::min(MaxSize, limit - group);
		const auto hot_end = hot_group + std::min(HotSize, limit - hot_group);
		const auto predicted_end = hot ? hot_end :
		                           local_hits != 0 ? local_end : start + std::min(width, limit - start);
		const auto end = std::max(predicted_end, address + size);
		owner_begin = base;
		owner_end = limit;
		previous_end = end;
		previous_tick = tick;
		valid = !is_write;
		return {begin, end};
	}
};

} // namespace Libs::Graphics
