// SPDX-License-Identifier: GPL-2.0-or-later
// tlg diag, not for commit: TLG_RB_DIAG=1, a ledger of every GPU -> CPU readback, per guest
// site. Never benchmark with it on (it reads guest memory twice per download).
//
// Buffers: per fault (read or write on GPU-tracked pages) what the emulator did (fast release,
// download, write without flush, image wait, nothing), the time split (queue wait for the GPU
// command thread, recording, GPU wait, write-back, image wait) and, per download, how many of
// the downloaded bytes differ from what the CPU sees (a download that changes nothing was not
// needed at that moment), whether the faulting bytes or page changed, which GPU write (shader
// stage + hash, fill, DMA, copy shader) produced the changed bytes and which label the CP
// wrote last before the fault.
// Images: per downloaded image (linear readbacks) the copies, their bytes, the bytes that
// changed when the copy landed, the CPU accesses while a copy was pending (per site), the GPU
// side waits per source and the GPU finishes forced by the readback budget.
//
// Every 5 s it prints "RBD ..." lines (key=value, parsed by devkit rbdiag.py). TLG_RB_TRACE=
// <file> also writes one CSV line per fault. TLG_RB_CONTROL=<file> is re-read every 0.5 s:
//   skip <site>...         skip buffer readbacks of these sites (guest offset hex, e.g. c459ed)
//   only <site>...         skip buffer readbacks of every other site
//   skipr / skipw          skip all read / all write buffer readbacks
//   skipimg <addr>...      do not copy these images back (guest address hex)
//   onlyimg <addr>...      copy only these images back
//   mark <text>            print a marker line (what the user saw at that moment)
// A skipped read fault only drops read protection (the CPU reads stale bytes); a skipped write
// fault marks the page CPU-modified without download (as TLG_READBACKS=0 does).
#pragma once

#include <cstdint>

namespace TlgRbDiag {
using u64 = std::uint64_t;
using u32 = std::uint32_t;

enum Outcome : u32 {
    NotMapped = 1u << 0,    // not GPU-mapped memory: not ours
    FastRelease = 1u << 1,  // read fault, no GPU-modified data: protection dropped only
    Download = 1u << 2,     // buffer download ran (copies may still be empty)
    Empty = 1u << 3,        // download found nothing to copy
    WriteNoFlush = 1u << 4, // write fault, page not GPU-modified: no download
    ImageWait = 1u << 5,    // waited for a pending image readback
    Skipped = 1u << 6,      // live control skipped the buffer readback
    Side = 1u << 7,         // copy ran on the readback queue (last write done)
    NumOutcomes = 8,
};

enum ProducerKind : u32 {
    // The shader kinds follow Shader::SwStage + 1.
    ProdNone, ProdFragment, ProdTessCtrl, ProdTessEval, ProdVertex, ProdGeometry, ProdCompute,
    ProdFill, ProdDmaCopy, ProdHleCopy, ProdOther, NumProducerKinds
};

enum LabelKind : u32 {
    LabelEop, LabelEos, LabelWriteData, LabelReleaseMem, LabelComputeWriteData, NumLabelKinds
};

/// The fault in progress on this thread (or the one a GPU-thread command serves).
struct Ctx {
    Ctx* previous = nullptr;
    u64 rip = 0, caller = 0, addr = 0;
    u64 frames[4]{}; // host return addresses (internal requests without a guest fault)
    bool write = false, gpu_thread = false, internal = false;
    u32 outcome = 0;
    u64 t_fault = 0;
    u64 queue_ns = 0, record_ns = 0, wait_ns = 0, writeback_ns = 0, image_ns = 0, diag_ns = 0;
    u64 downloads = 0, copies = 0, bytes = 0, changed = 0;
    u64 hot_bytes = 0, hot_changed = 0;
    u64 changed_hit = 0;  // changed bytes in [addr, addr + 8)
    u64 changed_page = 0; // changed bytes in the faulting 4 KiB page
    u64 min_age_ns = ~0ull; // time since the youngest recorded GPU write of the changed bytes
    u32 last_write_state = 0; // 0 unknown, 1 done, 2 submitted, 3 recording
    static constexpr int MaxProducers = 6;
    u32 producer[MaxProducers]{};
    u64 producer_changed[MaxProducers]{};
    u32 label_kind = NumLabelKinds; // the label written last before the fault
    u64 label_age_ns = 0;
};

bool Enabled();
u64 NowNs();

/// The fault handler: begins/ends the record of one guest fault (context = EXCEPTION_POINTERS
/// or ucontext_t).
void BeginFault(Ctx& ctx, void* context, u64 addr, bool write, bool gpu_thread);
void EndFault(Ctx& ctx, bool handled);

/// The context this thread works for (nullptr outside a fault).
Ctx* Current();
void AddOutcome(u32 outcome);

/// Installs a fault context on the thread that serves it (the GPU command thread).
struct Scope {
    explicit Scope(Ctx* ctx);
    ~Scope();
    Ctx* previous;
};

/// A readback request without a guest fault (emulator-internal invalidation, unmap): recorded
/// as its own site, keyed by the host call stack. Does nothing when a fault is in progress.
struct InternalScope {
    InternalScope(u64 addr, bool write);
    ~InternalScope();
    Ctx ctx;
    bool active = false;
};

/// Live control: true when the buffer readback of this fault / the copy of this image must be
/// skipped.
bool SkipBuffer(const Ctx& ctx);
bool SkipImage(u64 image_addr);

/// Test fix, not for commit; works with the diag off. TLG_RB_SKIP_READ / TLG_RB_SKIP_WRITE =
/// comma-separated guest code offsets (hex) whose buffer readbacks are skipped (as the control
/// file's skip does); TLG_RB_IMG_MAX_BYTES=N copies only images of at most N bytes back.
/// FixScope wraps the fault handler; FixSkip is true while it serves a fault of a listed site.
struct FixScope {
    FixScope(void* context, bool write);
    ~FixScope();
    bool previous;
};
bool FixSkip();
bool FixSkipImage(u64 size);

/// GPU writes: NoteGpuWrite on this thread is attributed to the producer of the innermost
/// ProducerScope (shader hash for shader stages); without one, to the caller's return address.
struct ProducerScope {
    ProducerScope(ProducerKind kind, u64 hash);
    ~ProducerScope();
    u64 previous;
};
void NoteGpuWrite(u64 addr, u64 size, const void* return_address);

/// One buffer download (BufferCache::DownloadMemory) on the serving thread. `downloaded` holds
/// the GPU bytes about to be written to [dst_addr, dst_addr + size).
void NoteCopy(u64 dst_addr, const void* downloaded, u64 size, bool hot);
void NoteDownload(u64 record_ns, u64 wait_ns, u64 writeback_ns, u32 state, bool side, bool empty);

/// Image readbacks: a copy queued, a copy landed (staging bytes before the write-back), a CPU
/// or GPU-side access that resolved pending copies (ctx = the fault, nullptr for GPU-side
/// sources; ns includes the queue wait), a GPU finish forced by ProcessDownloadImages
/// (reason 0: budget full, 1: copy too large, 2: not deferred) and a skipped copy.
void NoteImageQueue(u64 addr, u32 width, u32 height, u32 bits, u32 format, bool render_target,
                    u64 size);
void NoteImageLand(u64 addr, const void* staging, u64 size);
void NoteImageResolve(Ctx* ctx, u32 source, u64 addr, u64 size, bool pending, bool waited,
                      u64 ns);
void NoteImageFinish(u32 reason, u64 ns);
void NoteImageSkip(u64 addr);

/// Labels the CP writes (fences, WriteData, RELEASE_MEM).
void NoteLabel(LabelKind kind, u64 addr);
} // namespace TlgRbDiag
