# Job-worker "null object" crash family (2026-10-27)

Static analysis only: eboot.elf disassembly (guest offset = vaddr) plus the register dumps in
upstream issue reports and our route logs. No fix has been tried.

## Two different groups

### Group 1: the upstream AsyncMap path (#131, and probably #60 and #136)
- #131 (v2026.10.24-1a130dd, Windows, RX 9070) crashes in the menu with a write to 0x24 at 0x2719aa.
  - Site: a spinlock `lock cmpxchg [rdi+0x14]` with rdi = 0x10, so a null pool plus 0x10.
  - This is exactly the crash upstream TECNICO.md "Readback GPU→CPU (resolvido)" describes
    (0x12719aa in Ghidra's base). The caller 0xc4c570 reads async-compute results through
    `JsComputeContext::AsyncMap` (0x1473600, called only from 0xc49540 and 0xc4c570).
  - So Precise readbacks still let stale GPU-written data through on 1a130dd.
- #60 at 0x28c193: the object at `[caller+0x290]` has its count `+0x50` set but its array `+0x28`
  null, so it is half built or stale.
- #136 at 0x20654b: the caller 0xc9867c stores `(r14d==0) ? [rbp-0x268] : 0` and passes that
  pointer plus 0x70 without a null check, so a lookup returned null.
- Our own TLG_QUERY_PREV_FRAME patch hit the same 0x2719aa site (one-frame-old query data). It is
  off for that reason.

### Group 2: ours (7 crashes in 449 route logs, all in the Js GPU-object code 0x1479c52–0x14a3c9b)
- 5 of the 7 have **rdi = 0x104ad081b0**: one game render-context object at a fixed address in
  every run.
- **Path A** (0xc577f9 → 0xee8ec4 → 0x147e340 → 0x1479b70):
  - The function walks a slot mask at `[ctx+0x4670]+0x100` and the slot table at `ctx+0x2098`.
  - It calls 0x1479db0 with `[slot+0x30]`.
  - The faults are at 0x1479c52 (the slot entry itself is 0x91a0 or 0x4090000000000001), at
    0x1479ddd (`[slot+0x30]` = 0x000080100000000c) and at 0x14a3c9b.
- **Path B** (0xc58e0d → 0xc5d952 → 0xc77788 → 0x148e340 → 0x1488580):
  - The function walks a 128-entry slot table at `ctx+0x580` and calls 0x1479db0 the same way.
  - The slot entries are 0x1e and 0xffffffff00000000 (faults at 0x1488a22), and one fault is at
    0x147dcc6.
- **All three path-B crashes, and one path-A crash, come right after the game's own check:**
  "JsDeviceContext::CopyResource Error. Resrouce dimension / ByteWidth is not same".
  - That error appears in 5 of the 449 logs, and all 5 crashed.
  - It is an early warning that the resource state is already corrupt.
- Nearly all of them happen during a stage or demo load (the bench shows "no replay in the log").
- They happen across unrelated experiments: b16on (2 of 16 runs; b16off 0 of 12), cp2on, emuread,
  emuwrite, ord-smoke and s3-hot-c. They come from no single toggle, and the readback fixes were on
  in several of them.

**Reading:** the context's own slot words, in two separate areas of one object, are overwritten
with non-pointer data. That looks like **something writing over CPU-owned guest memory**, more
than a stale read. Candidates:
1. **A late emulator write into memory the game has freed and reused** during a load. The
   candidates are a buffer or image readback, a deferred readback, an EOP or query write, or
   a DMA. It fits the timing: resource churn at load.
2. **A GPU buffer range that covers the context's pages.** A readback then copies older GPU
   data over newer CPU writes.
3. A game race that the PC's timing exposes. This is the least likely, given the values.

## Results of the diagnostics (2026-10-27, diag build wt-crashdiag, local only)
- **Write watch** (`TLG_WRITE_WATCH=1000000000-107b000000`, every emulator write into guest
  memory through `TryWriteBacking`): 0 hits in 16 runs. No emulator write lands in the game's own
  memory.
- **Stale-readback check** (`TLG_STALE_CHECK=2`: every side-queue readback is compared with a
  second copy made after a full Finish): 0 stale ranges in 15 runs (3x `bench` = `s14_CondorClash`
  cutscene `ch_020`, 12x `terrace` = `s04_FirstAntenna` `_090_GardenTerrace_001`), ~2,600 checked
  copies per 5 s. The tick-based readback tracking is correct.
- **No crash since 2026-10-26 21:26** (cp2on_1): 0 in the 115 runs after it (the rest of that
  evening plus all of 10-27, on master, integration and diag builds). All 7 crashes were in
  experimental builds. No "CopyResource Error" since either.
- Static RE: CopyResource (0x147e860) is called only from 0xee5130, which sits in the same game
  render routine (0xc55690) as both crash paths. The game copies between its own resource
  objects there, and during a load they are sometimes of different sizes.

**Updated reading:** candidates 1 and 2 are ruled out for the cases we can watch. What is left is
3: a game-side race at load that our timing exposes, seen only with experimental timing changes
(bit 16, cp2on and others). It is not reproducible on the current code.

## Old proposed next step (done, see above)
The context address is the same every run, which makes a watch cheap:
- **Debug env (for example `TLG_WATCH=0x104ad081b0,0x4680`):** every host-side write into guest
  memory checks for overlap with the watched range. That covers readback copies,
  `runtime_memory_write_backing`, EOP/query/DMA writes and image readback. A hit logs the writer,
  the range and a backtrace.
- **The same check for GPU buffer and image ranges** that are created over the watched range.
- Run the load-heavy routes (fb-a, cp2on-style condor → s15) until a "CopyResource Error"
  appears. The first foreign write before it names the culprit.
- For group 1, run a #131-style menu repro on upstream main with `TLG_READBACKS=2`.

## Addendum (2026-10-28): the family came back

The line above saying "no crash since 2026-10-26 21:26" no longer holds. Five more crashes, at
the same sites:

| When | Path | Site | Fault address | Build | ByteWidth warning |
|---|---|---|---|---|---|
| 10-27 15:07 | A | 0x1479ddd | ffffffffffffffff | integration (plain, no env) | no |
| 10-27 15:35 | B | 0x1488a22 | 4e | experiment | yes |
| 10-27 19:11 | A | 0x1479c52 | 31 | experiment | no |
| 10-27 22:56 | B | 0x1488a22 | 4e | feat/dlss-upscaling | yes |
| 10-27 23:55 | A | 0x1479ddd | ffffffffffffffff | feat/dlss-upscaling | no |

- The 15:07 crash ran on the plain integration exe, so the family is not limited to
  experimental timing changes.
- The two branch crashes were 2 of 31 launches. Both happened during the CONTINUE load right
  after `s15_SecondFlight` demo `sf_010` became ready.
- The next instance on the same build ran 6.2 h in DLAA, through that load, with 0 faults.
- Across all builds the count is now 12. The other builds had 10 in 622 route logs, about 1.6%.
  Two events are too few to say whether the branch changes the rate.

### Options (not decided)
1. A guard patch like `cloth_null_guard`: at the two slot walks (path A `ctx+0x2098`, path B
   `ctx+0x580`), skip a slot whose word is not a valid guest pointer. That turns the crash into
   one skipped resource for one frame during a load. It would go on its own branch, proven with
   a crash loop.
2. Measure first: an overnight CONTINUE loop on master and on this branch, about 100 launches
   each.
3. Leave it closed, and watch upstream #60, #131 and #136.
