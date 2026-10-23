// SPDX-License-Identifier: GPL-2.0-or-later
// A color buffer must never produce a zero-extent image (the driver creates it as-is and its
// readback then asks the staging pool for a 0-byte buffer -> buffer.cpp:135 -> exit 23), and
// its readback must never ask for more than the guest allocation holds (a slice smaller than
// one row) — texture_cache skips that one instead of asserting.
#include <cassert>
#include "video_core/amdgpu/regs_color.h"
#include "video_core/texture_cache/image_info.h"

using AmdGpu::CbDbExtent;
using AmdGpu::ColorBuffer;
using AmdGpu::TileMode;
using VideoCore::ImageInfo;
using VideoCore::ReadbackSize;

// 2560x1440 RGBA8: pitch 2560; the slice register counts 64-pixel tiles, so a full slice is
// 2560*1440/64 = 57600 of them -> tile_max 57599.
static ColorBuffer MakeColorBuffer(u32 pitch_tile_max, u32 slice_tile_max) {
    ColorBuffer cb{};
    cb.base_address = 0x10000000 >> 8;
    cb.info.format = u32(AmdGpu::DataFormat::Format8_8_8_8);
    cb.info.number_type = u32(AmdGpu::NumberFormat::Unorm);
    cb.attrib.tile_mode_index = TileMode::DisplayLinearAligned;
    cb.pitch.tile_max = pitch_tile_max;
    cb.slice.tile_max = slice_tile_max;
    cb.view.slice_max = 0;
    return cb;
}

int main() {
    const ColorBuffer cb = MakeColorBuffer(319, 57599);
    assert(cb.Pitch() == 2560);
    assert(cb.Height() == 1440);

    // A complete hint is used verbatim, and the readback covers exactly the allocation.
    CbDbExtent hint{};
    hint.width = 1920;
    hint.height = 1080;
    const ImageInfo hinted(cb, hint);
    assert(hinted.size.width == 1920 && hinted.size.height == 1080);
    assert(ReadbackSize(hinted) == hinted.guest_size);

    // A half-zero hint is not an extent: fall back to the registers rather than build a
    // zero-row image.
    hint.width = 2560;
    hint.height = 0;
    const ImageInfo no_height(cb, hint);
    assert(no_height.size.width == 2560 && no_height.size.height == 1440);

    hint.width = 0;
    hint.height = 1440;
    const ImageInfo no_width(cb, hint);
    assert(no_width.size.width == 2560 && no_width.size.height == 1440);

    // Registers whose slice is smaller than one row deduce 0 rows; the image still gets the
    // one row it minimally needs instead of a zero-extent one. Linear guest_size follows the
    // extent, so the readback still fits the allocation and runs.
    const ColorBuffer degenerate = MakeColorBuffer(8, 0);  // pitch 72, slice 64 pixels
    assert(degenerate.Height() == 1);
    const ImageInfo no_hint(degenerate, CbDbExtent{});
    assert(no_hint.size.width == 72 && no_hint.size.height == 1);
    assert(ReadbackSize(no_hint) <= no_hint.guest_size);

    // Tiled: the slice registers are the allocation, so the same sub-row slice cannot hold
    // the clamped one-row extent — the readback has to be refused, not asserted on.
    ColorBuffer tiled = cb;
    tiled.attrib.tile_mode_index = TileMode::Display2DThin;
    const ImageInfo tiled_ok(tiled, CbDbExtent{});
    assert(ReadbackSize(tiled_ok) == tiled_ok.guest_size);

    ColorBuffer tiled_sub_row = MakeColorBuffer(8, 0);
    tiled_sub_row.attrib.tile_mode_index = TileMode::Display2DThin;
    const ImageInfo sub_row(tiled_sub_row, CbDbExtent{});
    assert(sub_row.size.height == 1);
    assert(ReadbackSize(sub_row) > sub_row.guest_size);
}
