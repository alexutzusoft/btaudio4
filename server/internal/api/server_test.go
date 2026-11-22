package api

import (
	"bytes"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"btaudio4-server/internal/analytics"
	"btaudio4-server/internal/store"
)

func TestChunkIngestionIdempotencyAndRestartRecovery(t *testing.T) {
	dir := t.TempDir()
	st, err := store.New(dir)
	if err != nil {
		t.Fatalf("store.New: %v", err)
	}
	sv := NewServer(st, false)
	ts := httptest.NewServer(sv)
	defer ts.Close()

	chunk1 := "[+0.1s] worker_start: v=1.9 fw=\"14.02\"\n[+0.2s] sender_sched: alive=1\n"
	chunk2 := "[+3.1s] audio_start: mac=001122334455\n"

	post := func(seq, offset int, body string) map[string]any {
		t.Helper()
		url := fmt.Sprintf("%s/api/logs?client=abc123&session=sess01&seq=%d&offset=%d&version=1.9&fw=14.02",
			ts.URL, seq, offset)
		resp, err := http.Post(url, "text/plain; charset=utf-8", strings.NewReader(body))
		if err != nil {
			t.Fatalf("POST: %v", err)
		}
		defer resp.Body.Close()
		if resp.StatusCode != http.StatusOK {
			t.Fatalf("status=%d, want 200", resp.StatusCode)
		}
		var out map[string]any
		if err := json.NewDecoder(resp.Body).Decode(&out); err != nil {
			t.Fatalf("decode: %v", err)
		}
		return out
	}

	r1 := post(1, 0, chunk1)
	if int(r1["received"].(float64)) != len(chunk1) {
		t.Fatalf("received=%v, want %d", r1["received"], len(chunk1))
	}

	// Retry chunk1 (simulating lost ACK): must be deduplicated cleanly.
	r1dup := post(1, 0, chunk1)
	if r1dup["duplicate"] != true {
		t.Fatalf("expected duplicate=true, got %v", r1dup)
	}
	if int(r1dup["received"].(float64)) != len(chunk1) {
		t.Fatalf("duplicate shifted received to %v", r1dup["received"])
	}

	// Partial overlap retry + new tail.
	partial := chunk1[len(chunk1)-10:] + chunk2
	r2 := post(2, len(chunk1)-10, partial)
	wantTotal := len(chunk1) + len(chunk2)
	if int(r2["received"].(float64)) != wantTotal {
		t.Fatalf("received=%v, want %d", r2["received"], wantTotal)
	}

	st.Close()

	// Reopen store from disk and verify state & log contents survive restart.
	st2, err := store.New(dir)
	if err != nil {
		t.Fatalf("store.New reload: %v", err)
	}
	defer st2.Close()

	data, meta, err := st2.ReadLog("abc123", "sess01", 0)
	if err != nil {
		t.Fatalf("ReadLog: %v", err)
	}
	if string(data) != chunk1+chunk2 {
		t.Fatalf("log mismatch:\ngot:  %q\nwant: %q", string(data), chunk1+chunk2)
	}
	if meta.BytesReceived != uint64(wantTotal) || meta.Gaps != 0 || meta.Version != "1.9" || meta.Firmware != "14.02" {
		t.Fatalf("unexpected meta: %+v", meta)
	}
}

func TestGapTrackingAndConcurrentClients(t *testing.T) {
	dir := t.TempDir()
	st, err := store.New(dir)
	if err != nil {
		t.Fatalf("store.New: %v", err)
	}
	defer st.Close()

	sv := NewServer(st, false)
	ts := httptest.NewServer(sv)
	defer ts.Close()

	// Gap test: offset jumps from 0 to 100.
	res, err := st.AppendChunk(store.ChunkInput{
		ClientID:  "c_gap",
		SessionID: "s_gap",
		Seq:       2,
		Offset:    100,
		Data:      []byte("after-gap\n"),
	})
	if err != nil {
		t.Fatalf("AppendChunk gap: %v", err)
	}
	if !res.Gap || res.BytesReceived != 110 {
		t.Fatalf("unexpected gap result: %+v", res)
	}

	// Concurrent chunk uploads across 10 clients.
	var wg sync.WaitGroup
	for c := 0; c < 10; c++ {
		wg.Add(1)
		go func(cid int) {
			defer wg.Done()
			offset := 0
			for seq := 1; seq <= 20; seq++ {
				line := fmt.Sprintf("[+%d.0s] line=%d\n", seq, seq)
				url := fmt.Sprintf("%s/api/logs?client=c%d&session=s1&seq=%d&offset=%d", ts.URL, cid, seq, offset)
				resp, err := http.Post(url, "text/plain", bytes.NewBufferString(line))
				if err != nil {
					t.Errorf("post failed: %v", err)
					return
				}
				resp.Body.Close()
				if resp.StatusCode != 200 {
					t.Errorf("status=%d", resp.StatusCode)
					return
				}
				offset += len(line)
			}
		}(c)
	}
	wg.Wait()

	sessions := st.ListSessions(50)
	if len(sessions) != 11 {
		t.Fatalf("len(sessions)=%d, want 11", len(sessions))
	}

	// Verify read/dashboard endpoints are NOT exposed over HTTP.
	for _, path := range []string{"/", "/api/sessions", "/api/sessions/c0/s1"} {
		resp, err := http.Get(ts.URL + path)
		if err != nil {
			t.Fatalf("GET %s: %v", path, err)
		}
		resp.Body.Close()
		if resp.StatusCode != http.StatusNotFound {
			t.Fatalf("GET %s status=%d, want 404", path, resp.StatusCode)
		}
	}
}

func TestKindAndClientDroppedMetadata(t *testing.T) {
	dir := t.TempDir()
	st, err := store.New(dir)
	if err != nil {
		t.Fatalf("store.New: %v", err)
	}
	sv := NewServer(st, false)
	ts := httptest.NewServer(sv)
	defer ts.Close()

	body := "[+0.1s] start: BinLoader (9090) unreachable\n"
	url := fmt.Sprintf("%s/api/logs?client=c_app&session=app-012345&seq=1&offset=0&version=1.10&kind=app&dropped=4", ts.URL)
	resp, err := http.Post(url, "text/plain; charset=utf-8", strings.NewReader(body))
	if err != nil {
		t.Fatalf("POST: %v", err)
	}
	resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("status=%d, want 200", resp.StatusCode)
	}
	st.Close()

	st2, err := store.New(dir)
	if err != nil {
		t.Fatalf("store.New reload: %v", err)
	}
	defer st2.Close()

	got, meta, err := st2.ReadLog("c_app", "app-012345", 0)
	if err != nil {
		t.Fatalf("ReadLog: %v", err)
	}
	if string(got) != body {
		t.Fatalf("log=%q, want %q", string(got), body)
	}
	if meta.Kind != "app" || meta.ClientDropped != 4 || meta.Version != "1.10" {
		t.Fatalf("unexpected meta: %+v", meta)
	}
}

func TestAllocateUIDAndSessionGrouping(t *testing.T) {
	dir := t.TempDir()
	st, err := store.New(dir)
	if err != nil {
		t.Fatalf("store.New: %v", err)
	}
	sv := NewServer(st, false)
	ts := httptest.NewServer(sv)
	defer ts.Close()

	const nClients = 20
	seen := make(map[string]struct{}, nClients)
	var mu sync.Mutex
	var wg sync.WaitGroup
	for i := 0; i < nClients; i++ {
		wg.Add(1)
		go func(idx int) {
			defer wg.Done()
			url := fmt.Sprintf("%s/api/uid?version=1.12&fw=14.02&legacy=0123456789abcdef", ts.URL)
			resp, err := http.Post(url, "application/json", nil)
			if err != nil {
				t.Errorf("POST /api/uid: %v", err)
				return
			}
			defer resp.Body.Close()
			if resp.StatusCode != http.StatusOK {
				t.Errorf("status=%d, want 200", resp.StatusCode)
				return
			}
			var out struct {
				OK  bool   `json:"ok"`
				UID string `json:"uid"`
			}
			if err := json.NewDecoder(resp.Body).Decode(&out); err != nil {
				t.Errorf("decode: %v", err)
				return
			}
			norm, ok := store.NormalizeUID(out.UID)
			if !ok || norm != out.UID {
				t.Errorf("invalid uid %q", out.UID)
				return
			}
			mu.Lock()
			if _, dup := seen[out.UID]; dup {
				t.Errorf("duplicate uid %q", out.UID)
			}
			seen[out.UID] = struct{}{}
			mu.Unlock()
		}(i)
	}
	wg.Wait()
	if len(seen) != nClients {
		t.Fatalf("allocated %d uids, want %d", len(seen), nClients)
	}

	var sampleUID string
	for uid := range seen {
		sampleUID = uid
		break
	}

	// Upload logs using lowercase UID to verify canonical uppercase grouping.
	logBody := "[+0.1s] uid test line\n"
	logURL := fmt.Sprintf("%s/api/logs?client=%s&session=sessA&seq=1&offset=0&version=1.12",
		ts.URL, strings.ToLower(sampleUID))
	resp, err := http.Post(logURL, "text/plain; charset=utf-8", strings.NewReader(logBody))
	if err != nil {
		t.Fatalf("POST /api/logs: %v", err)
	}
	resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("POST /api/logs status=%d, want 200", resp.StatusCode)
	}

	sessionPath := filepath.Join(dir, "sessions", sampleUID, "sessA.log")
	if _, err := os.Stat(sessionPath); err != nil {
		t.Fatalf("expected session log at %s: %v", sessionPath, err)
	}

	st.Close()

	// Reopen store and verify allocated UIDs and session logs reload cleanly.
	st2, err := store.New(dir)
	if err != nil {
		t.Fatalf("store.New reload: %v", err)
	}
	defer st2.Close()

	rec, ok := st2.Client(sampleUID)
	if !ok || rec.UID != sampleUID || rec.Version != "1.12" || rec.LegacyClient != "0123456789abcdef" {
		t.Fatalf("reloaded client record mismatch: ok=%v rec=%+v", ok, rec)
	}

	got, meta, err := st2.ReadLog(sampleUID, "sessA", 0)
	if err != nil {
		t.Fatalf("ReadLog: %v", err)
	}
	if string(got) != logBody || meta.ClientID != sampleUID {
		t.Fatalf("ReadLog mismatch: got=%q meta=%+v", string(got), meta)
	}
}

func TestIngestRecordsAnalytics(t *testing.T) {
	dir := t.TempDir()
	st, err := store.New(dir)
	if err != nil {
		t.Fatalf("store.New: %v", err)
	}
	defer st.Close()

	loc := time.FixedZone("EET", 2*3600)
	now := time.Date(2026, 11, 22, 15, 30, 10, 0, loc)
	an, err := analytics.NewWithClock(dir, func() time.Time { return now })
	if err != nil {
		t.Fatalf("analytics.NewWithClock: %v", err)
	}
	defer an.Close()

	sv := NewServerWithAnalytics(st, an, false)
	ts := httptest.NewServer(sv)
	defer ts.Close()

	postLog := func(query, xff string) {
		t.Helper()
		req, err := http.NewRequest(http.MethodPost, ts.URL+"/api/logs?"+query, strings.NewReader("line\n"))
		if err != nil {
			t.Fatalf("NewRequest: %v", err)
		}
		if xff != "" {
			req.Header.Set("X-Forwarded-For", xff)
		}
		resp, err := http.DefaultClient.Do(req)
		if err != nil {
			t.Fatalf("Do: %v", err)
		}
		resp.Body.Close()
		if resp.StatusCode != http.StatusOK {
			t.Fatalf("status=%d, want 200", resp.StatusCode)
		}
	}

	// Two requests from the same UID (case-insensitive) count as 1 user, plus 1 missing-UID IP fallback = 2 total.
	postLog("client=abcd-1234&session=s1&seq=1&offset=0", "203.0.113.1")
	postLog("client=ABCD-1234&session=s2&seq=1&offset=0", "203.0.113.2")
	postLog("session=s3&seq=1&offset=0", "203.0.113.9")

	now = now.Add(time.Minute)
	an.Flush()

	raw, err := os.ReadFile(filepath.Join(dir, "analytics", "2026-11-22.log"))
	if err != nil {
		t.Fatalf("read analytics log: %v", err)
	}
	want := "2026-11-22T15:30:00+02:00 active_users=2\n"
	if string(raw) != want {
		t.Fatalf("analytics log mismatch:\ngot:  %q\nwant: %q", string(raw), want)
	}
}
