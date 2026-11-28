#pragma once
#include <cstdint>

namespace Libs::Graphics {
struct SharedBufferPolicy {
	static constexpr uint64_t MiB = 1024 * 1024;
	static constexpr uint64_t MinSize = 4 * MiB;
	static constexpr uint64_t MaxSize = 64 * MiB;
	static constexpr uint64_t Budget = 256 * MiB;
	static constexpr uint32_t HotThreshold = 32;
	static constexpr bool Eligible(uint64_t size, uint64_t reserved, uint32_t faults) {
		return size >= MinSize && size <= MaxSize && reserved <= Budget &&
		       size <= Budget - reserved && faults >= HotThreshold;
	}
};
}
