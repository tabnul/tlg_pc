// SPDX-License-Identifier: GPL-2.0-or-later
#include "tlg_overlay.h"
#include "../../src/runtime_menu.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include "tlg_settings.h"
#include "tlg_lab.h"
#include "tlg_fun.h"
#include "tlg_lang.h"
#include "tlg_fun_icons.h"
#include "tlg_fun_points.h"
#include "tlg_graphics.h"
#include "tlg_perf_stats.h"
extern "C" void runtime_restart(void); // src/probe.c: starts the launch again, ends this process
#include "common/elf_info.h"
#include "imgui.h"
#include "imgui_internal.h" // RegisterUserTexture (save pictures)
#include "imgui_impl_vulkan.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_frame_capture.h"

// DejaVu Sans (Cyrillic), embedded (third_party/fonts, Bitstream Vera license).
#ifdef _WIN32
asm(".section .rdata,\"dr\"\n"
    ".balign 16\n"
    ".global tlg_font_ttf\n"
    "tlg_font_ttf:\n"
    ".incbin \"" TLG_FONT_PATH "\"\n"
    ".global tlg_font_ttf_end\n"
    "tlg_font_ttf_end:\n"
    ".text\n");
#else
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden tlg_font_ttf\n"
    ".global tlg_font_ttf\n"
    "tlg_font_ttf:\n"
    ".incbin \"" TLG_FONT_PATH "\"\n"
    ".hidden tlg_font_ttf_end\n"
    ".global tlg_font_ttf_end\n"
    "tlg_font_ttf_end:\n"
    ".previous\n");
#endif
extern "C" const unsigned char tlg_font_ttf[];
extern "C" const unsigned char tlg_font_ttf_end[];

namespace TlgOverlay {

// Notice() for code that already holds imgui_mutex (HandleEvent, PadEvent): Notice() itself
// takes the lock and would block forever there.
void NoticeLocked(const std::string& text, float seconds);

namespace {

std::mutex imgui_mutex; // the ImGui context: window thread (input) and present thread
bool initialized = false;
bool cjk_font_merged = false; // a system font with Japanese characters is in the menu font
std::atomic<bool> menu_open{false};
// tlg: the Fun move bar (F12): Trico's working moves as icons; the mouse goes to it while it is up.
std::atomic<bool> move_bar_open{false};
ImTextureData* move_icons = nullptr; // TlgFunIcons atlas, made on first use (present thread)
// tlg: controller combos (tlg.ini pad_combo_*). pad_mask = buttons held now (PadCombo bits).
u32 pad_mask = 0;
int capture_slot = -1;  // Controls tab: the combo being set (0 quit, 1 menu, 2 QA menu, 3 photo mode), -1 = none
u32 capture_mask = 0;   // buttons pressed during the capture
u32 capture_ignore = 0; // buttons already held when the capture started (the click on "Set")
std::atomic<bool> screenshot_requested{false}; // Cross in photo mode / Print Screen (TakeScreenshotRequest)
std::atomic<bool> pause_menu_open{false}; // the game's own pause menu is open (PauseMenuUI.lua)
std::atomic<int64_t> quit_hold_start{0}; // steady ms when the quit combo was complete, 0 = not held
constexpr int64_t QuitHoldMs = 2000;     // fixed: the quit combo is always a 2 second hold
std::atomic<u32>* ComboSlot(int slot) {
    auto& s = TlgSettings::Get();
    return slot == 0 ? &s.combo_quit : slot == 1 ? &s.combo_menu : slot == 2 ? &s.combo_qa : &s.combo_photo;
}

std::atomic<bool> loading{false};
std::atomic<u32> loading_done{0}, loading_total{0};
std::atomic<u32> background_done{0}, background_total{0};
bool dirty = false; // settings changed while open: saved on close
std::chrono::steady_clock::time_point aa_label_until{}; // the AA label after F8
int requested_tab = -1; // switch on the next frame (L1/R1, Q/E, menu opened)
// The game's text dialog (SetTextEntry), guarded by imgui_mutex.
bool text_entry_active = false;
std::string text_entry_prompt, text_entry_text;
// The game's save dialogs (sceSaveDataDialog): the list, a message the player answers
// (overwrite?, no space...), the "Saving..." panel and the "Game saved" toast. The save's text
// comes from the game; every other word is the port's, in the menu language. Guarded by
// imgui_mutex; the game's thread polls the atomics.
using SaveClock = std::chrono::steady_clock;
std::atomic<bool> choice_active{false};  // the list
std::atomic<int> choice_result{-1};      // -1 open, -2 cancelled, else the chosen index
std::atomic<bool> message_active{false};
std::atomic<int> message_result{-1};     // -1 open, 1 yes/OK, 2 no/cancel
int save_kind = 1;                       // 1 save, 2 load, 3 delete
std::vector<SaveRow> save_rows;
int choice_index = 0, choice_first = 0;  // selected row, first row shown (the list scrolls)
int message_code = 0, message_buttons = 0, message_focus = 1; // focus: 0 no/cancel, 1 yes/OK
u64 message_value = 0;
std::string message_text;
bool message_has_entry = false;
SaveRow message_entry;
bool progress_on = false;
int progress_kind = 1, progress_percent = -1;
std::string progress_text;
SaveClock::time_point progress_since{}, progress_until{}; // until: a panel shown for a moment
SaveClock::time_point toast_since{}, toast_until{};
int toast_kind = 0;
// The mouse (display pixels) and the last frame's rectangles: the rows, the message's buttons.
ImVec2 save_mouse{-1.0f, -1.0f};
std::vector<ImRect> save_row_rects;
ImRect save_button_rects[2];
// Save pictures: decoded on the game's thread, made textures on the present thread (null =
// none). Closed dialogs' textures wait in choice_retired until the backend has destroyed them.
std::vector<TlgPng::Image> choice_pictures;
std::vector<ImTextureData*> choice_textures, choice_retired;
std::atomic<bool> choice_retiring{false};
float base_scale = 1.0f;

// Present rate for the FPS counter.
std::chrono::steady_clock::time_point last_present{};
float frame_ms_avg = 0.0f;

void SetOpen(bool value) {
    if (menu_open.exchange(value) == value) {
        return;
    }
    ImGui::GetIO().MouseDrawCursor = value || move_bar_open;
    if (value) {
        requested_tab = TlgSettings::Get().menu_tab;
    }
    if (!value && dirty) {
        dirty = false;
        TlgSettings::Save();
    }
}

ImGuiKey KeyFromSdl(SDL_Keycode key) {
    switch (key) {
    case SDLK_TAB: return ImGuiKey_Tab;
    case SDLK_LEFT: return ImGuiKey_LeftArrow;
    case SDLK_RIGHT: return ImGuiKey_RightArrow;
    case SDLK_UP: return ImGuiKey_UpArrow;
    case SDLK_DOWN: return ImGuiKey_DownArrow;
    case SDLK_PAGEUP: return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
    case SDLK_HOME: return ImGuiKey_Home;
    case SDLK_END: return ImGuiKey_End;
    case SDLK_DELETE: return ImGuiKey_Delete;
    case SDLK_BACKSPACE: return ImGuiKey_Backspace;
    case SDLK_SPACE: return ImGuiKey_Space;
    case SDLK_RETURN: return ImGuiKey_Enter;
    case SDLK_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDLK_ESCAPE: return ImGuiKey_Escape;
    case SDLK_Q: return ImGuiKey_Q;
    case SDLK_E: return ImGuiKey_E;
    case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
    case SDLK_RCTRL: return ImGuiKey_RightCtrl;
    case SDLK_LSHIFT: return ImGuiKey_LeftShift;
    case SDLK_RSHIFT: return ImGuiKey_RightShift;
    case SDLK_LALT: return ImGuiKey_LeftAlt;
    case SDLK_RALT: return ImGuiKey_RightAlt;
    default: return ImGuiKey_None;
    }
}

ImGuiKey KeyFromGamepad(u8 button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceDown;
    case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceRight;
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_GAMEPAD_BUTTON_BACK: return ImGuiKey_GamepadBack;
    default: return ImGuiKey_None;
    }
}

float PixelDensity(SDL_WindowID id) {
    SDL_Window* window = SDL_GetWindowFromID(id);
    const float density = window ? SDL_GetWindowPixelDensity(window) : 1.0f;
    return density > 0.0f ? density : 1.0f;
}

// Marks the settings dirty when a widget changed them.
template <typename T>
void Store(std::atomic<T>& target, T value, bool changed) {
    if (changed) {
        target = value;
        dirty = true;
    }
}

void Checkbox(const char* label, std::atomic<bool>& value) {
    bool v = value;
    const bool changed = ImGui::Checkbox(label, &v);
    Store(value, v, changed);
}

// A combo over `count` labels storing an index or a value from `values`.
void Choice(const char* label, std::atomic<int>& target, const char* const* labels, const int* values,
            int count) {
    int current = 0;
    for (int i = 0; i < count; ++i) {
        if ((values ? values[i] : i) == target.load()) current = i;
    }
    if (ImGui::Combo(label, &current, labels, count)) {
        Store(target, values ? values[current] : current, true);
    }
}

// The note for the item under the mouse or the gamepad/keyboard focus, shown in the footer.
const char* footer_help = nullptr;

std::string footer_text;

// Menu text in the language pack in use (tlg_lang.h), English without one.
inline const char* TR(const char* en) { return TlgLang::Tr(en); }

void Help(const char* text) {
    if (text && (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) || ImGui::IsItemFocused())) {
        footer_help = text;
    }
}

// tlg: a row's default and its value on PS4, shown above its note. No PS4 value: not a PS4 setting
// or not known.
struct Marks {
    std::string def, ps4;
};

void Help(const char* text, const Marks& marks) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) || ImGui::IsItemFocused()) {
        footer_text = std::string(TR("Default: ")) + marks.def;
        if (!marks.ps4.empty()) footer_text += " · PS4: " + marks.ps4;
        if (text) (footer_text += '\n') += text;
        footer_help = footer_text.c_str();
    }
}

// The settings as they are without tlg.ini: the defaults the rows show.
const TlgSettings::Values& Defaults() {
    static const TlgSettings::Values defaults{};
    return defaults;
}

template <typename T>
std::string Format(const char* format, T value) {
    char text[48];
    std::snprintf(text, sizeof(text), format, value);
    return text;
}

std::string OnOff(bool on) {
    return on ? TR("on") : TR("off");
}

std::string ChoiceLabel(const char* const* labels, const int* values, int count, int value) {
    for (int i = 0; i < count; ++i) {
        if ((values ? values[i] : i) == value) return labels[i];
    }
    return std::to_string(value);
}

// The Default and PS4 buttons after a slider: the width to leave for them, then the buttons.
float ResetWidth(bool ps4) {
    const ImGuiStyle& style = ImGui::GetStyle();
    float width = ImGui::CalcTextSize(TR("Default")).x + style.FramePadding.x * 2 + style.ItemSpacing.x;
    if (ps4) width += ImGui::CalcTextSize("PS4").x + style.FramePadding.x * 2 + style.ItemSpacing.x;
    return width;
}

template <typename T>
void ResetButtons(std::atomic<T>& value, T def, std::optional<T> ps4, const Marks& marks) {
    ImGui::SameLine();
    if (ImGui::SmallButton(TR("Default"))) Store(value, def, true);
    Help(TR("Back to the default."), marks);
    if (ps4) {
        ImGui::SameLine();
        if (ImGui::SmallButton("PS4")) Store(value, *ps4, true);
        Help(TR("Back to the value on PS4."), marks);
    }
}

// Two-column rows: the name on the left, the control on the right.
bool BeginRows(const char* id) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp)) {
        return false;
    }
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 0.52f);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.48f);
    return true;
}

void Row(const char* label) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::PushID(label);
}

void CheckRow(const char* label, std::atomic<bool>& value, const char* help, bool def,
              std::optional<bool> ps4 = std::nullopt) {
    Row(label);
    Checkbox("##value", value);
    Help(help, {OnOff(def), ps4 ? OnOff(*ps4) : ""});
    ImGui::PopID();
}

void ChoiceRow(const char* label, std::atomic<int>& target, const char* const* labels, const int* values,
               int count, const char* help, int def, std::optional<int> ps4 = std::nullopt) {
    Row(label);
    Choice("##value", target, labels, values, count);
    Help(help, {ChoiceLabel(labels, values, count, def), ps4 ? ChoiceLabel(labels, values, count, *ps4) : ""});
    ImGui::PopID();
}

void InfoRow(const char* label, const char* text) {
    Row(label);
    ImGui::TextDisabled("%s", text);
    ImGui::PopID();
}

void SliderRow(const char* label, std::atomic<int>& value, int lo, int hi, const char* format, const char* help,
               int def, std::optional<int> ps4 = std::nullopt) {
    Row(label);
    const Marks marks{Format(format, def), ps4 ? Format(format, *ps4) : ""};
    ImGui::SetNextItemWidth(-ResetWidth(ps4.has_value()));
    int v = value;
    if (ImGui::SliderInt("##value", &v, lo, hi, format, ImGuiSliderFlags_AlwaysClamp)) {
        Store(value, std::clamp(v, lo, hi), true);
    }
    Help(help, marks);
    ResetButtons(value, def, ps4, marks);
    ImGui::PopID();
}

void FloatRow(const char* label, std::atomic<float>& value, float lo, float hi, const char* format, const char* help,
              float def) {
    Row(label);
    const Marks marks{Format(format, def), ""};
    ImGui::SetNextItemWidth(-ResetWidth(false));
    float v = value;
    if (ImGui::SliderFloat("##value", &v, lo, hi, format, ImGuiSliderFlags_AlwaysClamp)) {
        Store(value, std::clamp(v, lo, hi), true);
    }
    Help(help, marks);
    ResetButtons(value, def, std::optional<float>(), marks);
    ImGui::PopID();
}

// A float slider over a color range; the game's own value (PS4) is the range's neutral one.
void FloatSliderRow(const char* label, std::atomic<float>& value, const TlgGraphics::ColorRange& range,
                    const char* help, float def) {
    Row(label);
    const Marks marks{Format("%.2f", def), Format("%.2f", range.neutral)};
    ImGui::SetNextItemWidth(-ResetWidth(true));
    float v = value;
    if (ImGui::SliderFloat("##value", &v, range.min, range.max, "%.2f", ImGuiSliderFlags_AlwaysClamp)) {
        Store(value, TlgGraphics::ClampColor(range, v), true);
    }
    Help(help, marks);
    ResetButtons(value, def, std::optional<float>(range.neutral), marks);
    ImGui::PopID();
}

const char* UpscalerLabel(int upscaler) {
    switch (upscaler) {
    case TlgSettings::UpscalerFsr3: return "FSR 3.1";
    case TlgSettings::UpscalerFsr4: return "FSR 4";
    case TlgSettings::UpscalerFsr411: return "FSR 4.1.1";
    case TlgSettings::UpscalerTaa: return "TAA";
    case TlgSettings::UpscalerDlss: return "DLSS";
    default: return "";
    }
}


const Vulkan::Instance* overlay_instance = nullptr; // for the overlay's measurements

// The performance overlay's text with the current settings (also the menu's preview).
std::string OverlayText() {
    namespace P = TlgPerfStats;
    const auto& s = TlgSettings::Get();
    const unsigned items = s.overlay_items;
    if ((items & (P::Gpu | P::Cpu | P::Ram | P::Vram)) && overlay_instance) {
        P::Start(*overlay_instance);
    }
    P::Sample sample = P::Latest();
    sample.fps = frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f;
    sample.frame_ms = frame_ms_avg;
    sample.upscaler = UpscalerLabel(s.upscaler);
    return P::Format(sample, items, s.overlay_layout == 0);
}

constexpr const char* Unsupported = "Unavailable: game version or code signature mismatch.";


// tlg: graphics presets (TlgSettings::GraphicsPresetNames): every look setting at once, the
// texture pack included. Changing a setting by hand shows "Custom".
const char* PresetLabel(int index, const std::string& name) {
    return index == 0 ? TR("Stock (as shipped)")
           : index == 1 ? TR("Recommended (default)")
                        : name.c_str();
}

void PresetRows() {
    const std::vector<std::string> names = TlgSettings::GraphicsPresetNames();
    const int current = TlgSettings::MatchingGraphicsPreset();
    Row(TR("Graphics preset"));
    if (ImGui::BeginCombo("##value", current < 0 ? TR("Custom") : PresetLabel(current, names[current]))) {
        for (int i = 0; i < int(names.size()); ++i) {
            if (ImGui::Selectable(PresetLabel(i, names[i]), i == current)) {
                TlgSettings::ApplyGraphicsPreset(i);
                dirty = true;
            }
        }
        ImGui::EndCombo();
    }
    Help(TR("Switches all settings on this tab and the texture pack at once, live. Stock is the game as shipped; Recommended is the default. Your own presets are files in the presets folder, also listed in the launcher."));
    ImGui::PopID();
    static char name[48] = "";
    Row(TR("Save as preset"));
    const ImGuiStyle& style = ImGui::GetStyle();
    const char* save = TR("Save");
    const char* remove = TR("Delete");
    const bool own = current >= 2;
    float buttons = ImGui::CalcTextSize(save).x + style.FramePadding.x * 2 + style.ItemSpacing.x;
    if (own) buttons += ImGui::CalcTextSize(remove).x + style.FramePadding.x * 2 + style.ItemSpacing.x;
    ImGui::SetNextItemWidth(-buttons);
    ImGui::InputTextWithHint("##name", TR("name"), name, sizeof(name));
    Help(TR("Saves the current settings under this name (an existing preset with the name is replaced)."));
    ImGui::SameLine();
    ImGui::BeginDisabled(name[0] == '\0');
    if (ImGui::Button(save) && TlgSettings::SaveGraphicsPreset(name) >= 0) {
        name[0] = '\0';
    }
    ImGui::EndDisabled();
    if (own) {
        ImGui::SameLine();
        if (ImGui::Button(remove)) {
            TlgSettings::DeleteGraphicsPreset(current);
        }
        Help(TR("Deletes the selected preset file. The settings stay as they are."));
    }
    ImGui::PopID();
}

void ImageTab() {
    namespace G = TlgGraphics;
    auto& s = TlgSettings::Get();
    const auto& d = Defaults();
    ImGui::SeparatorText(TR("Preset"));
    if (BeginRows("##preset")) {
        PresetRows();
        ImGui::EndTable();
    }
    ImGui::SeparatorText(TR("Display"));
    if (BeginRows("##display")) {
        static const char* const modes[] = {TR("Windowed"), TR("Borderless window"), TR("Fullscreen")};
        ChoiceRow(TR("Display mode"), s.display_mode, G::DisplayModeCount == 3 ? modes : G::DisplayModeNames.data(),
                  nullptr, G::DisplayModeCount, TR("Windowed, borderless window or exclusive fullscreen."), d.display_mode);
        CheckRow("VSync", s.vsync, TR("Waits for the monitor: no tearing, a little more input delay."), d.vsync);
        CheckRow("HDR", s.hdr,
                 TR("The game's own HDR10 output, used when HDR is on in Windows display settings. Applies after a restart."), d.hdr);
        static const char* const fg_labels[] = {TR("Off"), "DLSS", "FSR 3.1"};
        static const int fg_values[] = {0, 1, 2};
        // tlg: greyed out and shown as Off while the game runs at 30 FPS (launch mode or Frame
        // rate limit): the game treats it as off then (TlgSettings::EffectiveFrameGeneration) and
        // the stored choice comes back with a higher rate. DLSS alone is greyed on a card without
        // DLSS frame generation.
        {
            const bool blocked = TlgSettings::FrameGenerationBlocked();
            const bool dlss_fg = s.dlss_fg_supported != 0;
            const int stored = s.frame_generation;
            const int shown = blocked || (stored == 1 && !dlss_fg) ? 0 : stored;
            static std::string fg_help;
            fg_help = TR("Shows one made-up image between two game frames: smoother motion, a little more input delay. DLSS needs NVIDIA RTX 40 or newer; FSR is not tied to one brand.");
            fg_help += ' ';
            fg_help += blocked ? TR("Off now: the game runs at 30 FPS (frame rate mode or Frame rate limit), too low for frame generation. Your choice comes back at a higher frame rate.")
                               : TR("With VSync on, the screen's refresh rate must be at least twice the game's frame rate.");
            Row(TR("Frame generation"));
            ImGui::BeginDisabled(blocked);
            if (ImGui::BeginCombo("##value", fg_labels[shown])) {
                for (int i = 0; i < 3; ++i) {
                    ImGui::BeginDisabled(i == 1 && !dlss_fg);
                    if (ImGui::Selectable(fg_labels[i], i == shown) && i != shown) {
                        Store(s.frame_generation, fg_values[i], true);
                    }
                    ImGui::EndDisabled();
                }
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();
            Help(fg_help.c_str(), {ChoiceLabel(fg_labels, fg_values, 3, d.frame_generation), ""});
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    const int iw = s.image_width, ih = s.image_height, ww = s.window_width, wh = s.window_height;
    // tlg: only spatial filters on the final image. The temporal upscalers (FSR 3/4, DLSS) work on
    // the scene color (vk_temporal_upscaler), so they are not offered here.
    ImGui::SeparatorText(TR("Sharpening"));
    if (BeginRows("##scaling")) {
        // tlg: one Sharpening setting (2026-11-07): with DLSS / FSR / TAA it is their own RCAS in
        // the AA step (scene only, the UI stays crisp), with Built-in AMD CAS on the final picture.
        const bool aa_sharpens = TlgSettings::AaSharpenedRecently();
        CheckRow(TR("Sharpening"), s.sharpen,
                 aa_sharpens ? TR("Sharpens the image. Now done by the anti-aliasing step itself (its RCAS), on the scene only.")
                             : TR("Sharpens the image. With Built-in it is AMD CAS on the final picture; with DLSS, FSR or TAA their own sharpening."),
                 d.sharpen, false);
        ImGui::BeginDisabled(!s.sharpen);
        {
            Row(TR("Sharpening strength"));
            const Marks marks{Format("%.0f%%", d.sharpness * 100.0f), ""};
            ImGui::SetNextItemWidth(-ResetWidth(false));
            float pct = s.sharpness * 100.0f;
            if (ImGui::SliderFloat("##value", &pct, 0.0f, 400.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) {
                Store(s.sharpness, std::clamp(std::round(pct) / 100.0f, 0.0f, 4.0f), true);
            }
            Help(aa_sharpens ? TR("How strong the sharpening is. Up to 100%: the upscaler's own sharpening; every 100% above adds one more sharpening pass.")
                             : TR("How strong the sharpening is. CAS stops at 100%."), marks);
            ResetButtons(s.sharpness, d.sharpness.load(), std::optional<float>(), marks);
            ImGui::PopID();
        }
        ImGui::EndDisabled();
        ImGui::EndTable();
    }
    ImGui::SeparatorText(TR("Upscaling and anti-aliasing"));
    if (BeginRows("##antialiasing")) {
        namespace S = TlgSettings;
        // tlg: three rows (user, 2026-11-10, laid out as Ratchet & Clank: Rift Apart). Upscale
        // method: Off or an upscaler (Built-in, DLSS, FSR 3.1, FSR 4); supersampling removed
        // (user 2026-11-10). Upscale quality: the render size (the launcher patches it in at
        // start, so it needs a restart). Anti-aliasing: free to choose only with method Off; an
        // upscaler is its own anti-aliasing, the row then shows Off, greyed out. Stored as before:
        // preset (0 off, 1-4 upscaling) and upscaler (the upscaler, or the anti-aliasing at
        // method Off). DLSS needs tlg_dlss.dll and an NVIDIA RTX card, FSR 4 a
        // card with its shader features; both grey out when the card lacks them.
        // TAA (test 2026-11-07): our own light temporal AA, native size only.
        enum Method { MethodOff, MethodBuiltIn, MethodDlss, MethodFsr3, MethodFsr4, MethodCount };
        static const char* const method_labels[MethodCount] = {TR("Off"), TR("Built-in (game)"), "DLSS (NVIDIA)",
                                                               "FSR 3.1", "FSR 4"};
        static const int method_upscalers[MethodCount] = {-1, S::UpscalerOff, S::UpscalerDlss, S::UpscalerFsr3,
                                                          S::UpscalerFsr4};
        const int upscaler = S::SelectedUpscaler() == S::UpscalerFsr411 ? int(S::UpscalerFsr4) : S::SelectedUpscaler();
        const int preset = std::clamp(s.preset.load(), 0, int(S::UltraPerformance));
        int method = MethodOff;
        if (preset == S::NativeAA || upscaler == S::UpscalerTaa) {
            method = MethodOff;  // TAA always runs at native size (RenderPreset)
        } else {
            for (int i = MethodBuiltIn; i <= MethodFsr4; ++i) {
                if (method_upscalers[i] == upscaler) method = i;
            }
        }
        Row(TR("Upscale method"));
        if (ImGui::BeginCombo("##value", method_labels[method])) {
            for (int i = 0; i < MethodCount; ++i) {
                const bool supported = (i != MethodDlss || s.dlss_supported) && (i != MethodFsr4 || s.fsr4_supported);
                ImGui::BeginDisabled(!supported);
                if (ImGui::Selectable(method_labels[i], i == method) && i != method) {
                    if (i == MethodOff) {
                        s.preset = S::NativeAA;
                    } else {
                        if (method == MethodOff) s.preset = S::Quality;
                        S::SetUpscaler(method_upscalers[i]);
                    }
                    dirty = true;
                }
                ImGui::EndDisabled();
            }
            ImGui::EndCombo();
        }
        Help(!s.dlss_supported && s.dlss_problem.load()
                 ? s.dlss_problem.load()
                 : TR("How the image gets its size. Off: drawn at the screen's size. Built-in, DLSS, FSR 3.1 and FSR 4: drawn smaller and enlarged, for more speed; DLSS needs an NVIDIA RTX card, FSR 4 any recent card (not only AMD; sharper and steadier than FSR 3.1), FSR 3.1 runs on any card, Built-in is the game's own upscale pass. An upscaler also smooths the edges itself. F8 switches between Built-in and your choice in game."),
             {method_labels[MethodOff], ""});
        ImGui::PopID();
        static const char* const quality_labels[S::UltraPerformance + 1] = {
            TR("Off"), TR("Quality"), TR("Balanced"),
            TR("Performance"), TR("Ultra Performance")};
        const auto preset_name = [](int p) {
            return std::string(quality_labels[std::clamp(p, 0, int(S::UltraPerformance))]);
        };
        int ow = 0, oh = 0, rw = 0, rh = 0;
        S::OutputSize(ow, oh);
        S::PresetRenderSize(s.preset, ow, oh, rw, rh);
        static std::string quality_help;
        quality_help = preset_name(s.preset) + ": ";
        if (s.preset == S::NativeAA) {
            quality_help += Format(TR("renders at the output size, %dx"), ow) + Format("%d.", oh);
        } else {
            quality_help += Format(TR("renders at %dx"), rw) + Format("%d, ", rh) +
                            Format(TR("upscaled to %dx"), ow) + Format("%d.", oh);
        }
        if (s.preset != s.startup_preset) {
            int aw = 0, ah = 0;
            S::PresetRenderSize(s.startup_preset, ow, oh, aw, ah);
            quality_help += Format(TR(" Until the restart the game keeps rendering at %dx"), aw) +
                            Format("%d (", ah) + preset_name(s.startup_preset) + ").";
        }
        quality_help += TR(" Output / 1.5 (Quality), 1.7 (Balanced), 2 (Performance) or 3 (Ultra Performance) per side; the upscaler fills in the rest. Set at start, so changing it needs Apply and restart.");
        Row(TR("Upscale quality (requires restart)"));
        ImGui::BeginDisabled(method == MethodOff);
        if (ImGui::BeginCombo("##value", method == MethodOff ? "" : quality_labels[preset])) {
            for (int i = S::Quality; i <= S::UltraPerformance; ++i) {
                if (ImGui::Selectable(quality_labels[i], i == preset) && i != preset) {
                    s.preset = i;
                    dirty = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        Help(quality_help.c_str());
        ImGui::PopID();
        static const char* const aa_labels[] = {TR("Off (game's own)"), "DLAA (NVIDIA)", "FSR 3.1", "FSR 4", "TAA"};
        static const int aa_values[] = {S::UpscalerOff, S::UpscalerDlss, S::UpscalerFsr3, S::UpscalerFsr4,
                                        S::UpscalerTaa};
        constexpr int aa_count = int(std::size(aa_values));
        // An upscaler as the method: the row shows Off, greyed out (user, 2026-11-10).
        const bool aa_free = method == MethodOff;
        int aa_index = 0;
        for (int i = 0; i < aa_count && aa_free; ++i) {
            if (aa_values[i] == upscaler) aa_index = i;
        }
        Row(TR("Anti-aliasing"));
        ImGui::BeginDisabled(method != MethodOff);
        if (ImGui::BeginCombo("##value", aa_labels[aa_index])) {
            for (int i = 0; i < aa_count; ++i) {
                const bool supported = (aa_values[i] != S::UpscalerDlss || s.dlss_supported) &&
                                       (aa_values[i] != S::UpscalerFsr4 || s.fsr4_supported);
                ImGui::BeginDisabled(!supported);
                if (ImGui::Selectable(aa_labels[i], i == aa_index) && i != aa_index) {
                    S::SetUpscaler(aa_values[i]);
                    dirty = true;
                }
                ImGui::EndDisabled();
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        Help(TR("Smooths jagged edges. Yours to choose with Upscale method Off; an upscaler does its own. DLAA (NVIDIA RTX cards) and FSR 4 (any recent card) are the sharpest, FSR 3.1 runs on any card, TAA is lighter and softer. Off keeps only the game's own."),
             {s.dlss_supported ? "DLAA (NVIDIA)" : aa_labels[s.fsr4_supported ? 3 : 2], ""});
        ImGui::PopID();
        CheckRow("SMAA 1x", s.smaa,
                 TR("Smooths jagged edges (feathers, grass, railings) on the final image. The game draws its HUD and subtitles into the same image, so text edges are smoothed too. About 0.2 to 0.4 ms per frame at 1080p (RTX 2060)."), d.smaa, false);
        ImGui::EndTable();
    }
    ImGui::SeparatorText(TR("Enhancements"));
    if (BeginRows("##enhancements")) {
        static const char* const shadow_labels[] = {TR("Game (2048)"), "4096", "8192"};
        static const int shadow_values[] = {1, 2, 4};
        ChoiceRow(TR("Shadow resolution"), s.shadow_scale, shadow_labels, shadow_values, 3,
                  TR("Draws the shadow maps at 2x or 4x the game's 2048x2048 (default: Game). The main gain is much less shimmer of tree and leaf shadows, with any anti-aliasing. 8192 costs about 8 FPS and up to about 2 GB of video memory (4096 about a quarter of that). With 8 GB of video memory or less keep Game: running out causes stutter and crashes."), d.shadow_scale, 1);
        SliderRow(TR("Field of view"), s.fov, 50, 110, "%d°",
                  TR("Camera field of view. Default 70° is as shipped; higher shows more around the boy. Applies while the game runs (not in the paused menu)."), d.fov, 70);
        ImGui::EndTable();
    }
    ImGui::SeparatorText(TR("Game effects (100% = as shipped)"));
    if (BeginRows("##effects")) {
        SliderRow(TR("Ambient occlusion"), s.ssao, 0, 400, "%d%%",
                  TR("Strength of the contact shadows in corners and under grass. 0% turns the pass off (saves a little GPU time). Above 150% the half-resolution AO shows blocky halos."), d.ssao, 100);
        SliderRow(TR("Bloom"), s.bloom, 0, 400, "%d%%",
                  TR("Glow around bright light. 0% turns the pass off."), d.bloom, 100);
        SliderRow(TR("Screen-space reflections"), s.ssr, 0, 400, "%d%%",
                  TR("Reflections and light glare on water and wet stone. 0% turns the pass off."), d.ssr, 100);
        SliderRow(TR("Motion blur"), s.motion_blur, 0, 400, "%d%%",
                  TR("Camera motion blur. 0% turns the pass off."), d.motion_blur, 100);
        SliderRow(TR("Vignette"), s.vignette, 0, 100, "%d%%",
                  TR("Darkened screen corners. The game ships with this off: 0% is as shipped and recommended."), d.vignette, 0);
        SliderRow(TR("Game sharpening"), s.game_sharpen, 0, 100, "%d%%",
                  TR("The game's own sharpening filter. On by default: 100% is the original and the maximum (recommended)."), d.game_sharpen, 100);
        SliderRow(TR("Fog distance"), s.fog_distance, 25, 400, "%d%%",
                  TR("Where the fog starts and reaches full strength. Default 100% is as shipped; lower brings the fog closer."), d.fog_distance, 100);
        SliderRow(TR("Haze color fringe"), s.haze_fringe, 0, 100, "%d%%",
                  TR("The game's chromatic aberration on the light haze: yellow and blue edges around objects against fog, strongest toward the screen edges. Sharp anti-aliasing (DLAA, DLSS) makes it stand out more. Default 0% gives clean edges; 100% is as shipped."), d.haze_fringe, 100);
        ImGui::EndTable();
    }
    ImGui::SeparatorText(TR("Wind and foliage"));
    if (BeginRows("##wind")) {
        static const char* const grass_labels[] = {TR("Every 4th frame (game)"),
                                                   TR("Every frame (default)")};
        static const int grass_values[] = {0, 1};
        ChoiceRow(TR("Grass animation"), s.grass_smooth, grass_labels, grass_values, 2,
                  TR("The game moves most grass only every 4th frame, which looks choppy at high frame rates. Every frame (default) moves it smoothly."), d.grass_smooth, 0);
        SliderRow(TR("Boy wind (hair + clothing)"), s.boy_wind, 0, 200, "%d%%",
                  TR("How strongly the wind moves the boy's hair and clothing. Default 100% is as shipped and can look like a storm; 50% is recommended, 30% is calm."), d.boy_wind, 100);
        SliderRow(TR("Tree wind"), s.tree_wind, 0, 200, "%d%%",
                  TR("How strongly the wind moves trees, leaves and ivy. Default 100% is as shipped; 50% looks more natural (recommended), 0% keeps them still."), d.tree_wind, 100);
        SliderRow(TR("Trico feather wind"), s.trico_feathers, 0, 400, "%d%%",
                  TR("How strongly the wind moves Trico's feathers. Default 100% is as shipped; 0% keeps them still."), d.trico_feathers, 100);
        ImGui::EndTable();
    }
    ImGui::SeparatorText(TR("Textures"));
    if (BeginRows("##textures")) {
        Row(TR("Texture LOD bias (DLAA)"));
        int bias = s.lod_bias;
        char bias_text[32];
        std::snprintf(bias_text, sizeof(bias_text), "%+.2f", bias * 0.25f);
        // PS4: the game's own bias (0).
        const Marks bias_marks{Format("%+.2f", d.lod_bias * 0.25f), Format("%+.2f", 0.0f)};
        ImGui::SetNextItemWidth(-ResetWidth(true));
        if (ImGui::SliderInt("##value", &bias, -12, 4, bias_text, ImGuiSliderFlags_AlwaysClamp)) {
            Store(s.lod_bias, std::clamp(bias, -12, 4), true);
        }
        Help(TR("Sharper (below 0) or softer (above 0) scene textures under DLAA; no effect without DLAA. -1.00 is NVIDIA's advice for native resolution. Lower brings out fine detail, mostly on grass (-3.00 has 31% more fine detail than +1.00), but can shimmer in motion."), bias_marks);
        ResetButtons(s.lod_bias, d.lod_bias.load(), std::optional<int>(0), bias_marks);
        ImGui::PopID();
        static const char* const aniso_labels[] = {TR("Game"), "2x", "4x", "8x", "16x"};
        static const int aniso_values[] = {0, 2, 4, 8, 16};
        ChoiceRow(TR("Anisotropic filtering"), s.aniso, aniso_labels, aniso_values, 5,
                  TR("Keeps ground and wall textures sharp at shallow angles. The game uses trilinear and 4x. In our tests (a stone path, a pillar hall) 16x looked the same as Game; it stays on because the cost is very small."), d.aniso, 0);
        static const auto pack_names = [] {
            std::vector<std::string> names{TR("Off (default)")};
            for (const auto& name : TlgSettings::TexturePackNames()) names.push_back(name);
            return names;
        }();
        static const auto pack_labels = [] {
            std::vector<const char*> labels;
            for (const auto& name : pack_names) labels.push_back(name.c_str());
            return labels;
        }();
        static const auto pack_values = [] {
            std::vector<int> values;
            for (int i = 0; i < int(pack_names.size()); ++i) values.push_back(i);
            return values;
        }();
        ChoiceRow(TR("Texture pack"), s.texture_pack, pack_labels.data(), pack_values.data(),
                  int(pack_labels.size()),
                  TR("AI-upscaled versions of the game's textures, made from your own copy of the game (ps4-texturepack-generator, docs/texture-pack.md); the game files are not changed. Switches live; textures appear as they load (a second or two)."), d.texture_pack, 0);
        ImGui::EndTable();
    }
    ImGui::SeparatorText(TR("Color"));
    if (BeginRows("##color")) {
        CheckRow(TR("Color adjustments"), s.color_adjust,
                 TR("Brightness, contrast, saturation and gamma of the final image (the HUD included). SDR only; HDR is left as is. Default on, all neutral (gamma 1); off is the game as shipped. No measurable cost."), d.color_adjust, false);
        ImGui::BeginDisabled(!s.color_adjust);
        FloatSliderRow(TR("Brightness"), s.brightness, G::Brightness, TR("Adds light or shadow evenly; 0 is unchanged."), d.brightness);
        FloatSliderRow(TR("Contrast"), s.contrast, G::Contrast, TR("Spreads tones away from mid gray; 1 is unchanged."), d.contrast);
        FloatSliderRow(TR("Saturation"), s.saturation, G::Saturation, TR("Color intensity: 0 is black and white, 1 is unchanged."), d.saturation);
        FloatSliderRow(TR("Gamma"), s.color_gamma, G::Gamma, TR("Above 1 lightens the midtones, below 1 darkens them; blacks and whites stay. Default 1, the game's own; 0.75 tames the over-lit SDR image outdoors."), d.color_gamma);
        ImGui::EndDisabled();
        ImGui::EndTable();
    }
    ImGui::SeparatorText(TR("Information"));
    if (BeginRows("##image_info")) {
        char text[96];
        std::snprintf(text, sizeof(text), "%d x %d", iw, ih);
        InfoRow(TR("Game image"), text);
        std::snprintf(text, sizeof(text), "%d x %d", ww, wh);
        InfoRow(TR("Windowed"), text);
        InfoRow(TR("Sharpening"), !s.sharpen ? TR("off") : s.rcas_applied ? TR("applied (CAS)") : TlgSettings::AaSharpenedRecently() ? TR("applied (RCAS, in the AA step)") : TR("not applied"));
        ImGui::EndTable();
    }
}

// tlg: tuning tests, all live: the rows from TLG_LAB (tlg_lab.h), not saved; they last until reset
// or the game closes. Settings that prove worth keeping move to the Image tab.
[[maybe_unused]] void LabTab() {
    auto& s = TlgSettings::Get();
    const auto& d = Defaults();
    const std::vector<TlgLab::Row> rows = TlgLab::Rows();
    ImGui::SeparatorText(TR("Game values (lab.txt, not saved)"));
    if (rows.empty()) {
        ImGui::TextDisabled("%s", std::getenv("TLG_LAB") ? TR("No rows in the TLG_LAB file.")
                                                         : TR("Start with port\\tools\\jitter\\start.cmd for the lab rows (TLG_LAB)."));
    } else if (ImGui::BeginTable("##lab_rows", 3, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 0.40f);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.44f);
        ImGui::TableSetupColumn("game", ImGuiTableColumnFlags_WidthStretch, 0.16f);
        for (size_t i = 0; i < rows.size(); ++i) {
            const TlgLab::Row& row = rows[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            if (row.active) {
                ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.45f, 1.0f), "%s", row.label.c_str());
            } else {
                ImGui::TextUnformatted(row.label.c_str());
            }
            ImGui::TableNextColumn();
            ImGui::PushID(int(i));
            const ImGuiStyle& style = ImGui::GetStyle();
            ImGui::SetNextItemWidth(-(ImGui::CalcTextSize("R").x + style.FramePadding.x * 2 + style.ItemSpacing.x));
            // A "=" row shows the game's value until it is moved.
            float v = row.value;
            if (!row.active && row.mode == '=' && !row.game.empty()) {
                v = std::strtof(row.game.c_str(), nullptr);
            }
            if (ImGui::SliderFloat("##value", &v, row.lo, row.hi, row.format.c_str())) {
                TlgLab::Set(i, v);
            }
            Help(row.help.empty() ? nullptr : row.help.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("R")) {
                TlgLab::Reset(i);
            }
            Help(TR("Back to the game's value."));
            ImGui::PopID();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", row.game.empty() ? "?" : row.game.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    if (ImGui::Button(TR("Reset all"))) {
        TlgLab::ResetAll();
        s.lod_bias = -4;
        s.aniso = 16;
        dirty = true;
    }
    Help(TR("Fixed rows back to their defaults, lab rows back to the game's values."));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", TR("Ctrl+click a slider to type a value. The right column is the game's own value (the part the row changes). Rows change the game's objects every 15 frames and follow area changes; pausing stops them."));
    ImGui::PopTextWrapPos();
}

void PerformanceTab() {
    namespace G = TlgGraphics;
    auto& s = TlgSettings::Get();
    const auto& d = Defaults();
    // tlg: no render-resolution or engine-frame-rate patches here (TLG has its own) and no readback
    // shortcuts: they crash TLG (it needs precise readbacks).
    ImGui::SeparatorText(TR("Live"));
    if (BeginRows("##live")) {
        static const char* const fps_labels[] = {"30", "60", "120", "240", TR("Unlimited")};
        // tlg: the engine ceiling is the launch's frame rate mode (launcher, TLG_FPS): a limit above it
        // has no effect.
        const char* fps_tip = s.applied_engine_fps <= 30
            ? TR("Caps the presented frames. The game runs at 30 FPS (as on PS4); 60 FPS is a launcher option.")
            : s.applied_engine_fps <= 60
            ? TR("Caps the presented frames. 60 FPS mode (launcher): limits above 60 have no effect.")
            : TR("Caps the presented frames. Unlocked mode (launcher): Unlimited follows the display refresh, up to 120.");
        ChoiceRow(TR("Frame rate limit"), s.fps_limit, fps_labels, G::FpsLimits.data(), int(G::FpsLimits.size()),
                  fps_tip, d.fps_limit, 30);
        static const char* const queued_labels[] = {TR("1 (lowest latency)"), "2", TR("Unlimited")};
        ChoiceRow(TR("Frames queued"), s.frames_queued, queued_labels, G::FramesQueued.data(),
                  int(G::FramesQueued.size()), TR("More frames queued: steadier FPS, slower response to input."), d.frames_queued);
        static const char* const batch_labels[] = {TR("Off (recommended)"), "16 MB",
                                                   TR("64 MB (not recommended)")};
        ChoiceRow(TR("GPU readback batch"), s.readback_batch, batch_labels,
                  G::ReadbackBatchMb.data(), int(G::ReadbackBatchMb.size()),
                  TR("When the game reads GPU-written memory, also copy back the other GPU-written data around it. "
                     "Off is fastest (2-5 FPS more in tests); 64 MB is not recommended."),
                  d.readback_batch);
        static const char* const slow_labels[] = {TR("30 FPS during slow motion (advised)"),
                                                  TR("Skip the slow motion")};
        static const int slow_values[] = {0, 1};
        ChoiceRow(TR("Slow-motion freeze fix (60 FPS / unlocked)"),
                  s.slow_motion, slow_labels, slow_values, 2,
                  TR("Fixes a known bug of the game at 60 FPS and unlocked: two slow-motion moments turn choppy and can "
                     "freeze the game. The same bug made the PS4 60 FPS patches freeze here. The moments: the jump to "
                     "Trico's tail (s06_FallingBridge, cutscene fb_110) and the collapsing bridge (s10_Pit). "
                     "Issues #39 and #40. At 30 FPS nothing changes. Applies when the scene starts."),
                  d.slow_motion);
        CheckRow(TR("Texture cleanup after loads"), s.texture_cleanup,
                 TR("After each stage or checkpoint load, frees the textures of earlier stages that the new one does "
                    "not use. Without it they stay in video memory until it runs short. In one test it kept 69 FPS "
                    "where the game had dropped to 57 after 28 loads, but in a longer test the game still got "
                    "slower over many loads (issue #20), so it is off by default for now. Off is the emulator's "
                    "original behaviour."),
                 d.texture_cleanup);
        CheckRow(TR("Keep GPU-only textures"), s.keep_gpu_textures,
                 TR("Keeps a texture the GPU drew into while its only correct copy is on the GPU (a game write right next to it blocks the copy back), until video memory is almost full. Without it, cleanup can throw that picture away and the game gets old data back. From SpartanPort (@maze1377). On by default; turn it off if video memory runs full more often."),
                 d.keep_gpu_textures);
        // Test switches for the buffer memory return of v0.5.2 (SotC port), issue #38.
        CheckRow(TR("Debug: give back freed buffer memory"), s.arena_release,
                 TR("Since v0.5.2: when the game frees memory, its graphics card copy goes too. Test switch (issue #38): "
                    "turn it off to check whether a problem started with v0.5.2. Off is the behaviour before v0.5.2. "
                    "From the Shadow of the Colossus PC port (@AjncSystem, @Oussama268)."),
                 d.arena_release);
        CheckRow(TR("Debug: texture cleanup follows the driver"), s.gc_live_budget,
                 TR("Since v0.5.2: texture cleanup starts at the video memory the driver offers now, never below the "
                    "start-up limits. Test switch (issue #38): off uses only the start-up limits (the behaviour before "
                    "v0.5.2), so textures are cleaned up sooner."),
                 d.gc_live_budget);
        ImGui::EndTable();
    }
}

void OverlayTab() {
    namespace P = TlgPerfStats;
    auto& s = TlgSettings::Get();
    const auto& d = Defaults();
    if (BeginRows("##overlay_on")) {
        CheckRow(TR("Show overlay (F10)"), s.show_fps, TR("The performance overlay in a corner of the screen."), d.show_fps);
        ImGui::EndTable();
    }
    ImGui::BeginDisabled(!s.show_fps);
    ImGui::SeparatorText(TR("What to show"));
    if (ImGui::BeginTable("##overlay_items", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableNextColumn();
        static const char* const names[] = {"FPS", TR("Frametime"), "Upscaler", TR("GPU (name and use)"), TR("CPU (game)"),
                                            TR("RAM (game / total)"), TR("VRAM (used / available)")};
        for (unsigned item = 0; item < 7; ++item) {
            const unsigned bit = 1u << item;
            bool on = (s.overlay_items & bit) != 0;
            if (ImGui::Checkbox(names[item], &on)) {
                s.overlay_items = on ? (s.overlay_items | bit) : (s.overlay_items & ~bit);
                dirty = true;
            }
        }
        ImGui::TableNextColumn();
        ImGui::TextDisabled(TR("Preview"));
        ImGui::BeginChild("##preview", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
        ImGui::SetWindowFontScale(s.overlay_scale / 100.0f);
        const std::string text = OverlayText();
        ImGui::TextUnformatted(text.empty() ? TR("(nothing selected)") : text.c_str());
        ImGui::SetWindowFontScale(1.0f);
        ImGui::EndChild();
        ImGui::EndTable();
    }
    ImGui::SeparatorText(TR("Appearance"));
    if (BeginRows("##appearance")) {
        SliderRow(TR("Font size"), s.overlay_scale, 50, 300, "%d%%", TR("Size of the overlay text."), d.overlay_scale);
        static const char* const corners[] = {TR("Top left"), TR("Top right"), TR("Bottom left"), TR("Bottom right")};
        ChoiceRow(TR("Corner"), s.overlay_corner, corners, nullptr, 4, TR("Where the overlay sits."), d.overlay_corner);
        SliderRow(TR("Background opacity"), s.overlay_opacity, 0, 100, "%d%%", TR("0% is no background."), d.overlay_opacity);
        static const char* const layouts[] = {TR("One line"), TR("One item per line")};
        ChoiceRow(TR("Layout"), s.overlay_layout, layouts, nullptr, 2, TR("Items side by side or stacked."), d.overlay_layout);
        ImGui::EndTable();
    }
    ImGui::EndDisabled();
    ImGui::SeparatorText(TR("About"));
    if (BeginRows("##about")) {
        const auto& elf = Common::ElfInfo::Instance();
        char text[160];
        std::snprintf(text, sizeof(text), "%.*s v%.*s", int(elf.GameSerial().size()), elf.GameSerial().data(),
                      int(elf.AppVer().size()), elf.AppVer().data());
        InfoRow(TR("Game"), text);
        const u32 total = background_total;
        if (total) {
            std::snprintf(text, sizeof(text), TR("loading %u%%"), u32(u64(background_done) * 100 / total));
        } else {
            std::snprintf(text, sizeof(text), TR("ready"));
        }
        InfoRow(TR("Shader cache"), text);
        ImGui::EndTable();
    }
}

// Sound output (src/runtime_audio.c, tlg.ini audio_device / audio_surround). The game opens its
// audio at boot, so both apply at the next start. The device list is SDL's (the runtime's own).
void SoundRows() {
    auto& s = TlgSettings::Get();
    ImGui::SeparatorText(TR("Sound"));
    if (!BeginRows("##sound")) {
        return;
    }
    Row(TR("Output device"));
    const std::string current = s.audio_device.empty() ? std::string(TR("System default")) : s.audio_device;
    if (ImGui::BeginCombo("##value", current.c_str())) {
        if (ImGui::Selectable(TR("System default"), s.audio_device.empty()) && !s.audio_device.empty()) {
            s.audio_device.clear();
            dirty = true;
        }
        int count = 0;
        SDL_AudioDeviceID* ids = SDL_GetAudioPlaybackDevices(&count);
        for (int i = 0; ids && i < count; ++i) {
            const char* name = SDL_GetAudioDeviceName(ids[i]);
            if (name && ImGui::Selectable(name, s.audio_device == name) && s.audio_device != name) {
                s.audio_device = name;
                dirty = true;
            }
        }
        SDL_free(ids);
        ImGui::EndCombo();
    }
    Help(TR("Where the game's sound goes. Applies at the next start; an unplugged device falls back to the system default."));
    ImGui::PopID();
    CheckRow(TR("Always send 7.1"), s.audio_surround,
             TR("Sends all 8 channels even to a stereo device. Turn on for Windows Sonic or Dolby Atmos for "
                "Headphones (headphone surround). Off: stereo devices get a stereo mix. Applies at the next start."),
             false);
    ImGui::EndTable();
}

// Startup options changed since this launch (applied by "Apply and restart").
// Mouse and keyboard (src/runtime_pad.c): the mouse settings apply at once; the layout is the
// default one unless the launcher's Controls (tlg.ini key_map) or TLG_KEY_MAP changed it.
void ControlsTab() {
    auto& s = TlgSettings::Get();
    const auto& d = Defaults();
    ImGui::SeparatorText(TR("Mouse"));
    if (BeginRows("##mouse")) {
        CheckRow(TR("Mouse turns the camera"), s.mouse_enabled,
                 TR("The game window takes the mouse while it has focus. Esc releases it; a click in the window takes it back."), d.mouse_enabled);
        ImGui::BeginDisabled(!s.mouse_enabled);
        FloatRow(TR("Sensitivity"), s.mouse_sensitivity, 0.1f, 5.0f, "%.2f",
                 TR("How far the camera turns for the same mouse movement."), d.mouse_sensitivity);
        CheckRow(TR("Invert Y axis"), s.mouse_invert_y,
                 TR("Moving the mouse forward looks down."), d.mouse_invert_y);
        FloatRow(TR("Smoothing"), s.mouse_smoothing, 0.0f, 0.9f, "%.2f",
                 TR("Evens out the camera motion; higher values feel softer and a little slower."), d.mouse_smoothing);
        FloatRow(TR("Acceleration"), s.mouse_accel, 0.0f, 2.0f, "%.2f",
                 TR("Fast movements turn more than slow ones. 0 is linear."), d.mouse_accel);
        FloatRow(TR("Anti-dead zone"), s.mouse_deadzone, 0.0f, 0.5f, "%.2f",
                 TR("Small mouse movements skip the game's stick dead zone. Raise it if slow movements do nothing."), d.mouse_deadzone);
        ImGui::EndDisabled();
        ImGui::EndTable();
    }
    ImGui::SeparatorText(TR("Controller"));
    if (BeginRows("##combos")) {
        const char* labels[4] = {TR("Quit game (hold 2 seconds)"), TR("In-game menu"), TR("QA menu"),
                                 TR("Photo mode on / off")};
        const u32 defaults[4] = {TlgSettings::PadComboQuit, TlgSettings::PadComboMenu, TlgSettings::PadComboQa,
                                 TlgSettings::PadComboPhoto};
        for (int slot = 0; slot < 4; ++slot) {
            Row(labels[slot]);
            if (capture_slot == slot) {
                ImGui::TextUnformatted(TR("Hold the buttons, then release..."));
                ImGui::SameLine();
                if (ImGui::SmallButton(TR("Cancel"))) {
                    capture_slot = -1;
                }
            } else {
                const u32 combo = ComboSlot(slot)->load();
                ImGui::TextUnformatted(TlgSettings::ComboLabel(combo).c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton(TR("Set"))) {
                    capture_slot = slot;
                    capture_mask = 0;
                    capture_ignore = pad_mask;
                }
                if (combo != defaults[slot]) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton(TR("Default"))) {
                        ComboSlot(slot)->store(defaults[slot]);
                        dirty = true;
                    }
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", TR("Set: press and hold two or more buttons together, then let go. Keyboard: Alt+Q quits, Insert opens this menu."));
    ImGui::PopTextWrapPos();
    ImGui::SeparatorText(TR("Keyboard and mouse layout"));
    if (BeginRows("##layout")) {
        InfoRow(TR("Move / camera"), TR("WASD / mouse or arrow keys"));
        InfoRow(TR("Jump (Cross)"), TR("Space"));
        InfoRow(TR("Grab, hold on (Triangle)"), TR("Left click or Q"));
        InfoRow(TR("Call Trico (R1)"), TR("Right click or 3"));
        InfoRow(TR("Square / Circle"), TR("E or middle click / Left Shift"));
        InfoRow("L1 / L2 / R2", "1 / R / F");
        InfoRow("L3 / R3", "Z / C");
        InfoRow(TR("D-pad"), "I J K L");
        InfoRow("Options", "Enter");
        InfoRow(TR("Touchpad (photo / pause)"), "Tab / Backspace");
        InfoRow(TR("Release the mouse"), "Esc");
        const char* env = std::getenv("TLG_KEY_MAP");
        const std::string remap = env && env[0] ? std::string(env) : s.key_map;
        if (!remap.empty()) {
            InfoRow(TR("Remapped (launcher)"), remap.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", TR("Remap keys and mouse buttons in the launcher (Controls); it applies at the next start."));
    ImGui::PopTextWrapPos();
    SoundRows();
}

// tlg: Fun tab: jump to any start point of the story (as the game's QA menu does, without
// turning it on) and extra Tricos. Commands run at the game's next settings tick.
// The move bar: the working moves of TlgFun::Actions (before "May not work"), one row per group,
// each an icon (TlgFunIcons, same order) with its name under it and as a tooltip. Fun tab and F12.
void MakeMoveIcons() {
    static bool tried = false;
    if (tried) {
        return;
    }
    tried = true;
    TlgPng::Image atlas;
    if (!TlgPng::DecodeRgba(TlgFunIcons::Png, sizeof(TlgFunIcons::Png), atlas) || atlas.rgba.empty()) {
        std::printf("Fun: move icons did not decode\n");
        return;
    }
    move_icons = IM_NEW(ImTextureData)();
    move_icons->Create(ImTextureFormat_RGBA32, atlas.width, atlas.height);
    std::memcpy(move_icons->GetPixels(), atlas.rgba.data(), atlas.rgba.size());
    ImGui::RegisterUserTexture(move_icons);
}

// rows 0: one row per group (the menu tab); rows > 0: the moves spread evenly over that many rows (the
// F12 bar, 2 rows: user 2026-11-05).
void MoveBar(float slot, int rows = 0) {
    const float label_h = ImGui::GetTextLineHeight() * 0.8f * 2.0f;
    int per_row = 0;
    if (rows > 0) {
        int moves = 0;
        for (const auto& a : TlgFun::Actions) {
            if (!a.command && std::strcmp(a.name, "May not work") == 0) {
                break;
            }
            moves += a.command ? 1 : 0;
        }
        per_row = (moves + rows - 1) / rows;
    }
    int icon = 0;
    bool row_started = false;
    for (const auto& a : TlgFun::Actions) {
        if (!a.command) {
            if (std::strcmp(a.name, "May not work") == 0) {
                break;
            }
            if (per_row == 0) {
                row_started = false;
            }
            continue;
        }
        if (per_row > 0 && icon > 0 && icon % per_row == 0) {
            row_started = false;
        }
        if (row_started) {
            ImGui::SameLine();
        }
        row_started = true;
        ImGui::PushID(icon);
        ImGui::BeginGroup();
        bool pressed;
        if (move_icons && icon < TlgFunIcons::Count) {
            const float u = float(TlgFunIcons::Size) / float(move_icons->Width);
            const float v = float(TlgFunIcons::Size) / float(move_icons->Height);
            const float x = float(icon % TlgFunIcons::Columns) * u, y = float(icon / TlgFunIcons::Columns) * v;
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, 2));
            pressed = ImGui::ImageButton("##move", move_icons->GetTexRef(), ImVec2(slot, slot), ImVec2(x, y),
                                         ImVec2(x + u, y + v));
            ImGui::PopStyleVar();
        } else {
            pressed = ImGui::Button("##move", ImVec2(slot + 4, slot + 4));
        }
        const bool hovered = ImGui::IsItemHovered();
        if (hovered) {
            ImGui::SetTooltip("%s", a.name);
        }
        if (pressed) {
            TlgFun::Act(a.command);
        }
        // The name under the icon, up to the bracket ("Roll on the ground (needs room)").
        const char* end = std::strstr(a.name, " (");
        const std::string label = end ? std::string(a.name, end) : std::string(a.name);
        ImGui::BeginChild("##label", ImVec2(slot + 4, label_h), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground);
        ImGui::SetWindowFontScale(0.8f);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(hovered ? ImVec4(1.0f, 0.86f, 0.5f, 1.0f) : ImVec4(0.92f, 0.92f, 0.92f, 1.0f), "%s",
                           label.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        ImGui::EndGroup();
        ImGui::PopID();
        ++icon;
    }
}

void MoveBarWindow() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y - 16.0f * base_scale),
                            ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::Begin("##move_bar", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextDisabled("Trico's moves (F12 hides)");
    MoveBar(48.0f * base_scale, 2);
    ImGui::End();
}

// tlg: Trico Tricks, Trico's moves on their own tab (user 2026-11-05: moves out of the Fun tab).
void TricksTab() {
    ImGui::TextDisabled("Moves that work (also as a bar in the game: F12, click a move)");
    MoveBar(44.0f * base_scale);
    ImGui::TextDisabled("Every move, also the ones that may not work:");
    static int action = 1;
    const auto& picked = TlgFun::Actions[action];
    ImGui::SetNextItemWidth(260);
    if (ImGui::BeginCombo("##action", picked.name)) {
        for (int i = 0; i < int(std::size(TlgFun::Actions)); ++i) {
            const auto& a = TlgFun::Actions[i];
            if (!a.command) {
                ImGui::SeparatorText(a.name);
                continue;
            }
            if (ImGui::Selectable(a.name, i == action)) {
                action = i;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Do it")) {
        TlgFun::Act(picked.command);
    }
    Help("Asks the story's Trico to do it, as the game itself does. He first gets into the pose the move needs (stands up, sits or lies down) and gets up to 3 tries. His own mind can still win, and some moves need room (a roll can push the boy off a ledge). \"May not work\": never seen in our tests, or not tested.");
}

void FunTab() {
    static int stage = 0, point = 0;
    std::vector<const char*> stages;
    for (const auto& p : TlgFun::Points) {
        if (stages.empty() || std::strcmp(stages.back(), p.stage) != 0) {
            stages.push_back(p.stage);
        }
    }
    stage = std::clamp(stage, 0, int(stages.size()) - 1);
    std::vector<int> points;
    for (int i = 0; i < int(std::size(TlgFun::Points)); ++i) {
        if (std::strcmp(TlgFun::Points[i].stage, stages[stage]) == 0) {
            points.push_back(i);
        }
    }
    point = std::clamp(point, 0, int(points.size()) - 1);
    ImGui::SeparatorText(TR("Jump to a place"));
    if (BeginRows("##jump")) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(TR("Stage"));
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##stage", stages[stage])) {
            for (int i = 0; i < int(stages.size()); ++i) {
                if (ImGui::Selectable(stages[i], i == stage)) {
                    stage = i;
                    point = 0;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(TR("Start point"));
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##point", TlgFun::Points[points[point]].area)) {
            for (int i = 0; i < int(points.size()); ++i) {
                if (ImGui::Selectable(TlgFun::Points[points[i]].area, i == point)) {
                    point = i;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::EndTable();
    }
    if (ImGui::Button(TR("Jump there"))) {
        const auto& p = TlgFun::Points[points[point]];
        TlgFun::Jump(p.stage, p.area);
        SetOpen(false);
    }
    Help(TR("Starts the stage at this point, as the game's own level select does. The story continues from there, and the game can save there at its next checkpoint: keep a copy of your save (launcher: Saves folder) if you want to go back."));

    ImGui::SeparatorText(TR("Extra Tricos"));
    // The count comes from the game; ask again every 2 seconds while this tab shows.
    static auto asked = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (now - asked > std::chrono::seconds(2)) {
        asked = now;
        TlgFun::CountTricos();
    }
    const int tricos = TlgFun::Tricos();
    char text[64];
    std::snprintf(text, sizeof(text), "%s / %d", tricos < 0 ? "?" : std::to_string(tricos).c_str(), TlgFun::MaxTricos);
    if (BeginRows("##tricos")) {
        InfoRow(TR("Extra Tricos here"), text);
        ImGui::EndTable();
    }
    ImGui::BeginDisabled(tricos >= TlgFun::MaxTricos);
    if (ImGui::Button(TR("Add a Trico"))) {
        TlgFun::AddTrico();
    }
    ImGui::SameLine();
    if (ImGui::Button("Add an enemy Trico")) {
        TlgFun::AddEnemyTrico();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(tricos == 0);
    if (ImGui::Button(TR("Remove extra Tricos"))) {
        TlgFun::RemoveTricos();
    }
    ImGui::EndDisabled();
    Help(TR("A new Trico appears next to the boy. At most 8 extra: with more, the game's physics runs out of room and the game crashes. For fun only: they do not follow the story and can block the way. A jump or another stage ends them."));
    Help("An enemy Trico is the armoured one from later in the story. It appears a bit further away from the boy. It counts to the same limit of 8.");

    ImGui::SeparatorText("Trico's size");
    float size = TlgFun::SizeNow();
    ImGui::SetNextItemWidth(260);
    if (ImGui::SliderFloat("##size", &size, TlgFun::SizeMin, TlgFun::SizeMax, "%.2fx")) {
        TlgFun::Size(size);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(size == 1.0f);
    if (ImGui::Button("Normal size")) {
        TlgFun::Size(1.0f);
    }
    ImGui::EndDisabled();
    Help("Makes the story's Trico bigger or smaller, the whole of him. A big Trico may not fit through narrow spots, so make him normal again to pass. A jump or a new stage brings back his normal size.");

    ImGui::SeparatorText("Free camera");
    bool free_cam = TlgFun::FreeCameraOn();
    ImGui::BeginDisabled(TlgFun::PhotoModeOn());
    if (ImGui::Checkbox("Free camera##freecam", &free_cam)) {
        TlgFun::FreeCamera(free_cam);
    }
    ImGui::EndDisabled();
    Help("The developers' own camera: it lets go of the normal camera that follows the boy. Turn it off to get the normal camera back. A jump or a new stage turns it off.");

    ImGui::SeparatorText(TR("Photo mode"));
    bool photo = TlgFun::PhotoModeOn();
    if (ImGui::Checkbox(TR("Photo mode##photo"), &photo)) {
        TlgFun::PhotoMode(photo);
        if (photo) SetOpen(false);
    }
    Help(TR("The world stands still and you fly the camera to take a picture. Left stick: fly. Right stick: look around. L2 / R2: down / up. Hold R1: fast, hold L1: slow. Cross or Print Screen: save a screenshot to user/screenshots. L3 + R3, F9 (or this menu) turns it on and off. A jump turns it off."));
    ImGui::BeginDisabled(photo);
    int freeze = int(TlgFun::PhotoModeFreeze());
    const char* freeze_names[] = {TR(TlgFun::PhotoFreezeNames[0]), TR(TlgFun::PhotoFreezeNames[1])};
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::Combo(TR("Freeze##photofreeze"), &freeze, freeze_names, 2)) {
        TlgFun::PhotoModeFreeze(TlgFun::PhotoFreeze(freeze));
    }
    Help(TR("How the world is frozen. Pause the game: the game's own pause, everything stops. Stop time: the game's own slow motion, turned down until nothing moves; try it if the camera does not move with the first one."));
    ImGui::EndDisabled();

    ImGui::SeparatorText("Tricopilot");
    const bool pilot = TlgFun::TricopilotOn();
    ImGui::BeginDisabled(pilot);
    if (ImGui::Button(TR("Start Tricopilot"))) {
        TlgFun::Tricopilot(true);
        SetOpen(false);
    }
    Help(TR(TlgFun::TricopilotHelp));
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!pilot);
    if (ImGui::Button(TR("Stop Tricopilot"))) {
        TlgFun::Tricopilot(false);
    }
    Help(TR("Gives Trico and the camera back to you. He stops where he stands, and the normal camera follows the boy again."));
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", pilot ? TR("driving") : TR("off"));

    int cam = int(TlgFun::TricopilotCamera());
    const char* cam_names[] = {TR(TlgFun::PilotCamNames[0]),
                               TR(TlgFun::PilotCamNames[1]),
                               TR(TlgFun::PilotCamNames[2]),
                               TR(TlgFun::PilotCamNames[3])};
    ImGui::SetNextItemWidth(180.0f);
    if (ImGui::Combo(TR("Camera##pilotcam"), &cam, cam_names, 4)) {
        TlgFun::TricopilotCamera(TlgFun::PilotCam(cam));
    }
    Help(TR(TlgFun::TricopilotCamHelp));

    ImGui::SeparatorText(TR("Trico's eyes"));
    bool disco = TlgFun::DiscoOn();
    if (ImGui::Checkbox(TR("Disco eyes"), &disco)) {
        TlgFun::Disco(disco);
    }
    static float colour[3] = {1.0f, 0.08f, 0.08f};
    if (ImGui::ColorEdit3(TR("Eye colour"), colour, ImGuiColorEditFlags_NoInputs)) {
        TlgFun::EyeColour(int(colour[0] * 255 + 0.5f), int(colour[1] * 255 + 0.5f), int(colour[2] * 255 + 0.5f));
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!TlgFun::EyeCustom());
    if (ImGui::Button(TR("Normal colours"))) {
        TlgFun::EyeNormal();
    }
    ImGui::EndDisabled();
    Help(TR("Trico's eyes keep changing to random colours: the game's own hidden effect, normally only seen in a few story moments. Turning it off brings the normal colours back. A jump turns it off."));

    ImGui::SeparatorText("Trico's drool");
    if (BeginRows("##drool")) {
        static const char* const drool_labels[] = {"Off (as released)", "Always", "As the developers planned"};
        static const int drool_values[] = {0, 1, 2};
        ChoiceRow("Drool", TlgSettings::Get().trico_drool, drool_labels, drool_values, 3, TlgFun::DroolHelp, 0);
        ImGui::EndTable();
    }

    // Mods (also on the launcher's Mods page; tlg_lua.cpp Treasures / Horns).
    ImGui::SeparatorText(TR("Mods"));
    if (BeginRows("##mods")) {
        CheckRow(TR("All treasures"), TlgSettings::Get().mod_treasures,
                 TR("Unlocks every treasure (outfits, Trico's feathers) and the Treasures entry in the options menu, "
                    "with the developers' own unlock. Stored in your save: turning it off does not take them back. "
                    "Your saves are backed up before the first unlock (the launcher's save manager can restore them). "
                    "Select them in the game's pause menu, under Items."),
                 false);
        // The horn models only exist after the launcher built them (Mods page); the game mounts
        // them at start (runtime_file.c), so check once: without them the checkmark does nothing.
        static const bool horns_built = [] {
            const char* ini = std::getenv("TLG_CONFIG");
            std::string dir = ini && ini[0] ? ini : "tlg.ini";
            const size_t slash = dir.find_last_of("/\\");
            dir = slash == std::string::npos ? std::string(".") : dir.substr(0, slash);
            FILE* file = std::fopen((dir + "/mods-builtin/trico_other.psarc").c_str(), "rb");
            if (file) std::fclose(file);
            return file != nullptr;
        }();
        ImGui::BeginDisabled(!horns_built);
        CheckRow(TR("ICO horns"), TlgSettings::Get().mod_horns,
                 TR("The boy wears horns, a nod to Ico. "
                    "Can be combined with the game's own Ico outfit (one of the treasures). "
                    "Turn it on in the launcher once first: the launcher builds the horn models on your PC."),
                 false);
        ImGui::EndDisabled();
        if (!horns_built) {
            Row("");
            ImGui::TextDisabled("%s", TR("Turn on ICO horns in the launcher (Mods page) once first."));
            ImGui::PopID();
        }
        CheckRow(TR("Trico plays"), TlgSettings::Get().mod_plays,
                 TR("A Trico behaviour the developers built and then sealed \"for now\": now and then he sniffs the boy and paws at him. It only happens where his cheek rubs can happen too, with the boy clear of ledges."), true);
        static const char* const throw_labels[] = {TR("Off (as released)"), TR("As the developers designed")};
        static const int throw_values[] = {0, 1};
        ChoiceRow(TR("Trico throws up"), TlgSettings::Get().mod_throw_up, throw_labels, throw_values, 2,
                  TR("The developers sealed Trico throwing up \"by order\" in 2016. As the developers designed: when his stomach is full of barrels, he throws up the way they planned. The Throw up button in Trico Tricks works either way."), 0);
        CheckRow(TR("Cut narrator hints"), TlgSettings::Get().mod_hints,
                 TR("The 23 narrator hints the developers cut in September 2016. On: when you seem stuck, the narrator helps again, where they first placed his lines. They give puzzle solutions away."), true);
        ImGui::EndTable();
    }
}

int RestartChanges() {
    const auto& s = TlgSettings::Get();
    return (s.render_resolution != s.startup_render_resolution) + (s.engine_fps != s.startup_engine_fps) +
           (s.deferred_readback != s.startup_deferred_readback) + (s.stale_readback != s.startup_stale_readback) +
           (s.hdr != s.startup_hdr) + (s.preset != s.startup_preset);
}

void Menu() {
    auto& s = TlgSettings::Get();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(760.0f * base_scale, viewport->WorkSize.x * 0.95f),
                                    std::min(680.0f * base_scale, viewport->WorkSize.y * 0.9f)),
                             ImGuiCond_Appearing);
    bool keep_open = true;
    // tlg: the game's own title (param.sfo), not a fixed one, then this port's name (Japanese
    // title removed, user 2026-11-10).
    static const std::string heading = [] {
        const std::string_view title = Common::ElfInfo::Instance().Title();
        const char* app_name = std::getenv("TLG_APP_NAME");
        return std::string(!title.empty() ? title : app_name && *app_name ? app_name : "The Last Guardian") +
               " - The Definitive Edition  (Insert / R3+L2)";
    }();
    if (!ImGui::Begin(heading.c_str(), &keep_open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    footer_help = nullptr;
    constexpr int TabCount = 6; // tlg: Lab tab out of the menu (user 2026-11-05)
    const ImGuiIO& io = ImGui::GetIO();
    if (!io.WantTextInput) {
        const int step = ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyPressed(ImGuiKey_E, false) ? 1
                         : ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_Q, false)
                             ? -1
                             : 0;
        if (step) {
            requested_tab = (s.menu_tab + step + TabCount) % TabCount;
        }
    }
    // Restart row, help box (fixed height) and Close row; a long help text never pushes the
    // "Apply and restart" button out of the window (user 2026-11-07).
    const float help_height = ImGui::GetTextLineHeightWithSpacing() * 5.0f;
    const float footer = ImGui::GetFrameHeightWithSpacing() * 2.0f + help_height + ImGui::GetStyle().ItemSpacing.y * 3.0f;
    if (ImGui::BeginTabBar("##tabs")) {
        static const char* const tabs[TabCount] = {"Overlay", TR("Image"), TR("Performance"),
                                                   TR("Controls"), TR("Fun"),
                                                   "Trico Tricks"};
        for (int tab = 0; tab < TabCount; ++tab) {
            const ImGuiTabItemFlags flags = requested_tab == tab ? ImGuiTabItemFlags_SetSelected : 0;
            if (!ImGui::BeginTabItem(tabs[tab], nullptr, flags)) {
                continue;
            }
            if (s.menu_tab != tab && requested_tab < 0) {
                s.menu_tab = tab;
                dirty = true;
            }
            if (ImGui::BeginChild("##body", ImVec2(0, -footer))) {
                switch (tab) {
                case 1: ImageTab(); break;
                case 2: PerformanceTab(); break;
                case 3: ControlsTab(); break;
                case 4: FunTab(); break;
                case 5: TricksTab(); break;
                default: OverlayTab(); break;
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (requested_tab >= 0) {
            s.menu_tab = requested_tab;
            dirty = true;
            requested_tab = -1;
        }
        ImGui::EndTabBar();
    }

    ImGui::Separator();
    if (const int changes = RestartChanges()) {
        ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.45f, 1.0f), "%d change%s need%s a restart (unsaved progress is lost).",
                           changes, changes == 1 ? "" : "s", changes == 1 ? "s" : "");
        ImGui::SameLine();
        ImGui::BeginDisabled(!std::getenv("TLG_RESTART_COMMAND"));
        if (ImGui::Button(TR("Apply and restart"))) {
            // The new launch confirms the options once the game shows; tlg_run.py restores the
            // previous ones if it never does (restart_unconfirmed).
            s.restart_unconfirmed = true;
            TlgSettings::Save();
            runtime_restart();
        }
        ImGui::EndDisabled();
    } else {
        ImGui::NewLine();
    }
    if (ImGui::BeginChild("##help", ImVec2(0, help_height))) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", footer_help ? footer_help : TR("L1 / R1 or Q / E: switch tabs. Settings are saved in tlg.ini."));
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();
    const float close_width = ImGui::CalcTextSize(TR("Close")).x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - close_width);
    if (ImGui::Button(TR("Close"))) {
        keep_open = false;
    }
    ImGui::End();
    if (!keep_open) {
        SetOpen(false);
    }
}

// The game's text dialog: what is typed, and how to finish (keyboard or controller).
void TextEntryBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.42f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(viewport->WorkSize.x * 0.32f, 0.0f),
                                        ImVec2(viewport->WorkSize.x * 0.8f, FLT_MAX));
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::Begin("##text_entry", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.45f, 1.0f), "%s", text_entry_prompt.c_str());
    ImGui::Separator();
    ImGui::SetWindowFontScale(1.4f);
    ImGui::Text("%s_", text_entry_text.c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Separator();
    ImGui::TextDisabled("Keyboard: type, Backspace to erase, Enter = OK, Esc = cancel");
    ImGui::TextDisabled("%s", menu_circle_confirm()
        ? "Controller: Circle (B) = OK, Cross (A) = cancel"
        : "Controller: Cross (A) = OK, Circle (B) = cancel");
    ImGui::End();
}

// ---- Save dialogs: drawing. The layout is in design pixels of a 1280x720 screen, times k. ----
constexpr u32 kSaveAccent = 0x5aa9ff, kSaveText = 0xeef2f8, kSaveMuted = 0xaab5c7, kSaveDim = 0x8b97ab,
              kSavePanel = 0x141923, kSaveLine = 0x263042, kSaveCard = 0x161c28, kSaveCardOn = 0x1f2a3d,
              kSaveThumb = 0x1d2433, kSaveBorder = 0x2b3445, kSaveKnob = 0x3a4659, kSaveWarn = 0xf0a43a,
              kSaveOk = 0x4fd1a5;

ImU32 SaveColor(u32 rgb, float alpha = 1.0f) {
    return IM_COL32(rgb >> 16, (rgb >> 8) & 255, rgb & 255, int(std::clamp(alpha, 0.0f, 1.0f) * 255.0f));
}

struct SavePainter {
    ImDrawList* dl;
    ImFont* font;
    float k;
    ImVec2 at; // the panel's top-left corner, in display pixels

    ImVec2 P(float x, float y) const { return ImVec2(at.x + x * k, at.y + y * k); }
    float Width(float size, const std::string& text) const {
        return font->CalcTextSizeA(size * k, FLT_MAX, 0.0f, text.c_str()).x / k;
    }
    // The text cut with "..." to fit `width` design pixels.
    std::string Fit(float size, const std::string& text, float width) const {
        if (Width(size, text) <= width) return text;
        std::string cut = text;
        while (!cut.empty()) {
            while (!cut.empty() && (u8(cut.back()) & 0xc0) == 0x80) cut.pop_back(); // UTF-8 tail
            if (!cut.empty()) cut.pop_back();
            if (Width(size, cut + "...") <= width) return cut + "...";
        }
        return "...";
    }
    // Text at (x, y) design pixels; bold is drawn twice, a hair apart (one embedded face).
    void Text(float x, float y, float size, u32 rgb, const std::string& text, bool bold = false,
              float alpha = 1.0f, float wrap = 0.0f) const {
        const ImU32 col = SaveColor(rgb, alpha);
        dl->AddText(font, size * k, P(x, y), col, text.c_str(), nullptr, wrap * k);
        if (bold) dl->AddText(font, size * k, P(x + 0.55f, y), col, text.c_str(), nullptr, wrap * k);
    }
    void TextRight(float right, float y, float size, u32 rgb, const std::string& text, bool bold = false) const {
        Text(right - Width(size, text), y, size, rgb, text, bold);
    }
    void Box(float x0, float y0, float x1, float y1, u32 fill, float rounding, float alpha = 1.0f) const {
        dl->AddRectFilled(P(x0, y0), P(x1, y1), SaveColor(fill, alpha), rounding * k);
    }
    void Frame(float x0, float y0, float x1, float y1, u32 rgb, float rounding, float thickness,
               float alpha = 1.0f) const {
        dl->AddRect(P(x0, y0), P(x1, y1), SaveColor(rgb, alpha), rounding * k, 0, thickness * k);
    }
    void Line(float x0, float y0, float x1, float y1, u32 rgb, float thickness, float alpha = 1.0f) const {
        dl->AddLine(P(x0, y0), P(x1, y1), SaveColor(rgb, alpha), thickness * k);
    }
    // Semantic action: 0 confirm, 1 back; choose its physical glyph by menu policy.
    void Button(float x, float y, int which) const {
        which ^= menu_circle_confirm();
        const ImVec2 c = P(x, y);
        dl->AddCircleFilled(c, 11.0f * k, SaveColor(kSaveThumb));
        dl->AddCircle(c, 11.0f * k, SaveColor(kSaveKnob), 0, 1.0f * k);
        if (which == 0) {
            Line(x - 4, y - 4, x + 4, y + 4, 0x8fb6ff, 2.0f);
            Line(x + 4, y - 4, x - 4, y + 4, 0x8fb6ff, 2.0f);
        } else {
            dl->AddCircle(c, 5.0f * k, SaveColor(0xff8a8a), 0, 2.0f * k);
        }
    }
    // The panel itself: shadow, fill, border.
    void Panel(float w, float h) const {
        dl->AddRectFilled(P(0, 18), P(w, h + 18), SaveColor(0x000000, 0.35f), 18.0f * k);
        Box(0, 0, w, h, kSavePanel, 18.0f, 0.97f);
        Frame(0, 0, w, h, kSaveBorder, 18.0f, 1.0f);
    }
    // A save's picture (16:9) or a placeholder; `is_new` draws the dashed "+" slot.
    void Picture(float x, float y, float w, float h, ImTextureData* tex, bool is_new) const {
        if (tex) {
            dl->AddImageRounded(tex->GetTexRef(), P(x, y), P(x + w, y + h), ImVec2(0, 0), ImVec2(1, 1),
                                IM_COL32_WHITE, 10.0f * k);
            Frame(x, y, x + w, y + h, 0x323d52, 10.0f, 1.0f);
            return;
        }
        if (is_new) {
            for (float t = 0; t < 1.0f; t += 0.1f) { // dashes around the slot
                Line(x + w * t, y, x + w * (t + 0.05f), y, kSaveKnob, 2.0f);
                Line(x + w * t, y + h, x + w * (t + 0.05f), y + h, kSaveKnob, 2.0f);
            }
            for (float t = 0; t < 1.0f; t += 0.18f) {
                Line(x, y + h * t, x, y + h * (t + 0.09f), kSaveKnob, 2.0f);
                Line(x + w, y + h * t, x + w, y + h * (t + 0.09f), kSaveKnob, 2.0f);
            }
            Line(x + w / 2, y + h / 2 - 11, x + w / 2, y + h / 2 + 11, kSaveDim, 2.0f);
            Line(x + w / 2 - 11, y + h / 2, x + w / 2 + 11, y + h / 2, kSaveDim, 2.0f);
            return;
        }
        Box(x, y, x + w, y + h, kSaveThumb, 10.0f);
        Frame(x, y, x + w, y + h, 0x323d52, 10.0f, 1.0f);
    }
};

SavePainter SavePainterFor(float w, float h) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    // 720p design scaled to the display, kept inside the screen on narrow windows.
    const float k = std::min(viewport->WorkSize.y / 720.0f, viewport->WorkSize.x * 0.94f / w);
    SavePainter p{ImGui::GetForegroundDrawList(), ImGui::GetFont(), k, {}};
    p.at = ImVec2(viewport->WorkPos.x + (viewport->WorkSize.x - w * k) * 0.5f,
                  viewport->WorkPos.y + (viewport->WorkSize.y - h * k) * 0.5f);
    return p;
}

void SaveBackdrop(float alpha) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::GetForegroundDrawList()->AddRectFilled(
        viewport->WorkPos, ImVec2(viewport->WorkPos.x + viewport->WorkSize.x, viewport->WorkPos.y + viewport->WorkSize.y),
        SaveColor(0x05070c, alpha));
}

std::string SaveWhen(s64 mtime) {
    if (mtime <= 0) return {};
    const time_t t = time_t(mtime);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char out[64];
    static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const int hour = tm.tm_hour % 12 ? tm.tm_hour % 12 : 12;
    std::snprintf(out, sizeof(out), "%s %d, %d · %d:%02d %s", months[tm.tm_mon % 12], tm.tm_mday,
                  tm.tm_year + 1900, hour, tm.tm_min, tm.tm_hour < 12 ? "AM" : "PM");
    return out;
}

std::string SaveAgo(s64 mtime) {
    const s64 seconds = s64(std::time(nullptr)) - mtime;
    if (mtime <= 0 || seconds < 0) return {};
    char out[64];
    if (seconds < 60) return TR("just now");
    if (seconds < 3600) {
        const int m = int(seconds / 60);
        std::snprintf(out, sizeof(out), m == 1 ? TR("%d minute ago") : TR("%d minutes ago"), m);
    } else if (seconds < 86400) {
        const int h = int(seconds / 3600);
        std::snprintf(out, sizeof(out), h == 1 ? TR("%d hour ago") : TR("%d hours ago"), h);
    } else {
        const int d = int(seconds / 86400);
        if (d == 1) return TR("yesterday");
        std::snprintf(out, sizeof(out), TR("%d days ago"), d);
    }
    return out;
}

std::string SaveSize(u64 bytes) {
    if (!bytes) return {};
    char out[32];
    if (bytes < 1024 * 1024) {
        std::snprintf(out, sizeof(out), "%llu KB", (unsigned long long)((bytes + 1023) / 1024));
    } else {
        std::snprintf(out, sizeof(out), "%.1f MB", double(bytes) / (1024.0 * 1024.0));
    }
    return out;
}

const char* SaveTitle(int kind) {
    return kind == 2 ? TR("Load game") : kind == 3 ? TR("Delete save")
                                                         : TR("Save game");
}

ImTextureData* SaveTexture(size_t i) {
    return i < choice_textures.size() ? choice_textures[i] : nullptr;
}

// One save as a card: picture, the game's title and text, when it was saved.
void SaveCard(const SavePainter& p, float x0, float y0, float x1, const SaveRow& row, ImTextureData* tex,
              bool selected, float picture_w) {
    const float picture_h = picture_w * 9.0f / 16.0f, h = picture_h + 24.0f;
    if (selected) {
        p.Frame(x0 - 3, y0 - 3, x1 + 3, y0 + h + 3, kSaveAccent, 16.0f, 4.0f, 0.12f);
        p.Box(x0, y0, x1, y0 + h, kSaveCardOn, 14.0f);
        p.Frame(x0, y0, x1, y0 + h, kSaveAccent, 14.0f, 2.0f);
    } else {
        p.Box(x0, y0, x1, y0 + h, kSaveCard, 14.0f);
    }
    p.Picture(x0 + 16, y0 + 12, picture_w, picture_h, tex, row.is_new);
    const float text_x = x0 + 16 + picture_w + 18;
    std::string when, ago;
    if (row.is_new) {
        p.Text(text_x, y0 + h / 2 - 20, 17.0f, kSaveText, TR("New save"), true);
        p.Text(text_x, y0 + h / 2 + 4, 13.5f, kSaveMuted, TR("Create a new save"));
        return;
    }
    when = SaveWhen(row.mtime);
    ago = SaveAgo(row.mtime);
    const std::string size = SaveSize(row.bytes);
    if (!size.empty()) ago = ago.empty() ? size : ago + " · " + size;
    const float right = x1 - 16;
    const float right_w = std::max(p.Width(14.0f, when), p.Width(12.5f, ago));
    const float text_w = right - text_x - (right_w > 0 ? right_w + 20 : 0);
    std::string line = row.subtitle;
    if (!row.detail.empty()) line = line.empty() ? row.detail : line + " · " + row.detail;
    for (char& c : line) if (c == '\n' || c == '\r') c = ' ';
    p.Text(text_x, y0 + h / 2 - 20, 17.0f, kSaveText, p.Fit(17.0f, row.title, text_w), true);
    p.Text(text_x, y0 + h / 2 + 4, 13.5f, kSaveMuted, p.Fit(13.5f, line, text_w));
    p.TextRight(right, y0 + h / 2 - 18, 14.0f, 0xdfe6f1, when, true);
    p.TextRight(right, y0 + h / 2 + 5, 12.5f, kSaveDim, ago);
}

void SaveListBox() {
    const int count = int(save_rows.size());
    const int shown = std::min(count, 4);
    const float w = 880, row_h = 96, gap = 10, header_h = 78, footer_h = 54, pad = 18;
    const float list_h = shown * row_h + std::max(shown - 1, 0) * gap + pad * 2;
    const float h = header_h + list_h + footer_h;
    SaveBackdrop(0.62f);
    SavePainter p = SavePainterFor(w, h);
    p.Panel(w, h);

    // Header: a floppy, the title, the game and the number of saves.
    const float ix = 28, iy = 25;
    p.Frame(ix, iy, ix + 26, iy + 26, kSaveAccent, 4.0f, 1.8f);
    p.Frame(ix + 6, iy, ix + 20, iy + 9, kSaveAccent, 1.0f, 1.8f);
    p.Frame(ix + 6, iy + 15, ix + 20, iy + 26, kSaveAccent, 1.0f, 1.8f);
    p.Text(70, 18, 24.0f, kSaveText, SaveTitle(save_kind), true);
    const int saves = count - int(std::count_if(save_rows.begin(), save_rows.end(), [](const SaveRow& r) { return r.is_new; }));
    char sub[160];
    const char* game = std::getenv("TLG_APP_NAME");
    std::snprintf(sub, sizeof(sub), "%s%s%d %s", game && *game ? game : "", game && *game ? " · " : "", saves,
                  saves == 1 ? "save" : "saves");
    p.Text(70, 48, 13.0f, kSaveDim, sub);
    p.Line(0, header_h, w, header_h, kSaveLine, 1.0f);

    // Rows, scrolled so the selected one is shown.
    if (choice_index < choice_first) choice_first = choice_index;
    if (choice_index >= choice_first + shown) choice_first = choice_index - shown + 1;
    choice_first = std::clamp(choice_first, 0, std::max(count - shown, 0));
    save_row_rects.assign(count, ImRect());
    for (int i = choice_first; i < choice_first + shown; ++i) {
        const float y = header_h + pad + (i - choice_first) * (row_h + gap);
        SaveCard(p, 20, y, w - 20, save_rows[i], SaveTexture(size_t(i)), i == choice_index, 128.0f);
        save_row_rects[i] = ImRect(p.P(20, y), p.P(w - 20, y + row_h));
    }
    if (count > shown) { // scroll bar
        const float track = list_h - pad * 2, y0 = header_h + pad;
        const float knob = track * shown / count, top = y0 + track * choice_first / count;
        p.Box(w - 11, y0, w - 8, y0 + track, kSaveLine, 2.0f);
        p.Box(w - 11, top, w - 8, top + knob, kSaveDim, 2.0f);
    }

    // Footer: what the buttons do.
    const float fy = header_h + list_h;
    p.Line(0, fy, w, fy, kSaveLine, 1.0f);
    const char* confirm = save_kind == 2 ? TR("Load") : save_kind == 3 ? TR("Delete")
                                                                                  : TR("Save here");
    float x = 40;
    p.Button(x, fy + footer_h / 2, 0);
    p.Text(x + 18, fy + footer_h / 2 - 9, 13.5f, 0xb8c2d3, confirm);
    x += 18 + p.Width(13.5f, confirm) + 34;
    p.Button(x, fy + footer_h / 2, 1);
    p.Text(x + 18, fy + footer_h / 2 - 9, 13.5f, 0xb8c2d3, TR("Back"));
    p.TextRight(w - 28, fy + footer_h / 2 - 8, 12.5f, 0x7f8ba0,
                TR("Enter / click: choose · Esc / right click: back"));
}

// The message's words for a system message type (sceSaveDataDialog), the game's own text or an
// error code. warn: drawn with the warning sign.
struct SaveMessageWords {
    std::string title, body;
    int buttons; // 0 OK, 1 yes/no, 3 OK/cancel
    bool warn;
};

SaveMessageWords SaveMessageFor(int kind, int code, u64 value, const std::string& text) {
    switch (code) {
    case 1:
        return {TR("No saved data"),
                TR("There is no saved data to load."), 0, false};
    case 2:
        if (kind == 2) return {TR("Load this save?"),
                               TR("Progress that was not saved will be lost."), 1, false};
        if (kind == 3) return {TR("Delete this save?"),
                               TR("This cannot be undone."), 1, true};
        return {TR("Save the game?"), TR("A new save will be created."), 1, false};
    case 3:
        return {TR("Overwrite this save?"),
                TR("The current progress will replace this save. This cannot be undone."), 1, true};
    case 4:
    case 8:
        return {TR("Not enough space"),
                TR("Free some disk space and try again."), 0, true};
    case 6:
        return {TR("This save is damaged"),
                TR("It cannot be used."), 0, true};
    case 10:
        return {TR("A damaged save was found"),
                TR("It was deleted."), 0, true};
    case 11:
        return {TR("A damaged save was found"),
                TR("A new save was created in its place."), 0, true};
    case 13:
        return {TR("A damaged save was found"),
                TR("It was restored."), 0, true};
    case 14:
        return {TR("The saves are too large"),
                TR("Delete some saves and try again."), 0, true};
    case 100:
        return {SaveTitle(kind), text, 0, false};
    case 101: {
        char body[96];
        std::snprintf(body, sizeof(body), TR("Error code: 0x%08X"), unsigned(value));
        return {TR("Save data error"), body, 0, true};
    }
    default:
        return {SaveTitle(kind), text, 0, false};
    }
}

void SaveMessageBox() {
    const SaveMessageWords words = SaveMessageFor(save_kind, message_code, message_value, message_text);
    const int buttons = message_buttons >= 0 ? message_buttons : words.buttons;
    const float w = 560, pad = 28, inner = w - pad * 2;
    // Height: title, the save's card, the wrapped text, the buttons.
    ImFont* font = ImGui::GetFont();
    const float k0 = std::min(ImGui::GetMainViewport()->WorkSize.y / 720.0f,
                              ImGui::GetMainViewport()->WorkSize.x * 0.94f / w);
    const float body_h = words.body.empty() ? 0.0f
        : font->CalcTextSizeA(14.5f * k0, FLT_MAX, inner * k0, words.body.c_str()).y / k0;
    const float card_h = message_has_entry ? 87.0f : 0.0f;
    float h = pad + 30 + 20 + (card_h ? card_h + 20 : 0) + (body_h ? body_h + 20 : 0) + 46 + pad;
    SaveBackdrop(0.72f);
    SavePainter p = SavePainterFor(w, h);
    p.Panel(w, h);

    float y = pad;
    if (words.warn) { // triangle
        const ImVec2 a = p.P(pad + 13, y + 2), b = p.P(pad + 26, y + 26), c = p.P(pad, y + 26);
        p.dl->AddTriangle(a, b, c, SaveColor(kSaveWarn), 2.0f * p.k);
        p.Line(pad + 13, y + 11, pad + 13, y + 18, kSaveWarn, 2.0f);
        p.dl->AddCircleFilled(p.P(pad + 13, y + 22), 1.4f * p.k, SaveColor(kSaveWarn));
    } else {
        p.dl->AddCircle(p.P(pad + 13, y + 14), 12.0f * p.k, SaveColor(kSaveAccent), 0, 2.0f * p.k);
        p.Text(pad + 8.5f, y + 4, 16.0f, kSaveAccent, "?", true);
    }
    p.Text(pad + 40, y + 1, 22.0f, kSaveText, p.Fit(22.0f, words.title, inner - 40), true);
    y += 30 + 20;
    if (message_has_entry) {
        p.Box(pad, y, w - pad, y + card_h, kSaveCard, 14.0f);
        p.Frame(pad, y, w - pad, y + card_h, kSaveBorder, 14.0f, 1.0f);
        p.Picture(pad + 14, y + 12, 112, 63, SaveTexture(0), message_entry.is_new);
        const float tx = pad + 14 + 112 + 16, tw = w - pad - 14 - tx;
        std::string line = message_entry.subtitle;
        if (!message_entry.detail.empty()) line = line.empty() ? message_entry.detail : line + " · " + message_entry.detail;
        for (char& c : line) if (c == '\n' || c == '\r') c = ' ';
        std::string when = SaveWhen(message_entry.mtime);
        const std::string ago = SaveAgo(message_entry.mtime);
        if (!when.empty() && !ago.empty()) when += " (" + ago + ")";
        p.Text(tx, y + 16, 16.0f, kSaveText,
               p.Fit(16.0f, message_entry.is_new ? std::string(TR("New save")) : message_entry.title, tw), true);
        p.Text(tx, y + 39, 13.0f, kSaveMuted, p.Fit(13.0f, line, tw));
        p.Text(tx, y + 59, 12.5f, kSaveDim, p.Fit(12.5f, when, tw));
        y += card_h + 20;
    }
    if (body_h) {
        p.Text(pad, y, 14.5f, 0xc3ccdb, words.body, false, 1.0f, inner);
        y += body_h + 20;
    }

    // Buttons, right-aligned: [no/cancel] [yes/OK]; the focused one is filled.
    const char* yes = buttons == 1 ? TR("Yes") : "OK";
    const char* no = buttons == 1 ? TR("No") : TR("Cancel");
    const bool two = buttons == 1 || buttons == 3;
    if (!two) message_focus = 1;
    float right = w - pad;
    save_button_rects[0] = save_button_rects[1] = ImRect();
    for (int b = 1; b >= (two ? 0 : 1); --b) {
        const char* label = b == 1 ? yes : no;
        const float bw = 22 + 20 + 10 + p.Width(15.0f, label) + 22, x0 = right - bw;
        const bool focused = b == message_focus;
        p.Box(x0, y, right, y + 46, focused ? kSaveAccent : kSaveCard, 12.0f);
        if (!focused) p.Frame(x0, y, right, y + 46, kSaveKnob, 12.0f, 1.0f);
        p.Button(x0 + 32, y + 23, b == 1 ? 0 : 1);
        p.Text(x0 + 52, y + 13, 15.0f, focused ? 0x0b0e14 : 0xdfe6f1, label, true);
        save_button_rects[b] = ImRect(p.P(x0, y), p.P(right, y + 46));
        right = x0 - 12;
    }
}

void SaveProgressBox(SaveClock::time_point now) {
    const float w = 520, h = 140;
    SaveBackdrop(0.45f);
    SavePainter p = SavePainterFor(w, h);
    p.Panel(w, h);
    // A spinning arc.
    const float t = std::chrono::duration<float>(now - progress_since).count();
    const ImVec2 c = p.P(28 + 13, 26 + 13);
    p.dl->PathArcTo(c, 11.0f * p.k, t * 6.0f, t * 6.0f + 4.4f, 24);
    p.dl->PathStroke(SaveColor(kSaveAccent), 0, 2.4f * p.k);
    const char* title = progress_kind == 2 ? TR("Loading...")
                        : progress_kind == 3 ? TR("Deleting...")
                                             : TR("Saving...");
    p.Text(66, 20, 21.0f, kSaveText, title, true);
    p.Text(66, 47, 13.0f, kSaveDim, TR("Do not close the game."));
    const float bx0 = 28, bx1 = w - 28, by = 84;
    p.Box(bx0, by, bx1, by + 10, kSaveThumb, 5.0f);
    if (progress_percent >= 0) {
        const float f = std::clamp(progress_percent, 0, 100) / 100.0f;
        if (f > 0) p.Box(bx0, by, bx0 + (bx1 - bx0) * f, by + 10, kSaveAccent, 5.0f);
        char pct[16];
        std::snprintf(pct, sizeof(pct), "%d%%", std::clamp(progress_percent, 0, 100));
        p.TextRight(bx1, by + 20, 13.0f, 0xdfe6f1, pct, true);
    } else { // no figure: a segment sweeps the bar
        const float span = (bx1 - bx0) * 0.3f, phase = std::fmod(t * 0.8f, 1.0f);
        const float s0 = bx0 + (bx1 - bx0 + span) * phase - span;
        p.Box(std::max(s0, bx0), by, std::min(s0 + span, bx1), by + 10, kSaveAccent, 5.0f);
    }
    if (!progress_text.empty()) {
        std::string text = progress_text;
        for (char& ch : text) if (ch == '\n' || ch == '\r') ch = ' ';
        p.Text(bx0, by + 20, 13.0f, kSaveMuted, p.Fit(13.0f, text, bx1 - bx0 - 60));
    }
}

void SaveToastBox(SaveClock::time_point now) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const char* text = toast_kind == 2 ? TR("Save loaded")
                       : toast_kind == 3 ? TR("Save deleted")
                                         : TR("Game saved");
    const float in = std::chrono::duration<float>(now - toast_since).count();
    const float out = std::chrono::duration<float>(toast_until - now).count();
    const float alpha = std::clamp(std::min(in / 0.15f, out / 0.4f), 0.0f, 1.0f);
    const float k = viewport->WorkSize.y / 720.0f;
    SavePainter p{ImGui::GetForegroundDrawList(), ImGui::GetFont(), k, {}};
    const float w = 18 + 22 + 12 + p.Width(14.0f, text) + 20, h = 46;
    p.at = ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - (32 + w) * k,
                  viewport->WorkPos.y + viewport->WorkSize.y - (28 + h) * k);
    p.Box(0, 0, w, h, kSavePanel, 14.0f, 0.92f * alpha);
    p.Frame(0, 0, w, h, kSaveBorder, 14.0f, 1.0f, alpha);
    p.Line(18, 24, 24, 30, kSaveOk, 2.4f, alpha); // a check mark
    p.Line(24, 30, 36, 17, kSaveOk, 2.4f, alpha);
    p.Text(52, 14, 14.0f, kSaveText, text, true, alpha);
}

// Present thread, imgui_mutex held, after NewFrame: whichever save dialog is up.
void SaveDialogs() {
    const auto now = SaveClock::now();
    if (choice_active) {
        SaveListBox();
    } else if (message_active) {
        SaveMessageBox();
    } else if (progress_on || now < progress_until) {
        SaveProgressBox(now);
    }
    if (now < toast_until && !choice_active && !message_active) {
        SaveToastBox(now);
    }
}

void LoadingScreen() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x * 0.4f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::Begin("##loading", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
    const u32 done = loading_done, total = loading_total;
    ImGui::SetWindowFontScale(1.6f);
    ImGui::TextUnformatted(total ? "Loading shader cache..." : "Preparing graphics...");
    ImGui::SetWindowFontScale(1.0f);
    if (total) {
        char count[64];
        std::snprintf(count, sizeof(count), "%u / %u (%u%%)", done, total,
                      u32(u64(done) * 100 / total));
        ImGui::ProgressBar(float(done) / float(total), ImVec2(-1.0f, 0.0f), count);
    }
    ImGui::TextDisabled("The first start after an update takes longer.");
    ImGui::End();
}

void FpsCounter() {
    const auto& s = TlgSettings::Get();
    const std::string text = OverlayText();
    if (text.empty()) {
        return;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    const int corner = s.overlay_corner;
    const bool right = corner == 1 || corner == 3, bottom = corner >= 2;
    ImGui::SetNextWindowPos(ImVec2(right ? viewport->WorkPos.x + viewport->WorkSize.x - pad : viewport->WorkPos.x + pad,
                                   bottom ? viewport->WorkPos.y + viewport->WorkSize.y - pad : viewport->WorkPos.y + pad),
                            ImGuiCond_Always, ImVec2(right ? 1.0f : 0.0f, bottom ? 1.0f : 0.0f));
    ImGui::SetNextWindowBgAlpha(s.overlay_opacity / 100.0f);
    ImGui::Begin("##fps", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::SetWindowFontScale(s.overlay_scale / 100.0f);
    ImGui::TextUnformatted(text.c_str());
    ImGui::End();
}

std::string notice_text;
std::chrono::steady_clock::time_point notice_until;

// tlg: photo mode button help, bottom left. Our overlay is drawn after the screenshot copy
// (vk_presenter.cpp), so it never shows in the saved picture.
void PhotoHelpBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + 24.0f * base_scale,
                                   viewport->WorkPos.y + viewport->WorkSize.y - 24.0f * base_scale),
                            ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::Begin("##photohelp", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextUnformatted(TR("Photo mode"));
    ImGui::Separator();
    // Key column as wide as its longest text (packs translate it) plus a clear gap.
    const std::string exit_keys = TlgSettings::ComboLabel(TlgSettings::Get().combo_photo.load()) + " / F9";
    const std::pair<const char*, const char*> rows[] = {
        {TR("Cross / Print Screen"), TR("Screenshot")},
        {TR("Left stick"), TR("Fly")},
        {TR("Right stick"), TR("Look around")},
        {"L2 / R2", TR("Down / up")},
        {TR("Hold R1 / L1"), TR("Fast / slow")},
        {exit_keys.c_str(), TR("Exit photo mode")},
    };
    float key_w = 0.0f;
    for (const auto& r : rows) key_w = std::max(key_w, ImGui::CalcTextSize(r.first).x);
    const float col = ImGui::GetCursorPosX() + key_w + 32.0f * base_scale;
    for (const auto& r : rows) {
        ImGui::TextUnformatted(r.first);
        ImGui::SameLine(col);
        ImGui::TextUnformatted(r.second);
    }
    ImGui::End();
}

void NoticeBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + 40.0f * base_scale),
                            ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.75f);
    ImGui::Begin("##notice", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::SetWindowFontScale(1.3f);
    ImGui::TextUnformatted(notice_text.c_str());
    ImGui::End();
}

/// tlg: while the game's pause menu is open: how to quit and open the menus (the player's own combos).
void QuitHintBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y - 40.0f * base_scale),
                            ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::Begin("##quit_hint", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::SetWindowFontScale(1.1f);
    // One line per combo, with the player's own buttons.
    const auto line = [](const char* with, const char* without, u32 combo) {
        std::string text = combo ? with : without;
        if (const size_t at = text.find("%s"); combo && at != std::string::npos) {
            text.replace(at, 2, TlgSettings::ComboLabel(combo));
        }
        ImGui::TextUnformatted(text.c_str());
    };
    const auto& s = TlgSettings::Get();
    line(TR("Quit game: hold %s for 2 seconds, or press Alt+Q"), TR("Quit game: press Alt+Q"), s.combo_quit);
    line(TR("Port menu: %s, or press Insert"), TR("Port menu: press Insert"), s.combo_menu);
    // The keyboard keys (1 + Z) reach the QA menu only through the default combo (game pad map).
    // The launcher note shows always, so players learn the QA menu exists.
    std::string qa_with = s.combo_qa == TlgSettings::PadComboQa ? TR("QA menu: %s, or press 1 + Z") : TR("QA menu: %s");
    std::string qa_without = TR("QA menu: off");
    const std::string note = std::string(" ") + TR("(turn on Level select and QA menu in the launcher first)");
    qa_with += note;
    qa_without += note;
    line(qa_with.c_str(), qa_without.c_str(), s.combo_qa);
    ImGui::End();
}

void BackgroundLoading() {
    const u32 done = background_done, total = std::max(background_total.load(), 1u);
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + pad,
                                   viewport->WorkPos.y + viewport->WorkSize.y - pad),
                            ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.35f);
    ImGui::Begin("##background_loading", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextDisabled("Loading shader cache %u%%", u32(u64(done) * 100 / total));
    ImGui::End();
}

} // namespace

void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count) {
    std::scoped_lock lock{imgui_mutex};
    if (initialized) {
        return;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // window positions are not kept
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigNavSwapGamepadButtons = menu_circle_confirm();
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "tlg";

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.Colors[ImGuiCol_WindowBg].w = 0.92f;

    ImFontConfig font_config;
    font_config.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(tlg_font_ttf),
                                   int(tlg_font_ttf_end - tlg_font_ttf), 18.0f, &font_config);
    // A language pack in Japanese, Chinese, Korean, ...: DejaVu Sans lacks those characters, so a
    // system font is merged in (ImGui 1.92 loads the characters it draws on demand). Without
    // such a pack too, for the Japanese title in the menu's heading (a Japanese font first).
    {
        bool merged = false;
        for (const auto& extra : TlgLang::ExtraFontCandidates()) {
            if (std::FILE* f = std::fopen(extra.path.c_str(), "rb")) {
                std::fclose(f);
            } else {
                continue;
            }
            ImFontConfig merge_config;
            merge_config.MergeMode = true;
            merge_config.FontNo = extra.face;
            if (io.Fonts->AddFontFromFileTTF(extra.path.c_str(), 18.0f, &merge_config)) {
                std::fprintf(stderr, "[tlg] language pack font: %s (face %d)\n", extra.path.c_str(), extra.face);
                merged = true;
                break;
            }
        }
        cjk_font_merged = merged;
        if (!merged && TlgLang::NeedsExtraFont()) {
            std::fprintf(stderr, "[tlg] language pack font: none found, set TLG_LANG_FONT=<font file>\n");
        }
    }

    const vk::Instance vk_instance = instance.GetInstance();
    ImGui_ImplVulkan_LoadFunctions(
        instance.ApiVersion(),
        [](const char* name, void* user) {
            return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr(
                *static_cast<const vk::Instance*>(user), name);
        },
        const_cast<vk::Instance*>(&vk_instance));

    const VkFormat color_format = static_cast<VkFormat>(format);
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = instance.ApiVersion();
    info.Instance = vk_instance;
    info.PhysicalDevice = instance.GetPhysicalDevice();
    info.Device = instance.GetDevice();
    info.QueueFamily = instance.GetGraphicsQueueFamilyIndex();
    info.Queue = instance.GetGraphicsQueue();
    // Font atlas plus the save list pictures (up to 33, and a closed list's until freed).
    info.DescriptorPoolSize = 128;
    info.MinImageCount = std::max(image_count, 2u);
    info.ImageCount = std::max(image_count, 2u);
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format,
    };
    if (!ImGui_ImplVulkan_Init(&info)) {
        std::printf("Overlay: ImGui Vulkan backend init failed\n");
        ImGui::DestroyContext();
        return;
    }
    overlay_instance = &instance;
    initialized = true;
    std::printf("Overlay: menu ready (Insert or R3+L2)\n");
}

void UpdateTextInput(SDL_Window* window) {
    bool want = false;
    {
        std::scoped_lock lock{imgui_mutex};
        want = initialized && menu_open && ImGui::GetIO().WantTextInput;
    }
    if (want != SDL_TextInputActive(window)) {
        if (want) {
            SDL_StartTextInput(window);
        } else {
            SDL_StopTextInput(window);
        }
    }
}

// imgui_mutex held. The backend frees a texture some frames after its last use.
void RetireChoiceTextures() {
    for (ImTextureData* tex : choice_textures) {
        if (tex) {
            tex->WantDestroyNextFrame = true;
            choice_retired.push_back(tex);
        }
    }
    choice_textures.clear();
    choice_pictures.clear();
    choice_retiring = !choice_retired.empty();
}

// imgui_mutex held, before NewFrame: new pictures become textures, destroyed ones are freed.
void UpdateChoiceTextures() {
    if (!choice_pictures.empty()) {
        choice_textures.assign(choice_pictures.size(), nullptr);
        for (size_t i = 0; i < choice_pictures.size(); ++i) {
            const TlgPng::Image& picture = choice_pictures[i];
            if (picture.rgba.empty()) {
                continue;
            }
            ImTextureData* tex = IM_NEW(ImTextureData)();
            tex->Create(ImTextureFormat_RGBA32, picture.width, picture.height);
            std::memcpy(tex->GetPixels(), picture.rgba.data(), picture.rgba.size());
            ImGui::RegisterUserTexture(tex);
            choice_textures[i] = tex;
        }
        choice_pictures.clear();
    }
    std::erase_if(choice_retired, [](ImTextureData* tex) {
        if (tex->Status != ImTextureStatus_Destroyed) {
            return false;
        }
        ImGui::UnregisterUserTexture(tex);
        IM_DELETE(tex);
        return true;
    });
    choice_retiring = !choice_retired.empty();
}

void FinishSaveDialog() {
    RetireChoiceTextures();
    ImGui::GetIO().MouseDrawCursor = menu_open || move_bar_open;
}

// The save list and messages take the keys, buttons and mouse that answer them; the rest stays
// with the game (held neutral by CapturesInput).
bool HandleSaveEvent(const SDL_Event& event) {
    int move = 0, finish = 0; // finish: 1 accept, 2 back
    bool moved = false, click = false;
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN: {
        const SDL_Keycode key = event.key.key;
        move = key == SDLK_UP || key == SDLK_W || key == SDLK_LEFT || key == SDLK_A ? -1
               : key == SDLK_DOWN || key == SDLK_S || key == SDLK_RIGHT || key == SDLK_D ? 1
                                                                                      : 0;
        finish = key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE ? 1
                 : key == SDLK_ESCAPE || key == SDLK_BACKSPACE                   ? 2
                                                                                 : 0;
        break;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
        const u8 button = event.gbutton.button;
        move = button == SDL_GAMEPAD_BUTTON_DPAD_UP || button == SDL_GAMEPAD_BUTTON_DPAD_LEFT ? -1
               : button == SDL_GAMEPAD_BUTTON_DPAD_DOWN || button == SDL_GAMEPAD_BUTTON_DPAD_RIGHT ? 1
                                                                                                  : 0;
        finish = menu_button_result(button == SDL_GAMEPAD_BUTTON_SOUTH,
                                    button == SDL_GAMEPAD_BUTTON_EAST);
        break;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        const float density = PixelDensity(event.motion.windowID);
        save_mouse = ImVec2(event.motion.x * density, event.motion.y * density);
        ImGui::GetIO().AddMousePosEvent(save_mouse.x, save_mouse.y);
        moved = true;
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        click = event.button.button == SDL_BUTTON_LEFT;
        finish = event.button.button == SDL_BUTTON_RIGHT ? 2 : 0;
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        move = event.wheel.y > 0 ? -1 : event.wheel.y < 0 ? 1 : 0;
        break;
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        return true;
    default:
        return false;
    }
    if (message_active) {
        const bool two = (message_buttons >= 0 ? message_buttons
                          : SaveMessageFor(save_kind, message_code, message_value, message_text).buttons) % 2 == 1;
        if (two && move) message_focus = move < 0 ? 0 : 1;
        for (int b = 0; b < 2 && (moved || click); ++b) {
            if (save_button_rects[b].GetWidth() > 0 && save_button_rects[b].Contains(save_mouse)) {
                message_focus = b;
                finish = click ? 1 : finish;
            }
        }
        if (finish) {
            message_result = !two || (finish == 1 && message_focus == 1) ? 1 : 2;
            message_active = false;
            FinishSaveDialog();
        }
        return true;
    }
    const int count = int(save_rows.size());
    if (move && count) {
        choice_index = (choice_index + move + count) % count;
    }
    for (int i = 0; i < int(save_row_rects.size()) && (moved || click); ++i) {
        if (save_row_rects[i].GetWidth() > 0 && save_row_rects[i].Contains(save_mouse)) {
            choice_index = i;
            finish = click ? 1 : finish;
        }
    }
    if (finish) {
        choice_result = finish == 1 && count ? choice_index : -2;
        choice_active = false;
        FinishSaveDialog();
    }
    return true;
}

int64_t SteadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

/// tlg: tracks the held controller buttons and runs the combos (quit hold, our menu, QA menu).
/// Returns true when the event is used up. Caller holds imgui_mutex.
bool PadEvent(const SDL_Event& event) {
    u32 bit = 0;
    bool down = false;
    if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || event.type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
        if (event.gbutton.button >= 24) {
            return false;
        }
        bit = 1u << event.gbutton.button;
        down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    } else if (event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
        if (event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER) {
            bit = TlgSettings::PadL2;
        } else if (event.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
            bit = TlgSettings::PadR2;
        } else {
            return false;
        }
        down = event.gaxis.value > 16000;
        if (down == ((pad_mask & bit) != 0)) {
            return false; // trigger moved, but stayed on the same side of the threshold
        }
    } else {
        return false;
    }
    const u32 before = pad_mask;
    pad_mask = down ? pad_mask | bit : pad_mask & ~bit;
    if (capture_slot >= 0) {
        capture_ignore &= pad_mask;
        capture_mask |= pad_mask & ~capture_ignore;
        if (!pad_mask && capture_mask) {
            // All released: two or more buttons make the new combo; a single button keeps waiting.
            if (__builtin_popcount(capture_mask) >= 2) {
                ComboSlot(capture_slot)->store(capture_mask);
                std::printf("Settings: controller combo %d set to %s\n", capture_slot,
                            TlgSettings::ComboText(capture_mask).c_str());
                dirty = true;
                capture_slot = -1;
            }
            capture_mask = 0;
        }
        return true;
    }
    auto& s = TlgSettings::Get();
    const u32 quit = s.combo_quit;
    if (quit && (pad_mask & quit) == quit) {
        if (!quit_hold_start) {
            quit_hold_start = SteadyMs();
        }
    } else {
        quit_hold_start = 0;
    }
    if (!down) {
        return false;
    }
    // A combo fires when the press completes it (not again while it stays held).
    const auto completes = [&](u32 combo) {
        return combo && (bit & combo) && (pad_mask & combo) == combo && (before & combo) != combo;
    };
    if (completes(s.combo_menu)) {
        SetOpen(!menu_open);
        return true;
    }
    // The default QA combo stays in the game's Lua (works with keyboard keys too).
    if (s.combo_qa != TlgSettings::PadComboQa && completes(s.combo_qa)) {
        TlgFun::ToggleQaMenu();
        return true;
    }
    // Cross in photo mode: a screenshot (the menu is closed: photo mode closes it).
    if (bit == TlgSettings::PadCross && !menu_open && TlgFun::PhotoModeOn()) {
        screenshot_requested = true;
        return true;
    }
    if (completes(s.combo_photo)) {
        const bool on = !TlgFun::PhotoModeOn();
        TlgFun::PhotoMode(on);
        NoticeLocked(on ? TR("Photo mode on") : TR("Photo mode off"), 2.0f);
        return true;
    }
    return false;
}

/// tlg: the quit hold, once per presented frame: the note while held, the quit after 2 s.
void QuitHoldTick() {
    const int64_t start = quit_hold_start;
    if (!start || TlgQuit::Requested()) {
        return;
    }
    if (SteadyMs() - start >= QuitHoldMs) {
        quit_hold_start = 0;
        Notice(TR("Quitting..."), 10.0f);
        TlgQuit::Request("controller");
    } else {
        Notice(TR("Quitting... (release to cancel)"), 0.2f);
    }
}

bool HandleEvent(const SDL_Event& event) {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized) {
        return false;
    }
    if (PadEvent(event)) {
        return true;
    }
    if ((choice_active || message_active) && !menu_open) {
        return HandleSaveEvent(event);
    }
    ImGuiIO& io = ImGui::GetIO();
    const bool is_open = menu_open;
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        static const bool capture_key = [] {
            const char* env = std::getenv("TLG_FRAME_CAPTURE_KEY");
            return env && *env == '1';
        }();
        if (down && !event.key.repeat && capture_key && event.key.key == SDLK_F11) {
            Vulkan::FrameCapture::Request();
            return true;
        }
        // tlg: F8 switches between the anti-aliasing choice and the game's own SMAA (saved; the
        // label shows briefly).
        if (down && !event.key.repeat && event.key.key == SDLK_F8) {
            // Switches between Built-in and the last other upscaler (DLSS, else FSR 4, else FSR 3.1).
            static int last_upscaler = -1;
            const int current = TlgSettings::SelectedUpscaler();
            if (current != TlgSettings::UpscalerOff) {
                last_upscaler = current;
                TlgSettings::SetUpscaler(TlgSettings::UpscalerOff);
            } else {
                TlgSettings::SetUpscaler(last_upscaler >= 0 ? last_upscaler : TlgSettings::DefaultAaUpscaler());
            }
            aa_label_until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            std::printf("AA: %s (F8)\n", TlgSettings::AaOn() ? TlgSettings::AaModeName(TlgSettings::AaModeOf()) : "game SMAA");
            TlgSettings::Save();
            return true;
        }
        // tlg debug (TLG_JITTER_DEBUG=1): F7 switches the DLAA jitter (not saved), F6 marks a
        // spot: the log gets the draw map and jitter suspects of one frame plus a frame capture.
        static const bool jitter_debug = [] {
            const char* env = std::getenv("TLG_JITTER_DEBUG");
            return env && *env == '1';
        }();
        if (down && !event.key.repeat && jitter_debug && event.key.key == SDLK_F7) {
            auto& settings = TlgSettings::Get();
            settings.jitter = !settings.jitter;
            notice_text = settings.jitter ? "Jitter ON" : "Jitter OFF";
            notice_until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            std::printf("Jitter: %s (F7)\n", settings.jitter ? "on" : "off");
            std::fflush(stdout);
            return true;
        }
        if (down && !event.key.repeat && jitter_debug && event.key.key == SDLK_F6) {
            static int marks = 0;
            TlgSettings::Get().jitter_mark = true;
            Vulkan::FrameCapture::Request();
            notice_text = "Mark " + std::to_string(++marks);
            notice_until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            std::printf("Jitter mark %d (F6, jitter %s)\n", marks,
                        TlgSettings::Get().jitter ? "on" : "off");
            std::fflush(stdout);
            return true;
        }
        // tlg: F12 shows or hides the Fun move bar; the mouse then picks a move.
        if (down && !event.key.repeat && event.key.key == SDLK_F12) {
            move_bar_open = !move_bar_open;
            io.MouseDrawCursor = is_open || move_bar_open;
            return true;
        }
        // tlg: F9 switches photo mode on and off (the keyboard twin of the photo combo).
        if (down && !event.key.repeat && event.key.key == SDLK_F9) {
            const bool on = !TlgFun::PhotoModeOn();
            TlgFun::PhotoMode(on);
            if (on) {
                SetOpen(false);
            }
            NoticeLocked(on ? TR("Photo mode on") : TR("Photo mode off"), 2.0f);
            return true;
        }
        // tlg: Print Screen saves a screenshot (user/screenshots, PNG at render resolution).
        if (down && !event.key.repeat && event.key.key == SDLK_PRINTSCREEN) {
            screenshot_requested = true;
            return true;
        }
        // tlg: Alt+Q quits through the game's own quit (settings and progress saved).
        if (down && !event.key.repeat && event.key.key == SDLK_Q && (event.key.mod & SDL_KMOD_ALT)) {
            TlgQuit::Request("Alt+Q");
            return true;
        }
        // tlg: F10 shows or hides the performance overlay (FPS counter) at any time.
        if (down && !event.key.repeat && event.key.key == SDLK_F10) {
            auto& settings = TlgSettings::Get();
            settings.show_fps = !settings.show_fps;
            return true;
        }
        if (down && !event.key.repeat &&
            (event.key.key == SDLK_INSERT || (is_open && event.key.key == SDLK_ESCAPE))) {
            SetOpen(event.key.key == SDLK_INSERT ? !is_open : false);
            return true;
        }
        if (!is_open) {
            return false;
        }
        io.AddKeyEvent(ImGuiMod_Ctrl, (event.key.mod & SDL_KMOD_CTRL) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (event.key.mod & SDL_KMOD_SHIFT) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (event.key.mod & SDL_KMOD_ALT) != 0);
        if (const ImGuiKey key = KeyFromSdl(event.key.key); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        const u8 button = event.gbutton.button;
        // The menu combo (default R3+L2) is handled in PadEvent.
        if (!is_open) {
            return false;
        }
        if (const ImGuiKey key = KeyFromGamepad(button); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        return false; // triggers: PadEvent
    }
    case SDL_EVENT_TEXT_INPUT: {
        // Typed characters (Ctrl+click on a slider, a text field): key events alone erase but
        // do not type. SDL sends them while text input is on (UpdateTextInput).
        if (!is_open) {
            return false;
        }
        io.AddInputCharactersUTF8(event.text.text);
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        if (!is_open && !move_bar_open) {
            return false;
        }
        const float density = PixelDensity(event.motion.windowID);
        io.AddMousePosEvent(event.motion.x * density, event.motion.y * density);
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (!is_open && !move_bar_open) {
            return false;
        }
        const int button = event.button.button == SDL_BUTTON_LEFT    ? 0
                           : event.button.button == SDL_BUTTON_RIGHT  ? 1
                           : event.button.button == SDL_BUTTON_MIDDLE ? 2
                                                                      : -1;
        if (button >= 0) {
            io.AddMouseButtonEvent(button, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        }
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (!is_open) {
            return false;
        }
        io.AddMouseWheelEvent(event.wheel.x, event.wheel.y);
        return true;
    default:
        return false;
    }
}

// After "Apply and restart", the new options are kept once the game has shown about ten
// seconds of frames after loading (tlg_run.py restores the previous ones otherwise).
void ConfirmRestart() {
    static u32 frames = 0;
    auto& s = TlgSettings::Get();
    if (!s.restart_unconfirmed || loading || ++frames < 600) {
        return;
    }
    s.restart_unconfirmed = false;
    TlgSettings::Save();
    std::puts("Settings: graphics options confirmed after restart");
}

void SetBackgroundLoading(u32 done, u32 total) {
    background_done = done;
    background_total = total;
}

void SetLoading(bool active, u32 done, u32 total) {
    loading_done = done;
    loading_total = total;
    loading = active;
}

void SetPauseMenu(bool open) {
    pause_menu_open = open;
}

bool TakeScreenshotRequest() {
    return screenshot_requested.exchange(false);
}

void NoticeLocked(const std::string& text, float seconds) {
    notice_text = text;
    notice_until = std::chrono::steady_clock::now() +
                   std::chrono::milliseconds(static_cast<int>(seconds * 1000.0f));
}

void Notice(const std::string& text, float seconds) {
    std::scoped_lock lock{imgui_mutex};
    notice_text = text;
    notice_until = std::chrono::steady_clock::now() +
                   std::chrono::milliseconds(static_cast<int>(seconds * 1000.0f));
}

static bool NoticeActive() {
    return std::chrono::steady_clock::now() < notice_until;
}

/// tlg: the AA label (top right), for a moment after F8.
bool AaLabelOn() {
    const auto& s = TlgSettings::Get();
    return s.upscaler != TlgSettings::UpscalerOff && s.upscaler != TlgSettings::UpscalerTaa &&
           std::chrono::steady_clock::now() < aa_label_until;
}

bool Visible() {
    const auto& s = TlgSettings::Get();
    const auto now = std::chrono::steady_clock::now();
    return initialized && (loading || background_total || menu_open || text_entry_active || choice_active ||
                           message_active || progress_on || now < progress_until || now < toast_until || AaLabelOn() ||
                           NoticeActive() || quit_hold_start != 0 || pause_menu_open ||
                           choice_retiring || s.show_fps || move_bar_open ||
                           TlgFun::PhotoModeOn());
}

bool CapturesInput() {
    return menu_open || text_entry_active || choice_active || message_active;
}

void BeginSaveList(int kind, std::vector<SaveRow> rows, std::vector<TlgPng::Image> pictures, int focus) {
    std::scoped_lock lock{imgui_mutex};
    RetireChoiceTextures();
    choice_pictures = std::move(pictures);
    choice_pictures.resize(rows.size());
    save_kind = kind;
    save_rows = std::move(rows);
    save_row_rects.clear();
    choice_index = focus >= 0 && focus < int(save_rows.size()) ? focus : 0;
    choice_first = 0;
    choice_result = -1;
    message_active = false;
    progress_on = false;
    choice_active = true;
    ImGui::GetIO().MouseDrawCursor = true;
}

int PollSaveList() {
    return choice_result;
}

void BeginSaveMessage(int kind, int code, u64 value, const std::string& text, int buttons,
                      const SaveRow* entry, TlgPng::Image picture) {
    std::scoped_lock lock{imgui_mutex};
    RetireChoiceTextures();
    message_has_entry = entry != nullptr;
    if (entry) {
        message_entry = *entry;
        choice_pictures.clear();
        choice_pictures.push_back(std::move(picture));
    }
    save_kind = kind;
    message_code = code;
    message_value = value;
    message_text = text;
    message_buttons = buttons;
    // Deleting starts on "No"; everything else on the answer the player came for.
    message_focus = kind == 3 ? 0 : 1;
    save_button_rects[0] = save_button_rects[1] = ImRect();
    message_result = -1;
    choice_active = false;
    progress_on = false;
    message_active = true;
    ImGui::GetIO().MouseDrawCursor = true;
}

int PollSaveMessage() {
    return message_result;
}

void CloseSaveDialog() {
    std::scoped_lock lock{imgui_mutex};
    if (choice_active.exchange(false)) {
        choice_result = -2;
    }
    if (message_active.exchange(false)) {
        message_result = 2;
    }
    FinishSaveDialog();
}

void SaveProgress(int kind, int active, int percent, const char* text) {
    std::scoped_lock lock{imgui_mutex};
    const auto now = SaveClock::now();
    if (!active) {
        progress_on = false;
        return;
    }
    if (!progress_on && now >= progress_until) {
        progress_since = now;
    }
    progress_kind = kind;
    progress_percent = percent;
    if (text) {
        progress_text = text;
    }
    if (active == 2) {
        progress_until = now + std::chrono::milliseconds(1200);
    } else {
        progress_on = true;
    }
}

void SaveToast(int kind) {
    std::scoped_lock lock{imgui_mutex};
    const auto now = SaveClock::now();
    // A dialog's "saved" and the save's own unmount come together: one toast.
    if (now < toast_until + std::chrono::seconds(3)) {
        toast_until = std::max(toast_until, now + std::chrono::milliseconds(2600));
        return;
    }
    toast_kind = kind;
    toast_since = now;
    toast_until = now + std::chrono::milliseconds(2600);
}

void SetTextEntry(bool active, const std::string& prompt, const std::string& text) {
    std::scoped_lock lock{imgui_mutex};
    text_entry_active = active;
    text_entry_prompt = prompt;
    text_entry_text = text;
}

void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent) {
    // Present interval for the FPS readout (measured also while nothing is drawn).
    const auto now = std::chrono::steady_clock::now();
    const float ms = std::chrono::duration<float, std::milli>(now - last_present).count();
    last_present = now;
    if (ms > 0.0f && ms < 1000.0f) {
        frame_ms_avg = frame_ms_avg == 0.0f ? ms : frame_ms_avg * 0.95f + ms * 0.05f;
    }
    ConfirmRestart();
    QuitHoldTick();
    if (!Visible()) {
        return;
    }
    std::scoped_lock lock{imgui_mutex};
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(extent.width), float(extent.height));
    io.DeltaTime = ms > 0.0f && ms < 1000.0f ? ms / 1000.0f : 1.0f / 60.0f;
    // UI scale follows the display height (1080p = 1).
    const float scale = std::max(float(extent.height) / 1080.0f, 0.75f);
    if (std::abs(scale - base_scale) > 0.01f) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(scale / base_scale);
        style.FontScaleMain = scale;
        base_scale = scale;
    }

    UpdateChoiceTextures();
    MakeMoveIcons();
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    if (loading) {
        LoadingScreen();
    }
    if (menu_open) {
        Menu();
    }
    if (TlgSettings::Get().show_fps && !menu_open) {
        FpsCounter();
    }
    if (background_total && !loading && !menu_open) {
        BackgroundLoading();
    }
    if (text_entry_active) {
        TextEntryBox();
    }
    if (!menu_open) {
        SaveDialogs();
    }
    if (NoticeActive() && !menu_open) {
        NoticeBox();
    }
    if (TlgFun::PhotoModeOn() && !menu_open && !loading) {
        PhotoHelpBox();
    }
    if (pause_menu_open && !menu_open && !quit_hold_start) {
        QuitHintBox();
    }
    if (move_bar_open && !menu_open && !loading) {
        MoveBarWindow();
    }
    if (AaLabelOn() && !menu_open) {
        const auto& s = TlgSettings::Get();
        const int aa_mode = TlgSettings::AaModeOf();
        const int mode = s.jitter && TlgSettings::AaModeSupported(aa_mode) ? aa_mode : TlgSettings::AaOff;
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos({size.x - 8.0f, 8.0f}, ImGuiCond_Always, {1.0f, 0.0f});
        ImGui::SetNextWindowBgAlpha(0.6f);
        ImGui::Begin("##aa_label", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
        ImGui::Text("AA: %s  (F8)", mode == TlgSettings::AaOff ? "game SMAA" : TlgSettings::AaModeName(mode));
        ImGui::End();
    }
    ImGui::Render();

    const vk::RenderingAttachmentInfo attachment{
        .imageView = view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmdbuf.beginRendering(vk::RenderingInfo{
        .renderArea = {{0, 0}, extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    });
    {
        // Font atlas uploads submit to the graphics queue themselves.
        std::scoped_lock submit_lock{Vulkan::Scheduler::submit_mutex};
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmdbuf);
    }
    cmdbuf.endRendering();
}

} // namespace TlgOverlay
