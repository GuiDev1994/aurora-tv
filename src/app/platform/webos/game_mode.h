#pragma once

#include <stdbool.h>

/**
 * webOS Game picture/sound mode (app-plane ALLM stand-in).
 * Only works on webosbrew-rooted TVs via Homebrew Channel exec + luna-send.
 * See docs/superpowers/specs/2026-08-16-webos-game-mode-design.md
 */

bool webos_game_mode_is_rooted(void);

typedef struct webos_game_mode_state webos_game_mode_state_t;

/** Apply Game picture/sound (and HDR peak brightness). NULL / empty if nothing applied. */
webos_game_mode_state_t *webos_game_mode_enter(bool hdr);

/** Restore settings changed by enter; frees state. Safe with NULL. */
void webos_game_mode_restore(webos_game_mode_state_t *state);
