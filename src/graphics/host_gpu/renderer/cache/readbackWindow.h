#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_READBACKWINDOW_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_READBACKWINDOW_H_

#include "common/alignment.h"

#include <algorithm>
#include <cstdint>

namespace Libs::Graphics {

class ReadbackWindow {
public:
	static constexpr uint64_t BaseSize  = 512 * 1024;
	static constexpr uint64_t LocalSize = 4 * BaseSize;
	static constexpr uint64_t HotSize   = 4 * LocalSize;

	struct Range {
		uint64_t begin;
		uint64_t end;
		bool     expanded;
	};

	// The request is nonempty, ends on a tracker page boundary and fits in the owner.
	[[nodiscard]] Range Select(uint64_t base, uint64_t limit, uint64_t address,
	                           uint64_t required_end, uint64_t tick, bool is_write) {
		const auto base_window = Common::AlignDown(address, BaseSize);
		const auto base_begin  = std::max(base_window, base);
		const auto base_end    = base_begin + std::min(BaseSize, limit - base_begin);
		const auto local_group = Common::AlignDown(address, LocalSize);
		const auto hot_group   = Common::AlignDown(address, HotSize);

		const bool same_owner =
		    m_has_previous && m_owner_begin == base && m_owner_end == limit && tick >= m_tick;
		const bool recent     = same_owner && tick - m_tick <= 32;
		const bool same_local = Common::AlignDown(m_address, LocalSize) == local_group;
		const bool nearby =
		    recent && same_local && Common::AlignDown(m_address, BaseSize) != base_window;
		const bool consecutive = recent && !m_was_write && !is_write && m_end == base_begin;
		m_adjacent_reads       = consecutive ? std::min(m_adjacent_reads + 1u, 3u) : 0;

		// Prefetch across neighboring 2 MiB groups only after repeated cross-group faults.
		// Successive faults within one group reduce confidence in cross-group prefetch.
		const bool hot_recent = same_owner && tick - m_tick <= 128 && limit - base >= HotSize &&
		                        Common::AlignDown(m_address, HotSize) == hot_group;
		if (!hot_recent) {
			m_cross_group_hits = 0;
		} else if (!same_local) {
			m_cross_group_hits = std::min(m_cross_group_hits + 1u, 3u);
		} else if (m_cross_group_hits > 0) {
			--m_cross_group_hits;
		}

		auto begin = base_begin;
		auto end   = base_end;
		if (m_cross_group_hits >= 2) {
			begin = std::max(hot_group, base);
			end   = hot_group + std::min(HotSize, limit - hot_group);
		} else if (nearby) {
			begin = std::max(local_group, base);
			end   = local_group + std::min(LocalSize, limit - local_group);
		} else if (m_adjacent_reads >= 2) {
			const auto width = m_adjacent_reads == 2 ? 2 * BaseSize : LocalSize;
			end              = begin + std::min(width, limit - begin);
		}

		if (required_end > end) {
			// A request crossing the prediction's end keeps the original start.
			begin = base_begin;
			end   = std::max(base_end, required_end);
		}

		m_owner_begin  = base;
		m_owner_end    = limit;
		m_address      = address;
		m_end          = end;
		m_tick         = tick;
		m_was_write    = is_write;
		m_has_previous = true;
		return {begin, end, end - begin > std::max(base_end, required_end) - base_begin};
	}

private:
	uint64_t m_owner_begin      = 0;
	uint64_t m_owner_end        = 0;
	uint64_t m_address          = 0;
	uint64_t m_end              = 0;
	uint64_t m_tick             = 0;
	uint32_t m_adjacent_reads   = 0;
	uint32_t m_cross_group_hits = 0;
	bool     m_was_write        = false;
	bool     m_has_previous     = false;
};

} // namespace Libs::Graphics

#endif
