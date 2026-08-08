# Task 8 Report

## Build fix: PanelPhaseLoosen visibility (SMP)

**Status:** Fixed

### Problem

`smp_player.c` calls `SS4S_PlayerGetPanelPhaseLoosen()` but the build failed with an implicit declaration warning/error.

**Root cause:** SMP includes `ss4s/modapi.h` first (`smp_player.h`), which defines `SS4S_MODAPI_H`. When `player.h` is pulled in later via `stats.h`, the entire public API block guarded by `#ifndef SS4S_MODAPI_H` is skipped — including the PanelPhaseLoosen declarations. NDL avoids this by including `ss4s/player.h` before `ss4s/modapi.h` in `ndl_common.h`.

### Fix

Moved `SS4S_PlayerSetPanelPhaseLoosen` / `SS4S_PlayerGetPanelPhaseLoosen` declarations **outside** the `#ifndef SS4S_MODAPI_H` guard in `third_party/ss4s/include/ss4s/player.h`. The `SS4S_Player` typedef remains available via `video.h` / `audio.h` (included at the top of `player.h`).

### Commits

| Repo | Hash | Message |
|------|------|---------|
| ss4s (`third_party/ss4s`) | `3927705` | `fix(webos-smp): expose PanelPhaseLoosen decls to modules` |
| moonlight-tv (parent) | `b4979dfd` | `chore: ss4s fix PanelPhaseLoosen visibility for SMP modules` |

### Next

Re-run RelWithDebInfo IPK build (`scripts/webos/wsl_build_once.sh`) to confirm SMP compiles cleanly.

## Follow-up: module-safe PanelPhaseLoosen synchronization

**Status:** Implemented and built.

### Root cause

SMP and NDL are loadable modules, not consumers of `libss4s`. Calling
`SS4S_PlayerGetPanelPhaseLoosen()` from either module leaves an unresolved
`libss4s` symbol at module link/load time.

### Fix

- Added the optional `SS4S_PlayerDriver::SetPanelPhaseLoosen` callback.
- `SS4S_PlayerSetPanelPhaseLoosen()` stores the public player flag and forwards
  its value to the active video player context.
- SMP and webOS 5 NDL now keep an atomic `panelPhaseLoosen` flag in their own
  contexts; their PTS logic reads that local flag instead of calling back into
  `libss4s`.

### Verification

The RelWithDebInfo webOS build completed all relevant module targets:
`ss4s-module-smp-webos`, `ss4s-module-smp-webos4`,
`ss4s-module-smp-webos3`, and `ss4s-module-ndl-webos5`.

The final IPK copy into `dist/` was blocked by an existing Windows-side
permission denial; packaging itself completed in `/tmp/aurora-cpack-out`.
