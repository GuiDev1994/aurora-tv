# On-screen log overlay (punktfunk-style)

**Date:** 2026-08-15  
**Status:** Approved  
**Goal:** Allow non-rooted webOS TVs to inspect Aurora logs on-glass while streaming (camera pan / stutter diagnosis).

## Behaviour

- Magic Remote **Yellow** cycles: **Off → Live → Frozen → Off** (same as [punktfunk-webos](https://github.com/dyptan-io/punktfunk-webos)).
- **Live:** last ~80 lines, refresh ~2 Hz.
- **Frozen:** snapshot for stable reading; ring capture paused.
- Overlay: bottom of screen, semi-transparent, visible during fullscreen stream (independent of pause overlay).
- Experimental setting **Show logs**: On = Live, Off = Off (persisted); Yellow still cycles all three states.

## Out of scope

- Send logs to developer, log-level dropdown, TCP remote sink.

## Files

- `src/app/util/log_overlay.{c,h}` — ring + state machine + LVGL panel helpers
- `third_party/commons/util/logging/*` — optional listener after format
- Streaming view/controller — panel + timer
- `experimental.pane.c` + `app_settings` — Show logs toggle
- `lv_drv_sdl_key.c` — Yellow consumes cycle (not disconnect button)
