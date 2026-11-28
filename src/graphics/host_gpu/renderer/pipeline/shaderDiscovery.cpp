#include "graphics/host_gpu/renderer/pipeline/shaderDiscovery.h"

#include "common/archive.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <map>
#include <span>
#include <vector>
#include <fmt/format.h>
#include <xxhash.h>

namespace Libs::Graphics {
namespace {

constexpr size_t FooterSize = 0x30;
constexpr uint64_t MaxShaderSize = 4 * 1024 * 1024;
constexpr uint32_t ChunkSize = 8 * 1024 * 1024;
constexpr size_t MaxFiles = 1000000;
constexpr size_t MaxHashesPerFile = 100000;

// Match shader.cpp's version-1 barefoot footer. The candidate assumes there
// is no extra padding between the literal buffer and footer. Only a matching
// runtime hash establishes its identity; never feed speculative bytes to
// the assert-based guest decoder.
constexpr uint64_t CandidateSize(uint32_t version, uint32_t code, uint32_t literals) {
    const auto size = ((uint64_t{code} + 15) & ~uint64_t{15}) + literals;
    return version == 1 && code != 0 && code % 4 == 0 &&
           literals % 4 == 0 && size <= MaxShaderSize ? size : 0;
}
static_assert(CandidateSize(1, 20, 4) == 36);
static_assert(CandidateSize(0, 20, 4) == 0);
static_assert(CandidateSize(1, 0, 4) == 0);
static_assert(CandidateSize(1, 21, 4) == 0);
static_assert(CandidateSize(1, UINT32_MAX - 3, 4) == 0);

struct Entry {
    uint64_t size = 0;
    int64_t stamp = 0;
    std::vector<uint64_t> hashes;
};
using Index = std::map<std::string, Entry>;

Index ReadIndex(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::string magic;
    if (!(in >> magic) || magic != "KytyShaderDiscovery1") return {};
    Index index;
    std::string name;
    Entry entry;
    size_t count = 0;
    while (in >> std::quoted(name) >> entry.size >> entry.stamp >> count) {
        if (index.size() >= MaxFiles || count > MaxHashesPerFile) return {};
        entry.hashes.resize(count);
        for (auto& hash: entry.hashes) {
            if (!(in >> hash)) return {};
        }
        index.insert_or_assign(name, std::move(entry));
        entry = {};
    }
    return in.eof() ? std::move(index) : Index{};
}

bool WriteIndex(const std::filesystem::path& path, const Index& index) {
    auto temporary = path;
    temporary += ".tmp";
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << "KytyShaderDiscovery1\n";
    for (const auto& [name, entry]: index) {
        out << std::quoted(name) << ' ' << entry.size << ' ' << entry.stamp
            << ' ' << entry.hashes.size();
        for (auto hash: entry.hashes) out << ' ' << hash;
        out << '\n';
    }
    out.close();
    if (!out) return false;
    return Common::File::RenameFile(temporary, path);
}

bool ScanFile(const std::filesystem::path& path, Entry& entry) {
    if (entry.size < FooterSize) return true;
    Common::File file;
    if (!file.Open(path, Common::File::Mode::Read) || file.Size() != entry.size) return false;
    std::vector<uint8_t> bytes(ChunkSize + FooterSize - 1);
    std::vector<uint8_t> program;
    for (uint64_t offset = 0; offset < entry.size;) {
        const auto body = static_cast<uint32_t>(std::min<uint64_t>(ChunkSize, entry.size - offset));
        const auto want = static_cast<uint32_t>(
            std::min<uint64_t>(bytes.size(), entry.size - offset));
        uint32_t read = 0;
        if (!file.Seek(offset)) return false;
        file.Read(bytes.data(), want, &read);
        if (read != want) return false;
        if (read >= FooterSize) {
            const auto limit = std::min<size_t>(body, read - FooterSize + 1);
            size_t pos = 0;
            while (pos < limit) {
                const auto* hit = static_cast<const uint8_t*>(
                    std::memchr(bytes.data() + pos, 'b', limit - pos));
                if (hit == nullptr) break;
                pos = static_cast<size_t>(hit - bytes.data()) + 1;
                if (std::memcmp(hit, "barefoot", 8) != 0) continue;
                uint32_t fields[3] {};
                std::memcpy(fields, hit + 0x10, sizeof(fields));
                const auto length = CandidateSize(fields[0], fields[1], fields[2]);
                const auto footer = offset + static_cast<uint64_t>(hit - bytes.data());
                if (length == 0 || length > footer) continue;
                if (entry.hashes.size() >= MaxHashesPerFile) return false;
                program.resize(static_cast<size_t>(length));
                uint32_t got = 0;
                if (!file.Seek(footer - length)) return false;
                file.Read(program.data(), static_cast<uint32_t>(length), &got);
                if (got != length) return false;
                entry.hashes.push_back(XXH3_64bits(program.data(), program.size()));
            }
        }
        offset += body;
    }
    std::sort(entry.hashes.begin(), entry.hashes.end());
    entry.hashes.erase(std::unique(entry.hashes.begin(), entry.hashes.end()), entry.hashes.end());
    return true;
}

} // namespace

std::unordered_set<uint64_t> DiscoverGameShaders(const std::filesystem::path& root,
                                                const std::filesystem::path& index_path) {
    std::unordered_set<uint64_t> hashes;
    if (root.empty() || !Common::File::IsDirectoryExisting(root)) return hashes;
    const auto old = ReadIndex(index_path);
    Index next;
    struct Directory { std::filesystem::path path; unsigned depth; };
    std::vector<Directory> directories {{root, 0}};
    uint64_t scanned_bytes = 0;
    size_t scanned = 0, reused = 0, skipped = 0, visited = 0;
    const auto start = std::chrono::steady_clock::now();
    auto last_log = start;
    Log::WriteToConsoleAndLog(fmt::format(
        "Shader discovery: scanning {} before guest execution\n", Common::PathToString(root)));
    while (!directories.empty() && visited < MaxFiles) {
        auto dir = std::move(directories.back());
        directories.pop_back();
        if (dir.depth > 64) { ++skipped; continue; }
        for (const auto& item: Common::File::GetDirEntries(dir.path)) {
            if (++visited > MaxFiles) break;
            if (item.name.empty() || item.name == "." || item.name == "..") continue;
            const auto leaf = Common::PathFromUtf8(item.name);
            if (leaf.has_root_path() || leaf.has_parent_path()) { ++skipped; continue; }
            const auto path = dir.path / leaf;
            if (!Common::IsArchivePath(path)) {
                std::error_code error;
                if (std::filesystem::is_symlink(std::filesystem::symlink_status(path, error)) ||
                    error) { ++skipped; continue; }
            }
            if (!item.is_file) {
                directories.push_back({path, dir.depth + 1});
                continue;
            }
            const auto info = Common::File::GetInfo(path);
            if (!info || !info->is_file) { ++skipped; continue; }
            const auto times = Common::File::GetTimes(path);
            const auto name = Common::PathToGenericString(path);
            Entry entry {info->size, times ? times->write_ns : 0, {}};
            const auto previous = old.find(name);
            if (times && entry.stamp != 0 && previous != old.end() &&
                previous->second.size == entry.size && previous->second.stamp == entry.stamp) {
                entry.hashes = previous->second.hashes;
                ++reused;
            } else {
                if (!ScanFile(path, entry)) { ++skipped; continue; }
                ++scanned;
                scanned_bytes += entry.size;
            }
            hashes.insert(entry.hashes.begin(), entry.hashes.end());
            next.insert_or_assign(name, std::move(entry));
            const auto now = std::chrono::steady_clock::now();
            if (now - last_log >= std::chrono::seconds(5)) {
                Log::WriteToConsoleAndLog(fmt::format(
                    "Shader discovery: {} files scanned, {} reused, {:.1f} MiB, {} candidates\n",
                    scanned, reused, scanned_bytes / 1048576.0, hashes.size()));
                last_log = now;
            }
        }
    }
    Common::File::CreateDirectories(index_path.parent_path());
    if (!WriteIndex(index_path, next)) {
        Log::WriteToConsoleAndLog("Shader discovery: could not save index; continuing\n");
    }
    Log::WriteToConsoleAndLog(fmt::format(
        "Shader discovery: {} unique candidates, {} files scanned, {} reused, {} skipped in {} ms\n",
        hashes.size(), scanned, reused, skipped,
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count()));
    return hashes;
}

} // namespace Libs::Graphics
