#pragma once

#include <stdbool.h>

/**
 * webOS Game picture/sound + Instant Game Response (app path; not HDMI ALLM).
 * Only works on webosbrew-rooted TVs via Homebrew Channel exec + luna-send.
 * See docs/superpowers/specs/2026-08-16-webos-game-mode-design.md
 */

bool webos_game_mode_is_rooted(void);

typedef struct webos_game_mode_state webos_game_mode_state_t;

/** Apply Game picture/sound. NULL if nothing applied. Connecting UI is SDR — hdrGame is applied later via on_hdr. */
webos_game_mode_state_t *webos_game_mode_enter(bool hdr);

/** Switch to hdrGame once the host enables HDR. */
void webos_game_mode_on_hdr(webos_game_mode_state_t *state, bool hdr);

/** Restore settings changed by enter; frees state. Safe with NULL. */
void webos_game_mode_restore(webos_game_mode_state_t *state);

/**
 * Lock tv.hw.SoCOutputFrameRate (e.g. 120Hz) so a 120 fps stream is not shown
 * on a 144 Hz SoC clock (5:6 pulldown hitch). Restored with restore().
 * Allocates state if NULL. Returns state (may be unchanged if not rooted).
 */
webos_game_mode_state_t *webos_game_mode_lock_soc_hz(webos_game_mode_state_t *state, int hz);
