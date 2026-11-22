# BTAudio4 Server

Lightweight plain-HTTP Go backend server for BTAudio4, handling console UID allocation, diagnostic log ingestion, and global announcements. Logs and client records are stored privately on disk with no public read endpoints.

## Layout

```text
server/
├── cmd/server/       # CLI entrypoint, flags, and graceful HTTP server lifecycle
├── internal/
│   ├── analytics/    # Per-minute active user tracking with hourly/daily peak summaries
│   ├── api/          # HTTP handlers, IP rate limiting, and announcement serving
│   └── store/        # Disk-backed session logs, metadata, UID allocation, and sanitization
└── go.mod
```

## Run & Build

Requires Go 1.22+.

```bash
cd server
go run ./cmd/server -addr 127.0.0.1:8080 -data ./data
```

Or build a binary:

```bash
cd server
go build -o btaudio4-server ./cmd/server
./btaudio4-server -addr 127.0.0.1:8080 -data /opt/company/btaudio/data
```

Run tests:

```bash
cd server
go test ./...
```

### Flags & Environment Variables

| Flag | Environment Variable | Default | Description |
|---|---|---|---|
| `-addr` | `BTA_ADDR` / `PORT` | `:8080` | HTTP listen address |
| `-data` | `BTA_DATA_DIR` | `./data` | Data directory for logs, UIDs, and `announcement.json` |
| `-v` | `BTA_VERBOSE=1` | `false` | Log allocated UIDs and incoming log chunks to stdout |

## Endpoints

- `POST /api/uid` — Allocates and persists a unique `CCCC-CCCC` console UID (`<data>/clients/<UID>.json`). Accepts optional `version`, `fw`, and `legacy` query parameters (or `X-BTA-Version`, `X-BTA-FW`, `X-BTA-Legacy` headers). Rate-limited to 120 requests/minute per client IP.
- `POST /api/logs` — Ingests a raw log chunk (`text/plain`, up to 1 MiB) using query parameters or `X-BTA-*` headers (`client`, `session`, `seq`, `offset`, `version`, `fw`, `kind`, `dropped`). Deduplicates retries by byte offset, records gap markers on forward jumps, and persists `<data>/sessions/<client>/<session>.log` alongside `<session>.meta.json`.
- `GET /api/announcement` — Returns the current announcement JSON (`Cache-Control: no-store`), `204 No Content` when no announcement is published, or `503 Service Unavailable` if `announcement.json` is invalid or unreadable.
- `GET /healthz` — Plain-text health check returning `ok`.

## Data Directory

```text
<data>/
├── analytics/YYYY-MM-DD.log           # Per-minute active user counts + hourly/daily peaks (local time)
├── announcement.json                  # Optional global announcement
├── clients/<UID>.json                 # Issued CCCC-CCCC console records
└── sessions/<client>/
    ├── <session>.log                  # Raw diagnostic log stream
    └── <session>.meta.json            # Session counters, firmware/version, and timestamps
```

## Publishing an announcement

Save a UTF-8 JSON file named `announcement.json` (max 16 KiB) inside the `-data` directory. The server reads it on every `GET /api/announcement` request. Write edits to a temporary file next to it and rename into place for atomic updates; delete `announcement.json` to clear it.

```json
{
  "id": "maintenance-2026-11",
  "title": "Scheduled maintenance",
  "body": "The diagnostic server will be offline briefly on Saturday.\n\nBluetooth audio will continue working."
}
```

- `id`: 1–64 ASCII characters (`A-Z`, `a-z`, `0-9`, `-`, `_`). Use a new ID for each announcement so consoles show it again.
- `title`: Non-empty single-line string, max 128 bytes.
- `body`: Non-empty string, max 2,048 bytes (use `\n` for line breaks).

The PS4 payload checks for announcements at startup and every minute; the PS4 app fetches directly from this server if the payload is unavailable.

## Overriding the Server URL on PS4

By default, BTAudio4 connects to `http://bta.alexutzu.pro`. To point a PS4 at a custom or local server, create `/data/bta_audio/diag_url` (or `/data/bta_audio/diag_url.txt`) on the PS4 containing the base URL:

```text
http://192.168.1.50:8080
```
