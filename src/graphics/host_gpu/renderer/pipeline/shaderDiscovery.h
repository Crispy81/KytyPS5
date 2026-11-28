#pragma once

#include <cstdint>
#include <filesystem>
#include <unordered_set>

namespace Libs::Graphics {

// Best-effort candidate inventory. Unknown stage/resource state is not compiled.
std::unordered_set<uint64_t> DiscoverGameShaders(const std::filesystem::path& root,
                                                const std::filesystem::path& index_path);
}
