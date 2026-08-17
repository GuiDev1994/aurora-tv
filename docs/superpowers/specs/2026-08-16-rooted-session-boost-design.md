# Rooted session boost + Settings nav fix

**Date:** 2026-08-16  
**Status:** Approved — execute  
**Approach:** Expand existing Game Mode enter/restore; add priority + stream HUD; fix embedded Settings navigation.

## Scope

1. **TV gamer pack (C):** extend `game_mode.c` with TruMotion/OLED Motion Pro/AI Picture off, energy saving off, auto volume off, Instant Game Response / Game Optimiser on (aliases + no-op on failure); restore reverse order.
2. **High priority stream:** `stream_priority.c` — nice −10 + SCHED_FIFO prio 10 when rooted + toggle (default ON); restore on session end.
3. **Stream HUD (B):** compact overlay; Blue toggle; NET/GAME/PRIO/RX/fps/DRIFT/SUB/RQ; frame metrics via in-process diag when HUD on.
4. **Settings nav fix:** icons on Input/Host/Experimental rows; isolate popup focus/scroll from parent; Back (ESC) closes popup and main settings.

## Out of scope

eth0 rename in-app, 4K NDL delay root-cause, Flydigi USB passthrough.

## Defaults

| Setting | Default | Rooted-only UI |
|---|---|---|
| game_mode | ON | yes |
| high_priority_stream | ON | yes |
| stream_hud | OFF | no (useful without root) |
