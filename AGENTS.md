# AGENTS.md

Guidance for AI coding agents working in this repository.

## What this is

Aurora (`com.aurora.gamestream`) is a fork of mariotaku's moonlight-tv: a GameStream
client in C11 whose primary target is LG webOS TVs, with a desktop Linux build kept
for development. CMake build, SDL2 + LVGL v8 UI, gettext i18n. `TARGET_WEBOS` is
auto-detected from the toolchain triple (`arm-webos-linux-gnueabi`) and gates a lot
of behaviour in the root `CMakeLists.txt`.

Upstream's Raspberry Pi and Steam Link CI targets have been removed from this fork —
only webOS and desktop are built.

## Build

Submodules are required, and `nanors` is nested inside `moonlight-common-c`, so always
clone/update with `--recursive`.

Desktop Linux:

```bash
sudo apt-get install libsdl2-dev libsdl2-image-dev libopus-dev libcurl4-openssl-dev uuid-dev \
  libavcodec-dev libavutil-dev libexpat1-dev libmbedtls-dev libfontconfig1-dev gettext
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DTARGET_DESKTOP=ON
cmake --build build
```

webOS, from Windows via WSL (this is the path that is actually used here — it downloads
and caches the buildroot NDK on first run). The distro must be Ubuntu 24.04+; use
`-d Ubuntu-24.04` if your `Ubuntu` distro is older:

```bash
wsl -d Ubuntu -- python3 /mnt/c/.../scripts/webos/run_wsl_build.py RelWithDebInfo
```

Gotchas:

- The NDK (`2026.08-webos`, gcc 16) needs glibc ≥ 2.36 on the build host: Ubuntu 24.04+,
  not 22.04. Every NDK release ships the same archive and directory name, so local SDK
  caches are keyed by version — keep it that way when bumping the tag.
- `gettext` (msgfmt) is a hard configure-time `FATAL_ERROR` for non-webOS builds
  (`cmake/MoonlightI18n.cmake`). webOS builds skip gettext and convert `.po` →
  `cstrings.json` at package time via `scripts/webos/po2json.awk`.
- WSL `/tmp` is wiped between separate `wsl` invocations. CPack often cannot write to
  the `/mnt/c` dist directory and falls back to `/tmp/aurora-cpack-out`, so **build and
  deploy must happen inside a single `wsl` call** or the IPK disappears.
- Changes to streaming behaviour can only be validated on a real device against a real
  host. CI proves compilation, nothing more.
- Aurora needs webOS 5 or newer. webOS 4 and older ship curl only as `libcurl.so.5`; the
  `commons-curl-abi-fix` shim that bridged that made the C5 fail to start and was removed
  in v1.3.1. Don't bring it back. The CI compatibility check runs with `-r '>=5'`.

## Tests

`BUILD_TESTS` defaults to ON for desktop, OFF for webOS. Unity framework; tests link
against `moonlight-lib`.

```bash
cmake --build build && ctest --test-dir build
ctest --test-dir build -R test_settings
xvfb-run ctest -C Debug          # e2e tests create SDL windows
```

## Architecture

Three layers, dependency-ordered:

1. **Protocol / backend** — `core/moonlight-common-c` (Limelight `Li*` API, decode
   units, input) and `core/libgamestream` (GameStream pairing/launch). `src/app/backend/`
   adds `pcmanager` (discovery, pairing, WoL) and `apploader` (game list + covers).
2. **Session** — `src/app/stream/`. `session.c` owns lifecycle/config;
   `video/session_video.c` and `audio/session_audio.c` are the Limelight callbacks that
   feed **SS4S** (`third_party/ss4s`), which picks an output driver at runtime. On a C5/G5
   (webOS 25) the selected module is `ndl-webos5`.
3. **UI** — `src/app/ui/` on LVGL v8 (mariotaku fork, SDL renderer). Fragment-based:
   `launcher/`, `settings/panes/`, `streaming/`.

Cross-cutting mechanics worth knowing before touching anything:

- **Main loop** (`app.c`): `app_process_events` + `lv_task_handler` + `SDL_Delay(1)` at
  ~1 kHz. The 1 ms cadence and `LV_INDEV_DEF_READ_PERIOD 1` are deliberate — this is the
  input-latency floor. Don't "optimize" it upward.
- **Event bus** (`util/bus.h`): cross-thread work reaches the main thread as
  `SDL_USEREVENT`s. Dispatch runs inside `SDL_FilterEvents`, which holds SDL's event-queue
  mutex — long work in a bus callback stalls every thread that pushes events.
- **Timing units**: `DECODE_UNIT.receiveTimeUs`/`enqueueTimeUs`/`presentationTimeUs` are
  microseconds and share an epoch with `LiGetMicroseconds()` only because both resolve to
  `PltGetMicroseconds()`. Re-verify on every moonlight-common-c bump.
- **Direct submit**: video sets `CAPABILITY_DIRECT_SUBMIT`, so `submitDecodeUnit` runs on
  the RTP receive thread. Anything that blocks in the feed path stops the socket from
  being drained. Audio deliberately does *not* set it on webOS, because the NDL Opus 5.1
  transcode is expensive enough to starve reception (issue #66).
- **Game grid** (`launcher/apps.controller.c`): `lv_gridview` cannot change its column count
  in place (`clear_rows()` skips the last row and the focused item), and `appitem_view()`
  sizes tiles only once. So when the "Game grid" setting (`launcher_grid_rows`) changes,
  Settings sends `USER_LAUNCHER_GRID_CHANGED` on close and the apps fragment recreates
  its view.

## Settings — the part that will bite you

Adding a setting means touching five places: the `app_settings_t` field
(`app_settings.h`), the default in `settings_initialize`, `ini_write_*`, the
`INI_NAME_MATCH`/`INI_FULL_MATCH` parse branch, and a `pref_*` widget in a pane. The
global `app_configuration` aliases `&app->settings`.

**The trap**: `src/app/ui/settings/panes/` does not decide what is visible. The
`entries[]` array in `settings.controller.c` does, and it registers only four panes —
Stream (`basic.pane.c`), Input, Host and Experimental. A pane that is compiled but not
in `entries[]` is dead UI; a setting placed there is invisible with no build error.
This fork already lost `video.pane.c` and `audio.pane.c` to exactly that (deleted).
Experimental settings belong in `experimental.pane.c`. Settings users should find right
away go in `basic.pane.c`: the launcher's settings overlay shows it inline as the main
list, and Input/Host/Experimental only as submenu rows below it.

Settings are stored in `aurora.ini` in the config dir (`CONF_NAME_SETTINGS`). Builds before
the rename used `moonlight.ini`; `settings_read()` moves it to `aurora.ini` once, if
`aurora.ini` does not exist yet.

Retired INI keys are collected in a shared "legacy keys ignored" branch in
`app_settings.c` so old config files don't warn — add to it rather than dropping the key.

## Translations (i18n)

Two backends behind `locstr()` (`util/i18n.h`): desktop uses gettext
(`platform/common/i18n_gettext.c`, `.mo` built by `msgfmt`); webOS uses libwebosi18n's
`ResBundle` (`platform/webos/i18n_webos.c`) on `cstrings.json` files that
`scripts/webos/po2json.awk` generates at package time. po2json skips empty `msgstr`, so an
untranslated string just falls back to English.

**A language only ships when three names agree**: the `src/i18n/<code>/` directory, its
entry in `I18N_LOCALES` (root `CMakeLists.txt`), and its `i18n_locales[]` code in
`i18n_common.c`. Upstream moonlight-tv never showed Spanish with "System Language" because
`es-ES` was missing from `I18N_LOCALES` and the entry was `es`. This fork fixed that in
v1.2.9 (`d9ae5edc`). `PackageWebOS.cmake` installs `es-ES` as `resources/es/ES/cstrings.json`.

Locale resolution on webOS (verified against webosose/libwebosi18n
`ResBundle::loadAllTranslations`):

- `resBundle_createWithRootPath(locale, "cstrings.json", root)` appends `/resources` itself
  and merges `<root>/resources/<lang>/` and `<root>/resources/<lang>/<region>/`.
- `app_init_locale` (`app_webos.c`) reads `localeInfo.locales.UI` from
  `com.webos.settingsservice/getSystemSettings`, then falls back to `SDL_GetPreferredLocales`
  and then `$LANG`. Every tag goes through `i18n_entry()`: an exact match wins, otherwise
  any `xx-YY` entry with the same language does, so `es-MX` loads `es-ES`.
- `bundle_is_translated()` rejects a catalog unless `[Localized Language]` or `Cancel` is
  translated. Keep both translated in every `.po`. `[Localized Language]` is the language's
  own name (`Español (España)`), not a literal translation of the words.

**Extraction traps** — xgettext only picks up `locstr("...")` and `translatable("...")` with
a literal argument:

- A literal that reaches `locstr(var)` through a variable (option tables, button arrays for
  `lv_msgbox_create_i18n`) must be wrapped in `translatable()` where it is defined.
  Otherwise it is never extracted and stays English with no build error.
- `streaming_error()` and `app_fatal_error()` translate their format and title internally.
  Pass `translatable("...")` at the call site. `streaming_error(s, code, "")` clears the
  message and must not reach `locstr` (`gettext("")` returns the `.po` header).
- Translated format strings must keep the same conversion specifiers in the same order.
- Debug-only text (log overlay, compact stats tokens like `RTT`/`FD`, soft-keyboard key caps)
  is intentionally left in English.

**Runtime costs**: on webOS `resBundle_getLocString` returns a fresh `strdup` that
`locstr()` never frees. Keep `locstr` out of per-frame paths, and translate once per
session, like `session_audio.c` does for the channel label. The detailed stats panel
already leaks a few bytes per ~1 Hz refresh (`locstr("total")`, `locstr("not available")`).
`locstr` is called from worker threads (connection callbacks, session worker). Both
backends do a read-only lookup there, so that is safe.

Updating catalogs:

```bash
cmake --build build --target i18n-update-pot   # xgettext over src/, needs gettext
msgmerge --update src/i18n/<lang>/messages.po src/i18n/messages.pot
```

The `i18n-merge-po` target only merges `ja`, so don't rely on it. Until 2026-10 the `.pot`
was stale and still carried strings from the deleted video/audio panes. Since then `es-ES`
is complete (209/209). The rest only partly cover the current `.pot` (pt-BR 105, fr 87,
zh-CN 80, others fewer; ko/uk/zh-TW 0) and are not merged against it yet.
Spanish (Spain) conventions: formal *usted*; "transmisión" for streaming; "mando" for
gamepad and "mando a distancia" for the remote; "ratón"; "ordenador"; `«»` quotes; "host"
left as is. Long Spanish labels can overflow TV rows, so check them on the device.

## 5.1 audio (C5 + eARC) — verified, do not invert

Three conventions exist: GFE native, Moonlight WAVE (`FL FR C LFE RL RR`), and
webOS NDL passthrough `{0,1,4,5,2,3}` (FL FR SL SR FC LFE packed as
`surroundParams=642014523`).

What is live on HEAD (same as upstream moonlight-tv; user validated original
Moonlight on their eARC setup — Aurora's client PCM + RemapPcm51 path was wrong):

| Path | What it does | Result |
|------|----------------|--------|
| `surroundParams=642014523` in `session_worker.c` | Host advertises the mapping `IsOpusPassthroughSupported()` already expects | Required. Nulling it (v1.2.0–1.2.3) forced `opus_fix` re-encode and wrong speakers. |
| NDL Opus passthrough | Opus preferred when SS4S advertises it and host mapping matches | Correct 5.1 on user's setup. Do **not** force client PCM or RemapPcm51. |

Experimental → "5.1 audio mode" (`surround_51_mode`, default passthrough) holds the
escape hatches for eARC bars where NDL Opus passthrough cuts out (#80):

- **Re-encode** (forcing `opus_fix` with a matching host mapping) was tried and removed: no
  cuts, but speakers as wrong as PCM — same as upstream 1.28 in mariotaku/moonlight-tv#506.
- **PCM** stops the dropouts but is not discrete on an LG 5.1 bar: with the measured slots
  FL RR FR RL C LFE, RL is silent and RR also plays from the front. FL FR SL SR C LFE swaps
  fronts/rears; WAVE order silences the Sub. Kept only for other bars.

Do not make PCM the default.

**Cut root cause (#80):** C5/G5 logs showed ~45% of Opus packets stamped on the same ms as
the previous one (Limelight delivers them in pairs). `audio_sequential_pts` (default on,
`SS4S_NDL_AUDIO_SEQUENTIAL_PTS`) steps each stamp one frame past the last and sheds packets
more than 4 frames ahead, so the stamp cannot ratchet. Watch `shed` in "Audio feed 1s".

Do **not** send Sunshine `surround-params` a second time on top of `642014523`.
Do not "fix" 5.1 by deleting `surroundParams` again.

## webOS frame pacing — read before attempting

Pan microstutter on the C5 has resisted a long series of attempts. What is already
ruled out, so nobody repeats it:

| Attempt | Result |
|---------|--------|
| PTS grid snapping (`SS4S_SMOOTH_PACING`) | no change |
| Host PTS mapping (`SS4S_SMOOTH_PACING_HOST_ONLY`) | no change |
| Presentation offset (`SS4S_PRESENTATION_OFFSET_US`) | no change |
| Panel-phase clock (`SS4S_PANEL_PHASE_PACING`) | no change |
| Holding the Feed call on a frame grid ("V-Sync gate") | no change, removed |
| Future PTS / render-buffer V-Sync (`render_queue_frames`) | no change on C5 (buffer stays empty; PTS ignored); option removed — made PAN hitch worse |
| Clearing `CAPABILITY_DIRECT_SUBMIT` (decoder thread) | **worse**: decoded FPS 115–118 and real stutter; restore direct submit |
| 4K 120 @ 270 Mbps | **accumulating delay** (USB2 + decode queue). 80 Mbps 4K is stable; 120 Mbps slight drift |
| Bitrate 270 → below 100 Mbps (3.6K pan hitch) | no change |
| 120 Hz vs 119.94 Hz | no change |
| Host-clock cadence pacing (punktfunk-style: host PTS → NDL clock, p98 jitter cushion, +0–2 frames), with primed Opus audio plane | no change: PTS led arrival by up to 40 ms yet render queue stayed 0–2; C5 NDL HEVC presents on arrival. Removed |
| Frame drop threshold 2/3 armed before load (#101, `videoRenderQueueThreshold` reaches the esplayer payload) | **4K120 delay unchanged** (2026-10-26, Auto/2/3). Render queue stays 0–1, so the threshold never fires. Reverted in v1.3.3 (ss4s `623e1c1`) |

The 4K120 delay is the picture only: audio and input stay real time, and a phone decoding the
same host at 4K120 HDR 290 Mbps is fine. Frame diag shows arrival vs host PTS constant and feed
at 120 fps. Delay grows with pixels × bitrate (3712×2088 slower than 4K, 80 Mbps stable, 150+
grows), which points at the C5 HEVC decoder falling behind with input piling up before it. The
esplayer `Dropped Frames: x of N` count follows input, not decoder output, so it reads 120 fps
even then. ss4s `ndl-webos5` advertises `maxBitrate` 95 Mbps on high-end SoCs, and
`session_config_init` deliberately skips that cap on webOS.

Facts that matter and are easy to get wrong:

- `lowDelayMode` is set in `smp_player.c` only. The **NDL module never sets it**, so any
  reasoning that starts "Starfish runs in lowDelayMode" does not apply to the C5 path.
- `session_worker.c` forces `SS4S_SMOOTH_PACING=0` and `SS4S_PANEL_PHASE_PACING=0`, which
  makes `SS4S_NDL_webOS5_NextVideoPts` return plain wall clock and leaves the entire
  smooth-pacing/offset machinery below it unreachable. Check the env before concluding a
  code path was tested.
- The NDL v2 video API is only `Play(buf, size, pts)`, `GetRenderBufferLength`,
  `FlushRenderBuffer` and `SetFrameDropThreshold`. PTS is milliseconds, which cannot
  represent an 8.333 ms period at 120 fps exactly.
- `NDL_DirectVideoGetRenderBufferLength` is the one real observable: if the renderer holds
  no frames it cannot align anything to vsync, and presentation phase just follows arrival.

## AV1 120 Hz (C5 NDL) — verified, do not chase with PTS

NDL feeds 120 AV1 frames/s (overlay FPS is submit rate). UFOTest 120 looks identical
to 60, with stutter; HEVC 120 is clearly sharper. PTS-now, unique-ms, 8 ms future
grid, no frame-drop threshold, and an AV1 feed thread did not change that.

HEVC on the same NDL path ignores PTS and presents on arrival. AV1 honors the
decoder clock and presents at 60 Hz. That is the TV AV1 DirectVideo path, not
bitrate. Do not "fix" it by changing HEVC. For 120 fps, use HEVC.

## Submodule / dependency notes

- `third_party/lvgl` is mariotaku's fork on an LVGL-v8 branch, intentionally divergent
  from upstream v9 — don't try to "update" it.
- `third_party/ss4s` and `third_party/commons` are the actively-maintained deps; `commons`
  provides `sps_parser` (H.264/HEVC only, so AV1 streams report no dimensions in stats).
- webOS video capability caps in ss4s are measured device limits, not guesses (streams
  above the cap crash some TVs) — don't raise them without hardware testing.
- `core/moonlight-common-c` is the GuiDev1994 fork and carries Aurora-only changes, among
  them the Rubylight link hints: `STREAM_CONFIGURATION.clientWireless`/`clientLinkMbps`
  become `x-rl-video[0].clientWireless` and `x-ss-video[0].pyrowaveLinkMbps` in the SDP
  (Sunshine-family hosts only). `stream/link_hint.c` fills them from sysfs. Rebasing the
  fork onto upstream must keep them. PyroWave itself is unusable on webOS (no Vulkan);
  the link hint applies to H.264/HEVC/AV1 pacing too and does not request it.
- Rubylight's bandwidth test lives in libgamestream: serverinfo's `BandwidthProbeVersion`
  gates `gs_probe_bandwidth` (`GET /bandwidth-probe`, 4 MiB, the host stops at 3 s, 409
  while a stream runs, 429 with `Retry-After`). The host menu's "Test connection speed"
  only offers to lower the bitrate to 80% of the link, never to raise it: Rubylight found
  Nightjar's automatic start-bitrate test worse on a shaped link. `app_gs_client_new` sets
  `User-Agent: Aurora/<version> (<platform>)`, which Rubylight lists per device; a request
  without one clears it there, so new `GS_CLIENT`s must keep going through that function.
