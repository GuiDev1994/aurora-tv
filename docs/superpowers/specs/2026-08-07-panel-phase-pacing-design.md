# Panel-phase stream pacing (Smooth vs Low latency)

**Date:** 2026-08-07  
**Status:** Approved for planning  
**Platform:** webOS (LG C5 primary); SS4S SMP/Starfish + NDL  
**Goal:** Reduce camera-pan judder via optional panel-phase PTS alignment, without reintroducing the failed 0.5-frame synthetic grid.

## Context (what we already learned)

| Experiment | Result on C5 |
|---|---|
| Aggressive smooth pacing (≈0.5-frame PTS grid) | **Worse** microstutters |
| Host PTS + presentation slack (“Smooth presentation”) | **No meaningful difference** |
| Wall-clock PTS (`SS4S_SMOOTH_PACING=0`) | **Best baseline** (v1.1.11); residual judder remains |
| `pauseAtDecodeTime` ON/OFF | Not the cause of residual / “worse” builds |

**Client-side signal:** Host RTSS / in-game FPS can stay at 120 while Aurora overlay FPS drops to ~117 or ~110 during strong stutters → hitch is on the receive→feed→decode/present path, not PC render.

**Non-goals:** Guaranteed zero stutter; Artemis MediaCodec “Tight VSync” drop policy; bitrate soft-recovery during pans; changing NTSC/host `mode=` rules.

## Requirements

1. **Setting (webOS Video → Presentation):** Stream pacing
   - **Low latency** — wall-clock PTS (current v1.1.11 behavior)
   - **Smooth** — panel-phase alignment (this design)
2. **Persistence:** INI key `stream_pacing` = `latency` | `smooth`
3. **Default:** First run / missing key → `latency`. After that, remember last choice.
4. **Primary success metric:** Noticeably less camera-pan judder in Smooth vs Low latency (same title, same refresh).
5. **Latency budget (Smooth):** up to ~½–1 frame of added presentation delay.

## Architecture

```
Settings (stream_pacing)
    → session_worker env
        → SS4S SMP / NDL NextVideoPts
            → Starfish / NDL present
```

### Modes

**Low latency (`latency`)**

- `SS4S_SMOOTH_PACING=0` (and NDL equivalent)
- Unset host-only / presentation-offset / interval / max-drift envs used by the old grid
- `SS4S_PAUSE_AT_DECODE_TIME=1` (unchanged from v1.1.11)
- `NextVideoPts` returns wall-clock base immediately

**Smooth (`smooth`) — panel phase (new)**

Do **not** enable the legacy interval grid (`smoothHostOnly` / 0.5-frame clamp path).

New SS4S path (SMP + NDL), gated by env e.g. `SS4S_PANEL_PHASE_PACING=1`:

1. **Target interval** from, in order:
   - Panel Hz via `SDL_webOSGetRefreshRate` (app passes interval µs or Hz×100 in env), else
   - `clientRefreshRateX100`, else
   - stream fps
2. **Anchor** on first presented frame (wall-clock player timeline).
3. Each subsequent PTS: snap forward to the next panel phase boundary relative to wall-clock, with **max hold ≤ 1 frame interval**.
4. Enforce Starfish monotonicity (existing ~1 ms minimum step).
5. If wall-clock is already past the chosen phase by more than the budget, present ASAP (no multi-frame backlog).

App responsibilities:

- Map setting → env before `LiStartConnection` / player open (same place as today’s pacing env).
- Pass panel refresh into env when available (so SS4S does not depend on SDL).

## FPS dip loosen (Smooth only)

When streaming stats show `receivedFps` or `decodedFps` more than ~3% below target for ≥ ~1 s (e.g. 120 → 117/110):

- **Do not** cut bitrate.
- Temporarily disable panel-phase snap (fall back to wall-clock) for ~2–3 s.
- Rate-limited log: `phase-pace: loosen`.
- Re-enable snap after the window if FPS recovers.

Implementation note: sampling already exists in `vdec_stat_submit` / overlay; loosen can be a lightweight flag/env flip or an in-process SS4S counter updated from the app via existing player userdata patterns—prefer the smallest change that SS4S can read each `NextVideoPts`.

## Overlay

- Keep compact stats useful for A/B.
- Surface **received** and **submitted/decoded** FPS distinctly enough to see 120→117/110 client dips while host RTSS stays flat (exact label layout may stay compact: e.g. `FPS Rx … De …` or dual values).

## Files (expected touch list)

| Area | Files |
|---|---|
| Setting | `app_settings.h/.c`, `video.pane.c` |
| Env apply | `session_worker.c` |
| Overlay | `streaming.controller.c` (and view only if new labels needed) |
| SS4S | `smp_player.c/.h`, `ndl_player.c`, `ndl_common.h` |

No version bump / release until Smooth A/B passes on C5.

## Test plan (C5)

Same game, slow camera pan, 120 and 119.88, HDR and SDR:

1. Low latency — residual judder baseline  
2. Smooth — pan must feel clearly more stable; +½–1 frame latency OK  
3. Strong stutter — overlay Rx/De drop visible; optional `phase-pace: loosen` in log  

**Accept Smooth:** user-visible pan improvement.  
**Reject:** Smooth ≥ baseline or worse (treat like failed grid) → keep default `latency`; leave Smooth experimental or remove.

## Out of scope

- Claiming “VRR” or perfect sync on a fixed-refresh OLED  
- Reintroducing soft-recovery bitrate cuts on FPS dip  
- Desktop / non-webOS pacing UI
