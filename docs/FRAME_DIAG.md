# Aurora frame pacing diagnostic & A/B toggles

Observation-only instrumentation for camera-pan microstutter on webOS (Starfish / NDL).
Does **not** change PTS generation, `pauseAtDecodeTime`, ABR, or UDP buffers.

## WebOS Dev Manager (recommended)

1. Install the IPK (Apps → Install).
2. Open **Terminal** (root SSH via Homebrew Channel).
3. Enable frame diag (flag file — launcher does not pass env vars):

```sh
touch /tmp/aurora_frame_diag.enable
# optional A/B — hide LVGL overlay (restart stream after):
touch /tmp/aurora_hide_overlay.enable
```

4. Launch Aurora from the TV, start a stream, do a smooth camera pan (~30–60 s).
5. Stop the stream (flushes the log).
6. In Dev Manager **Files**, download:

`/tmp/aurora_frame_diag.ndjson`

Or from Terminal on the PC (with device configured):

```sh
ares-pull -d webos /tmp/aurora_frame_diag.ndjson ./
```

7. Disable when done:

```sh
rm -f /tmp/aurora_frame_diag.enable /tmp/aurora_hide_overlay.enable
```

### What to look for in the NDJSON

- Stable `delta_feed_wall_ns` ≈ frame period (e.g. ~8.33 ms @ 120 Hz) and stable `render_queue_length` while the camera still shows microstutter → variance is **inside SMP / compositor** (platform ceiling).
- Spikes in `delta_feed_wall_ns` or RQ / `BUFFERFULL` / `DROPPED_FRAME` lined up with the hitch → still influenceable **before** or **at** Feed.

## Env vars (shell launch only)

| Env / flag file | Effect |
|-----------------|--------|
| `AURORA_FRAME_DIAG=1` or `/tmp/aurora_frame_diag.enable` | Per-feed + SMP event NDJSON |
| `AURORA_FRAME_DIAG_PATH` | Output path (default `/tmp/aurora_frame_diag.ndjson`) |
| `AURORA_HIDE_OVERLAY=1` or `/tmp/aurora_hide_overlay.enable` | Hide streaming UI |

Writes are buffered (~1 s / ~256 KiB) then flushed — not per line.

## A/B — NDL vs SMP

**Settings → Video → Video decoder** → `webOS SMP` vs `webOS NDL`, then reconnect.

## 5.1 PCM channel order

Host keeps SDL/Vorbis order via `surroundParams=642012345`. Client remaps once to
device order `E PD D PE C LFE` (LFE last) in SMP/NDL PCM feed, unity gain. Do not
also send `642014523` — that double-swaps channels (see PR #55).
