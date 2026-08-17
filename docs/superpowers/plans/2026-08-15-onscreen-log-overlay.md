# On-screen log overlay — implementation plan

## Task 1: Commons log listener
- Add `commons_log_set_listener` + call from `logging_pmlog.c` / `logging_stdio.c` after format.

## Task 2: `log_overlay` module
- Ring (~80 lines), Off/Live/Frozen, Yellow cycle, LVGL panel on `lv_layer_top()`, 500ms refresh when Live.

## Task 3: Wire Yellow + settings
- `lv_drv_sdl_key.c`: Yellow cycles overlay (no disconnect bind).
- Experimental “Show logs” + `app_settings.show_logs`.
- Init listener in `app.c`.
