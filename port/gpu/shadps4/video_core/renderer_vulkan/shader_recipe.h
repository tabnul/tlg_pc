// SPDX-FileCopyrightText: Copyright 2026 tlg_pc contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// tlg: shader recipes. Every shader translation also stores its inputs (program code, user data,
// runtime info, bindings, the flattened user data and the guest memory the compiler read:
// fetch shaders, resource descriptors) in Storage::RecipeDir(). A cache that lacks a program
// (new install, new GPU or driver, a build with another recompiler) gets it translated again from
// its recipe at start, in a child process (tlg-probe --import-shaders) that maps the recorded
// memory at its guest addresses. The game then loads it like any cached shader.
// Ported from deadinside28/bloodborne_pc (docs/shader_recipes.md, render/shader_capture.cpp,
// render/shader_rebuild.cpp), thanks to @deadinside28.
#pragma once

#include <filesystem>
#include <span>
#include <vector>

#include "common/types.h"

namespace Shader {
struct Info;
struct RuntimeInfo;
namespace Backend {
struct Bindings;
}
} // namespace Shader

namespace Vulkan::ShaderRecipe {

/// Starts recording the guest memory the compiler reads on this thread.
void Begin();
/// Ends recording; the recipe of the translation since Begin(). `lookup_runtime` and `start` are
/// the runtime info and bindings as the lookup passed them (before translation adjusted them),
/// `info` the translated info (its flattened user data). Empty when nothing should be kept.
std::vector<u8> Take(const Shader::Info& info, std::span<const u32> code,
                     const Shader::RuntimeInfo& lookup_runtime,
                     const Shader::Backend::Bindings& start, size_t perm_idx);
/// Stores a recipe from Take() (no-op when empty or recipes are off).
void Save(u64 pgm_hash, size_t perm_idx, std::vector<u8>&& recipe);

/// The game's side at start: runs the import child for `cache_dir` when RecipeDir() has
/// programs the cache lacks, and waits for it. True when the child ran without errors.
bool ImportMissing(const std::filesystem::path& cache_dir);

/// The child (tlg-probe --import-shaders CACHE RECIPES): translates every recipe whose program
/// `cache_dir` does not have with the cache's profile.bin and writes its .spv and .meta, then
/// copies the recipe folder's pipeline keys the cache lacks. 0 on success.
int ImportChild(const std::filesystem::path& cache_dir, const std::filesystem::path& recipe_dir);

} // namespace Vulkan::ShaderRecipe
