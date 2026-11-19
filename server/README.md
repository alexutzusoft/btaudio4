# BTAudio4 Diagnostic Server

Lightweight plain-HTTP Go ingestion server for receiving chunked diagnostic logs from the BTAudio4 PS4 payload. Logs are stored privately on disk (`<data>/sessions/<client>/<session>.log`) with no public read/UI endpoints—inspect logs directly over SSH.

## Run

```bash
cd server
go run . -addr 127.0.0.1:8080 -data ./data
```

Or build a binary:

```bash
cd server
go build -o btaudio4-server .
./btaudio4-server -addr 127.0.0.1:8080 -data /opt/company/btaudio/data
```

### Flags / Environment Variables

- `-addr` (`BTA_ADDR` / `PORT`): Listen address (default `:8080`).
- `-data` (`BTA_DATA_DIR`): Log storage directory (default `./data`).
- `-v` (`BTA_VERBOSE=1`): Log every received chunk to stdout.

## Endpoints

- `POST /api/uid` — Allocates and persists a unique `CCCC-CCCC` console UID (`<data>/clients/<uid>.json`) with optional `version`, `fw`, and `legacy` params.
- `POST /api/logs` — Receives raw log chunks (`text/plain`) with query params `client`, `session`, `seq`, `offset`, `version`, `fw`. Deduplicates retries by byte offset and stores sessions under `<data>/sessions/<client>/<session>.log`.
- `GET /healthz` — Health check (`ok`).

## Overriding the Server URL on PS4

By default, BTAudio4 sends logs automatically to `http://bta.alexutzu.pro` (port 80, plain HTTP, no user setup required).

To point a PS4 at a custom/local server, manually create `/data/bta_audio/diag_url` (or `/data/bta_audio/diag_url.txt`) on the PS4 containing the server URL, for example:

```text
http://192.168.1.50:8080
```
