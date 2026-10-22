# Windows: deadlock between speculative recovery and GPU readback

## Evidence

The local log `run-20261017-202612.log` stopped after 2,427 frames. The `FREEZE`
report showed `tlg:DrawPrep1` and `workerThread1` inside the page fault handler,
waiting for `GPU thread (command)`. The main thread and the other workers were
waiting for rendering. The frame counter stayed still while the vblanks kept
advancing.

## Cause

`PipelineCache::GetProgram` holds `programs_mutex` shared while a speculative
worker runs `RefreshFlatBuf`/`StageSpecialization`. These guest memory accesses
have a recovery point (`runtime_fault_recover`), because the data may be
GPU-protected or may have been reused.

On Windows, `vectored_fault` called `tlggpu_handle_fault` before checking that
point. The readback could queue a command and wait for the GPU thread; that
thread, when it had to publish or update shaders, needed `programs_mutex`
exclusively, still held by the worker. The wait was circular. The POSIX handler
already recovered the speculative read before the readback.

## Fix

The Windows handler now recovers the speculative access first, through the
existing `tlg_longjmp` trampoline. The worker abandons preparing that draw and
releases the lock. Only accesses without a recovery point go to the GPU's
tracking and readback. Precise readbacks and the workers stay enabled.

## Regression test

`test_windows_fault_recovery.py` extracts the production handler and compiles it
with `compat_win.c`. It uses really protected Windows pages and checks:

- 100 speculative faults recovered without calling the GPU callback;
- the recovery point in TLS is cleared;
- normal read and write faults still go to the GPU tracking.

The same test **fails with the old handler** (code 99) and **passes with the
fix**. The simulated callback fails at once on the forbidden path, so the test
does not hang in the deadlock. The step is part of the Windows CI.

```powershell
python -m unittest discover -s port/tests -p test_windows_fault_recovery.py -v
```

It needs an MSYS2 CLANG64 compiler on the PATH or in `TLG_TEST_CLANG`.
The 12 interface tests passed too. The full engine was rebuilt with
Clang/MSYS2 and its DLLs were packaged from the import tree.
The original install and the user's saves were left untouched.

## Validation in a run

In the local session `run-20261017-203640.log`, the rebuilt build ran more than
three minutes on the title screen and menu, with recent statistics at 30 FPS and
no `FREEZE` report. The menu responded to input and opened New Game. The test
kept the 8 DrawPrep workers and the precise readback; it did not turn off the
optimization or hide the freeze detector. This validates start-up and continued
rendering; the full campaign was not tested.

On 18 October 2026 the user ran the launcher again with the fixed build and
confirmed the game works. This confirmation adds to the local start-up/menu
test; it is not a validation of the full campaign.

Local artifacts: `port/dist/tlg-ui-test-windows-fixed.zip` and the existing test
copy, updated, in `port/dist/tlg-ui-test-windows/`.
