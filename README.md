# PS4 Bluetooth Audio (**any** firmware)

Homebrew for a **jailbroken PS4** running GoldHEN. Sends game audio from the console's built-in Bluetooth radio straight to Bluetooth Classic headphones—no dongle or PC in the audio path. Output only (no microphone).

> **Experimental.** Discovers kernel symbols and patch offsets dynamically at runtime instead of using hardcoded firmware tables (verifies unique kernel instruction signatures before patching and aborts cleanly on mismatches). Patches live in RAM only and disappear on reboot. **Tested on firmware v12.50, v13.52, and v14.02.** Tested on the developer's (`alexutzusoft`) console (PS4 Pro, firmware 14.02) and verified working in GTA V and NFS Rivals on JBL Tune 770NC.

## Install

Requirements: jailbroken PS4, **GoldHEN** with **FTP (port 2121)** and **BinLoader (port 9090)** enabled.

### Option A: Home-screen app (PKG)

`BTAudio4-1.11.pkg` installs **BTAudio4** (`BTAD00001`) and bundles the payload.

1. Copy `BTAudio4-1.11.pkg` to `/data/pkg/BTAudio4-1.11.pkg` via FTP (or install from USB).
2. On the PS4, open **GoldHEN → Debug Settings → Package Installer** and install the PKG. If asked whether you want to continue installing, select **Yes**.
3. Ensure **BinLoader (port 9090)** is enabled, then launch **Bluetooth Audio** from the home screen.

On launch, the app checks the running payload via `GET /api/version` and replaces a different release with its bundled payload. The swap briefly stops Bluetooth audio and requires an available BinLoader slot. To update the payload GoldHEN loads at boot, use the AutoRun install below.

### Option B: Payload only

Send the flat payload directly from your PC (do not open a bare connection to port 9090, as BinLoader only accepts one payload per jailbreak):

```powershell
python tools/deploy_variant.py build/BTAudio4-1.11.bin --host <PS4_IP> --connect
```

Optional GoldHEN AutoRun install:

```powershell
python tools/install_bta.py --host <PS4_IP> --payload build/BTAudio4-1.11.bin
python tools/install_bta.py --host <PS4_IP> --disable-autorun   # undo
```

## Usage

Put your headphones in pairing mode, then pair and connect using either:
- **Home-screen app:** `Triangle` scan/disconnect, `D-pad` select, `X` connect/start service, `Square` toggle TV sound, `Circle` close app (service stays running).
- **Web UI:** `http://<PS4_IP>:8097/` from any browser on your local network (no authentication—use on a trusted network only).

**Tips:**
- If a pair connects with no sound, try reconnecting or toggling **Joint Stereo** off per headphone in the web UI.
- If a pair won't appear in scans, remove it in **Settings → Devices → Bluetooth Devices** first, then put it back in pairing mode.
- Runtime state and logs live in `/data/bta_audio/` (check `/data/bta_audio/app.log` if the service fails to start).

## Privacy

To help diagnose Bluetooth, kernel-offset, and audio-stuttering issues across different PS4 firmwares and headphones, the BTAudio4 payload automatically sends runtime diagnostic logs to `http://bta.alexutzu.pro` (overrideable via `/data/bta_audio/diag_url` or `/data/bta_audio/diag_url.txt`).

- **What is sent:** a randomly generated anonymous `client_id` (`/data/bta_audio/client_id`), a per-run `session_id`, BTAudio4 version, PS4 system firmware version, kernel offset/patch diagnostics, selected Bluetooth audio device name/MAC, and connection/stream health counters (plus the connecting IP seen by the server). No PSN account info, game data, or audio is ever captured or sent.
- **How it is used:** Logs are stored privately with no public read endpoints and are used **strictly for debugging and fixing BTAudio4**—nothing else.

## Files

| Path | Description |
|---|---|
| `build/BTAudio4-1.11.pkg` | Built home-screen PKG (bundles `BTA-Audio.bin`) |
| `build/BTAudio4-1.11.bin` | Built flat payload |
| `src/payload/` | Payload source (launcher, A2DP worker, diagnostic reporter, web UI, dynamic kernel resolver, device cache, BT accept patch) |
| `src/native/` | OpenOrbis home-screen app source and asset generator |
| `src/native/art/` | SVG sources for the native logo, home-screen icon, and launch splash |
| `server/` | Plain-HTTP Go diagnostic log ingestion server (private disk storage, no public read endpoints) |
| `tools/` | Build, FTP install, and payload deploy scripts |

## Build from source

Requires `..\toolchain` beside the repo (`toolchain/llvm/bin` and `toolchain/OpenOrbis/PS4Toolchain`), Python 3.10 or newer, and .NET for `PkgTool.Core`:

```powershell
python -m pip install pillow resvg-py==0.5.0
powershell -ExecutionPolicy Bypass -File tools/build_app.ps1      # builds build/BTAudio4-1.11.bin
powershell -ExecutionPolicy Bypass -File tools/build_native.ps1   # builds build/BTAudio4-1.11.pkg
```

## Credits & Licenses

- u/YoxScorpion for testing with QCY H3 headphones and reporting audio latency and DVD TV mute issues.
- u/Party-Chance-4125 for testing multiple Bluetooth audio devices and reporting audio dropouts in games.
- Font assets: Inter (`src/native/art/Inter-OFL.txt`).
- AutoRun entry format: documented in [orbisRPC](https://github.com/SirHumza/orbisRPC/tree/main/installer).
- Requires [GoldHEN](https://github.com/GoldHEN/GoldHEN) (not included).
- The PKG bundles OpenOrbis sample modules (`libc.prx`, `libSceFios2.prx`, `right.sprx`) from [OpenOrbis-PS4-Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain).
- Project code is released under the [MIT License](LICENSE).
