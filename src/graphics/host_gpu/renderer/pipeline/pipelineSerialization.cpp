#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"

#include "common/logging/log.h"
#include "common/parallelFor.h"
#include "common/stringUtils.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/recompiler/ir/Serialize.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <chrono>
#include <cstring>
#include <fmt/format.h>
#include <ranges>
#include <xxhash.h>
#include <zstd.h>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

// The shader store: a header, then zstd-compressed records of everything a run created first.
// Records are only appended, so a store cut short by a crash keeps every complete record.
namespace Libs::Graphics {

namespace {

using ShaderRecompiler::IR::IoAll;
using ShaderRecompiler::IR::Reader;
using ShaderRecompiler::IR::Writer;

enum class Record : uint8_t { Module, GraphicsPipeline, TessellationLayout };

// Precedes each record's zstd frame.
struct Frame {
	uint32_t compressed_size = 0;
	uint32_t raw_size        = 0;
	uint64_t checksum        = 0; // XXH3 of the compressed bytes.
};

// Hash the exact executable, not its filesystem timestamp. Identical binary copies can reuse
// records; changed binaries still invalidate them. Zero when the executable cannot be read.
uint64_t BuildStamp() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	std::array<wchar_t, 32768> name {};
	GetModuleFileNameW(nullptr, name.data(), static_cast<DWORD>(name.size()));
	const std::filesystem::path path = name.data();
#elif defined(__APPLE__)
	std::array<char, 4096> name {};
	auto size = static_cast<uint32_t>(name.size());
	_NSGetExecutablePath(name.data(), &size);
	const std::filesystem::path path = name.data();
#else
	const std::filesystem::path path = "/proc/self/exe";
#endif
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		return 0;
	}
	std::unique_ptr<XXH3_state_t, decltype(&XXH3_freeState)> state(XXH3_createState(), XXH3_freeState);
	if (!state || XXH3_64bits_reset(state.get()) != XXH_OK) {
		return 0;
	}
	std::vector<char> chunk(256 * 1024);
	while (file.read(chunk.data(), static_cast<std::streamsize>(chunk.size())) || file.gcount() != 0) {
		if (XXH3_64bits_update(state.get(), chunk.data(), static_cast<size_t>(file.gcount())) != XXH_OK) {
			return 0;
		}
	}
	return file.eof() && !file.bad() ? XXH3_64bits_digest(state.get()) : 0;
}

// The setting affects startup workers only. Zero or an invalid value uses the
// existing conservative default (half the logical CPUs).
size_t WarmupThreads() {
	const size_t available = std::max(1u, std::thread::hardware_concurrency());
	const char* value = std::getenv("KYTY_SHADER_THREADS");
	if (value != nullptr) {
		size_t requested = 0;
		const auto end = value + std::strlen(value);
		const auto result = std::from_chars(value, end, requested);
		if (result.ec == std::errc {} && result.ptr == end && requested != 0) {
			return std::min(requested, available);
		}
	}
	return Common::ParallelThreads();
}

// All jobs are independent. The calling thread reports progress and pumps
// window events; worker threads never touch SDL or mutable cache containers.
template <typename Job>
void CompileWarmupJobs(size_t count, size_t workers, std::string_view phase,
                       const ShaderWarmupProgress& progress, const Job& job) {
	if (count == 0) {
		return;
	}
	std::atomic_size_t next {0}, completed {0};
	std::mutex mutex;
	std::condition_variable ready;
	const auto report = [&](size_t done) {
		if (progress) {
			progress(phase, done, count);
		}
	};
	Log::WriteToConsoleAndLog(fmt::format("Shader precompile: {} ({} jobs, {} workers)\n",
	                                      phase, count, std::min(count, workers)));
	report(0);
	std::vector<std::jthread> threads;
	for (size_t i = 0; i < std::min(count, workers); ++i) {
		threads.emplace_back([&] {
			for (auto index = next.fetch_add(1); index < count; index = next.fetch_add(1)) {
				job(index);
				if (completed.fetch_add(1, std::memory_order_acq_rel) + 1 == count) {
					std::lock_guard lock(mutex);
					ready.notify_one();
				}
			}
		});
	}
	{
		std::unique_lock lock(mutex);
		while (completed.load(std::memory_order_acquire) != count) {
			ready.wait_for(lock, std::chrono::milliseconds(100));
			const auto done = completed.load(std::memory_order_acquire);
			lock.unlock();
			report(done);
			lock.lock();
		}
	}
	threads.clear(); // Join before any cache data is used or released.
	report(count);
	Log::WriteToConsoleAndLog(fmt::format("Shader precompile: {} complete ({}/{})\n",
	                                      phase, count, count));
}

// What every record depends on besides its own fields; empty when the build is unknown.
std::vector<uint8_t> StoreHeader(const GraphicContext& graphics) {
	const auto stamp = BuildStamp();
	if (stamp == 0) {
		return {};
	}
	Writer header;
	IoAll(header, stamp, SpirvProfile(graphics),
	      graphics.GetPhysicalDeviceProperties().limits.maxComputeSharedMemorySize);
	return std::move(header.bytes);
}

// Record fields, shared by writing and reading.
template <class S, class Key> void IoKey(S& io, Key& key) {
	auto& [stage, hash, user_data_count, static_state] = key;
	IoAll(io, stage, hash, user_data_count, static_state);
}

template <class S, class Module> void IoModule(S& io, Module& stage) {
	// The handle comes from the SPIR-V; stored programs have no function code.
	auto& [id, handle, info, specialization, function_code, rect_parameters, mesh_task] = stage;
	IoAll(io, id, info, specialization, rect_parameters, mesh_task);
}

} // namespace

void ShaderStoreWriter::Open(const std::filesystem::path& path, std::span<const uint8_t> header,
                             uint64_t kept) {
	Common::File::CreateDirectories(path.parent_path());
	if (kept == 0) {
		if (m_file.Create(path)) {
			m_file.Write(header.data(), static_cast<uint32_t>(header.size()));
		}
	} else if (m_file.Open(path, Common::File::Mode::ReadWrite)) {
		m_file.Truncate(kept);
		m_file.Seek(kept);
	}
}

ShaderStoreWriter::ShaderStoreWriter() {
	m_thread = std::jthread([this](std::stop_token stop) {
		std::unique_lock lock(m_mutex);
		// Once stop is requested the wait returns false only when the queue is empty.
		while (m_ready.wait(lock, stop, [this] { return !m_jobs.empty(); })) {
			const auto job = std::move(m_jobs.front());
			m_jobs.pop();
			lock.unlock();
			job();
			lock.lock();
		}
	});
}

void ShaderStoreWriter::Append(std::vector<uint8_t>&& record) {
	Post([this, record = std::move(record)] {
		std::vector<uint8_t> bytes(sizeof(Frame) + ZSTD_compressBound(record.size()));
		const auto size = ZSTD_compress(bytes.data() + sizeof(Frame), bytes.size() - sizeof(Frame),
		                                record.data(), record.size(), 3);
		EXIT_IF(ZSTD_isError(size));
		const Frame frame {.compressed_size = static_cast<uint32_t>(size),
		                   .raw_size        = static_cast<uint32_t>(record.size()),
		                   .checksum        = XXH3_64bits(bytes.data() + sizeof(Frame), size)};
		std::memcpy(bytes.data(), &frame, sizeof(frame));
		// A failed write leaves a partial record; the next run drops it and everything after it.
		m_file.Write(bytes.data(), static_cast<uint32_t>(sizeof(Frame) + size));
	});
}

void ShaderStoreWriter::Post(Common::UniqueFunction<void>&& job) {
	{
		std::scoped_lock lock(m_mutex);
		m_jobs.push(std::move(job));
	}
	m_ready.notify_one();
}

void PipelineCache::WarmUp(const std::filesystem::path& path, const ShaderWarmupProgress& progress) {
	if (path.empty()) {
		return;
	}
	const auto header = StoreHeader(m_graphics);
	if (header.empty()) {
		return;
	}
	const auto           start = std::chrono::steady_clock::now();
	const auto workers = WarmupThreads();
	std::vector<uint8_t> bytes;
	if (Common::File file;
	    Common::File::IsFileExisting(path) && file.Open(path, Common::File::Mode::Read)) {
		bytes.resize(file.Size());
		uint32_t read = 0;
		file.Read(bytes.data(), static_cast<uint32_t>(bytes.size()), &read);
		bytes.resize(read);
	}
	Log::WriteToConsoleAndLog(fmt::format(
	    "Shader cache: compiling shaders and pipelines from {} ({:.1f} MB) with {} threads\n",
	    Common::PathToString(path), static_cast<double>(bytes.size()) / 1'000'000.0,
	    workers));
	auto       records = std::span<const uint8_t>(bytes);
	const bool reusable =
	    records.size() >= header.size() && std::ranges::equal(records.first(header.size()), header);
	if (reusable) {
		records = records.subspan(header.size());
		// Decode records on the startup thread. Batch shader modules so the
		// extra uncompressed SPIR-V memory is bounded, and join each batch
		// before further insertions can move the modules in a program.
		std::vector<uint8_t> raw;
		std::vector<WarmupModule> pending;
		size_t batch_bytes = 0;
		const auto compile_batch = [&] {
			CompileWarmupJobs(pending.size(), workers, "Compiling cached shaders", progress,
			                  [&](size_t i) {
				auto& entry = pending[i];
				entry.program->modules[entry.index].module = CompileSPV(entry.spirv, m_graphics.device);
			});
			pending.clear();
			batch_bytes = 0;
		};
		while (true) {
			const auto before = pending.size();
			if (!LoadRecord(records, raw, &pending)) {
				break;
			}
			if (pending.size() != before) {
				batch_bytes += pending.back().spirv.size() * sizeof(uint32_t);
			}
			if (pending.size() >= 256 || batch_bytes >= 64 * 1024 * 1024) {
				compile_batch();
			}
		}
		compile_batch();
	}
	// A damaged record ends the store; the records before it stay.
	m_store.Open(path, header, reusable ? bytes.size() - records.size() : 0);

	const auto modules = MapModules();
	std::vector<Common::UniqueFunction<void>> jobs;
	jobs.reserve(m_graphics_pipelines.size() + modules.size());
	for (auto& node: m_graphics_pipelines) {
		jobs.emplace_back([this, &modules, &node] {
			CreateGraphicsPipeline(node.second, node.first, modules);
		});
	}
	// A compute pipeline depends only on its module.
	for (const auto* module: modules | std::views::values) {
		if (module != nullptr && module->info.stage == ShaderType::Compute) {
			jobs.emplace_back([this, module, &pipeline = m_compute_pipelines[module->id]] {
				CreatePipelineInternal(m_graphics, pipeline, *module, m_driver_cache);
			});
		}
	}
	CompileWarmupJobs(jobs.size(), workers, "Compiling cached pipelines", progress,
	                  [&](size_t i) { jobs[i](); });
	// A key with a stage missing from the store keeps no pipeline.
	std::erase_if(m_graphics_pipelines,
	              [](const auto& entry) { return entry.second.pipeline == nullptr; });
	if (!m_graphics_pipelines.empty() || !m_compute_pipelines.empty()) {
		CheckpointDriverCache();
	}
	Log::WriteToConsoleAndLog(fmt::format(
	    "Shader cache: loaded {} programs, {} modules, {} pipelines in {} ms\n",
	    m_program_cache.size(), modules.size() - 1,
	    m_graphics_pipelines.size() + m_compute_pipelines.size(),
	    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
	                                                          start)
	        .count()));
}

bool PipelineCache::LoadRecord(std::span<const uint8_t>& records, std::vector<uint8_t>& raw,
                               std::vector<WarmupModule>* pending) {
	Frame frame;
	if (records.size() < sizeof(frame)) {
		return false;
	}
	std::memcpy(&frame, records.data(), sizeof(frame));
	if (records.size() - sizeof(frame) < frame.compressed_size) {
		return false;
	}
	const auto compressed = records.subspan(sizeof(frame), frame.compressed_size);
	if (XXH3_64bits(compressed.data(), compressed.size()) != frame.checksum ||
	    ZSTD_getFrameContentSize(compressed.data(), compressed.size()) != frame.raw_size) {
		return false;
	}
	raw.resize(frame.raw_size);
	if (ZSTD_decompress(raw.data(), raw.size(), compressed.data(), compressed.size()) != raw.size()) {
		return false;
	}
	Reader     reader {raw};
	auto       type     = Record::Module;
	const auto complete = [&] { return reader.ok && reader.bytes.empty(); };
	IoAll(reader, type);
	switch (type) {
		case Record::Module: {
			ProgramKey            key;
			std::vector<uint8_t>  plan;
			ShaderModule          module;
			std::vector<uint32_t> spirv;
			IoKey(reader, key);
			IoAll(reader, plan);
			IoModule(reader, module);
			IoAll(reader, spirv);
			if (!complete()) {
				return false;
			}
			const auto [program, inserted] = m_program_cache.try_emplace(std::move(key));
			if (inserted &&
			    !ShaderRecompiler::IR::LoadResourcePlan(plan, program->second.resource_plan)) {
				m_program_cache.erase(program);
				return false;
			}
			if (pending != nullptr) {
				const auto index = program->second.modules.size();
				module.module = nullptr;
				program->second.modules.push_back(std::move(module));
				pending->push_back({&program->second, index, std::move(spirv)});
			} else {
				module.module = CompileSPV(spirv, m_graphics.device);
				program->second.modules.push_back(std::move(module));
			}
			break;
		}
		case Record::GraphicsPipeline: {
			GraphicsPipelineKey key;
			reader.Raw(&key, sizeof(key));
			if (!complete()) {
				return false;
			}
			// WarmUp creates the pipeline once every record is loaded.
			m_graphics_pipelines.try_emplace(key);
			break;
		}
		case Record::TessellationLayout: {
			uint64_t                    key = 0;
			ShaderTessellationInputInfo layout;
			IoAll(reader, key, layout);
			if (!complete()) {
				return false;
			}
			m_tess_layouts.try_emplace(key, layout);
			break;
		}
		default: return false;
	}
	records = records.subspan(sizeof(frame) + compressed.size());
	return true;
}

// Records are encoded here, since programs and modules change later.
void PipelineCache::StoreModule(const ProgramKey& key, const Program& program,
                                const ShaderModule& module, const std::vector<uint32_t>& spirv) {
	if (!m_store) {
		return;
	}
	Writer record;
	IoAll(record, Record::Module);
	IoKey(record, key);
	IoAll(record, ShaderRecompiler::IR::SaveResourcePlan(program.resource_plan));
	IoModule(record, module);
	IoAll(record, spirv);
	m_store.Append(std::move(record.bytes));
}

void PipelineCache::StorePipeline(const GraphicsPipelineKey& key) {
	if (!m_store) {
		return;
	}
	Writer record;
	IoAll(record, Record::GraphicsPipeline);
	record.Raw(&key, sizeof(key));
	m_store.Append(std::move(record.bytes));
}

void PipelineCache::StoreLayout(uint64_t key, const ShaderTessellationInputInfo& layout) {
	if (!m_store) {
		return;
	}
	Writer record;
	IoAll(record, Record::TessellationLayout, key, layout);
	m_store.Append(std::move(record.bytes));
}

PipelineCache::ModuleMap PipelineCache::MapModules() const {
	ModuleMap modules {{0, nullptr}};
	for (const auto& program: m_program_cache | std::views::values) {
		for (const auto& module: program.modules) {
			modules.emplace(module.id, &module);
		}
	}
	return modules;
}

bool PipelineCache::CreateGraphicsPipeline(Pipeline& pipeline, const GraphicsPipelineKey& key,
                                           const ModuleMap& modules) {
	std::array<const ShaderModule*, 4> stages {};
	for (size_t i = 0; i < stages.size(); i++) {
		const auto found = modules.find(key.stage_ids[i]);
		if (found == modules.end()) {
			return false;
		}
		stages[i] = found->second;
	}
	CreatePipelineInternal(m_graphics, pipeline, key, stages, m_driver_cache);
	return true;
}

} // namespace Libs::Graphics
