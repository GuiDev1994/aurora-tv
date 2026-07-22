# Aggressive pacing + UI cleanup (3.6K, About, stats)

**Date:** 2026-07-20  
**Status:** Shipped in v1.1.7  
**Pacing choice:** Tight PTS grid (0.5 frame max drift) + panel-refresh interval when near stream fps. P-frame dropping was rejected (HEVC corruption).

## Scope

1. Restore **3.6K (3584×2016)** resolution preset (C5 practical limit).
2. Remove settings: Soft recovery, Pause at decode time, Smooth frame pacing (legacy INI keys ignored).
3. Always-on aggressive frame pacing (NDL webOS5 + SMP): `SS4S_SMOOTH_PACING_MAX_DRIFT_FRAMES=0.5`; interval from panel Hz when within ±2 of stream.
4. Remove controller battery from performance overlay.
5. Move About from settings tabs into launcher **?** help popup.

## Non-goals

- Reintroduce Flush+IDR / SMP force / Starfish A-B-C / destructive P-frame drops.

## Success

3.6K selectable; Video pane without the three dead toggles; less microstutter at 1080p/2K/3.6K; clean stats; About under ?.
