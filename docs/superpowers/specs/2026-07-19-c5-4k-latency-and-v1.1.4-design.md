# C5 4K Latency Instrumentation + Aurora v1.1.4 Release

**Date:** 2026-07-19  
**Status:** Approved  
**Version target:** 1.1.4

## Problem

### A — LG C5 4K input/video delay

On **LG OLED C5** only, decoding at **3840×2160** (typically 120 fps HDR) causes video/input delay that **starts OK and grows over time**. Audio stays comparatively timely, so sound appears ahead of picture/controls. At **~3.6K (3584×2016)** the same stream has normal input lag. **G5, G4, and C1** work correctly at 4K 120 HDR.

Prior attempts (NDL render-queue flush + IDR watchdog, feed-time backpressure stats) were reverted as ineffective (`0efdb0ea`). Overlay often showed NDL `Q` near 0 while delay was seconds — backlog is likely **inside Starfish/SMP**, not the NDL render buffer API previously instrumented.

### B — Claude findings (partially outdated, partially actionable)

| Claim | Verdict on pin `ss4s@6c17100` |
|-------|-------------------------------|
| `SS4S_SMOOTH_PACING` is a no-op | **False** on current pin — SMP + NDL webOS5 read it and rewrite PTS |
| `pauseAtDecodeTime` hardcoded `true` | **True** — never wired to settings |
| `bufferingCtrInfo` already at floor | **True** |
| `getVideoRenderQueueLength` / `setTimeToDecode` unused | **True** — present in SDK header, not in C wrapper |

Smooth frame pacing today only affects **PTS stamping**, not `pauseAtDecodeTime`. Those must remain **separate** controls.

## Goals

1. Ship **instrumentation + opt-in experiment** so C5 4K backlog can be measured (`RQ`) and `pauseAtDecodeTime=false` can be tested without conflating it with Smooth PTS.
2. Prefer **stable cadence** over aggressive flush+IDR recovery (soft recovery is Phase B, after device data).
3. Ship **Aurora v1.1.4** integrating community PRs #41–#50, credit contributors in README, CI green, then merge + GitHub Release (English notes) which triggers the existing Release Action.

## Non-goals

- Reintroduce the reverted NDL low-latency flush/IDR watchdog as default.
- Playout clock / multi-frame hold queues (previously worsened hitching).
- Auto-fallback to 3.6K by model (manual preset remains the workaround).
- Changing default Smooth PTS behavior.
- Phase B soft recovery as a hard requirement of the 1.1.4 tag if C5 smoke is not finished — may follow in 1.1.4.x / 1.1.5.

## Architecture

```text
Limelight → session_video Feed
              ↓
         SS4S SMP (C5 HDR path)
              ↓
    Load(pauseAtDecodeTime from setting/env)
              ↓
    Feed(PTS: wall | host+smooth)   ← smooth_frame_pacing unchanged
              ↓
    getVideoRenderQueueLength → overlay RQ
              ↓
    [Phase B] sustained RQ growth → ABR / light throttle (no flush+IDR loop)
```

## Phase A — Instrumentation (in v1.1.4)

1. **Setting** `pause_at_decode_time` (webOS Video pane), default **true** (current Starfish behavior).
2. **Env wiring** in `session_worker.c`: `SS4S_PAUSE_AT_DECODE_TIME=0|1` before player open (same pattern as smooth pacing).
3. **SMP** `MakeLoadPayload`: set `pauseAtDecodeTime` from that env (default true if unset).
4. **Wrapper** `StarfishMediaAPIs_getVideoRenderQueueLength` via weak Itanium symbol (mirror `setHdrInfo` / `getAudioBufferSize`). Confirm mangled name with `nm -D libStarfishMediaAPIs.so` on device before trusting.
5. **SS4S API** `SS4S_PlayerGetVideoRenderQueueLength(player, int *length)` → false if unsupported.
6. **Overlay**: Starfish `RQ` was prototyped then dropped from the compact overlay (symbol unavailable on device → always `-`). Compact stats show a single FPS plus audio layout (Stereo / 5.1) instead.
7. Keep Smooth PTS / host PTS mapping as in v1.1.3.
8. **4K + NTSC:** launch `mode=` stays integer at 4K (avoids black screen on C5); `clientRefreshRateX100` still carries NTSC for encode pacing.

## Phase B — Soft recovery (after C5 RQ validation)

Trigger when render queue (or agreed proxy) stays above threshold for N seconds on ≥~4K streams:

- Prefer **ABR bitrate reduction** and optional light non-ref feed throttle.
- **Do not** loop `Flush` + IDR as the primary recovery (failed previously; conflicts with cadence preference).
- `pauseAtDecodeTime=false` remains an **opt-in experiment**, not the recovery default.

## Release v1.1.4

### Branch

`release/v1.1.4` from current `main`.

### Community PRs (merge/cherry-pick in order 41→50)

| PR | Author | Title |
|----|--------|-------|
| [#41](https://github.com/GuiDev1994/aurora-tv/pull/41) | [@KrisEnigma](https://github.com/KrisEnigma) | fix: inverted wheel scroll direction, and animate instantly instead of per-tick |
| [#42](https://github.com/GuiDev1994/aurora-tv/pull/42) | [@KrisEnigma](https://github.com/KrisEnigma) | feat: add visible scrollbar to settings pane |
| [#43](https://github.com/GuiDev1994/aurora-tv/pull/43) | [@KrisEnigma](https://github.com/KrisEnigma) | fix: server popup list icons use icon font instead of image src |
| [#44](https://github.com/GuiDev1994/aurora-tv/pull/44) | [@KrisEnigma](https://github.com/KrisEnigma) | fix: derive cover art width from row height for 600x800 source aspect |
| [#45](https://github.com/GuiDev1994/aurora-tv/pull/45) | [@KrisEnigma](https://github.com/KrisEnigma) | fix: cover art corner rounding and shadow size |
| [#46](https://github.com/GuiDev1994/aurora-tv/pull/46) | [@KrisEnigma](https://github.com/KrisEnigma) | fix: preserve focused app across apploader list rebuilds |
| [#47](https://github.com/GuiDev1994/aurora-tv/pull/47) | [@KrisEnigma](https://github.com/KrisEnigma) | fix: stop game grid cover art flickering on every poll |
| [#48](https://github.com/GuiDev1994/aurora-tv/pull/48) | [@KrisEnigma](https://github.com/KrisEnigma) | fix: stop settings checkboxes double-toggling on remote OK press |
| [#49](https://github.com/GuiDev1994/aurora-tv/pull/49) | [@KrisEnigma](https://github.com/KrisEnigma) | fix: app icon sizing, background color, and crop source |
| [#50](https://github.com/GuiDev1994/aurora-tv/pull/50) | [@danbrun](https://github.com/danbrun) | Fixed audio channels when streaming 5.1 surround |

### Version / docs

- Bump `MOONLIGHT_VERSION` in `CMakeLists.txt` to `1.1.4`.
- Add **Contributors** section to `README.md` listing KrisEnigma and danbrun with PR links.
- Release notes **in English**, crediting each contributor.

### Gate and publish

1. Push `release/v1.1.4` → wait for **Build (webOS)** Action to pass.
2. Device/UI smoke as practical (see Test plan).
3. Merge release branch to `main` (close/merge PRs #41–#50 with credit preserved).
4. Publish GitHub Release `v1.1.4` with English body → existing [`.github/workflows/release.yml`](../../../.github/workflows/release.yml) builds and attaches IPK + Homebrew manifest.

## Test plan

- [ ] CI webOS build green on `release/v1.1.4`
- [ ] UI smoke for KrisEnigma PRs (wheel, settings scroll/scrollbar, server popup icons, cover art, focus, flicker, checkbox, app icon)
- [ ] Audio smoke: 5.1 surround channel map (danbrun)
- [ ] C5 4K HDR: confirm `RQ` rises as delay grows; compare D with `pauseAtDecodeTime` on vs off (experiment only)
- [ ] Regression: 3.6K and non-C5 4K unchanged with defaults

## Success criteria

- v1.1.4 published with community credits and green CI/Release Action.
- Overlay can show Starfish render queue depth when the symbol exists.
- `pauseAtDecodeTime` is controllable without breaking Smooth PTS defaults.
- Clear path to Phase B once C5 `RQ` data confirms backlog location.
