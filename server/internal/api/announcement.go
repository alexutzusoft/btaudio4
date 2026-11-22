package api

import (
	"encoding/json"
	"errors"
	"io"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"unicode/utf8"
)

type announcement struct {
	ID    string `json:"id"`
	Title string `json:"title"`
	Body  string `json:"body"`
}

func (a announcement) valid() bool {
	if len(a.ID) == 0 || len(a.ID) > 64 {
		return false
	}
	for _, c := range a.ID {
		if !(c >= 'A' && c <= 'Z' || c >= 'a' && c <= 'z' || c >= '0' && c <= '9' || c == '-' || c == '_') {
			return false
		}
	}
	return validAnnouncementText(a.Title, 128, false) && validAnnouncementText(a.Body, 2048, true)
}

func validAnnouncementText(text string, limit int, multiline bool) bool {
	if len(text) > limit || strings.Trim(text, " \t\r\n") == "" {
		return false
	}
	for _, c := range text {
		if c < ' ' && !(multiline && (c == '\n' || c == '\r' || c == '\t')) {
			return false
		}
	}
	return true
}

func (sv *Server) handleAnnouncement(w http.ResponseWriter, _ *http.Request) {
	w.Header().Set("Cache-Control", "no-store")
	path := filepath.Join(sv.store.DataDir(), "announcement.json")
	f, err := os.Open(path)
	if errors.Is(err, os.ErrNotExist) {
		w.WriteHeader(http.StatusNoContent)
		return
	}
	if err != nil {
		log.Printf("open announcement: %v", err)
		http.Error(w, "announcement unavailable", http.StatusServiceUnavailable)
		return
	}
	defer f.Close()
	const fileLimit = 16 << 10
	data, err := io.ReadAll(io.LimitReader(f, fileLimit+1))
	var a announcement
	if err != nil || len(data) > fileLimit || !utf8.Valid(data) || json.Unmarshal(data, &a) != nil || !a.valid() {
		http.Error(w, "invalid announcement file", http.StatusServiceUnavailable)
		return
	}
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	encoder := json.NewEncoder(w)
	encoder.SetEscapeHTML(false)
	_ = encoder.Encode(a)
}
