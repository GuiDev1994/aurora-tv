# Smooth presentation Implementation Plan

> **For agentic workers:** Implement task-by-task. No version bump / release until user tests.

**Goal:** Optional webOS “Smooth presentation” (host PTS + ~0.75 frame slack); default off = wall-clock.

**Architecture:** Settings flag → `session_apply_smooth_pacing_env` sets `SS4S_SMOOTH_PACING` / `HOST_ONLY` / `SS4S_PRESENTATION_OFFSET_US`. NDL+SMP apply offset in host-PTS-only path.

**Tech Stack:** C, SS4S webOS NDL/SMP, LVGL settings

**Global Constraints:** No `MAX_DRIFT` grid; no version bump; name is not Tight VSync.

---

## Task 1: Settings + UI
- [ ] `smooth_presentation` in app_settings.h/c (default false, ini r/w)
- [ ] Checkbox + hint in video.pane.c (#if TARGET_WEBOS)

## Task 2: Session env
- [ ] `session_apply_smooth_pacing_env` branches on flag; compute offset_us; logs

## Task 3: SS4S offset
- [ ] NDL + SMP: read `SS4S_PRESENTATION_OFFSET_US`, add to host-PTS-only base before monotonic clamp

## Task 4: Build IPK (no version bump)
- [ ] WSL build; copy IPK to dist for user test
