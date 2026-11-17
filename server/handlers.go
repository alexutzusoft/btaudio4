package main

import (
	"encoding/json"
	"io"
	"log"
	"net"
	"net/http"
	"strconv"
	"strings"
)

const maxChunkBytes = 1 << 20 // 1 MiB safety cap per request

type Server struct {
	store   *Store
	verbose bool
	mux     *http.ServeMux
}

func NewServer(store *Store, verbose bool) *Server {
	sv := &Server{
		store:   store,
		verbose: verbose,
		mux:     http.NewServeMux(),
	}
	sv.mux.HandleFunc("POST /api/logs", sv.handleIngest)
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

func (sv *Server) handleIngest(w http.ResponseWriter, r *http.Request) {
	r.Body = http.MaxBytesReader(w, r.Body, maxChunkBytes)
	body, err := io.ReadAll(r.Body)
	if err != nil {
		http.Error(w, "invalid or oversized body", http.StatusRequestEntityTooLarge)
		return
	}

	seq, _ := strconv.ParseUint(paramOrHeader(r, "seq", "X-BTA-Seq"), 10, 64)
	offset, _ := strconv.ParseUint(paramOrHeader(r, "offset", "X-BTA-Offset"), 10, 64)

	in := ChunkInput{
		ClientID:  paramOrHeader(r, "client", "X-BTA-Client"),
		SessionID: paramOrHeader(r, "session", "X-BTA-Session"),
		Seq:       seq,
		Offset:    offset,
		Version:   paramOrHeader(r, "version", "X-BTA-Version"),
		Firmware:  paramOrHeader(r, "fw", "X-BTA-FW"),
		RemoteIP:  clientIP(r),
		Data:      body,
	}

	res, err := sv.store.AppendChunk(in)
	if err != nil {
		log.Printf("ingest error client=%q session=%q: %v", in.ClientID, in.SessionID, err)
		http.Error(w, "storage error", http.StatusInternalServerError)
		return
	}

	if sv.verbose {
		log.Printf("chunk client=%s session=%s seq=%d offset=%d bytes=%d received=%d dup=%v gap=%v ip=%s",
			sanitizeID(in.ClientID, "unknown", 64),
			sanitizeID(in.SessionID, "default", 64),
			in.Seq, in.Offset, len(body), res.BytesReceived, res.Duplicate, res.Gap, in.RemoteIP)
	}

	writeJSON(w, http.StatusOK, struct {
		OK bool `json:"ok"`
		AppendResult
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
