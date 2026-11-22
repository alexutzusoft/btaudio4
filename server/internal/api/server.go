package api

import (
	"encoding/json"
	"io"
	"log"
	"net"
	"net/http"
	"strconv"
	"strings"
	"time"

	"btaudio4-server/internal/analytics"
	"btaudio4-server/internal/store"
)

const maxChunkBytes = 1 << 20 // 1 MiB safety cap per request

// Server handles HTTP diagnostic log ingestion, UID allocation, and announcements.
type Server struct {
	store     *store.Store
	analytics *analytics.Tracker
	verbose   bool
	mux       *http.ServeMux
	uidLim    *uidLimiter
}

// NewServer constructs an HTTP handler backed by st.
func NewServer(st *store.Store, verbose bool) *Server {
	return NewServerWithAnalytics(st, nil, verbose)
}

// NewServerWithAnalytics constructs an HTTP handler backed by st and optional active-user analytics.
func NewServerWithAnalytics(st *store.Store, an *analytics.Tracker, verbose bool) *Server {
	sv := &Server{
		store:     st,
		analytics: an,
		verbose:   verbose,
		mux:       http.NewServeMux(),
		uidLim:    newUIDLimiter(),
	}
	sv.mux.HandleFunc("POST /api/uid", sv.handleAllocateUID)
	sv.mux.HandleFunc("POST /api/logs", sv.handleIngest)
	sv.mux.HandleFunc("GET /api/announcement", sv.handleAnnouncement)
	sv.mux.HandleFunc("GET /healthz", sv.handleHealth)
	return sv
}

func (sv *Server) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	sv.mux.ServeHTTP(w, r)
}

func clientIP(r *http.Request) string {
	if xff := strings.TrimSpace(r.Header.Get("X-Forwarded-For")); xff != "" {
		if idx := strings.IndexByte(xff, ','); idx >= 0 {
			return strings.TrimSpace(xff[:idx])
		}
		return xff
	}
	if xri := strings.TrimSpace(r.Header.Get("X-Real-IP")); xri != "" {
		return xri
	}
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err == nil && host != "" {
		return host
	}
	return r.RemoteAddr
}

func paramOrHeader(r *http.Request, queryKey, headerKey string) string {
	if v := r.URL.Query().Get(queryKey); v != "" {
		return v
	}
	return r.Header.Get(headerKey)
}

func (sv *Server) handleAllocateUID(w http.ResponseWriter, r *http.Request) {
	r.Body = http.MaxBytesReader(w, r.Body, 4096)
	_, _ = io.Copy(io.Discard, r.Body)

	ip := clientIP(r)
	if !sv.uidLim.allow(ip, time.Now()) {
		http.Error(w, "rate limit exceeded", http.StatusTooManyRequests)
		return
	}

	in := store.UIDInput{
		Version:      paramOrHeader(r, "version", "X-BTA-Version"),
		Firmware:     paramOrHeader(r, "fw", "X-BTA-FW"),
		RemoteIP:     ip,
		LegacyClient: paramOrHeader(r, "legacy", "X-BTA-Legacy"),
	}
	rec, err := sv.store.AllocateUID(in)
	if err != nil {
		log.Printf("uid allocation error ip=%s: %v", ip, err)
		http.Error(w, "storage error", http.StatusInternalServerError)
		return
	}
	if sv.verbose {
		log.Printf("uid allocated uid=%s version=%s fw=%s legacy=%s ip=%s",
			rec.UID, rec.Version, rec.Firmware, rec.LegacyClient, rec.RemoteIP)
	}
	writeJSON(w, http.StatusOK, struct {
		OK  bool   `json:"ok"`
		UID string `json:"uid"`
	}{
		OK:  true,
		UID: rec.UID,
	})
}

func (sv *Server) handleIngest(w http.ResponseWriter, r *http.Request) {
	r.Body = http.MaxBytesReader(w, r.Body, maxChunkBytes)
	body, err := io.ReadAll(r.Body)
	if err != nil {
		http.Error(w, "invalid or oversized body", http.StatusRequestEntityTooLarge)
		return
	}

	seq, _ := strconv.ParseUint(paramOrHeader(r, "seq", "X-BTA-Seq"), 10, 64)
	offset, _ := strconv.ParseUint(paramOrHeader(r, "offset", "X-BTA-Offset"), 10, 64)
	dropped, _ := strconv.ParseUint(paramOrHeader(r, "dropped", "X-BTA-Dropped"), 10, 64)

	in := store.ChunkInput{
		ClientID:      paramOrHeader(r, "client", "X-BTA-Client"),
		SessionID:     paramOrHeader(r, "session", "X-BTA-Session"),
		Seq:           seq,
		Offset:        offset,
		Version:       paramOrHeader(r, "version", "X-BTA-Version"),
		Firmware:      paramOrHeader(r, "fw", "X-BTA-FW"),
		Kind:          paramOrHeader(r, "kind", "X-BTA-Kind"),
		ClientDropped: dropped,
		RemoteIP:      clientIP(r),
		Data:          body,
	}

	res, err := sv.store.AppendChunk(in)
	if err != nil {
		log.Printf("ingest error client=%q session=%q: %v", in.ClientID, in.SessionID, err)
		http.Error(w, "storage error", http.StatusInternalServerError)
		return
	}
	if sv.analytics != nil {
		sv.analytics.RecordActive(in.ClientID, in.RemoteIP)
	}

	if sv.verbose {
		log.Printf("chunk client=%s session=%s kind=%s seq=%d offset=%d bytes=%d received=%d dup=%v gap=%v dropped=%d ip=%s",
			store.SanitizeID(in.ClientID, "unknown", 64),
			store.SanitizeID(in.SessionID, "default", 64),
			store.SanitizeID(in.Kind, "worker", 16),
			in.Seq, in.Offset, len(body), res.BytesReceived, res.Duplicate, res.Gap, in.ClientDropped, in.RemoteIP)
	}

	writeJSON(w, http.StatusOK, struct {
		OK bool `json:"ok"`
		store.AppendResult
	}{
		OK:           true,
		AppendResult: res,
	})
}

func (sv *Server) handleHealth(w http.ResponseWriter, _ *http.Request) {
	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write([]byte("ok\n"))
}

func writeJSON(w http.ResponseWriter, code int, v any) {
	raw, err := json.Marshal(v)
	if err != nil {
		http.Error(w, "encode error", http.StatusInternalServerError)
		return
	}
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(code)
	_, _ = w.Write(raw)
}
