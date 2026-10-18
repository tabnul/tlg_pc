// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace AmdGpu {

union CbDbExtent {
    struct {
        u16 width;
        u16 height;
    };
    u32 raw;

    /// A hint is only usable when it carries both dimensions: a half-zero raw value (the
    /// NOP payload of a CB/DB setup that does not describe a real extent) would otherwise
    /// build a zero-row image, which the driver happily creates and which then reads back
    /// as a 0-byte copy.
    bool Valid() const {
        return width != 0 && height != 0;
    }
};

} // namespace AmdGpu
