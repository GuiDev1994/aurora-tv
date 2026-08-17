# Rooted session boost + Settings nav — Implementation Plan

> **For agentic workers:** Implement task-by-task. Steps use checkbox syntax.

**Goal:** Ship TV gamer settings pack, stream process priority, stream HUD, and fix Settings navigation on embedded launcher UI.

**Architecture:** Extend `webos_game_mode_enter/restore`; new `stream_priority` + `stream_hud`; Experimental toggles; fix `settings.controller.c` embed popup focus/scroll/Back/icons.

**Tech Stack:** C, LVGL, webOS luna/hbchannel, POSIX nice/sched

## Global Constraints

- Root detect: hbchannel service dir (existing).
- Settings apply via hbchannel `exec` + `luna-send` (existing pattern).
- No bitrate caps / NDL delay work in this plan.
- Do not commit unless user asks.

---

### Task 1: Fix Settings navigation (embedded)

**Files:** `src/app/ui/settings/settings.controller.c`

- [ ] Add icons to `embed_add_submenu_row` (use `entries[i].icon` + iconfont)
- [ ] On pane popup open: disable parent `detail` scroll; on close: restore
- [ ] Ensure ESC/Back closes popup then main settings (`on_detail_key` + close_btn/backdrop KEY handlers); remove brittle `param == NULL` early-return in `on_back_request` if it blocks CANCEL
- [ ] `lv_group_set_wrap(detail_group, true)` so Input/Host/Experimental rows are reachable
- [ ] Popup focus stays in `pane_popup_group`; stop parent `scroll_to_view` while popup open

### Task 2: Expand Game Mode pack

**Files:** `game_mode.c`

- [ ] Apply extra keys with alias tries; skip restore entry on failure
- [ ] Update Experimental subtext

### Task 3: Stream priority

**Files:** new `stream_priority.c/h`, `session_worker.c`, `app_settings`, `experimental.pane.c`, CMakeLists

- [ ] enter/leave nice + sched; settings key `high_priority_stream` default true
- [ ] Wire session_worker; UI toggle if rooted

### Task 4: Stream HUD

**Files:** new `stream_hud.c/h`, streaming view/controller, `lv_drv_sdl_key.c` (Blue), settings, frame diag bridge

- [ ] Overlay + 2 Hz refresh; Blue toggle; Experimental toggle default off
- [ ] Sample net/sysfs + session flags + frame diag averages when enabled

### Task 5: Build & deploy smoke

- [ ] WSL build IPK; deploy to TV; smoke Settings Back/popup + Experimental toggles
