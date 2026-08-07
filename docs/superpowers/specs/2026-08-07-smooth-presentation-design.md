# Smooth presentation (host PTS + slack) — webOS

**Date:** 2026-08-07  
**Status:** Approved — implementing (no version bump until user tests) 
**Version target:** next (post v1.1.11)

## Problem

LG C5 microstutters improved with synthetic PTS grid **off** (wall-clock PTS). Residual judder remains. Artemis “Tight VSync” is MediaCodec drop-threshold policy (display period) — **not portable** 1:1 to NDL/Starfish.

User priority: **smoothness** (accept slight latency), not Artemis-style early drops.

## Goals

- Optional setting that **actually changes** presentation timing on webOS (not a no-op).
- Default **Off** = current v1.1.11 behavior (wall-clock PTS).
- **On** = host presentation timestamps + small slack (~0.75 frame at panel/stream Hz).
- Do **not** reintroduce `MAX_DRIFT=0.5` interval grid (known stutter on C5).
- Honest naming (not “Tight VSync”).

## Non-goals

- Port Artemis Tight VSync / LFR / Choreographer.
- Pre-feed sleep/drop gate (phase 2 if needed).
- Changing host NTSC / Vibepollo multiplier behavior.
- Re-enabling aggressive smooth pacing grid.

## Behavior

| Mode | Env / PTS path |
|------|----------------|
| Off (default) | `SS4S_SMOOTH_PACING=0` — wall-clock PTS (today) |
| On | `SS4S_SMOOTH_PACING=1`, `SS4S_SMOOTH_PACING_HOST_ONLY=1`, presentation offset ≈ `0.75 * period` |

Offset period source (same preference order as former panel pacing):

1. Stream `clientRefreshRateX100` if set  
2. Else stream FPS  
3. Else `SDL_webOSGetRefreshRate` if available  

Formula: `offset_us = (int)(0.75 * 1e6 * 100 / x100)` (clamp e.g. 2000–12000 µs).

SS4S (NDL + SMP): when host-PTS-only is on, apply offset to mapped base PTS before monotonic clamp (`+1 ms` / `+1e6 ns` rules unchanged).

## UI / settings

- Checkbox under Video (webOS only): **Smooth presentation**
- Hint: *Uses host frame timestamps with a small display slack. Slightly higher latency; may reduce microstutter. Default off.*
- INI: `smooth_presentation` (bool, default false)
- Legacy ignored keys unchanged (`smooth_frame_pacing`, etc.)

## Logging

On session start:

- Off: `Smooth pacing OFF (wall-clock PTS)`
- On: `Smooth presentation ON (host PTS + %ld µs slack)`

## Verification

1. C5 same res/FPS/bitrate: Off vs On, subjective judder + input feel.  
2. Confirm logs for mode.  
3. Overlay FD still flat expected; no requirement for new stats in v1.  
4. Audio path unchanged (PCM 5.1 / AF counter).

## Risks

- Slack too large → noticeable lag; start at 0.75 frame, tunable via env later if needed.  
- Host PTS missing (`presentationTimeUs == 0`) → fall back to wall-clock for that frame (existing hostPtsUs &lt; 0 path).

## Out of scope for first ship

- Multi-mode Balanced/Tight dropdown.  
- User-editable offset in UI.
