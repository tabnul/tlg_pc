// SPDX-FileCopyrightText: Copyright 2026 tlg_pc contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// tlg: shader recipes, see shader_recipe.h. Ported from deadinside28/bloodborne_pc, thanks to
// @deadinside28.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <unordered_set>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "common/hash.h"
#include "common/logging/log.h"
#include "common/serdes.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/decode.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/specialization.h"
#include "video_core/amdgpu/regs_shader.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/shader_recipe.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"

namespace Vulkan::ShaderRecipe {

namespace {

constexpr u32 Magic = 0x52474c54; // TLGR
constexpr u32 Version = 1;

/// Guest memory the compiler read on this thread since Begin(), by address.
thread_local bool recording = false;
thread_local std::map<u64, std::vector<u8>> regions;

void Record(const void* address, size_t size) {
    if (!recording || !address) {
        return;
    }
    if (size == 0) {
        // A fetch shader: its length is where it returns (s_setpc_b64), as ParseFetchShader.
        const auto* code = static_cast<const u32*>(address);
        Shader::Gcn::GcnCodeSlice slice(code, code + std::numeric_limits<u32>::max());
        Shader::Gcn::GcnDecodeContext decoder;
        while (!slice.atEnd() && size < 4096) {
            const auto inst = decoder.decodeInstruction(slice);
            size += inst.length;
            if (inst.opcode == Shader::Gcn::Opcode::S_SETPC_B64) {
                break;
            }
        }
    }
    auto& bytes = regions[reinterpret_cast<u64>(address)];
    if (bytes.size() < size) {
        bytes.resize(size);
        AmdGpu::ReadShaderCode(address, bytes.data(), size);
    }
}

struct Writer {
    std::vector<u8> out;
    template <typename T>
    void Pod(const T& value) {
        const auto* p = reinterpret_cast<const u8*>(&value);
        out.insert(out.end(), p, p + sizeof(T));
    }
    void Bytes(const void* data, u64 size) {
        Pod(size);
        const auto* p = static_cast<const u8*>(data);
        out.insert(out.end(), p, p + size);
    }
    void Words(std::span<const u32> words) {
        Bytes(words.data(), words.size_bytes());
    }
};

struct Reader {
    std::span<const u8> in;
    size_t pos = 0;
    bool ok = true;
    template <typename T>
    T Pod() {
        T value{};
        if (pos + sizeof(T) > in.size()) {
            ok = false;
            return value;
        }
        std::memcpy(&value, in.data() + pos, sizeof(T));
        pos += sizeof(T);
        return value;
    }
    std::vector<u8> Bytes() {
        const auto size = Pod<u64>();
        if (!ok || size > in.size() - pos) {
            ok = false;
            return {};
        }
        std::vector<u8> bytes(in.begin() + pos, in.begin() + pos + size);
        pos += size;
        return bytes;
    }
    std::vector<u32> Words() {
        const auto bytes = Bytes();
        std::vector<u32> words(bytes.size() / sizeof(u32));
        std::memcpy(words.data(), bytes.data(), words.size() * sizeof(u32));
        return words;
    }
};

struct Recipe {
    Shader::HwStage hw_stage;
    Shader::SwStage sw_stage;
    u64 pgm_hash;
    u64 perm_idx;
    u64 pgm_base;
    std::vector<u32> user_data;
    std::vector<u32> vs_copy;
    std::vector<u32> flat;
    Shader::RuntimeInfo runtime;
    Shader::Backend::Bindings start;
    std::vector<std::pair<u64, std::vector<u8>>> regions;
};

std::optional<Recipe> Parse(std::span<const u8> data) {
    Reader r{data};
    if (r.Pod<u32>() != Magic || r.Pod<u32>() != Version ||
        r.Pod<u32>() != sizeof(Shader::RuntimeInfo) ||
        r.Pod<u32>() != sizeof(Shader::Backend::Bindings)) {
        return std::nullopt;
    }
    Recipe recipe{};
    recipe.hw_stage = r.Pod<Shader::HwStage>();
    recipe.sw_stage = r.Pod<Shader::SwStage>();
    recipe.pgm_hash = r.Pod<u64>();
    recipe.perm_idx = r.Pod<u64>();
    recipe.pgm_base = r.Pod<u64>();
    recipe.user_data = r.Words();
    recipe.vs_copy = r.Words();
    recipe.flat = r.Words();
    recipe.runtime = r.Pod<Shader::RuntimeInfo>();
    recipe.start = r.Pod<Shader::Backend::Bindings>();
    const auto count = r.Pod<u64>();
    for (u64 i = 0; r.ok && i < count; ++i) {
        const auto address = r.Pod<u64>();
        recipe.regions.emplace_back(address, r.Bytes());
    }
    if (!r.ok || recipe.user_data.size() != Shader::ShaderParams::NumShaderUserData) {
        return std::nullopt;
    }
    recipe.runtime.hw.gs.vs_copy = {};
    return recipe;
}

std::string RecipeName(u64 pgm_hash, size_t perm_idx) {
    return fmt::format("{:#018x}_{}", pgm_hash, perm_idx);
}

/// Guest pages mapped at their own addresses in the child, for one recipe.
class GuestMapping {
public:
    explicit GuestMapping(const std::vector<std::pair<u64, std::vector<u8>>>& regions) {
        constexpr u64 Page = 0x10000; // Windows' allocation granularity, also fine on Linux
        std::map<u64, u64> spans; // start -> end, page aligned and merged
        for (const auto& [address, bytes] : regions) {
            u64 start = address & ~(Page - 1);
            u64 end = (address + bytes.size() + Page - 1) & ~(Page - 1);
            auto it = spans.upper_bound(start);
            if (it != spans.begin() && std::prev(it)->second >= start) {
                --it;
                start = it->first;
            }
            while (it != spans.end() && it->first <= end) {
                end = std::max(end, it->second);
                it = spans.erase(it);
            }
            spans.emplace(start, end);
        }
        for (const auto& [start, end] : spans) {
            if (!Map(start, end - start)) {
                ok = false;
                return;
            }
            mapped.emplace_back(start, end - start);
        }
        for (const auto& [address, bytes] : regions) {
            std::memcpy(reinterpret_cast<void*>(address), bytes.data(), bytes.size());
        }
    }
    ~GuestMapping() {
        for (const auto& [address, size] : mapped) {
#ifdef _WIN32
            VirtualFree(reinterpret_cast<void*>(address), 0, MEM_RELEASE);
#else
            munmap(reinterpret_cast<void*>(address), size);
#endif
        }
    }
    bool ok = true;

private:
    static bool Map(u64 address, u64 size) {
#ifdef _WIN32
        return VirtualAlloc(reinterpret_cast<void*>(address), size, MEM_RESERVE | MEM_COMMIT,
                            PAGE_READWRITE) == reinterpret_cast<void*>(address);
#else
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
        void* p = mmap(reinterpret_cast<void*>(address), size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p == MAP_FAILED) {
            return false;
        }
        if (p != reinterpret_cast<void*>(address)) { // a kernel without MAP_FIXED_NOREPLACE
            munmap(p, size);
            return false;
        }
        return true;
#endif
    }
    std::vector<std::pair<u64, u64>> mapped;
};

std::vector<u8> ReadFile(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{file}, {}};
}

bool WriteFile(const std::filesystem::path& path, const void* data, size_t size) {
    auto temp = path;
    temp += ".tmp";
    {
        std::ofstream file{temp, std::ios::binary};
        file.write(static_cast<const char*>(data), std::streamsize(size));
        if (!file) {
            return false;
        }
    }
    std::error_code error;
    std::filesystem::rename(temp, path, error);
    return !error;
}

/// Programs (pgm_hash) with any shader binary in `dir`.
std::unordered_set<u64> CachedPrograms(const std::filesystem::path& dir) {
    std::unordered_set<u64> programs;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator{dir, error}) {
        if (entry.path().extension() == ".spv") {
            programs.insert(std::strtoull(entry.path().stem().string().c_str(), nullptr, 16));
        }
    }
    return programs;
}

struct RecipeFile {
    u64 pgm_hash;
    size_t perm_idx;
    std::filesystem::path path;
    bool operator<(const RecipeFile& other) const {
        return std::tie(pgm_hash, perm_idx) < std::tie(other.pgm_hash, other.perm_idx);
    }
};

/// Recipes of programs `cache_dir` lacks, per program in permutation order.
std::vector<RecipeFile> MissingRecipes(const std::filesystem::path& cache_dir,
                                       const std::filesystem::path& recipe_dir) {
    const auto cached = CachedPrograms(cache_dir);
    std::vector<RecipeFile> missing;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator{recipe_dir, error}) {
        if (entry.path().extension() != ".in") {
            continue;
        }
        const auto stem = entry.path().stem().string();
        const auto sep = stem.find('_');
        if (sep == std::string::npos) {
            continue;
        }
        const u64 hash = std::strtoull(stem.c_str(), nullptr, 16);
        if (!cached.contains(hash)) {
            missing.push_back({hash, size_t(std::strtoull(stem.c_str() + sep + 1, nullptr, 10)),
                               entry.path()});
        }
    }
    std::sort(missing.begin(), missing.end());
    return missing;
}

} // namespace

void Begin() {
    regions.clear();
    recording = true;
    Shader::guest_read_hook = &Record;
}

std::vector<u8> Take(const Shader::Info& info, std::span<const u32> code,
                     const Shader::RuntimeInfo& lookup_runtime,
                     const Shader::Backend::Bindings& start, size_t perm_idx) {
    Shader::guest_read_hook = nullptr;
    recording = false;
    auto read = std::move(regions);
    regions.clear();
    if (Storage::RecipeDir().empty() || info.translation_failed) {
        return {};
    }
    // Motion vertex shaders embed session-local buffer device addresses (never cached either).
    if (lookup_runtime.hw_stage == Shader::HwStage::Vertex &&
        lookup_runtime.hw.vs.motion_vectors) {
        return {};
    }
    const auto ud = info.UserData();
    if (ud.size() != Shader::ShaderParams::NumShaderUserData) {
        return {};
    }
    // The program itself, at its guest address: the info's base and inline constants use it.
    auto& program = read[info.pgm_base];
    if (program.size() < code.size_bytes()) {
        program.resize(code.size_bytes());
        std::memcpy(program.data(), code.data(), code.size_bytes());
    }

    Writer w;
    w.Pod(Magic);
    w.Pod(Version);
    w.Pod(u32(sizeof(Shader::RuntimeInfo)));
    w.Pod(u32(sizeof(Shader::Backend::Bindings)));
    w.Pod(info.hw_stage);
    w.Pod(info.sw_stage);
    w.Pod(info.pgm_hash);
    w.Pod(u64(perm_idx));
    w.Pod(u64(info.pgm_base));
    w.Words(ud);
    w.Words(lookup_runtime.hw_stage == Shader::HwStage::Geometry ? lookup_runtime.hw.gs.vs_copy
                                                                  : std::span<const u32>{});
    w.Words(info.FlatUserData());
    auto runtime = lookup_runtime;
    runtime.hw.gs.vs_copy = {};
    w.Pod(runtime);
    w.Pod(start);
    w.Pod(u64(read.size()));
    for (const auto& [address, bytes] : read) {
        w.Pod(address);
        w.Bytes(bytes.data(), bytes.size());
    }
    return std::move(w.out);
}

void Save(u64 pgm_hash, size_t perm_idx, std::vector<u8>&& recipe) {
    if (recipe.empty()) {
        return;
    }
    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderInput,
                                       RecipeName(pgm_hash, perm_idx), std::move(recipe));
}

bool ImportMissing(const std::filesystem::path& cache_dir) {
    const auto recipe_dir = Storage::RecipeDir();
    if (recipe_dir.empty() || MissingRecipes(cache_dir, recipe_dir).empty()) {
        return false;
    }
    // The child reads profile.bin, which the IO thread may still be writing.
    Storage::DataBase::Instance().Flush(std::chrono::seconds{5});
    const auto started = std::chrono::steady_clock::now();
    int code = -1;
#ifdef _WIN32
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring command = L"\"" + std::wstring{exe} + L"\" --import-shaders \"" +
                           cache_dir.wstring() + L"\" \"" + recipe_dir.wstring() + L"\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si,
                       &pi)) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD exit_code = 1;
        GetExitCodeProcess(pi.hProcess, &exit_code);
        code = int(exit_code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
#else
    const auto cache = cache_dir.string();
    const auto recipes = recipe_dir.string();
    if (const pid_t pid = fork(); pid == 0) {
        execl("/proc/self/exe", "tlg-probe", "--import-shaders", cache.c_str(), recipes.c_str(),
              static_cast<char*>(nullptr));
        _exit(127);
    } else if (pid > 0) {
        int status = 0;
        waitpid(pid, &status, 0);
        code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
#endif
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started)
                        .count();
    LOG_INFO(Render_Vulkan, "tlg: shader recipes imported in {} ms (exit {})", ms, code);
    std::printf("Shader recipes: import took %lld ms (exit %d)\n", static_cast<long long>(ms),
                code);
    return code == 0;
}

int ImportChild(const std::filesystem::path& cache_dir, const std::filesystem::path& recipe_dir) {
    const auto profile_data = ReadFile(cache_dir / "profile.bin");
    if (profile_data.size() != sizeof(Shader::Profile)) {
        std::fprintf(stderr, "import-shaders: no profile.bin in %s\n", cache_dir.string().c_str());
        return 2;
    }
    Shader::Profile profile{};
    std::memcpy(&profile, profile_data.data(), sizeof(profile));

    const auto started = std::chrono::steady_clock::now();
    Shader::Pools pools;
    u32 built = 0, skipped = 0;
    std::set<u64> failed; // a program whose first permutation failed keeps no later one
    for (const auto& file : MissingRecipes(cache_dir, recipe_dir)) {
        if (failed.contains(file.pgm_hash)) {
            ++skipped;
            continue;
        }
        const auto data = ReadFile(file.path);
        auto recipe = Parse(data);
        if (!recipe || recipe->pgm_hash != file.pgm_hash) {
            ++skipped;
            failed.insert(file.pgm_hash);
            continue;
        }
        GuestMapping mapping{recipe->regions};
        const auto program = std::ranges::find(recipe->regions, recipe->pgm_base,
                                               &std::pair<u64, std::vector<u8>>::first);
        if (!mapping.ok || program == recipe->regions.end()) {
            ++skipped;
            failed.insert(file.pgm_hash);
            continue;
        }
        if (!recipe->vs_copy.empty()) {
            recipe->runtime.hw.gs.vs_copy = recipe->vs_copy;
        }
        const Shader::ShaderParams params{
            .user_data = std::span<const u32, Shader::ShaderParams::NumShaderUserData>{
                recipe->user_data.data(), Shader::ShaderParams::NumShaderUserData},
            .code = {reinterpret_cast<const u32*>(recipe->pgm_base),
                     program->second.size() / sizeof(u32)},
            .hash = recipe->pgm_hash,
        };
        Shader::replay_flat = &recipe->flat;
        Shader::Info info{recipe->hw_stage, recipe->sw_stage, params};
        auto runtime = recipe->runtime;
        auto binding = recipe->start;
        std::vector<u32> spv;
        std::optional<Shader::StageSpecialization> spec;
        try {
            const auto ir = Shader::TranslateProgram(params.code, pools, info, runtime, profile);
            spec.emplace(info, recipe->runtime, profile, recipe->start);
            spv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime, ir, binding);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "import-shaders: %s: %s\n", file.path.filename().string().c_str(),
                         e.what());
        }
        Shader::replay_flat = nullptr;
        pools.ReleaseContents();
        if (spv.empty() || info.translation_failed) {
            ++skipped;
            failed.insert(file.pgm_hash);
            continue;
        }
        spec->info = &info;
        const auto name = RecipeName(file.pgm_hash, file.perm_idx);
        const auto meta = EncodeShaderMeta(info, *spec, HashCombine(file.pgm_hash, file.perm_idx),
                                           file.perm_idx);
        WriteFile(cache_dir / fmt::format("{:#018x}.meta",
                                          HashCombine(file.pgm_hash, file.perm_idx)),
                  meta.data(), meta.size());
        WriteFile(cache_dir / (name + ".spv"), spv.data(), spv.size() * sizeof(u32));
        ++built;
    }

    // Pipelines need their keys; the recipe folder keeps a copy of every key.
    u32 keys = 0;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator{recipe_dir, error}) {
        if (entry.path().extension() == ".key" &&
            !std::filesystem::exists(cache_dir / entry.path().filename())) {
            std::filesystem::copy_file(entry.path(), cache_dir / entry.path().filename(), error);
            keys += !error;
        }
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started)
                        .count();
    std::printf("import-shaders: %u shaders built, %u skipped, %u pipeline keys, %lld ms\n",
                built, skipped, keys, static_cast<long long>(ms));
    return 0;
}

} // namespace Vulkan::ShaderRecipe

extern "C" int tlg_import_shaders(const char* cache_dir, const char* recipe_dir) {
    return Vulkan::ShaderRecipe::ImportChild(std::filesystem::u8path(cache_dir),
                                             std::filesystem::u8path(recipe_dir));
}

// tlg: debug check (tlg-probe --compare-meta DIR_A DIR_B): loads every .meta of DIR_A and the
// same file in DIR_B as the game does and compares the fields the game uses, not raw bytes
// (unused list slots and padding hold leftover memory).
extern "C" int tlg_compare_meta(const char* dir_a, const char* dir_b) {
    namespace fs = std::filesystem;
    const auto read = [](const fs::path& p, Shader::StageSpecialization& spec, Shader::Info& info,
                         u64& perm_hash, size_t& perm_idx) {
        std::ifstream file{p, std::ios::binary};
        std::vector<u8> data{std::istreambuf_iterator<char>{file}, {}};
        if (data.empty()) {
            return false;
        }
        Serialization::Archive ar{std::move(data)};
        Serialization::Reader meta{ar};
        u32 meta_version{}, binary_version{};
        meta.Read(meta_version);
        meta.Read(binary_version);
        meta.Read(perm_hash);
        meta.Read(perm_idx);
        spec.Deserialize(ar);
        return info.Deserialize(ar);
    };
    // Immediates count only for dwords not loaded from the flat buffer.
    const auto same_fetch = [](const auto& a, const auto& b) {
        if (a.offsets != b.offsets || a.load_mask != b.load_mask) {
            return false;
        }
        for (size_t i = 0; i < a.immediates.size(); ++i) {
            if (!(a.load_mask >> i & 1) && a.immediates[i] != b.immediates[i]) {
                return false;
            }
        }
        return true;
    };
    const auto same = [&](const auto& a, const auto& b) {
        using T = std::decay_t<decltype(a)>;
        if constexpr (std::is_same_v<T, Shader::BufferResource>) {
            return same_fetch(a.sharp_fetch, b.sharp_fetch) && a.used_types == b.used_types &&
                   a.buffer_type == b.buffer_type && a.is_written == b.is_written &&
                   a.is_formatted == b.is_formatted && a.post_op == b.post_op &&
                   a.post_op_dw1_mask == b.post_op_dw1_mask;
        } else if constexpr (std::is_same_v<T, Shader::ImageResource>) {
            return same_fetch(a.sharp_fetch, b.sharp_fetch) && a.is_depth == b.is_depth &&
                   a.is_atomic == b.is_atomic && a.is_array == b.is_array &&
                   a.is_written == b.is_written && a.is_r128 == b.is_r128 &&
                   a.needs_native == b.needs_native &&
                   a.constant_mip_index == b.constant_mip_index &&
                   a.mip_fallback_mode == b.mip_fallback_mode && a.post_op == b.post_op;
        } else if constexpr (std::is_same_v<T, Shader::SamplerResource>) {
            return same_fetch(a.sharp_fetch, b.sharp_fetch) && a.post_op == b.post_op &&
                   a.post_op_tsharp_dw3_off == b.post_op_tsharp_dw3_off &&
                   a.is_depth == b.is_depth;
        } else {
            return a.sharp_idx == b.sharp_idx;
        }
    };
    const auto same_list = [&](const auto& a, const auto& b) {
        if (a.size() != b.size()) {
            return false;
        }
        for (size_t i = 0; i < a.size(); ++i) {
            if (!same(a[i], b[i])) {
                return false;
            }
        }
        return true;
    };
    std::map<std::string, u32> diffs;
    u32 files = 0, equal = 0;
    for (const auto& entry : fs::directory_iterator{fs::u8path(dir_a)}) {
        if (entry.path().extension() != ".meta") {
            continue;
        }
        ++files;
        Shader::StageSpecialization sa, sb;
        Shader::Info ia, ib;
        u64 ha{}, hb{};
        size_t pa{}, pb{};
        if (!read(entry.path(), sa, ia, ha, pa) ||
            !read(fs::u8path(dir_b) / entry.path().filename(), sb, ib, hb, pb)) {
            ++diffs["unreadable"];
            continue;
        }
        std::vector<const char*> bad;
        const auto check = [&](bool ok, const char* what) {
            if (!ok) {
                bad.push_back(what);
            }
        };
        check(ha == hb && pa == pb, "perm hash/index");
        check(std::memcmp(&sa.start, &sb.start, sizeof(sa.start)) == 0, "spec.start");
        check(sa.runtime_info == sb.runtime_info, "spec.runtime_info");
        check(sa.bitset == sb.bitset, "spec.bitset");
        check(sa.fetch_shader_data == sb.fetch_shader_data, "spec.fetch_shader_data");
        check(sa.vs_attribs == sb.vs_attribs, "spec.vs_attribs");
        check(sa.buffers == sb.buffers, "spec.buffers");
        check(sa.images == sb.images, "spec.images");
        check(sa.fmasks == sb.fmasks, "spec.fmasks");
        check(sa.samplers == sb.samplers, "spec.samplers");
        check(same_list(ia.buffers, ib.buffers), "info.buffers");
        check(same_list(ia.images, ib.images), "info.images");
        check(same_list(ia.samplers, ib.samplers), "info.samplers");
        check(same_list(ia.fmasks, ib.fmasks), "info.fmasks");
        check(ia.ud_mask.mask == ib.ud_mask.mask, "info.ud_mask");
        check(ia.fetch_shader_sgpr_base == ib.fetch_shader_sgpr_base &&
                  ia.shared_memory_scratch_size == ib.shared_memory_scratch_size &&
                  ia.pgm_hash == ib.pgm_hash && ia.tess_consts_dword_offset == ib.tess_consts_dword_offset &&
                  ia.tess_consts_ptr_base == ib.tess_consts_ptr_base && ia.hw_stage == ib.hw_stage &&
                  ia.sw_stage == ib.sw_stage && ia.mrt_mask == ib.mrt_mask &&
                  ia.has_fetch_shader == ib.has_fetch_shader &&
                  ia.has_bitwise_xor == ib.has_bitwise_xor && ia.uses_dma == ib.uses_dma,
              "info.scalars");
        check(ia.flattened_ud_buf == ib.flattened_ud_buf, "info.flat");
        check(ia.srt_info.walker_func_size == ib.srt_info.walker_func_size &&
                  ia.srt_info.flattened_bufsize_dw == ib.srt_info.flattened_bufsize_dw,
              "info.srt");
        if (bad.empty()) {
            ++equal;
        }
        for (const char* what : bad) {
            if (diffs[what]++ == 0) {
                std::printf("first %s difference: %s\n", what, entry.path().filename().string().c_str());
            }
        }
    }
    std::printf("compare-meta: %u files, %u equal in every field\n", files, equal);
    for (const auto& [what, count] : diffs) {
        std::printf("  %s differs in %u\n", what.c_str(), count);
    }
    return equal == files ? 0 : 1;
}
