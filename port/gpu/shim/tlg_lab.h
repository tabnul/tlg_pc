// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: the menu's Lab tab, for tuning tests. Rows come from the file named by TLG_LAB (re-read
// when it changes), one per line: label | object | property | mode | min | max | format | help.
// The property may be a dotted path (skinningSettings.skinWindMaxFrameSkip). Mode: % scales the
// game's value, + adds to it, = sets it; a component list picks vector parts (%1,3 or =2). The
// Lua tick (tlg_lua.cpp) applies the moved rows and reports the game's own values back.

#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace TlgLab {

struct Row {
    std::string label, object, path, help, format;
    char mode = '%';
    std::vector<int> parts;
    float lo = 0, hi = 200, value = 0;
    bool active = false;
    std::string game; // the game's value as last reported ("" before the first tick)
};

/// The current rows (re-reads the file when it changed). Menu thread.
std::vector<Row> Rows();
/// Moves row `index` to `value` (it is applied from then on).
void Set(size_t index, float value);
/// Stops applying row `index` (or all rows); the game's value is written back.
void Reset(size_t index);
void ResetAll();
/// The game's value for the wind skinning frame skip (fixed menu row), "" when unknown.
std::string WindGame();

/// Lua for the settings tick: the helpers plus one call per row. Game thread.
std::string Lua(int wind_skip);
/// "<index> <text>" from a "--TLGVAL" chunk: the game's value of row `index` (-1: wind).
void Report(std::string_view text);

} // namespace TlgLab
