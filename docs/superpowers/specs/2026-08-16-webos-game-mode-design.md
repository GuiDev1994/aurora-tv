# Aurora Game Mode (ALLM stand-in) for rooted webOS

**Date:** 2026-08-16  
**Status:** Approved (user: default ON; deploy via SSH)  
**Reference:** [punktfunk-webos `platform/webos/game_mode.rs`](https://github.com/dyptan-io/punktfunk-webos)

## Goal

On webosbrew-rooted TVs, optionally switch picture/sound into Game mode for the stream (app-plane stand-in for HDMI ALLM), and restore previous settings on exit.

## Behaviour

- **Root detect:** directory `/media/developer/apps/usr/palm/services/org.webosbrew.hbchannel.service` exists (same proxy as punktfunk).
- **UI:** Experimental → **Game mode** checkbox **only if rooted**. Subtext: rooted / ALLM stand-in.
- **Default:** ON (`game_mode=true`).
- **Apply** at stream start (after launch OK, before/with LiStartConnection): via Homebrew Channel `exec` running privileged `luna-send` to `com.webos.settingsservice`:
  - `picture.pictureMode` → `game` or `hdrGame` (HDR if settings HDR + decoder HDR)
  - `sound.soundMode` → `game`
  - if HDR: `picture.peakBrightness` → `high`
- **Restore** on session cleanup (always attempt for applied keys).
- Non-rooted / failed calls: log and no-op; stream continues.

## Files

- `src/app/platform/webos/game_mode.{c,h}`
- `app_settings` + Experimental pane + `session_worker.c`
