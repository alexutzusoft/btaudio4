# PS4 Bluetooth Audio (**any** firmware)

Homebrew for a **jailbroken PS4** running GoldHEN. Sends game audio from the console's built-in Bluetooth radio straight to Bluetooth Classic headphones—no dongle or PC in the audio path. Output only (no microphone).

> **Stable for daily use** (bugs and rough edges may still exist). Discovers kernel symbols and patch offsets dynamically at runtime instead of using hardcoded firmware tables (verifies unique kernel instruction signatures before patching and aborts cleanly on mismatches). Patches live in RAM only and disappear on reboot. **Tested on firmware v14.02, v14.00, v13.52, v12.50, v12.00, v11.00, and v9.00.** Tested on the developer's (`alexutzusoft`) console (PS4 Pro, firmware 14.02) and verified working in GTA V and NFS Rivals on JBL Tune 770NC.

## Install

Requirements: jailbroken PS4, **GoldHEN** with **FTP (port 2121)** and **BinLoader (port 9090)** enabled.

### Option A: Home-screen app (PKG)

`BTAudio4-2.0.0.pkg` installs **BTAudio4** (`BTAD00001`) and bundles the payload.

1. Copy `BTAudio4-2.0.0.pkg` to `/data/pkg/BTAudio4-2.0.0.pkg` via FTP (or install from USB).
2. On the PS4, open **GoldHEN → Debug Settings → Package Installer** and install the PKG. If asked whether you want to continue installing, select **Yes**.
3. Ensure **BinLoader (port 9090)** is enabled, then launch **Bluetooth Audio** from the home screen.

On launch, the app checks the running payload via `GET /api/version` and replaces a different release with its bundled payload. The swap briefly stops Bluetooth audio and requires an available BinLoader slot. To update the payload GoldHEN loads at boot, use the AutoRun install below.

The payload checks GitHub for release metadata at startup and every minute, and sends a PS4 notification when it detects a newer release. The app reads the metadata from the payload and checks GitHub itself only if the payload is unreachable. Press **R2** in the app when a package is available, then install it through GoldHEN Package Installer.

### Option B: Payload only

Send the flat payload directly from your PC (do not open a bare connection to port 9090, as BinLoader only accepts one payload per jailbreak):

```powershell
python tools/deploy/payload.py build/BTAudio4-2.0.0.bin --host <PS4_IP> --connect
```

Optional GoldHEN AutoRun install:

```powershell
python tools/deploy/autorun.py --host <PS4_IP> --payload build/BTAudio4-2.0.0.bin
python tools/deploy/autorun.py --host <PS4_IP> --disable-autorun   # undo
```

## Usage

Put your headphones in pairing mode, then pair and connect using either:
- **Home-screen app:** `Triangle` scan/disconnect, `D-pad` select, `X` connect/start service, `Square` toggle TV sound, `L2` unload/start payload, `Circle` close app (service stays running).
- **Web UI:** `http://<PS4_IP>:8097/` from any browser on your local network (no authentication—use on a trusted network only).

**Tips:**
- If a pair connects with no sound, try reconnecting or toggling **Joint Stereo** off per headphone in the web UI.
- If a pair won't appear in scans, remove it in **Settings → Devices → Bluetooth Devices** first, then put it back in pairing mode.
- Runtime state and logs live in `/data/bta_audio/` (check `/data/bta_audio/app.log` if the service fails to start).

## Reporting issues

If you run into a problem, open a ticket on [GitHub Issues](https://github.com/alexutzusoft/btaudio4/issues). Simply describe your issue and include your user ID (**UID**) from the top right of BTAudio4 so the developer (`alexutzusoft`) can fix it.

## Test update responses from a PC

Create an empty file at `/data/bta_audio/enable_update_test` on the PS4 over FTP. The payload accepts test requests from the LAN while that file exists. Replace `192.168.1.50` with your PS4's IP address.

Save a GitHub-shaped response as `release.json`. This example simulates a release without a package.

```json
{"tag_name":"v99.0.0","assets":[]}
```

```powershell
$url = 'http://192.168.1.50:8097/api/test/update'
curl.exe -H 'X-BTA-Control: 1' $url
curl.exe -H 'X-BTA-Control: 1' -H 'Content-Type: application/json' --data-binary '@release.json' $url
```

To test **R2**, include a package asset with `name`, `size` in bytes, and a real BTAudio4 GitHub asset `url` or `browser_download_url`. The app uses its normal download flow and downloads that asset. Use a distinct version tag for each notification test because the payload shows each version only once per run.

Set `X-BTA-Upstream-Status` on a POST with an empty body to simulate `404` (no release), `403` or `429` (API refusal or rate limit), any `5xx`, or `timeout` (five seconds).

```powershell
curl.exe -X POST -H 'X-BTA-Control: 1' -H 'X-BTA-Upstream-Status: timeout' $url
curl.exe -X POST -H 'X-BTA-Control: 1' "$url/reset"
```

Changes take effect on the next scheduled release check. A fixture persists across restarts until you reset it. GET reports the configured upstream status, the last check result, and the time until the next check. Read `/api/update` to see the metadata the app receives. Delete the enable file to disable the test endpoint and return to GitHub on the next check.

## Announcements

Server announcements appear in the PS4 app and the web UI. The payload also shows a PS4 system notification once per undismissed announcement ID per payload run when the PS4 app is in the background or closed. Close unlocks after three seconds. In the PS4 app, scroll longer messages with D-pad Up and Down and press **X** to close. A dismissal is shared by both interfaces on that console and persists across restarts. Each new announcement needs a new ID. See [server/README.md](server/README.md#publishing-an-announcement) for publishing and clearing announcements.

### Test announcements from a PC

Create an empty file at `/data/bta_audio/enable_announcement_test` on the PS4 over FTP. Save a UTF-8 `announcement.json` file with a unique test ID. The title and body are plain text, and the body can contain line breaks.

```json
{"id":"preview-1","title":"Service announcement","body":"This is a preview.\n\nAdd more lines to test scrolling."}
```

```powershell
$url = 'http://192.168.1.50:8097/api/test/announcement'
curl.exe -H 'X-BTA-Control: 1' -H 'Content-Type: application/json' --data-binary '@announcement.json' $url
curl.exe -H 'X-BTA-Control: 1' $url
curl.exe -X POST -H 'X-BTA-Control: 1' "$url/reset"
```

Replace `192.168.1.50` with your PS4's IP address. The payload checks at startup and every minute. GET reports the configured status, the last check result, and the time until the next check. For error previews, POST an empty body with `X-BTA-Upstream-Status` set to `404` for no announcement, `403`, `429`, a `5xx` code, or `timeout`. Reset or delete the enable file to return to the live server on the next check. Use a new test ID to preview a dismissed message again.

## Privacy

To help diagnose Bluetooth, kernel-offset, and audio-stuttering issues across different PS4 firmwares and headphones, the BTAudio4 payload automatically sends runtime diagnostic logs to `http://bta.alexutzu.pro` (overrideable via `/data/bta_audio/diag_url` or `/data/bta_audio/diag_url.txt`).

- **What is sent:** a randomly generated anonymous `client_id` (`/data/bta_audio/client_id`), a per-run `session_id`, BTAudio4 version, PS4 system firmware version, kernel offset/patch diagnostics, selected Bluetooth audio device name/MAC, and connection/stream health counters (plus the connecting IP seen by the server). No PSN account info, game data, or audio is ever captured or sent.
- **How it is used:** Logs are stored privately with no public read endpoints and are used **strictly for debugging and fixing BTAudio4**—nothing else.

## Files

| Path | Description |
|---|---|
| `build/BTAudio4-2.0.0.pkg` | Built home-screen PKG (bundles `BTA-Audio.bin`) |
| `build/BTAudio4-2.0.0.bin` | Built flat payload |
| `src/app/` | OpenOrbis home-screen app source and PC UI harness |
| `src/assets/` | Asset generator (`generate.py`) and font/SVG sources (`art/`) |
| `src/common/` | Shared C headers (protocol, version, UID, announcements) |
| `src/payload/` | Payload source (launcher, A2DP worker, diagnostic reporter, web UI, and `kernel/` helpers) |
| `server/` | Plain-HTTP Go backend server (console UIDs, diagnostic log ingestion, and announcements; private disk storage, no public read endpoints) |
| `tools/build/` | Build scripts for the payload `.bin` and home-screen `.pkg` |
| `tools/deploy/` | FTP install, GoldHEN AutoRun, and RAM payload deploy scripts |

## Build from source

Requires Python 3.10 or newer (`pillow` and `resvg-py==0.5.0`), LLVM (`clang` / `ld.lld`), and the OpenOrbis PS4 toolchain (on Windows: `..\toolchain\llvm\bin` and `..\toolchain\OpenOrbis\PS4Toolchain` plus .NET for `PkgTool.Core`; on Linux: system `clang`/`ld.lld` or `../toolchain`, plus `OO_PS4_TOOLCHAIN` or `/opt/openorbis/OpenOrbis/PS4Toolchain` with OpenSSL 1.1 for `PkgTool.Core`):

```powershell
# Windows
python -m pip install pillow resvg-py==0.5.0
powershell -ExecutionPolicy Bypass -File tools/build/payload.ps1      # builds build/BTAudio4-2.0.0.bin
powershell -ExecutionPolicy Bypass -File tools/build/pkg.ps1          # builds build/BTAudio4-2.0.0.pkg
```

```bash
# Linux
python3 -m pip install pillow resvg-py==0.5.0
./tools/build/payload.sh                                              # builds build/BTAudio4-2.0.0.bin
./tools/build/pkg.sh                                                  # builds build/BTAudio4-2.0.0.pkg
```

## Credits & Licenses

- Built independently on the foundation of [XB-01/ps4-bt-audio](https://github.com/XB-01/ps4-bt-audio) by [XB-01](https://github.com/XB-01) (not a fork).
- u/YoxScorpion for testing with QCY H3 headphones and reporting audio latency and DVD TV mute issues.
- u/Party-Chance-4125 for testing multiple Bluetooth audio devices and reporting audio dropouts in games.
- Font assets: Inter (`src/assets/art/Inter-OFL.txt`).
- AutoRun entry format: documented in [orbisRPC](https://github.com/SirHumza/orbisRPC/tree/main/installer).
- Requires [GoldHEN](https://github.com/GoldHEN/GoldHEN) (not included).
- The PKG bundles OpenOrbis sample modules (`libc.prx`, `libSceFios2.prx`, `right.sprx`) from [OpenOrbis-PS4-Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain).
- Project code is released under the [MIT License](LICENSE).
