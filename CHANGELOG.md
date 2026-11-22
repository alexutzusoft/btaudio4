# Changelog

All notable changes and commits for **BTAudio4**, ordered reverse-chronologically. Tagged releases (`vX.Y.Z` / `vX.Y`) and untagged commits share the same timeline and are explicitly distinguished.

---

## [v2.0.0] — 2026-11-22
- **ASRC DSP pipeline & audio presets**: Added `src/common/audio_dsp.h` with 4-point Catmull-Rom cubic Hermite adaptive resampling (`bta_asrc_update`/`bta_dsp_process`), perceptual squared volume curve (`bta_target_gain`), TPDF dithering, and overflow crossfades (`bta_dsp_capture_xfade`). Added three live-switchable presets (**Low latency**, **Balanced**, **High stability**) persisted to `/data/bta_audio/preset.bin` via `POST /api/preset/{0..2}`, cycled with the **TouchPad** button in `src/app/main.c`/`ui.h` and selectable in `web.html`.
- **Lock-free sender & kernel BT tuning**: Converted `audio_tick` in `src/payload/worker.c` to lock-free atomics with seqlock stats/second snapshots, kernel-queue-aware 4 ms deadline pacing, hash-throttled `status.json` writes, and a 30s streaming `diag` interval (`DIAG_STREAM_INTERVAL_US` in `src/payload/diag.c`). Added lockless `bt_stream_peek` (`mode == 4` in `src/payload/kernel/bt_devices.c`) and `kresolve.c` discovery for stream ring/rate/pool offsets (`KR_RES_ENC_BP`, `KR_RES_STREAM_POOL`), updated HCI link policy to `0x0001` (disabling Sniff/Hold/Park), exposed `preset`/`bitpool`/`ring_drops`/`kern_q`/`asrc_ppm`/`neg_rate` telemetry, and tuned preset-bounded `tune_bitpool` adaptation.
- **Kernel 48 kHz rate patch**: Added `kr_resolve_rate_site` in `src/payload/kernel/kresolve.c` and an atomic kernel patch in `src/payload/kernel/bt_accept_patch.c` (`jle` → `jmp`, logged via `rate_rc`/`rate_before`/`rate_after`) to keep 48 kHz / `frm=3` active when the DualShock 4 speaker activates.
- **Global announcements**: Added `GET /api/announcement` to the Go server (`server/internal/api/announcement.go`, backed by `announcement.json`), shared parsing/URL helpers (`src/common/announcement.h`), payload polling + background PS4 notifications (`src/payload/update.c`, with bundled notification icon header), a scrollable announcement dialog with a 3-second close lock and extended UTF-8 Inter glyphs in `src/app/ui.h` / `src/assets/generate.py`, and web UI modal + `POST /api/announcement/dismiss` with persistent console-wide dismissal markers.
- **Payload-driven update checks & test endpoints**: Moved periodic GitHub release checking and update PS4 notifications into the payload (`GET /api/update`, suppressing notifications when the app reports foreground focus via `X-BTA-App-Foreground`), updated the PS4 app (with offline GitHub fallback) and web UI to consume `/api/update`, added `/api/test/update` and `/api/test/announcement` LAN test endpoints, and increased the app's bundled payload buffer to 1 MiB.
- **Server analytics & codebase reorganization**: Added per-minute active user and hourly/daily peak analytics (`server/internal/analytics/`, logged to `<data>/analytics/YYYY-MM-DD.log` on `POST /api/logs`); split `server/` into `cmd/server/`, `internal/api/`, `internal/analytics/`, and `internal/store/`; reorganized `src/` and `tools/` into `src/app/`, `src/assets/`, `src/common/`, `src/payload/kernel/`, `tools/build/`, and `tools/deploy/`; added Linux build scripts (`tools/build/payload.sh`, `pkg.sh`, `common.sh`); and updated `README.md` (stable for daily use, tested on `v9.00`–`v14.02`) and `LICENSE`.

## [v1.12.2] — 2026-11-19
*Release commit: `Added animations`*
- Added UI animations and off-screen compositing/blur in `src/native/ui.h`: eased device-selector highlight glide, smooth volume slider/thumb glow, widget and button state crossfades, held-button key/icon scale+blur feedback, and backdrop-blurred modal fade/crossfades.
- Updated `src/native/main.c` to switch `sceVideoOutSetFlipRate` from `1` to `0` (with 16 ms submit retry), smooth audio meter attack/decay, and mask `init_held` buttons so buttons held during launch or resume do not trigger press animations.

## [v1.12.1] — 2026-11-19
*Release commit: `Added L2 keybind to unload and reload payload`*
- Added the `L2` shortcut (`toggle_payload` and `SERVICE_UNLOADING` phase) in `src/native/main.c` and `src/native/ui.h` to flush app logs, stop the running worker via `POST /api/quit`, or start/reload the payload via BinLoader.
- Updated `bta_worker` shutdown in `src/payload/worker.c` to call `drop_link` and close client/listener/BT/HDMI descriptors before `diag_stop`, and cleared `last_launch_try` once the service comes online.

## [v1.12] — 2026-11-19
*Release commit: `Added console UID allocation and expanded diagnostics`*
- Added server-assigned `CCCC-CCCC` console UID allocation (`POST /api/uid` in `server/handlers.go` and `server/store.go` with per-IP rate limiting and `clients/<uid>.json` persistence), shared `src/uid.h` helpers, local offline fallback + legacy 16-hex migration, and header UID display in `src/native/ui.h`.
- Expanded diagnostics across components: added `bta_launch_trace` (`/data/bta_audio/launcher.bin`), `bt_full_diag` kernel struct dumps (`bt_dev_raw`, `bt_conn_hdr`/`tail`, `bt_stream_raw`), `wlog_shm_probe` across `/shm_acap*` rings, `kr_capture_dump` on kernel resolution failures, 512 KiB `worker.log` / `diag` ring limits, and HTTP/phase/SHM-silence/NaN/clipping telemetry.

## [v1.11] — 2026-11-18
*Release commit: `Expanded diagnostic reporting`*
- Forwarded native `app.log` entries to the worker via `POST /api/app-log` when online, or uploaded them directly (`kind=app`) from `src/native/main.c` when offline/BinLoader fails; added `kind` and `dropped` metadata to `server/`.
- Expanded `diag` (`DIAG_CHUNK_MAX` to 16 KiB, urgent flush, dropped-byte markers, error-stage tracking) and kernel/worker diagnostics: `bt_conn_slot` dumps on errors, `fail_sub`/`fail_aux`/`resolved_mask` in `kresolve`, `bt_accept_patch` failure surround-bytes capture, `btstate` transitions, SHM stall checks, and rejected HTTP request logging.

## [v1.10] — 2026-11-17
*Release commit: `Added diagnostic reporting`*
- Added automatic chunked HTTP diagnostic log reporting (`src/payload/diag.c`, `diag.h`) using a 256 KiB ring buffer and background thread targeting `http://bta.alexutzu.pro` (overridable via `/data/bta_audio/diag_url`).
- Added the Go ingestion server (`server/`) with offset-based chunk deduplication, per-client/session disk storage, and tests; fixed `tools/build_native.ps1` `param.sfo` version formatting for two-part versions like `1.10`.

## [v1.9] — 2026-11-17
*Release commit: `Updated UI`*
- Redesigned the native PS4 UI (`src/native/ui.h`, `make_assets.py`, `host_ui.c`) with a monochrome `#09090b` palette, side-by-side Audio output and Devices panels, speaker vs. headphone icon detection via Class of Device, redesigned shortcut bar, and unified PIN/download modal frames.
- Replaced DejaVu Sans (`matplotlib`) with bundled `InterVariable.ttf` and SVG-rendered UI icon masks (removing the `matplotlib` CI dependency); updated `.github/workflows/build.yml` version regex to support `major.minor` tags.

### Commit — 2026-11-16 *(untagged)*
*Commit: `Updated README`*
- Added tester credits (`u/YoxScorpion` and `u/Party-Chance-4125`) to `README.md`.

## [v1.8.1] — 2026-11-16
*Release commit: `Updated logo and launch splash`*
- Switched `src/native/make_assets.py` to render `btaudio4-logo.svg` and `btaudio4-launch-splash.svg` via `resvg-py==0.5.0`, generating and packaging `pic1.png` (launch splash) into the PKG.
- Deferred `sceSystemServiceHideSplashScreen()` in `src/native/main.c` until after the first video flip (`flip == 0`).

## [v1.8.0] — 2026-11-16
*Commit: `Fixed TWS audio stuttering (attempted)`*
- Added best-effort HCI role switch (`PS4 = Master` via `0x80d` / `0x80b` commands) in `bt_devices.c` and dynamic kernel discovery of A2DP stream/bitpool offsets in `kresolve.c`.
- Added `bt_stream_tune` and adaptive SBC bitpool scaling (`tune_bitpool`) in `worker.c` driven by kernel dropped-packet counters (`kdrops`).

## [v1.7.0] — 2026-11-16
*Release commit: `Added AVRCP headphone volume sync`*
- Added AVRCP hardware volume query (`0x80086213`), `SetAbsoluteVolume` (`0x80106212` with retry), and live volume event handling (`type == 11`) in `worker.c`, bypassing software gain scaling when hardware sync is active.
- Exposed `volume_synced` in `/api/status` and `bt_native`, updated the web slider step from 5% to 1%, and displayed hardware vs. software volume state in both native and web UIs.

## [v1.6.3] — 2026-11-16
*Release commit: `Reduced Bluetooth audio stuttering and improved logging`*
- Retained and retried the last refused PCM packet (`pending_pcm`) instead of dropping 4 ms of audio on `rc == 0`, resynced the capture cursor after test tones and overflows, and tuned sender sleep timing from remaining ring backlog.
- Added `rc768`/`rc1`/`rc_other` counts, `maxref_ms`, and peer/HID device counts at `audio_start` to `worker.log`.

## [v1.6.2] — 2026-11-15
*Release commit: `Updated in-app install instructions`*
- Updated the post-download PKG install instructions in `src/native/ui.h` (removed the instruction to delete the old app and added the prompt to select **Yes** if asked to continue installing).

## [v1.6.1] — 2026-11-15
*Release commit: `Fixed payload version mismatch on launch`*
- Added `GET /api/version` in `worker.c` and a version-sync state machine (`SERVICE_CHECK_VERSION`, `SERVICE_STOPPING`, `SERVICE_STARTING`, `SERVICE_MONITOR`) in `src/native/main.c` to automatically stop an older/mismatched running payload via `POST /api/quit` and launch the bundled payload.
- Closed the listener socket after BT/HDMI descriptors on worker exit and cleared PIN modal state while offline.

## [v1.6.0] — 2026-11-15
*Release commit: `Improved Bluetooth audio performance`*
- Refactored `audio_tick` and the dedicated sender thread in `worker.c` to release the mutex across blocking `/dev/bt` `ioctl` submissions, replace 500 µs polling with `scePthreadCondWait`/`Signal`, and use `poll(2)` (`wait_worker`) in the worker loop.
- Updated `.github/workflows/build.yml` to read `BTA_VERSION` from `src/version.h`, verify tag parity, and fail on missing release files.

## [v1.5.2] — 2026-11-15
*Release commit: `Fixed update download`*
- Fixed GitHub release PKG downloads on PS4 `libSceHttp` by preferring the GitHub API asset `url` (sending `Accept` headers only to `api.github.com`), raising `sceHttpSetResponseHeaderMaxSize` to 16 KiB, and expanding redirect URL buffers to 2048 bytes.

## [v1.5.1] — 2026-11-15
*Release commit: `Fixed CE-34878-0 on app close`*
- Fixed `CE-34878-0` on native app exit in `src/native/main.c` by unregistering video buffers, unmapping/releasing direct memory, and exiting via `sceSystemServiceLoadExec("exit", 0)`.

## [v1.5.0] — 2026-11-15
*Release commit: `Implemented updating`*
- Added GitHub release update checking (`src/native/version_check.h`, `main.c`) and an in-app PKG downloader (`R2` trigger, progress/completion/error dialogs saving to `/data/pkg/`), plus a release status banner in `web.html` (updating CSP `connect-src` for `api.github.com`).
- Centralized the version macro in `src/version.h` and updated `build_app.ps1` / `build_native.ps1` to derive filenames and `param.sfo` versions automatically.

## [v1.4.0] — 2026-11-15
*Release commit: `Improved logging`*
- Added rotating buffered worker logging (`/data/bta_audio/worker.log`, max 256 KiB) covering startup, `kresolve` offsets (`koff`, `koff_layout`), BT events/patch results, `bt_prepare_diag`, stream health summaries (`stream_1s`/`10s`/`30s`), and HTTP commands.
- Added relative timestamps and rotation (`app.prev.log`) to `/data/bta_audio/app.log`, logged HTTP/service transitions, and added a 5-second BinLoader retry cooldown in `src/native/main.c`.

### Commit — 2026-11-15 *(untagged)*
*Commit: `Updated README`*
- Updated `README.md` to list PS4 firmwares `v12.50`, `v13.52`, and `v14.02` as tested.

### Commit — 2026-11-15 *(untagged)*
*Commit: `Updated CI trigger and README wording`*
- Scoped `.github/workflows/build.yml` push triggers to `paths: ['src/**']` and changed the `README.md` heading to `PS4 Bluetooth Audio (**any** firmware)`.

## [v1.3.0] — 2026-11-15
*Release commit: `Added dynamic kernel discovery`*
- Replaced hardcoded kernel offsets and firmware version checks with runtime ELF/signature kernel discovery (`src/payload/kresolve.c`, `kresolve.h`) across `bt_accept_patch.c` and `bt_devices.c`, removing the firmware check in `launcher.c`.
- Exposed `list_rc` in `/api/status` and updated `README.md`/CI for firmware-independent builds.

## [v1.2.0] — 2026-11-15
*Release commit: `Added 13.52 firmware version support`*
- Added PS4 firmware `13.520.001` kernel offsets alongside `14.020.001` in `bt_accept_patch.c`, `bt_devices.c`, and `launcher.c`, and updated build/CI/README references.

## [v1.1.1] — 2026-11-14
*Release commit: `Updated GitHub actions`*
- Updated `.github/workflows/build.yml` to create GitHub Releases on pushes to `main`/`master`, `v*` tags, and `workflow_dispatch` (with `v1.1.1` fallback tag).

### Commit — 2026-11-14 *(untagged)*
*Commit: `Updated version`*
- Rebranded app title and build artifacts from `Bluetooth Audio` / `BTA-Audio` to `BTAudio4` (`1.1.1`) across `param.sfo`, build scripts, web UI, worker status, and `README.md`.

### Commit — 2026-11-14 *(untagged)*
*Commit: `Configured PKG as suspendable PS4 mini-app`*
- Configured `param.sfo` with `CATEGORY` = `gde` and `ATTRIBUTE` = `32` so the PKG suspends alongside active games, added controller `armed` reset on pad disconnect/resume in `src/native/main.c`, and bumped version strings from `2.7.5` to `1.1`.

## [v1.0.0] — 2026-11-14
*Release commit: `Fixed actions bug`*
- Fixed the `protocol.h` include path in `src/native/ui.h` (`../app/protocol.h` → `../payload/protocol.h`) so the native app compiles in CI.

### Commit — 2026-11-14 *(untagged)*
*Commit: `Initial commit`*
- Initial BTAudio4 codebase, built on the foundation of [XB-01/ps4-bt-audio](https://github.com/XB-01/ps4-bt-audio) (`2.7.5`, targeting PS4 `14.020.001`) as an independent project rather than a fork: kernel-relocating A2DP payload (`launcher.c`, `worker.c`, `bt_accept_patch.c`, `bt_devices.c`, `web.html`), OpenOrbis home-screen PKG app (`src/native/`), and Windows/GitHub Actions build & deployment scripts (`tools/`, `.github/workflows/build.yml`).
