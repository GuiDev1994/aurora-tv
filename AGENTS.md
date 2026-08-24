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
and caches the buildroot NDK on first run):

```bash
wsl -d Ubuntu -- python3 /mnt/c/.../scripts/webos/run_wsl_build.py RelWithDebInfo
```

Gotchas:

- `gettext` (msgfmt) is a hard configure-time `FATAL_ERROR` for non-webOS builds
  (`cmake/MoonlightI18n.cmake`). webOS builds skip gettext and convert `.po` →
  `cstrings.json` at package time via `scripts/webos/po2json.awk`.
- WSL `/tmp` is wiped between separate `wsl` invocations. CPack often cannot write to
  the `/mnt/c` dist directory and falls back to `/tmp/aurora-cpack-out`, so **build and
  deploy must happen inside a single `wsl` call** or the IPK disappears.
- Changes to streaming behaviour can only be validated on a real device against a real
  host. CI proves compilation, nothing more.

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
New or experimental settings belong in `experimental.pane.c`.

Retired INI keys are collected in a shared "legacy keys ignored" branch in
`app_settings.c` so old config files don't warn — add to it rather than dropping the key.

## 5.1 audio (C5 + eARC) — verified, do not invert

Three conventions exist: GFE native, Moonlight WAVE (`FL FR C LFE RL RR`), and
webOS NDL passthrough `{0,1,4,5,2,3}` (FL FR SL SR FC LFE packed as
`surroundParams=642014523`).

What is live on HEAD (LG C5, webOS 25, eARC Atmos bar, Windows 5.1 speaker test):

| Path | What it does | Result |
|------|----------------|--------|
| `surroundParams=642014523` in `session_worker.c` | Host advertises the mapping `IsOpusPassthroughSupported()` already expects | Required. Nulling it (v1.2.0–1.2.3) forced `opus_fix` re-encode and wrong speakers. |
| PCM Feed `SS4S_WebOS_RemapPcm51ToDevice` | WAVE → NDL 6ch `E, PD, D, PE, C, Sub` | E/C/D/PD/Sub verified on C5. PE is the correct speaker but quieter; digital boost (×2.5 wrap or +3 dB saturate) distorted — leave unity gain. |
| `surround_pcm` (Experimental) | Decode Opus in-process, skip NDL Opus/MAT | Needed on this eARC bar (Opus stub header clips). Default on for webOS. |

Do **not** send Sunshine `surround-params` a second time on top of `642014523`.
Do **not** feed unpermuted CEA PCM (LFE at index 3) — Sub goes silent.
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
