# C5 4K Latency + Aurora v1.1.4 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship Aurora 1.1.4 with community PRs #41–#50, contributor credits, and Phase A Starfish instrumentation (`pauseAtDecodeTime` toggle + render-queue `RQ` overlay) for the C5 4K growing-delay bug.

**Architecture:** Integration branch `release/v1.1.4` merges PRs in order, then wires a separate settings/env flag into SMP `MakeLoadPayload`, ports `getVideoRenderQueueLength` through the weak-symbol wrapper into SS4S + overlay. Publish only after Build (webOS) is green; Release workflow attaches IPK on tag publish.

**Tech Stack:** C/CMake, ss4s SMP/Starfish (submodule fork), LVGL UI, GitHub Actions (`build-webos.yml`, `release.yml`).

## Global Constraints

- Version string: `1.1.4` in `CMakeLists.txt` (`MOONLIGHT_VERSION`).
- Do not change default Smooth PTS behavior (`smooth_frame_pacing` stays independent).
- `pause_at_decode_time` default **true**.
- No NDL flush+IDR watchdog reintroduction.
- No commit of unrelated dirty `third_party/Unity` / `third_party/lvgl` working-tree noise.
- Release notes and PR/merge messages for the release: **English**.
- Credits: [@KrisEnigma](https://github.com/KrisEnigma) (#41–#49), [@danbrun](https://github.com/danbrun) (#50).
- Spec: `docs/superpowers/specs/2026-07-19-c5-4k-latency-and-v1.1.4-design.md`.

---

### Task 1: Create release branch and merge PRs #41–#50

**Files:**
- Modify: git history on `release/v1.1.4` only

**Interfaces:**
- Consumes: open PRs against `main` (#41–#50), all MERGEABLE
- Produces: branch containing all PR commits in order 41→50

- [ ] **Step 1: Ensure clean base and create branch**

```powershell
# From repo root; leave Unity/lvgl dirty paths unstaged
git fetch origin
git checkout main
git pull origin main
git checkout -b release/v1.1.4
```

- [ ] **Step 2: Merge each PR head in order**

```powershell
$cred = (echo "protocol=https`nhost=github.com`n" | git credential fill 2>$null)
$env:GH_TOKEN = ($cred | Where-Object { $_ -match '^password=' }) -replace '^password=',''

foreach ($n in 41..50) {
  gh pr checkout $n --repo GuiDev1994/aurora-tv
  git checkout release/v1.1.4
  $head = gh pr view $n --repo GuiDev1994/aurora-tv --json headRefOid -q .headRefOid
  git merge --no-ff $head -m "Merge PR #$n into release/v1.1.4"
}
```

If a merge conflicts, resolve preserving both intents; prefer newer cover-art/UI changes from higher PR numbers when overlapping.

- [ ] **Step 3: Verify log contains all PR authors**

```powershell
git log main..HEAD --oneline
```

Expected: commits from KrisEnigma PRs and danbrun surround fix present.

- [ ] **Step 4: Commit only if conflict resolutions needed** (otherwise merges already committed)

---

### Task 2: Version bump + Contributors README

**Files:**
- Modify: `CMakeLists.txt` (line with `MOONLIGHT_VERSION`)
- Modify: `README.md` (after Credits or new Contributors section)

- [ ] **Step 1: Bump version**

In `CMakeLists.txt` set:

```cmake
set(MOONLIGHT_VERSION "1.1.4")
```

- [ ] **Step 2: Add Contributors section to README.md**

Insert before `## License`:

```markdown
## Contributors

Thanks to everyone helping improve Aurora:

- [KrisEnigma](https://github.com/KrisEnigma) — UI polish and launcher fixes ([#41](https://github.com/GuiDev1994/aurora-tv/pull/41)–[#49](https://github.com/GuiDev1994/aurora-tv/pull/49))
- [danbrun](https://github.com/danbrun) — 5.1 surround channel mapping ([#50](https://github.com/GuiDev1994/aurora-tv/pull/50))
```

Keep existing `## Credits` for upstream project attribution.

- [ ] **Step 3: Commit**

```powershell
git add CMakeLists.txt README.md
git commit -m "chore: bump version to 1.1.4 and credit contributors"
```

---

### Task 3: Wire `pause_at_decode_time` setting → SMP Load payload

**Files:**
- Modify: `src/app/app_settings.h`
- Modify: `src/app/app_settings.c`
- Modify: `src/app/ui/settings/panes/video.pane.c`
- Modify: `src/app/stream/session_worker.c`
- Modify: `third_party/ss4s/modules/webos/smp/src/smp_player.c` (`MakeLoadPayload`)
- Modify: `tests/app/test_settings.c` (default/load/save if other bools are covered)

**Interfaces:**
- Consumes: existing `setenv` pattern in `session_apply_smooth_pacing_env`
- Produces: env `SS4S_PAUSE_AT_DECODE_TIME` (`1`/`0`); SMP reads it in `MakeLoadPayload`

- [ ] **Step 1: Add field next to `smooth_frame_pacing` in `app_settings.h`**

```c
/**
 * webOS SMP/Starfish: pass pauseAtDecodeTime in Load payload (default true).
 * Independent of smooth_frame_pacing (PTS grid). Set false only for latency experiments.
 */
bool pause_at_decode_time;
```

- [ ] **Step 2: Default true; ini read/write `pause_at_decode_time`** in `app_settings.c` mirroring `smooth_frame_pacing`.

- [ ] **Step 3: Checkbox in `video.pane.c`** (webOS only if smooth pacing is gated the same way):

```c
pref_checkbox(view, locstr("Pause at decode time (Starfish)"), &app_configuration->pause_at_decode_time, false);
```

- [ ] **Step 4: Env in `session_worker.c`**

```c
static void session_apply_starfish_load_env(void) {
#if TARGET_WEBOS
    const app_settings_t *cfg = app_configuration;
    bool pause = cfg == NULL || cfg->pause_at_decode_time;
    setenv("SS4S_PAUSE_AT_DECODE_TIME", pause ? "1" : "0", 1);
#endif
}
```

Call before `SS4S_PlayerOpen` / with smooth pacing apply.

- [ ] **Step 5: SMP `MakeLoadPayload`**

```c
static bool PauseAtDecodeTimeEnvEnabled(void) {
    const char *env = getenv("SS4S_PAUSE_AT_DECODE_TIME");
    if (env == NULL || env[0] == '\0') {
        return true;
    }
    if (env[0] == '0' || strcmp(env, "false") == 0 || strcmp(env, "off") == 0) {
        return false;
    }
    return true;
}

/* in esInfo object: */
jkeyval(J_CSTR_TO_JVAL("pauseAtDecodeTime"), jboolean_create(PauseAtDecodeTimeEnvEnabled())),
```

Log once at load: `SMP: pauseAtDecodeTime=%s`.

- [ ] **Step 6: Extend `tests/app/test_settings.c` for default true + round-trip.**

- [ ] **Step 7: Commit app + note ss4s dirty; ss4s commit on fork in Task 4**

```powershell
git add src/app/app_settings.h src/app/app_settings.c src/app/ui/settings/panes/video.pane.c src/app/stream/session_worker.c tests/app/test_settings.c
git commit -m "feat: expose Starfish pauseAtDecodeTime as a settings toggle"
```

---

### Task 4: Port `getVideoRenderQueueLength` + SS4S API + overlay `RQ`

**Files:**
- Modify: `third_party/ss4s/modules/webos/smp/wrapper/StarfishMediaAPIs_C.h`
- Modify: `third_party/ss4s/modules/webos/smp/wrapper/StarfishMediaAPIs_C.cpp`
- Modify: `third_party/ss4s/modules/webos/smp/src/smp_video.c` (or player get-latency path)
- Modify: `third_party/ss4s/modules/interface/include/ss4s/modapi.h`
- Modify: `third_party/ss4s/include/ss4s.h` (or public header where `SS4S_PlayerGetVideoLatency` is declared)
- Modify: `third_party/ss4s/src/player.c` / `src/video.c` as needed for public API
- Modify: `src/app/stream/video/session_video.c` / `session.h` stats fields
- Modify: `src/app/ui/streaming/streaming.controller.c` overlay formatting

**Interfaces:**
- Produces: `bool SS4S_PlayerGetVideoRenderQueueLength(SS4S_Player *player, int *length);`
- Overlay shows `RQ %d` or `RQ -`

- [ ] **Step 1: Wrapper (weak symbol)** in `StarfishMediaAPIs_C.cpp`:

```cpp
extern "C" bool
_ZN17StarfishMediaAPIs26getVideoRenderQueueLengthERi(StarfishMediaAPIs *api, int *length) __attribute__((weak));

bool StarfishMediaAPIs_getVideoRenderQueueLength(StarfishMediaAPIs_C *api, int *length) {
    if (_ZN17StarfishMediaAPIs26getVideoRenderQueueLengthERi == nullptr || length == nullptr) {
        return false;
    }
    return _ZN17StarfishMediaAPIs26getVideoRenderQueueLengthERi(&api->inner, length);
}
```

Declare in `StarfishMediaAPIs_C.h`.

- [ ] **Step 2: Module optional op** — add `GetVideoRenderQueueLength` to video driver ops (nullable). SMP implements via wrapper; others leave NULL.

- [ ] **Step 3: Public API** mirroring `SS4S_PlayerGetVideoLatency`:

```c
bool SS4S_PlayerGetVideoRenderQueueLength(SS4S_Player *player, int *length);
```

- [ ] **Step 4: Session stats** — sample each frame or with latency sample; store `int video_render_queue = -1` when unavailable.

- [ ] **Step 5: Overlay** — append `RQ %d` or `RQ -` next to decode stats in compact + full panel.

- [ ] **Step 6: Commit ss4s on fork branch, bump submodule pin, commit app**

```powershell
# in third_party/ss4s
git checkout -B aurora-v1.1.4
git add -A
git commit -m "feat(webos): pauseAtDecodeTime env and getVideoRenderQueueLength"
git push -u aurora HEAD

# parent repo
git add third_party/ss4s src/app/stream src/app/ui/streaming
git commit -m "feat: show Starfish render queue depth (RQ) in stats overlay"
```

---

### Task 5: Push branch, wait for Build (webOS), merge + release

**Files:**
- GitHub: branch, PRs, release

- [ ] **Step 1: Push release branch**

```powershell
git push -u origin release/v1.1.4
```

- [ ] **Step 2: Watch CI**

```powershell
gh run watch --repo GuiDev1994/aurora-tv $(gh run list --repo GuiDev1994/aurora-tv --branch release/v1.1.4 --workflow "Build (webOS)" --limit 1 --json databaseId -q .[0].databaseId)
```

Expected: success. If fail, fix and re-push; do not merge/release.

- [ ] **Step 3: Open PR release → main (English body with credits), merge when green**

```powershell
gh pr create --repo GuiDev1994/aurora-tv --base main --head release/v1.1.4 --title "Release v1.1.4" --body "..."
gh pr merge --merge
```

Also merge/close community PRs #41–#50 if still open (or they close as superseded once commits are on main — prefer merging each PR if GitHub still shows them open after cherry-picks, or close with comment pointing at release PR).

- [ ] **Step 4: Publish GitHub Release `v1.1.4` (English)**

```powershell
gh release create v1.1.4 --repo GuiDev1994/aurora-tv --title "Aurora v1.1.4" --notes-file release-notes-v1.1.4.md
```

Notes must credit KrisEnigma and danbrun and list Phase A instrumentation. Publishing triggers `.github/workflows/release.yml`.

- [ ] **Step 5: Confirm Release Action success and artifacts attached**

```powershell
gh run list --repo GuiDev1994/aurora-tv --workflow Release --limit 1
```

---

## Out of scope for this plan (Phase B)

Soft ABR/throttle recovery when `RQ` grows — implement only after C5 device confirms `RQ` tracks the growing delay.
