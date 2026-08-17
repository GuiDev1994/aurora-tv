# Panel-phase Stream Pacing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a webOS Video setting **Stream pacing** (`latency` | `smooth`) where Smooth snaps Starfish/NDL PTS to the TV panel phase (max ~1 frame hold) to reduce camera-pan judder, without re-enabling the failed 0.5-frame synthetic grid.

**Architecture:** App persists `stream_pacing`, maps it to env + panel interval before connect; SS4S SMP/NDL implement a new `panelPhase` path in `NextVideoPts`. FPS dips (Rx/De below target) set a player-level loosen flag so Smooth temporarily falls back to wall-clock. Overlay shows Rx and De FPS for A/B.

**Tech Stack:** C11, LVGL prefs, SS4S webOS SMP/NDL modules, Limelight session worker, RelWithDebInfo webOS IPK (`scripts/webos/wsl_build_once.sh`).

**Spec:** `docs/superpowers/specs/2026-08-07-panel-phase-pacing-design.md`

## Global Constraints

- webOS only for the setting UI; non-webOS ignores `stream_pacing` (always latency / no-op).
- Default / missing INI key: `latency` (v1.1.11 wall-clock).
- Never enable legacy `SS4S_SMOOTH_PACING=1` grid or `HOST_ONLY` for this feature.
- Smooth max hold ≤ 1 frame interval; Starfish monotonicity ≥ ~1 ms.
- FPS-dip loosen: no bitrate cuts; temporary wall-clock only.
- No version bump / GitHub release until C5 A/B accepts Smooth.
- Build IPKs with `CMAKE_BUILD_TYPE=RelWithDebInfo` (match release size/behavior).

## File map

| File | Responsibility |
|---|---|
| `src/app/app_settings.h` | `char stream_pacing[16]` (or fixed `"latency"`/`"smooth"`) |
| `src/app/app_settings.c` | default, INI read/write, ignore legacy keys |
| `src/app/ui/settings/panes/video.pane.c` | Presentation dropdown + desc |
| `src/app/stream/session_worker.c` | Map setting → env + panel interval µs |
| `third_party/ss4s/include/ss4s/player.h` + `src/player.c` | `SS4S_PlayerSet/GetPanelPhaseLoosen` |
| `third_party/ss4s/modules/webos/smp/src/smp_player.h/.c` | panel-phase PTS |
| `third_party/ss4s/modules/webos/ndl/webos5/ndl_common.h` + `ndl_player.c` | same algorithm (ms units) |
| `src/app/stream/video/session_video.c` | detect FPS dip → set loosen |
| `src/app/ui/streaming/streaming.controller.c` | compact `FPS Rx … De …` |

Optional shared helper (recommended to keep SMP/NDL identical):

| File | Responsibility |
|---|---|
| `third_party/ss4s/modules/webos/common/panel_phase_pts.h` | pure functions for snap math |

---

### Task 1: Settings + INI (`stream_pacing`)

**Files:**
- Modify: `src/app/app_settings.h`
- Modify: `src/app/app_settings.c`
- Modify: `src/app/ui/settings/panes/video.pane.c`
- Modify: `src/app/ui/settings/panes/pref_obj.h` (only if dropdown helpers already cover string; reuse `pref_dropdown_string`)

**Interfaces:**
- Produces: `app_configuration->stream_pacing` is either `"latency"` or `"smooth"` after load/save; default `"latency"`.

- [ ] **Step 1: Add field to settings struct**

In `app_settings.h`, after `use_ntsc_refresh`:

```c
    /**
     * webOS stream presentation pacing: "latency" (wall-clock) or "smooth" (panel phase).
     * Default "latency". Persisted as video.stream_pacing.
     */
    char stream_pacing[16];
```

- [ ] **Step 2: Default + write + read**

In `settings_initialize` / defaults path:

```c
strncpy(config->stream_pacing, "latency", sizeof(config->stream_pacing));
config->stream_pacing[sizeof(config->stream_pacing) - 1] = '\0';
```

INI write (video section):

```c
ini_write_string(fp, "stream_pacing", config->stream_pacing);
```

INI read:

```c
} else if (INI_FULL_MATCH("video", "stream_pacing")) {
    if (strcmp(value, "smooth") == 0) {
        strncpy(config->stream_pacing, "smooth", sizeof(config->stream_pacing));
    } else {
        strncpy(config->stream_pacing, "latency", sizeof(config->stream_pacing));
    }
    config->stream_pacing[sizeof(config->stream_pacing) - 1] = '\0';
}
```

Keep ignoring legacy `smooth_presentation` / `pause_at_decode_time` keys if still present.

- [ ] **Step 3: Video pane UI (webOS only)**

Near end of video pane create (after color-range block), add:

```c
#if TARGET_WEBOS
    pref_header(view, locstr("Presentation"));
    static pref_dropdown_string_entry_t pacing_entries[2];
    pacing_entries[0].name = locstr("Low latency");
    pacing_entries[0].value = "latency";
    pacing_entries[0].fallback = false;
    pacing_entries[1].name = locstr("Smooth");
    pacing_entries[1].value = "smooth";
    pacing_entries[1].fallback = false;
    /* Ensure pointer target is app_configuration->stream_pacing; prefer storing
       selected value via existing pref_dropdown_string(char ** or char*) API.
       If pref_dropdown_string only binds char**, keep a heap/static char* alias
       that points at stream_pacing, or use int enum 0/1 with pref_dropdown_int. */
    pref_desc_label(view,
                    locstr("Low latency matches v1.1.11 (wall-clock). "
                           "Smooth aligns frames to the TV refresh phase "
                           "(~½–1 frame latency) to reduce camera-pan judder."),
                    false);
#endif
```

**Implementation note:** Inspect `pref_dropdown_string` signature in `pref_obj.h`. If it needs `char **`, use:

```c
typedef enum { STREAM_PACING_LATENCY = 0, STREAM_PACING_SMOOTH = 1 } stream_pacing_id_t;
```

and store `int stream_pacing` instead of `char[]` (update INI accordingly: write `latency`/`smooth` from int). Prefer **int + pref_dropdown_int** if string binding is awkward—behavior must match spec strings in the INI file.

Example int approach (preferred if simpler):

```c
/* app_settings.h */
int stream_pacing; /* 0 = latency, 1 = smooth */

/* defaults */
config->stream_pacing = 0;

/* write */
ini_write_string(fp, "stream_pacing", config->stream_pacing == 1 ? "smooth" : "latency");

/* read */
config->stream_pacing = (strcmp(value, "smooth") == 0) ? 1 : 0;

/* UI */
pref_dropdown_int_entry_t pacing_entries[2] = {
    { .name = locstr("Low latency"), .value = 0 },
    { .name = locstr("Smooth"), .value = 1 },
};
pref_dropdown_int(view, pacing_entries, 2, &app_configuration->stream_pacing, false);
```

- [ ] **Step 4: Commit**

```bash
git add src/app/app_settings.h src/app/app_settings.c src/app/ui/settings/panes/video.pane.c
git commit -m "feat(settings): add webOS stream pacing latency/smooth preference"
```

---

### Task 2: Session env wiring

**Files:**
- Modify: `src/app/stream/session_worker.c`

**Interfaces:**
- Consumes: `app_configuration->stream_pacing` (0/1 or string from Task 1)
- Produces: env for SS4S before `LiStartConnection`:
  - Always: `SS4S_SMOOTH_PACING=0`, `SS4S_NDL_SMOOTH_PACING=0`, unset host-only/offset/interval/max-drift, `SS4S_PAUSE_AT_DECODE_TIME=1`
  - If smooth: `SS4S_PANEL_PHASE_PACING=1`, `SS4S_PANEL_PHASE_INTERVAL_US=<us>`
  - If latency: `SS4S_PANEL_PHASE_PACING=0` (or unset)

- [ ] **Step 1: Replace `session_apply_smooth_pacing_env` body**

```c
static void session_apply_decoder_env(const session_t *session) {
#if TARGET_WEBOS
    setenv("SS4S_SMOOTH_PACING", "0", 1);
    setenv("SS4S_NDL_SMOOTH_PACING", "0", 1);
    unsetenv("SS4S_SMOOTH_PACING_HOST_ONLY");
    unsetenv("SS4S_PRESENTATION_OFFSET_US");
    unsetenv("SS4S_SMOOTH_PACING_INTERVAL_US");
    unsetenv("SS4S_NDL_PACING_INTERVAL_US");
    unsetenv("SS4S_SMOOTH_PACING_MAX_DRIFT_FRAMES");
    setenv("SS4S_PAUSE_AT_DECODE_TIME", "1", 1);

    const bool smooth = app_configuration != NULL && app_configuration->stream_pacing == 1;
    if (!smooth) {
        setenv("SS4S_PANEL_PHASE_PACING", "0", 1);
        unsetenv("SS4S_PANEL_PHASE_INTERVAL_US");
        commons_log_info("Session", "Stream pacing: low latency (wall-clock PTS)");
        (void) session;
        return;
    }

    int x100 = 0;
    int panel_hz = 0;
    if (SDL_webOSGetRefreshRate(&panel_hz) && panel_hz >= 20 && panel_hz <= 240) {
        x100 = panel_hz * 100;
    } else if (session->config.stream.clientRefreshRateX100 > 0) {
        x100 = session->config.stream.clientRefreshRateX100;
    } else if (session->config.stream.fps > 0) {
        x100 = session->config.stream.fps * 100;
    } else {
        x100 = 6000;
    }
    /* interval_us = 1e8 / x100  (since x100 = Hz * 100) */
    long interval_us = (100000000L + (x100 / 2)) / x100;
    if (interval_us < 1000) interval_us = 1000;
    if (interval_us > 100000) interval_us = 100000;

    char buf[32];
    snprintf(buf, sizeof(buf), "%ld", interval_us);
    setenv("SS4S_PANEL_PHASE_PACING", "1", 1);
    setenv("SS4S_PANEL_PHASE_INTERVAL_US", buf, 1);
    commons_log_info("Session",
                     "Stream pacing: smooth (panel phase interval=%ld µs, x100=%d)",
                     interval_us, x100);
#else
    (void) session;
#endif
}
```

Add `#include <SDL.h>` under `#if TARGET_WEBOS` if not already pulled transitively for `SDL_webOSGetRefreshRate`.

Rename call site from `session_apply_smooth_pacing_env` → `session_apply_decoder_env`.

- [ ] **Step 2: Commit**

```bash
git add src/app/stream/session_worker.c
git commit -m "feat(session): wire stream pacing env for panel-phase smooth mode"
```

---

### Task 3: Pure panel-phase math helper

**Files:**
- Create: `third_party/ss4s/modules/webos/common/panel_phase_pts.h` (header-only inline OK)

**Interfaces:**
- Produces:
  - `uint64_t SS4S_PanelPhaseSnapPts(uint64_t wall, uint64_t anchor, uint64_t interval, uint64_t max_hold, uint64_t last_pts, uint64_t min_step, bool *initialized);`

Semantics (nanoseconds or any consistent unit):

```c
/* Snap wall to the next phase >= wall relative to anchor, then clamp delay to max_hold.
 * Ensure pts >= last_pts + min_step when initialized. */
static inline uint64_t SS4S_PanelPhaseSnapPts(uint64_t wall, uint64_t anchor, uint64_t interval,
                                             uint64_t max_hold, uint64_t last_pts, uint64_t min_step,
                                             bool *initialized) {
    if (interval == 0) {
        return wall;
    }
    if (!*initialized) {
        *initialized = true;
        return wall;
    }
    uint64_t phase = (wall >= anchor) ? ((wall - anchor) % interval) : 0;
    uint64_t wait = (phase == 0) ? 0 : (interval - phase);
    if (wait > max_hold) {
        wait = 0; /* present ASAP — do not backlog */
    }
    uint64_t pts = wall + wait;
    if (pts < last_pts + min_step) {
        pts = last_pts + min_step;
    }
    return pts;
}
```

- [ ] **Step 1: Add header** with the function above (and a short comment that units must match caller: SMP=ns, NDL=ms scaled).

- [ ] **Step 2: Sanity check on paper / small host compile if available**

Cases:
- `wall` on phase → `wait=0` → `pts=wall`
- `wall` mid-phase, `wait ≤ max_hold` → snap forward
- `wait > max_hold` → `pts=wall` (ASAP)
- monotonic clamp when `wall+wait < last+min_step`

- [ ] **Step 3: Commit**

```bash
git add third_party/ss4s/modules/webos/common/panel_phase_pts.h
git commit -m "feat(ss4s): add panel-phase PTS snap helper"
```

(If `common/` is new, ensure CMake for smp/ndl still compiles—header-only needs only `#include` path; add `include_directories` in those modules’ CMake if required.)

---

### Task 4: SS4S SMP panel-phase path

**Files:**
- Modify: `third_party/ss4s/modules/webos/smp/src/smp_player.h`
- Modify: `third_party/ss4s/modules/webos/smp/src/smp_player.c`
- Modify: module CMake if include path needed

**Interfaces:**
- Consumes: env `SS4S_PANEL_PHASE_PACING`, `SS4S_PANEL_PHASE_INTERVAL_US`; helper from Task 3; loosen flag from Task 5 (stub `false` until Task 5 lands—gate with `#ifndef` or always check API if already present)
- Produces: when panel-phase on and not loosen, `StarfishPlayerNextVideoPts` uses snap; legacy grid remains only if someone sets old envs (Aurora will not)

- [ ] **Step 1: Extend context**

```c
    bool panelPhasePacing;
    uint64_t panelPhaseIntervalNs;
    uint64_t panelPhaseAnchorNs;
    bool panelPhaseAnchored;
```

- [ ] **Step 2: Configure from env in `StarfishPlayerConfigureSmoothPacing` (or new configure called from Load/Open)**

```c
ctx->panelPhasePacing = false;
ctx->panelPhaseIntervalNs = 0;
ctx->panelPhaseAnchored = false;
{
    const char *pp = getenv("SS4S_PANEL_PHASE_PACING");
    if (pp && pp[0] == '1') {
        ctx->panelPhasePacing = true;
        const char *ius = getenv("SS4S_PANEL_PHASE_INTERVAL_US");
        long us = ius ? strtol(ius, NULL, 10) : 0;
        if (us > 1000 && us < 100000) {
            ctx->panelPhaseIntervalNs = (uint64_t) us * 1000ull;
        } else if (fpsNum > 0 && fpsDen > 0) {
            ctx->panelPhaseIntervalNs = (uint64_t) (1000000000.0 * fpsDen / fpsNum);
        } else {
            ctx->panelPhaseIntervalNs = 1000000000ull / 60ull;
        }
        StarfishLibContext->Log(SS4S_LogLevelInfo, "SMP",
                                "Panel-phase pacing interval=%.2fms",
                                ctx->panelPhaseIntervalNs / 1000000.0);
    }
}
/* Keep existing smoothPacing env parse, but Aurora forces it off. */
```

- [ ] **Step 3: Update `StarfishPlayerNextVideoPts`**

Order of decisions:

```c
uint64_t wall = StarfishPlayerGetTime() - ctx->openTime; /* or existing MapBasePts wall path */

if (ctx->panelPhasePacing) {
    bool loosen = false;
    if (ctx->player) {
        loosen = SS4S_PlayerGetPanelPhaseLoosen(ctx->player); /* Task 5 */
    }
    if (!loosen) {
        if (!ctx->panelPhaseAnchored) {
            ctx->panelPhaseAnchorNs = wall;
            ctx->panelPhaseAnchored = true;
        }
        uint64_t max_hold = ctx->panelPhaseIntervalNs; /* 1 frame */
        uint64_t min_step = 1000000ull; /* 1 ms */
        uint64_t pts = SS4S_PanelPhaseSnapPts(wall, ctx->panelPhaseAnchorNs,
                                             ctx->panelPhaseIntervalNs, max_hold,
                                             (uint64_t) ctx->smoothLastPts, min_step,
                                             &ctx->smoothPtsInitialized);
        ctx->smoothLastPts = (double) pts;
        return pts;
    }
    /* loosen: fall through to wall-clock */
    return wall;
}

/* existing smoothPacing / hostOnly / grid logic unchanged for non-Aurora use */
```

Include `"../common/panel_phase_pts.h"` or correct relative path.

- [ ] **Step 4: Commit in ss4s submodule**

```bash
cd third_party/ss4s
git add modules/webos/smp/src/smp_player.h modules/webos/smp/src/smp_player.c
git commit -m "feat(webos-smp): panel-phase PTS pacing for Smooth mode"
cd ../..
```

---

### Task 5: Player loosen API + session_video FPS dip

**Files:**
- Modify: `third_party/ss4s/include/ss4s/player.h`
- Modify: `third_party/ss4s/src/player.c` (and `player.h` private struct)
- Modify: `src/app/stream/video/session_video.c`

**Interfaces:**
- Produces:
  - `void SS4S_PlayerSetPanelPhaseLoosen(SS4S_Player *player, bool loosen);`
  - `bool SS4S_PlayerGetPanelPhaseLoosen(const SS4S_Player *player);`
- Consumes: `vdec_summary_stats.receivedFps`, `decodedFps`, target fps / x100

- [ ] **Step 1: Add atomic bool on `SS4S_Player`**

```c
void SS4S_PlayerSetPanelPhaseLoosen(SS4S_Player *player, bool loosen) {
    if (!player) return;
    atomic_store(&player->panelPhaseLoosen, loosen);
}
bool SS4S_PlayerGetPanelPhaseLoosen(const SS4S_Player *player) {
    if (!player) return false;
    return atomic_load(&player->panelPhaseLoosen);
}
```

Init to `false` in `SS4S_PlayerOpen`.

- [ ] **Step 2: In `vdec_stat_submit`, after FPS computed**

```c
#if TARGET_WEBOS
    if (player != NULL && app_configuration != NULL && app_configuration->stream_pacing == 1) {
        float target = (float) vdec_stream_target_fps;
        if (session && session->config.stream.clientRefreshRateX100 > 0) {
            target = session->config.stream.clientRefreshRateX100 / 100.0f;
        }
        const float threshold = target * 0.97f; /* ~3% below */
        static unsigned low_ms;
        static unsigned long loosen_until_ms;
        unsigned long now_ms = now; /* already have ticks */
        bool low = (dst->receivedFps > 1.0f && dst->receivedFps < threshold) ||
                   (dst->decodedFps > 1.0f && dst->decodedFps < threshold);
        if (low) {
            low_ms += (unsigned) delta; /* delta already computed in submit */
        } else {
            low_ms = 0;
        }
        if (low_ms >= 1000u) {
            loosen_until_ms = now_ms + 2500u; /* ~2.5 s */
            low_ms = 0;
            commons_log_info("Session", "phase-pace: loosen (rx=%.1f de=%.1f target=%.1f)",
                             dst->receivedFps, dst->decodedFps, target);
        }
        SS4S_PlayerSetPanelPhaseLoosen(player, now_ms < loosen_until_ms);
    }
#endif
```

Tune constants to spec (~1 s below, ~2–3 s loosen).

- [ ] **Step 3: Commit**

```bash
# ss4s
git -C third_party/ss4s add include/ss4s/player.h src/player.c
git -C third_party/ss4s commit -m "feat: Player panel-phase loosen flag API"
# app
git add src/app/stream/video/session_video.c third_party/ss4s
git commit -m "feat(video): loosen panel-phase pacing on sustained FPS dip"
```

---

### Task 6: NDL webOS5 parity

**Files:**
- Modify: `third_party/ss4s/modules/webos/ndl/webos5/ndl_common.h`
- Modify: `third_party/ss4s/modules/webos/ndl/webos5/ndl_player.c`

**Interfaces:** Same env + helper; NDL PTS is **milliseconds** (scale interval: `interval_ms = interval_us / 1000.0`, min_step `1.0` ms).

- [ ] **Step 1: Mirror SMP fields/configure/`NextVideoPts` panel-phase branch** using ms units and `SS4S_PanelPhaseSnapPts` with uint64 ms ticks (or convert helper inputs to integer ms).

- [ ] **Step 2: Commit ss4s + bump parent submodule pointer**

```bash
git -C third_party/ss4s add modules/webos/ndl/webos5/
git -C third_party/ss4s commit -m "feat(webos-ndl): panel-phase PTS pacing for Smooth mode"
# push ss4s branch if remote required for CI, then
git add third_party/ss4s
git commit -m "chore: point ss4s at panel-phase pacing"
```

---

### Task 7: Overlay Rx / De FPS

**Files:**
- Modify: `src/app/ui/streaming/streaming.controller.c`

**Interfaces:**
- Consumes: `dst->receivedFps`, `dst->decodedFps`

- [ ] **Step 1: Change compact line FPS segment**

From:

```c
"FPS %.1f "
```

To:

```c
"FPS Rx %.1f De %.1f "
```

with `dst->receivedFps`, `dst->decodedFps` (use existing `streaming_render_fps` only if you still show a separate render line in full stats—compact should show raw De for stutter diagnosis).

Full stats panel already has Network vs Render framerate—leave as-is unless labels are unclear.

- [ ] **Step 2: Commit**

```bash
git add src/app/ui/streaming/streaming.controller.c
git commit -m "feat(overlay): show received and decoded FPS in compact stats"
```

---

### Task 8: RelWithDebInfo IPK + C5 A/B

**Files:** none (build/test)

- [ ] **Step 1: Build**

```bash
wsl -d Ubuntu -- bash -lc "export CMAKE_BUILD_TYPE=RelWithDebInfo; sed 's/\r$//' /mnt/c/Projetos/moonlight/lg/moonlight-tv/scripts/webos/wsl_build_once.sh | bash"
```

Expected: `dist/com.aurora.gamestream_*_arm.ipk` ~1.85MB class (not ~2.0MB Release).

- [ ] **Step 2: Device A/B (user)**

Same title, slow pan, 120 and 119.88, HDR/SDR:

1. Stream pacing = Low latency → baseline  
2. Stream pacing = Smooth → pan must feel clearly better; note latency  
3. On strong stutter → compact shows Rx/De drop (117/110); log may show `phase-pace: loosen`

- [ ] **Step 3: Decision**

- Accept → keep feature, then version bump/release only when user asks  
- Reject → default stays latency; disable Smooth path or hide setting

---

## Spec coverage checklist

| Spec item | Task |
|---|---|
| Setting latency/smooth + INI | 1 |
| Default latency, remember choice | 1 (INI persistence) |
| Low latency = wall-clock, no legacy grid | 2 |
| Smooth = panel phase ≤1 frame | 3, 4, 6 |
| Panel Hz / x100 / fps fallback | 2 |
| FPS dip loosen, no bitrate cut | 5 |
| Overlay Rx/De | 7 |
| C5 A/B / no release until accept | 8 |
| NDL + SMP | 4, 6 |

## Placeholder / consistency review

- Env names fixed: `SS4S_PANEL_PHASE_PACING`, `SS4S_PANEL_PHASE_INTERVAL_US`
- Setting field: prefer `int stream_pacing` (0/1) with INI strings `latency`/`smooth`
- Helper units documented per caller (ns vs ms)
- Loosen API names match Tasks 4–5
