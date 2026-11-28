#pragma once
#include <algorithm>
#include <cstdint>

namespace Libs::Graphics {

// A BC texel block and an uncompressed texel may contain exactly the same
// bytes, but their image coordinates are in different units.
constexpr uint32_t EncodedBlockCount(uint32_t texels, uint32_t block) noexcept {
	return texels / block + (texels % block != 0);
}

constexpr uint32_t SharedEncodedBlocks(uint32_t source_texels, uint32_t source_block,
                                       uint32_t target_texels, uint32_t target_block) noexcept {
	return std::min(EncodedBlockCount(source_texels, source_block),
	                EncodedBlockCount(target_texels, target_block));
}

// Clamp the last block to the actual mip edge. The intermediate buffer still
// contains the entire encoded block, even for a 1x1 BC mip.
constexpr uint32_t EncodedCopyTexels(uint32_t blocks, uint32_t block,
                                     uint32_t texels) noexcept {
	return static_cast<uint32_t>(std::min<uint64_t>(
	    static_cast<uint64_t>(blocks) * block, texels));
}

} // namespace Libs::Graphics
