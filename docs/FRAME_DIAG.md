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

## Frame pacing — what has been ruled out

Pan microstutter on the C5 (NDL) survived every delivery-timing experiment:

| Attempt | Result |
|---------|--------|
| PTS grid snapping (`SS4S_SMOOTH_PACING`) | no change |
| Host PTS mapping (`SS4S_SMOOTH_PACING_HOST_ONLY`) | no change |
| Presentation offset (`SS4S_PRESENTATION_OFFSET_US`) | no change |
| Panel-phase clock (`SS4S_PANEL_PHASE_PACING`) | no change |
| V-Sync gate holding the Feed call (removed) | no change |
| Bitrate 270 → below 100 Mbps | no change |
| 120 Hz vs 119.94 Hz | no change |

What they have in common: all of them changed *when Aurora submits*, while the PTS
handed to `NDL_DirectVideoPlay` stayed at wall-clock "now". A frame whose PTS is
already due is shown at the next opportunity, so NDL's render buffer never fills, and
a renderer holding no frames cannot align anything to a vsync.

## Frame pacing — render buffer (current)

**Settings → Experimental → Frame pacing** places the PTS N frames in the future,
advancing on the host capture clock, so NDL queues frames and releases them on its own
vsync. `SS4S_RENDER_QUEUE_TARGET` (1–8) is the env behind it. A slow integrator on the
observed queue depth absorbs the host/TV clock difference.

The module logs this every 3600 frames and on unload:

```
Render pacing: N frames, queue avg 2.94 max 4 (target 3), 12 starved (0.4%), 1 resyncs, trim 0.4ms
```

This line is the experiment's verdict:

- **queue avg near target, starved near 0%** — the renderer is holding frames and doing
  its own vsync. If pans are still juddery with this, presentation is not the problem.
- **queue avg ~0, starved ~100%** — NDL ignores a future PTS and always presents on
  arrival. In that case pacing is impossible through NDL and the only remaining path is
  the `smp-webos` module, which drives Starfish directly.
- **rising resyncs** — host and TV clocks disagree more than the trim can absorb, or the
  source frame rate is not the negotiated one.

Cost is exactly the target depth in latency: 3 frames at 120 fps is 25 ms.

## 5.1 audio on webOS

The host is left on its own channel order (`surroundParams` is not sent). NDL's Opus
decoder only accepts mapping `[0,1,4,5,2,3]`; anything else makes the module decode
and re-encode every 5 ms packet. Check which path is live:

- `Opus 5.1 passthrough (no re-encode)` — ideal.
- `Channel config is not supported, enabling re-encoding` — transcode active.

The transcode runs at complexity 0 off the RTP receive thread. If 5.1 still drops,
**Settings → Audio → Decode 5.1 in the client (PCM)** removes it entirely, at the
cost of relying on the client remap to device order `E PD D PE C LFE`. Do not send
`surroundParams=642014523` on top of that — it double-swaps channels (see PR #55).
