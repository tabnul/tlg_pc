// SPDX-License-Identifier: GPL-2.0-or-later
#include "tlg_settings.h"
#include "tlg_graphics.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace TlgSettings {

namespace {

const char* Path() {
    const char* env = std::getenv("TLG_CONFIG");
    return env && env[0] ? env : "tlg.ini";
}

using Effect = std::atomic<int> Values::*;
constexpr std::pair<const char*, Effect> EffectKeys[] = {
    {"bloom", &Values::bloom}, {"dof", &Values::dof}, {"motion_blur", &Values::motion_blur},
    {"vignette_add", &Values::vignette}, {"ssao", &Values::ssao},
    {"ssr", &Values::ssr},
    {"game_sharpen", &Values::game_sharpen}, {"shadow_distance", &Values::shadow_distance},
    {"trico_feathers", &Values::trico_feathers}, {"boy_wind", &Values::boy_wind}, {"tree_wind", &Values::tree_wind},
    {"fog_distance", &Values::fog_distance}, {"haze_fringe", &Values::haze_fringe},
    {"grass_smooth", &Values::grass_smooth}};

std::atomic<int>* EffectSetting(Values& v, const std::string& key) {
    for (const auto& [name, member] : EffectKeys)
        if (key == name) return &(v.*member);
    return nullptr;
}

float Clamp(float v, float lo, float hi) {
    return std::clamp(v, lo, hi);
}

// tlg: tlg.ini has the anti-aliasing row's upscaler (aa_row=1). Before it the launcher chose DLSS
// for DLAA, and files kept the FSR 3.1 default.
bool aa_row_file = false;
// ConfigureDlssSupport ran: a reload without DLSS keeps the FSR 3.1 fallback.
bool dlss_checked = false;
// dlss_supported in tlg.ini: the game's last DLSS check, for the launcher (-1: none).
int dlss_supported_file = -1;
// dlss_fg_supported in tlg.ini: the game's last DLSS frame generation check, for the launcher (-1: none).
int dlss_fg_supported_file = -1;
// The upscaler tlg.ini selected before a fallback to FSR 3.1 (-1: none): Save writes it back.
int selected_upscaler = -1;
// fsr4_default in tlg.ini: the one-time switch from FSR 3.1 to FSR 4 native AA on a card that
// runs FSR 4 ran (once per install; later choices stay).
bool fsr4_default_done = false;
// fsr4_supported in tlg.ini: the game's last FSR 4 check, for the launcher (-1: none).
int fsr4_supported_file = -1;

void DlssFallback(Values& v) {
    if (v.upscaler == UpscalerDlss && dlss_checked && !v.dlss_supported) FallBackToFsr3();
}

void Set(Values& v, const std::string& key, const std::string& value) {
    const float f = float(std::atof(value.c_str()));
    const int i = std::atoi(value.c_str());
    static const char* const overlay_keys[] = {"overlay_fps", "overlay_frametime", "overlay_upscaler",
                                               "overlay_gpu", "overlay_cpu", "overlay_ram", "overlay_vram"};
    for (unsigned item = 0; item < 7; ++item) {
        if (key == overlay_keys[item]) {
            const unsigned bit = 1u << item;
            v.overlay_items = i ? (v.overlay_items | bit) : (v.overlay_items & ~bit);
            return;
        }
    }
    if (key == "overlay_scale") {
        v.overlay_scale = std::clamp(i, 50, 300);
        return;
    }
    if (key == "overlay_corner") {
        v.overlay_corner = i >= 0 && i <= 3 ? i : 1;
        return;
    }
    if (key == "overlay_opacity") {
        v.overlay_opacity = std::clamp(i, 0, 100);
        return;
    }
    if (key == "overlay_layout") {
        v.overlay_layout = i == 1 ? 1 : 0;
        return;
    }
    if (key == "menu_tab") {
        v.menu_tab = i >= 0 && i <= 5 ? i : 0;
        return;
    }
    if (key == "aa_row") {
        aa_row_file = i != 0;
    } else if (key == "dlss_supported") {
        dlss_supported_file = i != 0;
        return;
    } else if (key == "dlss_fg_supported") {
        dlss_fg_supported_file = i != 0;
        return;
    } else if (key == "fsr4_supported") {
        fsr4_supported_file = i != 0;
        return;
    } else if (key == "fsr4_default") {
        fsr4_default_done = i != 0;
        return;
    }
    if (key == "key_map") {
        v.key_map = value;
        return;
    }
    if (key == "audio_device") {
        v.audio_device = value;
        return;
    }
    if (key == "pad_combo_quit" || key == "pad_combo_menu" || key == "pad_combo_qa" ||
        key == "pad_combo_photo") {
        auto& target = key == "pad_combo_quit" ? v.combo_quit : key == "pad_combo_menu" ? v.combo_menu
                      : key == "pad_combo_qa"   ? v.combo_qa
                                                : v.combo_photo;
        target = ParseCombo(value, target.load());
        return;
    }
    if (key == "audio_surround") {
        v.audio_surround = std::atoi(value.c_str()) != 0;
        return;
    }
    if (key == "upscaler") {
        for (int u = 0; u < UpscalerCount; ++u) {
            if (value == UpscalerName(u)) {
                v.upscaler = u;
            }
        }
    } else if (key == "preset") {
        // Supersampling removed from the menus (user 2026-11-10): old files load it as Off.
        v.preset = IsSupersampling(i) ? int(NativeAA) : std::clamp(i, 0, PresetCount - 1);
    } else if (key == "sharpen") {
        v.sharpen = i != 0;
    } else if (key == "sharpness") {
        v.sharpness = Clamp(f, 0.0f, 4.0f);
    } else if (key == "jitter") {
        v.jitter = i != 0;
    } else if (key == "reactive") {
        v.reactive = i != 0;
    } else if (key == "object_motion") {
        v.object_motion = i != 0;
    } else if (key == "reactive_scale") {
        v.reactive_scale = Clamp(f, 0.0f, 16.0f);
    } else if (key == "reactive_threshold") {
        v.reactive_threshold = Clamp(f, 0.0f, 1.0f);
    } else if (key == "reactive_max") {
        v.reactive_max = Clamp(f, 0.0f, 1.0f);
    } else if (key == "fsr_velocity") {
        v.fsr_velocity = Clamp(f, 0.0f, 1.0f);
    } else if (key == "dlss_preset") {
        v.dlss_preset = std::clamp(i, 0, 15);
    } else if (key == "debug_view") {
        v.debug_view = std::clamp(i, 0, DebugViewCount - 1);
    } else if (key == "show_fps") {
        v.show_fps = i != 0;
    } else if (key == "display_mode") {
        v.display_mode = TlgGraphics::ParseDisplayMode(value);
    } else if (key == "vsync") {
        v.vsync = i != 0;
    } else if (key == "fps_limit") {
        v.fps_limit = TlgGraphics::ParseFpsLimit(i);
    } else if (key == "fsr1") {
        v.fsr1 = false;  // FSR 1 removed (user 2026-11-10): old files and presets load it off
    } else if (key == "rcas") {
        v.rcas = i != 0;
    } else if (key == "rcas_strength") {
        v.rcas_strength = TlgGraphics::ParseRcasStrength(value.empty() ? -1 : i);
    } else if (key == "dlaa") {
        v.dlaa = i != 0;
    } else if (key == "level_select") {
        v.level_select = i != 0;
    } else if (key == "shadow_scale") {
        v.shadow_scale = i >= 4 ? 4 : i >= 2 ? 2 : 1;
    } else if (std::atomic<int>* effect = EffectSetting(v, key)) {
        *effect = std::clamp(i, 0, 400);
    } else if (key == "fov") {
        v.fov = std::clamp(i, 50, 110);
    } else if (key == "lod_bias") {
        v.lod_bias = std::clamp(i, -12, 4);
    } else if (key == "texture_pack") {
        // A pack folder name (presets) or an index (older tlg.ini files).
        const auto& names = TexturePackNames();
        const auto found = std::find(names.begin(), names.end(), value);
        v.texture_pack = found != names.end() ? int(found - names.begin()) + 1 : std::clamp(i, 0, int(names.size()));
    } else if (key == "aniso") {
        v.aniso = i >= 16 ? 16 : i >= 8 ? 8 : i >= 4 ? 4 : i >= 2 ? 2 : 0;
    } else if (key == "smaa") {
        v.smaa = i != 0;
    } else if (key == "cas") {
        v.cas = i != 0;
    } else if (key == "cas_strength") {
        v.cas_strength = TlgGraphics::ParseRcasStrength(value.empty() ? -1 : i, 100);
    } else if (key == "color_adjust") {
        v.color_adjust = i != 0;
    } else if (key == "color_brightness") {
        v.brightness = TlgGraphics::ClampColor(TlgGraphics::Brightness, f);
    } else if (key == "color_contrast") {
        v.contrast = TlgGraphics::ClampColor(TlgGraphics::Contrast, value.empty() ? 1.0f : f);
    } else if (key == "color_saturation") {
        v.saturation = TlgGraphics::ClampColor(TlgGraphics::Saturation, value.empty() ? 1.0f : f);
    } else if (key == "color_gamma") {
        v.color_gamma = TlgGraphics::ClampColor(TlgGraphics::Gamma, value.empty() ? 1.0f : f);
    } else if (key == "frames_queued") {
        v.frames_queued = TlgGraphics::ParseFramesQueued(value.empty() ? -1 : i);
    } else if (key == "frame_generation") {
        v.frame_generation = i >= 0 && i <= 2 ? i : 0;
    } else if (key == "readback_batch") {
        v.readback_batch = TlgGraphics::ParseReadbackBatch(value.empty() ? 0 : i);
    } else if (key == "texture_cleanup") {
        v.texture_cleanup = value.empty() || i != 0;
    } else if (key == "credits_fps") {
        v.credits_fps = i == 0 ? 0 : i == 60 ? 60 : 30;
    } else if (key == "slow_motion") {
        v.slow_motion = i == 1 ? 1 : 0;
    } else if (key == "trico_drool") {
        v.trico_drool = i == 1 || i == 2 ? i : 0;
    } else if (key == "mod_treasures") {
        v.mod_treasures = i != 0;
    } else if (key == "mod_horns") {
        v.mod_horns = i != 0;
    } else if (key == "mod_plays") {
        v.mod_plays = i != 0;
    } else if (key == "mod_throw_up") {
        v.mod_throw_up = i == 1 ? 1 : 0;
    } else if (key == "mod_hints") {
        v.mod_hints = i != 0;
    } else if (key == "keep_gpu_textures") {
        v.keep_gpu_textures = value.empty() || i != 0;
    } else if (key == "arena_release") {
        v.arena_release = value.empty() || i != 0;
    } else if (key == "gc_live_budget") {
        v.gc_live_budget = value.empty() || i != 0;
    } else if (key == "render_resolution") {
        v.render_resolution = TlgGraphics::ParseResolution(value);
    } else if (key == "engine_fps") {
        v.engine_fps = TlgGraphics::ParseEngineFps(i);
    } else if (key == "hdr") {
        v.hdr = i != 0;
    } else if (key == "deferred_readback") {
        v.deferred_readback = i != 0;
    } else if (key == "stale_readback") {
        v.stale_readback = i != 0;
    } else if (key == "restart_unconfirmed") {
        v.restart_unconfirmed = i != 0;
    } else if (key == "fsr4_auto_exposure") {
        v.fsr4_auto_exposure = i != 0;
    } else if (key == "fsr4_invert_jitter") {
        v.fsr4_invert_jitter = i != 0;
    } else if (key == "live_resolution") {
        v.live_resolution = value == "auto" ? -1 : std::clamp(i, 0, 1);
    } else if (key == "mouse_enabled") {
        v.mouse_enabled = i != 0;
    } else if (key == "mouse_invert_y") {
        v.mouse_invert_y = i != 0;
    } else if (key == "mouse_sensitivity") {
        v.mouse_sensitivity = Clamp(value.empty() ? 1.0f : f, 0.05f, 10.0f);
    } else if (key == "mouse_smoothing") {
        v.mouse_smoothing = Clamp(f, 0.0f, 0.95f);
    } else if (key == "mouse_accel") {
        v.mouse_accel = Clamp(f, 0.0f, 2.0f);
    } else if (key == "mouse_deadzone") {
        v.mouse_deadzone = Clamp(f, 0.0f, 0.5f);
    } else if (key == "output_res") {
        for (int r = 0; r < OutputCount; ++r) {
            if (value == std::to_string(OutputWidths[r]) + "x" + std::to_string(OutputHeights[r])) {
                v.output_res = r;
            }
        }
    }
}

} // namespace

namespace {
struct ComboName { uint32_t bit; const char* key; const char* label; };
constexpr ComboName ComboNames[] = {
    {PadCross, "cross", "Cross"}, {PadCircle, "circle", "Circle"}, {PadSquare, "square", "Square"},
    {PadTriangle, "triangle", "Triangle"}, {PadL1, "l1", "L1"}, {PadR1, "r1", "R1"}, {PadL2, "l2", "L2"},
    {PadR2, "r2", "R2"}, {PadL3, "l3", "L3"}, {PadR3, "r3", "R3"}, {PadShare, "share", "Share"},
    {PadOptions, "options", "Options"}, {PadTouchpad, "touchpad", "Touchpad"}, {PadPs, "ps", "PS"},
    {PadUp, "up", "Up"}, {PadDown, "down", "Down"}, {PadLeft, "left", "Left"}, {PadRight, "right", "Right"},
};
} // namespace

uint32_t ParseCombo(const std::string& text, uint32_t fallback) {
    uint32_t bits = 0;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('+', start);
        if (end == std::string::npos) end = text.size();
        std::string part = text.substr(start, end - start);
        for (char& c : part) c = char(std::tolower(static_cast<unsigned char>(c)));
        for (const auto& n : ComboNames)
            if (part == n.key) bits |= n.bit;
        start = end + 1;
    }
    return bits ? bits : fallback;
}

static std::string ComboJoin(uint32_t bits, bool label) {
    std::string out;
    for (const auto& n : ComboNames) {
        if (!(bits & n.bit)) continue;
        if (!out.empty()) out += label ? " + " : "+";
        out += label ? n.label : n.key;
    }
    return out;
}
std::string ComboText(uint32_t bits) { return ComboJoin(bits, false); }
std::string ComboLabel(uint32_t bits) { return ComboJoin(bits, true); }

Values& Get() {
    static Values values;
    return values;
}

namespace {
const char* PackRoot() {
    const char* env = std::getenv("TLG_TEXPACK_DIR");
    return env && env[0] ? env : "texpack";
}
} // namespace

const std::vector<std::string>& TexturePackNames() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> list;
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(PackRoot(), error)) {
            if (entry.is_directory() && std::filesystem::exists(entry.path() / "index.txt")) {
                list.push_back(entry.path().filename().string());
            }
        }
        std::sort(list.begin(), list.end());
        return list;
    }();
    return names;
}

std::string TexturePackDir(int index) {
    const auto& names = TexturePackNames();
    return index >= 1 && index <= int(names.size()) ? std::string(PackRoot()) + "/" + names[index - 1]
                                                    : std::string();
}

namespace {
bool ReadFile(Values& v) {
    FILE* file = std::fopen(Path(), "r");
    if (!file) {
        return false;
    }
    char line[1024]; // key_map can be long
    while (std::fgets(line, sizeof(line), file)) {
        std::string text{line};
        text.erase(text.find_last_not_of(" \t\r\n") + 1);
        const auto eq = text.find('=');
        if (text.empty() || text[0] == '#' || eq == std::string::npos) {
            continue;
        }
        Set(v, text.substr(0, eq), text.substr(eq + 1));
    }
    std::fclose(file);
    return true;
}
} // namespace

void ReloadIfRequested() {
    static const char* trigger = std::getenv("TLG_SETTINGS_RELOAD");
    if (trigger && trigger[0] && std::remove(trigger) == 0 && ReadFile(Get())) {
        selected_upscaler = -1;
        DlssFallback(Get());
        std::printf("Settings: reloaded %s\n", Path());
    }
}

void Load() {
    auto& v = Get();
    if (ReadFile(v)) {
        std::printf("Settings: %s\n", Path());
    }
    // tlg: the upscaler row: Built-in (off), DLSS, FSR 3.1 or FSR 4; at Native AA quality the
    // choice is the anti-aliasing (DLAA, FSR native AA). Files from before the AA row, and
    // other upscalers, load as DLSS. The quality (preset) is the render size the launcher
    // patches in (startup); the menu keeps it for the next launch.
    if (!aa_row_file || (v.upscaler != UpscalerTaa && v.upscaler != UpscalerFsr3 &&
                         v.upscaler != UpscalerFsr4 && v.upscaler != UpscalerOff)) {
        v.upscaler = UpscalerDlss;
    }
    // tlg: the AA row is gone (2026-11-07); a file with AA off keeps its picture as Built-in.
    if (!v.dlaa && v.upscaler != UpscalerOff) {
        std::printf("Settings: anti-aliasing was off; upscaler set to Built-in\n");
        v.upscaler = UpscalerOff;
    }
    v.dlaa = v.upscaler != UpscalerOff;
    DlssFallback(v);
    // Environment overrides (scripts, A/B tests).
    if (const char* env = std::getenv("TLG_UPSCALER")) {
        v.upscaler = UpscalerOff;
        for (int u = 0; u < UpscalerCount; ++u) {
            if (std::strcmp(env, UpscalerName(u)) == 0) v.upscaler = u;
        }
    }
    const std::pair<const char*, const char*> env_keys[] = {
        {"TLG_FSR_SHARPNESS", "sharpness"},        {"TLG_JITTER", "jitter"},
        {"TLG_REACTIVE", "reactive"},              {"TLG_REACTIVE_SCALE", "reactive_scale"},
        {"TLG_REACTIVE_THRESHOLD", "reactive_threshold"}, {"TLG_REACTIVE_MAX", "reactive_max"},
        {"TLG_UPSCALE_PRESET", "preset"},
        {"TLG_DEFERRED_READBACK", "deferred_readback"},
        {"TLG_STALE_READBACK", "stale_readback"},     {"TLG_FSR1", "fsr1"},
        {"TLG_RCAS", "rcas"},                        {"TLG_FRAMES_AHEAD", "frames_queued"},
        {"TLG_SHOW_FPS", "show_fps"},                {"TLG_FSR_VELOCITY", "fsr_velocity"},
        {"TLG_DLSS_PRESET", "dlss_preset"},
        {"TLG_MOUSE", "mouse_enabled"},              {"TLG_MOUSE_SENS", "mouse_sensitivity"},
        {"TLG_MOUSE_INVERT_Y", "mouse_invert_y"},    {"TLG_MOUSE_SMOOTHING", "mouse_smoothing"},
        {"TLG_MOUSE_ACCEL", "mouse_accel"},          {"TLG_MOUSE_DEADZONE", "mouse_deadzone"},
        {"TLG_SMAA", "smaa"},                        {"TLG_DLAA", "dlaa"},
        {"TLG_LEVEL_SELECT", "level_select"},       {"TLG_TRICO_DROOL", "trico_drool"},
        {"TLG_MOD_TREASURES", "mod_treasures"},     {"TLG_MOD_HORNS", "mod_horns"},
        {"TLG_MOD_PLAYS", "mod_plays"},             {"TLG_MOD_THROW_UP", "mod_throw_up"},
        {"TLG_MOD_HINTS", "mod_hints"},
        {"TLG_CAS", "cas"},                          {"TLG_COLOR_ADJUST", "color_adjust"},
        {"TLG_HDR", "hdr"},                          {"TLG_FG", "frame_generation"},
        {"TLG_ARENA_RELEASE", "arena_release"},      {"TLG_GC_LIVE_BUDGET", "gc_live_budget"},
    };
    for (const auto& [env, key] : env_keys) {
        if (const char* value = std::getenv(env); value && value[0]) {
            Set(v, key, value);
        }
    }
    // Async shader compilation: environment only (tests), a stored async_shaders=1 is ignored.
    if (const char* value = std::getenv("TLG_ASYNC_SHADERS"); value && value[0]) {
        v.async_shaders = std::atoi(value) != 0;
    }
    // Object motion: environment only (launcher Debug > Flags); a stored object_motion=1 is ignored.
    v.object_motion = false;
    if (const char* value = std::getenv("TLG_OBJECT_MOTION"); value && value[0]) {
        v.object_motion = std::atoi(value) != 0;
    }
    v.startup_preset = v.preset;
    v.startup_upscaler = v.upscaler;
    v.startup_object_motion = v.object_motion;
    v.startup_output_res = v.output_res;
    v.startup_live_resolution = v.live_resolution;
    v.startup_render_resolution = v.render_resolution;
    v.startup_engine_fps = v.engine_fps;
    const char* applied = std::getenv("TLG_ENGINE_FPS");
    v.applied_engine_fps = applied && applied[0] ? TlgGraphics::ParseAppliedEngineFps(std::atoi(applied))
                                                 : v.engine_fps.load();
    v.startup_deferred_readback = v.deferred_readback;
    v.startup_stale_readback = v.stale_readback;
    v.startup_hdr = v.hdr;
}

bool GraphicsNeedRestart() {
    const auto& v = Get();
    return v.render_resolution != v.startup_render_resolution ||
           v.engine_fps != v.startup_engine_fps ||
           v.deferred_readback != v.startup_deferred_readback ||
           v.stale_readback != v.startup_stale_readback || v.hdr != v.startup_hdr ||
           QualityNeedsRestart();
}

bool QualityNeedsRestart() {
    const auto& v = Get();
    return v.preset != v.startup_preset;
}

void OutputSize(int& width, int& height) {
    width = 1920;
    height = 1080;
    if (const char* patch = std::getenv("TLG_RESOLUTION_PATCH"); patch && patch[0]) {
        if (std::sscanf(patch, "%dx%d", &width, &height) != 2 || width <= 0 || height <= 0) {
            width = 1920;
            height = 1080;
        }
    }
}

void StartupRenderSize(int& width, int& height) {
    OutputSize(width, height);
    if (const char* render = std::getenv("TLG_RENDER_RESOLUTION"); render && render[0]) {
        int w = 0, h = 0;
        if (std::sscanf(render, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
            width = w;
            height = h;
        }
    }
}

bool EngineUpscaleMode() {
    static const bool smaller = [] {
        int ow, oh, rw, rh;
        OutputSize(ow, oh);
        StartupRenderSize(rw, rh);
        return rw <= ow && rh <= oh && (rw < ow || rh < oh);
    }();
    return smaller && Get().upscaler != UpscalerOff;
}

bool AaOn() {
    return Get().dlaa || EngineUpscaleMode();
}

namespace {
std::atomic<long long> aa_sharpened_ms{-1000000};
std::atomic<const char*> aa_sharpener{"AA step"};
long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
} // namespace

void MarkAaSharpened(const char* sharpener) {
    aa_sharpener.store(sharpener, std::memory_order_relaxed);
    aa_sharpened_ms.store(NowMs(), std::memory_order_relaxed);
}

const char* AaSharpenerName() {
    return aa_sharpener.load(std::memory_order_relaxed);
}

bool AaSharpenedRecently() {
    return NowMs() - aa_sharpened_ms.load(std::memory_order_relaxed) < 250;
}

namespace {
// Rewrites only these keys in tlg.ini (the launcher reads them before the menu saves).
bool WriteKeys(const std::vector<std::pair<std::string, std::string>>& keys) {
    std::vector<std::string> lines;
    if (std::ifstream in{Path()}) {
        for (std::string line; std::getline(in, line);) {
            bool replaced = false;
            for (const auto& [key, value] : keys) replaced |= line.starts_with(key + "=");
            if (!replaced) lines.push_back(line);
        }
    }
    if (lines.empty()) return false;
    std::ofstream out{Path()};
    if (!out) return false;
    for (const auto& line : lines) out << line << '\n';
    for (const auto& [key, value] : keys) out << key << '=' << value << '\n';
    return true;
}
} // namespace

void ConfigureUpscalerSupport(bool fsr4, bool fsr411) {
    auto& v = Get();
    v.fsr4_supported = fsr4;
    v.fsr411_supported = fsr4 && fsr411;
    const int requested = v.upscaler;
    if ((requested == UpscalerFsr4 && !v.fsr4_supported) ||
        (requested == UpscalerFsr411 && !v.fsr411_supported)) {
        v.fsr4_problem = "GPU does not support the selected FSR 4 shaders; using FSR 3.1";
        std::printf("Upscaler: %s unsupported on this GPU; falling back to FSR 3.1 before the first frame\n",
                    UpscalerName(requested));
        FallBackToFsr3();
    }
}

void ConfigureDlssFgSupport(bool available) {
    Get().dlss_fg_supported = int(available);
    if (dlss_fg_supported_file != int(available)) {
        // Only this line changes: the launcher leaves DLSS out of its Frame generation row from it.
        if (WriteKeys({{"dlss_fg_supported", std::to_string(int(available))}}))
            dlss_fg_supported_file = available;
    }
}

bool FrameGenerationBlocked() {
    const auto& v = Get();
    return TlgGraphics::EffectiveFps(v.fps_limit, v.applied_engine_fps) <= 30;
}

int EffectiveFrameGeneration() {
    const auto& v = Get();
    const int mode = v.frame_generation;
    const std::uint32_t lock = v.credits_fps_lock;
    if (mode == 0 || FrameGenerationBlocked() || (lock != 0 && lock <= 30)) return 0;
    return mode == 1 && v.dlss_fg_supported == 0 ? 0 : mode;
}

void ConfigureDlssSupport(bool available, const char* problem) {
    auto& v = Get();
    v.dlss_supported = available;
    dlss_checked = true;
    if (dlss_supported_file != int(available)) {
        // Only this line changes: the launcher greys DLAA out from it.
        if (WriteKeys({{"dlss_supported", std::to_string(int(available))}}))
            dlss_supported_file = available;
    }
    static std::string kept;
    kept = problem ? problem : "";
    v.dlss_problem = available || kept.empty() ? nullptr : kept.c_str();
    if (v.upscaler == UpscalerDlss && !available) {
        std::printf("Upscaler: DLSS unavailable (%s); falling back to FSR 3.1\n",
                    kept.empty() ? "tlg_dlss.dll or nvngx_dlss.dll missing" : kept.c_str());
        FallBackToFsr3();
    }
    // tlg: once per install, a card that runs FSR 4 moves from FSR 3.1 native AA (chosen, or
    // the fallback from DLAA without DLSS) to FSR 4 native AA. Later choices stay; scripts that
    // set TLG_UPSCALER keep theirs.
    const char* env = std::getenv("TLG_UPSCALER");
    std::vector<std::pair<std::string, std::string>> keys;
    if (v.fsr4_supported && !fsr4_default_done && !(env && env[0])) {
        fsr4_default_done = true;
        keys.emplace_back("fsr4_default", "1");
        if (v.dlaa && v.upscaler == UpscalerFsr3) {
            std::printf("Upscaler: this GPU runs FSR 4; native AA moves from FSR 3.1 to FSR 4 (once)\n");
            v.upscaler = UpscalerFsr4;
            selected_upscaler = -1;
            v.preset = NativeAA;
            keys.emplace_back("upscaler", "fsr4");
        }
    }
    if (fsr4_supported_file != int(v.fsr4_supported.load())) {
        keys.emplace_back("fsr4_supported", std::to_string(int(v.fsr4_supported.load())));
        fsr4_supported_file = v.fsr4_supported;
    }
    if (!keys.empty()) WriteKeys(keys);
}

void FallBackToFsr3() {
    auto& v = Get();
    if (v.upscaler != UpscalerFsr3) selected_upscaler = v.upscaler;
    v.upscaler = UpscalerFsr3;
}

int SelectedUpscaler() {
    const auto& v = Get();
    return selected_upscaler >= 0 && v.upscaler == UpscalerFsr3 ? selected_upscaler : v.upscaler.load();
}

int DefaultAaUpscaler() {
    const auto& v = Get();
    return v.dlss_supported ? UpscalerDlss : v.fsr4_supported ? UpscalerFsr4 : UpscalerFsr3;
}

void SetUpscaler(int upscaler) {
    // tlg: the upscaler choice is the anti-aliasing too (user, 2026-11-07: no separate AA row):
    // Built-in = the game's own, any other = its AA (native) or upscaling (below native).
    auto& v = Get();
    v.upscaler = std::clamp(upscaler, 0, UpscalerCount - 1);
    v.dlaa = v.upscaler != UpscalerOff;
    selected_upscaler = -1;
    if (v.upscaler == UpscalerDlss && !v.dlss_supported) FallBackToFsr3();
}

bool FixedRenderSession() {
    const char* size = std::getenv("TLG_RENDER_RES");
    return size && size[0];
}

int RenderPreset() {
    // The resolution patch (TLG_RESOLUTION_PATCH, tlg_run.py) renders natively: no reduced presets.
    static const bool resolution_patch = std::getenv("TLG_RESOLUTION_PATCH") != nullptr;
    if (resolution_patch) return NativeAA;
    const auto& v = Get();
    return FixedRenderSession() ? v.startup_preset :
        v.upscaler == UpscalerTaa ? NativeAA : v.preset.load();
}

bool ResolutionNeedsRestart() {
    const auto& v = Get();
    // TAA needs the live path (native guest targets): the launcher selects it on restart.
    return FixedRenderSession() &&
        (v.preset != v.startup_preset || v.output_res != v.startup_output_res ||
         (v.upscaler == UpscalerOff) != (v.startup_upscaler == UpscalerOff) ||
         (v.upscaler == UpscalerTaa) != (v.startup_upscaler == UpscalerTaa));
}

void Save() {
    const auto& v = Get();
    // A fallback still running FSR 3.1 keeps the selection in the file.
    const int upscaler = selected_upscaler >= 0 && v.upscaler == UpscalerFsr3 ? selected_upscaler
                                                                              : v.upscaler.load();
    FILE* file = std::fopen(Path(), "w");
    if (!file) {
        std::printf("Settings: cannot write %s\n", Path());
        return;
    }
    std::fprintf(file,
                 "# tlg settings (in-game menu: Insert / R3+L2)\n"
                 "aa_row=1\nupscaler=%s\npreset=%d\nsharpen=%d\nsharpness=%.2f\njitter=%d\n"
                 "reactive=%d\nobject_motion=%d\nreactive_scale=%.2f\nreactive_threshold=%.2f\nreactive_max=%.2f\n"
                 "debug_view=%d\nshow_fps=%d\nfsr4_auto_exposure=%d\nfsr4_invert_jitter=%d\n",
                 UpscalerName(upscaler), v.preset.load(), int(v.sharpen.load()),
                 v.sharpness.load(), int(v.jitter.load()), int(v.reactive.load()),
                 int(v.object_motion.load()),
                 v.reactive_scale.load(), v.reactive_threshold.load(), v.reactive_max.load(),
                 v.debug_view.load(), int(v.show_fps.load()),
                 int(v.fsr4_auto_exposure.load()), int(v.fsr4_invert_jitter.load()));
    std::fprintf(file, "fsr_velocity=%.2f\n", v.fsr_velocity.load());
    std::fprintf(file, "dlss_preset=%d\n", v.dlss_preset.load());
    std::fprintf(file, "output_res=%dx%d\n", OutputWidths[v.output_res], OutputHeights[v.output_res]);
    std::fprintf(file,
                 "overlay_fps=%d\noverlay_frametime=%d\noverlay_upscaler=%d\noverlay_gpu=%d\n"
                 "overlay_cpu=%d\noverlay_ram=%d\noverlay_vram=%d\noverlay_scale=%d\noverlay_corner=%d\n"
                 "overlay_opacity=%d\noverlay_layout=%d\nmenu_tab=%d\n",
                 int(v.overlay_items >> 0 & 1), int(v.overlay_items >> 1 & 1), int(v.overlay_items >> 2 & 1),
                 int(v.overlay_items >> 3 & 1), int(v.overlay_items >> 4 & 1), int(v.overlay_items >> 5 & 1),
                 int(v.overlay_items >> 6 & 1), v.overlay_scale.load(), v.overlay_corner.load(),
                 v.overlay_opacity.load(), v.overlay_layout.load(), v.menu_tab.load());
    std::fprintf(file,
                 "display_mode=%s\nvsync=%d\nfps_limit=%d\n"
                 "fsr1=%d\nframes_queued=%d\n"
                 "readback_batch=%d\ntexture_cleanup=%d\nrender_resolution=%s\nengine_fps=%d\nhdr=%d\n"
                 "deferred_readback=%d\nstale_readback=%d\nrestart_unconfirmed=%d\n",
                 TlgGraphics::DisplayModeKeys[std::clamp(v.display_mode.load(), 0, 2)].data(),
                 int(v.vsync.load()), v.fps_limit.load(),
                 int(v.fsr1.load()), v.frames_queued.load(),
                 v.readback_batch.load(), int(v.texture_cleanup.load()), TlgGraphics::Resolutions[std::clamp(v.render_resolution.load(), 0, 5)].data(),
                 v.engine_fps.load(), int(v.hdr.load()), int(v.deferred_readback.load()), int(v.stale_readback.load()),
                 int(v.restart_unconfirmed.load()));
    std::fprintf(file, "frame_generation=%d\n", v.frame_generation.load());
    std::fprintf(file, "audio_device=%s\naudio_surround=%d\n", v.audio_device.c_str(), int(v.audio_surround.load()));
    std::fprintf(file, "keep_gpu_textures=%d\n", int(v.keep_gpu_textures.load()));
    std::fprintf(file, "arena_release=%d\ngc_live_budget=%d\n", int(v.arena_release.load()),
                 int(v.gc_live_budget.load()));
    std::fprintf(file,
                 "mouse_enabled=%d\nmouse_sensitivity=%.2f\nmouse_invert_y=%d\nmouse_smoothing=%.2f\n"
                 "mouse_accel=%.2f\nmouse_deadzone=%.2f\nkey_map=%s\n",
                 int(v.mouse_enabled.load()), v.mouse_sensitivity.load(), int(v.mouse_invert_y.load()),
                 v.mouse_smoothing.load(), v.mouse_accel.load(), v.mouse_deadzone.load(), v.key_map.c_str());
    std::fprintf(file,
                 "dlaa=%d\nshadow_scale=%d\nsmaa=%d\ncolor_adjust=%d\n"
                 "color_brightness=%.2f\ncolor_contrast=%.2f\ncolor_saturation=%.2f\ncolor_gamma=%.2f\n",
                 int(v.dlaa.load()),v.shadow_scale.load(), int(v.smaa.load()),
                 int(v.color_adjust.load()), v.brightness.load(), v.contrast.load(),
                 v.saturation.load(), v.color_gamma.load());
    std::fprintf(file, "level_select=%d\n", int(v.level_select.load()));
    std::fprintf(file, "pad_combo_quit=%s\npad_combo_menu=%s\npad_combo_qa=%s\npad_combo_photo=%s\n",
                 ComboText(v.combo_quit).c_str(), ComboText(v.combo_menu).c_str(), ComboText(v.combo_qa).c_str(),
                 ComboText(v.combo_photo).c_str());
    std::fprintf(file, "trico_drool=%d\n", v.trico_drool.load());
    std::fprintf(file, "mod_treasures=%d\nmod_horns=%d\n", int(v.mod_treasures.load()),
                 int(v.mod_horns.load()));
    std::fprintf(file, "mod_plays=%d\nmod_throw_up=%d\nmod_hints=%d\n", int(v.mod_plays.load()),
                 v.mod_throw_up.load(), int(v.mod_hints.load()));
    for (const auto& [name, member] : EffectKeys)
        std::fprintf(file, "%s=%d\n", name, (v.*member).load());
    std::fprintf(file, "fov=%d\n", v.fov.load());
    std::fprintf(file, "slow_motion=%d\ncredits_fps=%d\n", v.slow_motion.load(), v.credits_fps.load());
    const int pack = v.texture_pack;
    std::fprintf(file, "lod_bias=%d\naniso=%d\ntexture_pack=%s\n", v.lod_bias.load(), v.aniso.load(),
                 pack > 0 ? TexturePackNames()[pack - 1].c_str() : "off");
    if (dlss_checked || dlss_supported_file >= 0)
        std::fprintf(file, "dlss_supported=%d\n", dlss_checked ? int(v.dlss_supported.load()) : dlss_supported_file);
    if (const int fg = v.dlss_fg_supported >= 0 ? v.dlss_fg_supported.load() : dlss_fg_supported_file; fg >= 0)
        std::fprintf(file, "dlss_fg_supported=%d\n", fg);
    if (fsr4_supported_file >= 0) std::fprintf(file, "fsr4_supported=%d\n", fsr4_supported_file);
    if (fsr4_default_done) std::fprintf(file, "fsr4_default=1\n");
    // Read by the launcher at start.
    std::fprintf(file, "live_resolution=%s\n", v.live_resolution < 0 ? "auto"
                                                  : v.live_resolution ? "1" : "0");
    std::fclose(file);
}

namespace {
// The game as shipped: none of the port's filters, the game's own shadows, filtering and textures.
constexpr const char* StockLines = "dlaa=0\nsmaa=0\nfsr1=0\nsharpen=0\nshadow_scale=1\nfov=70\n"
                                   "color_adjust=0\nlod_bias=0\naniso=0\ntexture_pack=off\ngrass_smooth=0\nhaze_fringe=100\n";

void ApplyLines(Values& v, const std::string& lines) {
    std::istringstream stream{lines};
    std::string text;
    while (std::getline(stream, text)) {
        text.erase(text.find_last_not_of(" \t\r") + 1);
        const auto eq = text.find('=');
        if (!text.empty() && text[0] != '#' && eq != std::string::npos) {
            Set(v, text.substr(0, eq), text.substr(eq + 1));
        }
    }
}

std::string GraphicsText(const Values& v) {
    std::string text;
    char line[96];
    const auto add = [&](const char* key, int value) {
        std::snprintf(line, sizeof(line), "%s=%d\n", key, value);
        text += line;
    };
    const auto add_float = [&](const char* key, float value) {
        std::snprintf(line, sizeof(line), "%s=%.2f\n", key, value);
        text += line;
    };
    add("dlaa", v.dlaa);
    add("smaa", v.smaa);
    add("fsr1", v.fsr1);
    // One Sharpening setting (2026-11-07): rcas / cas keys are still read but no longer used.
    add("sharpen", v.sharpen);
    add_float("sharpness", v.sharpness);
    add("shadow_scale", v.shadow_scale);
    add("fov", v.fov);
    for (const auto& [name, member] : EffectKeys)
        add(name, (v.*member).load());
    add("color_adjust", v.color_adjust);
    add_float("color_brightness", v.brightness);
    add_float("color_contrast", v.contrast);
    add_float("color_saturation", v.saturation);
    add_float("color_gamma", v.color_gamma);
    add("lod_bias", v.lod_bias);
    add("aniso", v.aniso);
    const int pack = v.texture_pack;
    text += "texture_pack=" + (pack > 0 ? TexturePackNames()[pack - 1] : std::string("off")) + "\n";
    return text;
}

// The preset's lines on top of the defaults, in GraphicsText form (for matching).
std::string Normalized(const std::string& lines) {
    const auto v = std::make_unique<Values>();
    ApplyLines(*v, lines);
    return GraphicsText(*v);
}

std::filesystem::path PresetDir() {
    return std::filesystem::path(Path()).parent_path() / "presets";
}

struct GraphicsPreset {
    std::string name, text;
};

std::vector<GraphicsPreset> ReadPresets() {
    std::vector<GraphicsPreset> list{{"Stock", Normalized(StockLines)}, {"Recommended", Normalized("")}};
    std::vector<std::filesystem::path> files;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(PresetDir(), error)) {
        if (entry.path().extension() == ".ini") files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    for (const auto& file : files) {
        std::ifstream in{file};
        std::stringstream lines;
        lines << in.rdbuf();
        list.push_back({file.stem().string(), Normalized(lines.str())});
    }
    return list;
}

std::vector<GraphicsPreset>& Presets() {
    static std::vector<GraphicsPreset> list = ReadPresets();
    return list;
}
} // namespace

std::vector<std::string> GraphicsPresetNames() {
    std::vector<std::string> names;
    for (const auto& preset : Presets()) names.push_back(preset.name);
    return names;
}

void ApplyGraphicsPreset(int index) {
    if (index < 0 || index >= int(Presets().size())) return;
    auto& v = Get();
    ApplyLines(v, Presets()[index].text);
    // dlaa is the upscaler choice now (no AA row): Stock (dlaa=0) is Built-in, a preset with
    // dlaa=1 on Built-in picks the card's best upscaler.
    if (!v.dlaa && SelectedUpscaler() != UpscalerOff) SetUpscaler(UpscalerOff);
    if (v.dlaa && v.upscaler == UpscalerOff) SetUpscaler(DefaultAaUpscaler());
}

int MatchingGraphicsPreset() {
    const std::string now = GraphicsText(Get());
    for (int i = 0; i < int(Presets().size()); ++i) {
        if (Presets()[i].text == now) return i;
    }
    return -1;
}

int SaveGraphicsPreset(const std::string& name) {
    std::string clean;
    for (const char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_') clean += c;
    }
    clean.erase(0, clean.find_first_not_of(' '));
    clean.erase(clean.find_last_not_of(' ') + 1);
    if (clean.empty() || clean == "Stock" || clean == "Recommended") return -1;
    std::error_code error;
    std::filesystem::create_directories(PresetDir(), error);
    std::ofstream out{PresetDir() / (clean + ".ini")};
    if (!out) return -1;
    out << "# tlg graphics preset (in-game menu: Image tab; launcher)\n" << GraphicsText(Get());
    out.close();
    Presets() = ReadPresets();
    const auto names = GraphicsPresetNames();
    return int(std::find(names.begin(), names.end(), clean) - names.begin());
}

void DeleteGraphicsPreset(int index) {
    if (index < 2 || index >= int(Presets().size())) return;
    std::error_code error;
    std::filesystem::remove(PresetDir() / (Presets()[index].name + ".ini"), error);
    Presets() = ReadPresets();
}

float PresetScale(int preset) {
    static constexpr float scales[PresetCount] = {1.0f, 1.5f, 1.7f, 2.0f, 3.0f, 1.0f / 1.5f, 0.5f};
    return scales[std::clamp(preset, 0, PresetCount - 1)];
}

const char* PresetName(int preset) {
    static constexpr const char* names[PresetCount] = {
        "Native AA", "Quality", "Balanced", "Performance", "Ultra Performance",
        "Supersampling 150% (experimental)", "Supersampling 200% (experimental)"};
    return names[std::clamp(preset, 0, PresetCount - 1)];
}

void PresetRenderSize(int preset, int output_width, int output_height, int& width, int& height) {
    // The same rule as the launcher (tlg_run.render_resolution): output / scale, each side
    // rounded to the nearest even number.
    const float scale = PresetScale(preset);
    width = int(std::lround(output_width / scale / 2.0f)) * 2;
    height = int(std::lround(output_height / scale / 2.0f)) * 2;
}

const char* UpscalerName(int upscaler) {
    static constexpr const char* names[UpscalerCount] = {"off", "fsr3", "fsr4", "fsr411", "taa", "dlss"};
    return names[std::clamp(upscaler, 0, UpscalerCount - 1)];
}

int AaModeOf() {
    const auto& v = Get();
    if (!AaOn() || v.upscaler == UpscalerTaa || v.upscaler == UpscalerOff) return AaOff;
    if (v.upscaler == UpscalerFsr3) return AaFsr3Native;
    return v.upscaler == UpscalerFsr4 ? AaFsr4Native : AaDlaa;
}

void SetAaMode(int mode) {
    auto& v = Get();
    mode = std::clamp(mode, 0, AaModeCount - 1);
    v.dlaa = mode != AaOff;
    if (mode == AaOff) return;
    v.upscaler = mode == AaDlaa ? UpscalerDlss : mode == AaFsr4Native ? UpscalerFsr4 : UpscalerFsr3;
    selected_upscaler = -1;
    v.preset = NativeAA;
}

bool AaModeSupported(int mode) {
    if (mode == AaDlaa) return Get().dlss_supported;
    if (mode == AaFsr4Native) return Get().fsr4_supported;
    return true;
}

const char* AaModeName(int mode) {
    static constexpr const char* names[AaModeCount] = {"Off", "DLAA", "FSR 3.1 Native AA", "FSR 4 Native AA"};
    return names[std::clamp(mode, 0, AaModeCount - 1)];
}

} // namespace TlgSettings
