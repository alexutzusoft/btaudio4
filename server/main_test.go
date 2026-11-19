package main

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
)

func TestChunkIngestionIdempotencyAndRestartRecovery(t *testing.T) {
	dir := t.TempDir()
	store, err := NewStore(dir)
	if err != nil {
		t.Fatalf("NewStore: %v", err)
	}
	sv := NewServer(store, false)
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

	store.Close()

	// Reopen store from disk and verify state & log contents survive restart.
	store2, err := NewStore(dir)
	if err != nil {
		t.Fatalf("NewStore reload: %v", err)
	}
	defer store2.Close()

	data, meta, err := store2.ReadLog("abc123", "sess01", 0)
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
	store, err := NewStore(dir)
	if err != nil {
		t.Fatalf("NewStore: %v", err)
	}
	defer store.Close()

	sv := NewServer(store, false)
	ts := httptest.NewServer(sv)
	defer ts.Close()

	// Gap test: offset jumps from 0 to 100.
	res, err := store.AppendChunk(ChunkInput{
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

	sessions := store.ListSessions(50)
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

func TestIdleFileHandleEvictionAndReopen(t *testing.T) {
	dir := t.TempDir()
	store, err := NewStore(dir)
	if err != nil {
		t.Fatalf("NewStore: %v", err)
	}
	defer store.Close()

	line1 := "first chunk\n"
	line2 := "second chunk after idle close\n"
	if _, err := store.AppendChunk(ChunkInput{
		ClientID:  "c1",
		SessionID: "s1",
		Seq:       1,
		Offset:    0,
		Data:      []byte(line1),
	}); err != nil {
		t.Fatalf("AppendChunk 1: %v", err)
	}
	if store.openFiles.Load() != 1 {
		t.Fatalf("openFiles=%d, want 1", store.openFiles.Load())
	}

	closed := store.CloseIdleFiles(0)
	if closed != 1 || store.openFiles.Load() != 0 {
		t.Fatalf("closed=%d openFiles=%d, want 1 and 0", closed, store.openFiles.Load())
	}

	if _, err := store.AppendChunk(ChunkInput{
		ClientID:  "c1",
		SessionID: "s1",
		Seq:       2,
		Offset:    uint64(len(line1)),
		Data:      []byte(line2),
	}); err != nil {
		t.Fatalf("AppendChunk 2: %v", err)
	}
	if store.openFiles.Load() != 1 {
		t.Fatalf("openFiles=%d after reopen, want 1", store.openFiles.Load())
	}

	data, _, err := store.ReadLog("c1", "s1", 0)
	if err != nil {
		t.Fatalf("ReadLog: %v", err)
	}
	if string(data) != line1+line2 {
		t.Fatalf("got %q, want %q", string(data), line1+line2)
	}
}

func TestKindAndClientDroppedMetadata(t *testing.T) {
	dir := t.TempDir()
	store, err := NewStore(dir)
	if err != nil {
		t.Fatalf("NewStore: %v", err)
	}
	sv := NewServer(store, false)
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
	store.Close()

	store2, err := NewStore(dir)
	if err != nil {
		t.Fatalf("NewStore reload: %v", err)
	}
	defer store2.Close()

	got, meta, err := store2.ReadLog("c_app", "app-012345", 0)
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
	store, err := NewStore(dir)
	if err != nil {
		t.Fatalf("NewStore: %v", err)
	}
	sv := NewServer(store, false)
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
			norm, ok := NormalizeUID(out.UID)
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

	store.Close()

	// Reopen store and verify allocated UIDs and session logs reload cleanly.
	store2, err := NewStore(dir)
	if err != nil {
		t.Fatalf("NewStore reload: %v", err)
	}
	defer store2.Close()

	store2.mu.RLock()
	rec, ok := store2.clients[sampleUID]
	store2.mu.RUnlock()
	if !ok || rec.UID != sampleUID || rec.Version != "1.12" || rec.LegacyClient != "0123456789abcdef" {
		t.Fatalf("reloaded client record mismatch: ok=%v rec=%+v", ok, rec)
	}

	got, meta, err := store2.ReadLog(sampleUID, "sessA", 0)
	if err != nil {
		t.Fatalf("ReadLog: %v", err)
	}
	if string(got) != logBody || meta.ClientID != sampleUID {
		t.Fatalf("ReadLog mismatch: got=%q meta=%+v", string(got), meta)
	}
}
