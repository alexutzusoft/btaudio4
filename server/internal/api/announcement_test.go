package api

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"btaudio4-server/internal/store"
)

func TestAnnouncementPublication(t *testing.T) {
	dir := t.TempDir()
	st, err := store.New(dir)
	if err != nil {
		t.Fatal(err)
	}
	defer st.Close()
	server := NewServer(st, false)
	path := filepath.Join(dir, "announcement.json")
	read := func(wantStatus int) *httptest.ResponseRecorder {
		t.Helper()
		w := httptest.NewRecorder()
		server.ServeHTTP(w, httptest.NewRequest(http.MethodGet, "/api/announcement", nil))
		if w.Code != wantStatus {
			t.Fatalf("status = %d, want %d, response = %s", w.Code, wantStatus, w.Body)
		}
		if w.Header().Get("Cache-Control") != "no-store" {
			t.Fatal("announcement responses must be freshly fetched")
		}
		return w
	}
	write := func(data string) {
		t.Helper()
		if err := os.WriteFile(path, []byte(data), 0600); err != nil {
			t.Fatal(err)
		}
	}

	read(http.StatusNoContent)
	write("{\"id\":\"maintenance_1\",\"title\":\"Bună\",\"body\":\"First line\\n<script>literal text</script>\"}")
	w := read(http.StatusOK)
	var got announcement
	if err := json.Unmarshal(w.Body.Bytes(), &got); err != nil {
		t.Fatal(err)
	}
	want := announcement{"maintenance_1", "Bună", "First line\n<script>literal text</script>"}
	if got != want {
		t.Fatalf("announcement = %#v, want %#v", got, want)
	}

	for _, invalid := range []string{
		"{\"id\":\"../unsafe\",\"title\":\"Title\",\"body\":\"Body\"}",
		"{\"id\":\"new\",\"title\":\"Title\",\"body\":\"\\t\\n\"}",
		"{\"id\":\"new\",\"title\":\"Title\",\"body\":\"" + strings.Repeat("x", 2049) + "\"}",
		"{\"id\":\"new\",\"title\":\"Title\",\"body\":\"Body\"} {}",
	} {
		write(invalid)
		read(http.StatusServiceUnavailable)
	}
	if err := os.Remove(path); err != nil {
		t.Fatal(err)
	}
	read(http.StatusNoContent)
}
