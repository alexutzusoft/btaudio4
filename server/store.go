package main

import (
	"crypto/rand"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

const (
	idleFileTTL         = 2 * time.Minute
	janitorInterval     = 1 * time.Minute
	maxOpenSessionFiles = 256
	uidAlphabet         = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"
)

// ClientRecord describes a persistent console UID issued by the server.
type ClientRecord struct {
	UID          string    `json:"uid"`
	CreatedAt    time.Time `json:"created_at"`
	Version      string    `json:"version,omitempty"`
	Firmware     string    `json:"fw,omitempty"`
	RemoteIP     string    `json:"remote_ip,omitempty"`
	LegacyClient string    `json:"legacy_client,omitempty"`
}

// UIDInput holds optional metadata supplied when allocating a console UID.
type UIDInput struct {
	Version      string
	Firmware     string
	RemoteIP     string
	LegacyClient string
}

// SessionMeta describes one PS4 payload run's diagnostic log stream.
type SessionMeta struct {
	ClientID      string    `json:"client_id"`
	SessionID     string    `json:"session_id"`
	Kind          string    `json:"kind,omitempty"`
	Version       string    `json:"version,omitempty"`
	Firmware      string    `json:"fw,omitempty"`
	RemoteIP      string    `json:"remote_ip,omitempty"`
	BytesReceived uint64    `json:"bytes_received"`
	Chunks        uint64    `json:"chunks"`
	LastSeq       uint64    `json:"last_seq"`
	Gaps          uint64    `json:"gaps"`
	ClientDropped uint64    `json:"client_dropped,omitempty"`
	FirstSeen     time.Time `json:"first_seen"`
	LastSeen      time.Time `json:"last_seen"`
}

// ChunkInput is a single diagnostic log chunk uploaded by the payload.
type ChunkInput struct {
	ClientID      string
	SessionID     string
	Seq           uint64
	Offset        uint64
	Version       string
	Firmware      string
	Kind          string
	ClientDropped uint64
	RemoteIP      string
	Data          []byte
}

// AppendResult summarizes how a chunk was applied to the session log.
type AppendResult struct {
	BytesReceived uint64 `json:"received"`
	Seq           uint64 `json:"seq"`
	Duplicate     bool   `json:"duplicate,omitempty"`
	Gap           bool   `json:"gap,omitempty"`
}

type sessionState struct {
	mu       sync.Mutex
	meta     SessionMeta
	logPath  string
	metaPath string
	file     *os.File
	dirtyAt  time.Time
	lastIO   time.Time
}

// Store persists incoming log chunks per (client, session) safely and idempotently.
type Store struct {
	root       string
	clientsDir string
	mu         sync.RWMutex
	sessions   map[string]*sessionState
	clients    map[string]ClientRecord
	closed     bool
	openFiles  atomic.Int64
	stopCh     chan struct{}
	doneCh     chan struct{}
}

func NewStore(dataDir string) (*Store, error) {
	sessionsDir := filepath.Join(dataDir, "sessions")
	if err := os.MkdirAll(sessionsDir, 0o755); err != nil {
		return nil, fmt.Errorf("create sessions dir: %w", err)
	}
	clientsDir := filepath.Join(dataDir, "clients")
	if err := os.MkdirAll(clientsDir, 0o755); err != nil {
		return nil, fmt.Errorf("create clients dir: %w", err)
	}
	s := &Store{
		root:       sessionsDir,
		clientsDir: clientsDir,
		sessions:   make(map[string]*sessionState),
		clients:    make(map[string]ClientRecord),
		stopCh:     make(chan struct{}),
		doneCh:     make(chan struct{}),
	}
	if err := s.loadExisting(); err != nil {
		return nil, err
	}
	go s.janitorLoop()
	return s, nil
}

func (s *Store) janitorLoop() {
	defer close(s.doneCh)
	ticker := time.NewTicker(janitorInterval)
	defer ticker.Stop()
	for {
		select {
		case <-s.stopCh:
			return
		case <-ticker.C:
			s.CloseIdleFiles(idleFileTTL)
		}
	}
}

// NormalizeUID validates a CCCC-CCCC alphanumeric UID and normalizes letters to uppercase.
func NormalizeUID(raw string) (string, bool) {
	raw = strings.TrimSpace(raw)
	if len(raw) != 9 || raw[4] != '-' {
		return "", false
	}
	var b [9]byte
	for i := 0; i < 9; i++ {
		c := raw[i]
		if i == 4 {
			b[i] = '-'
			continue
		}
		if c >= 'a' && c <= 'z' {
			c -= 32
		} else if !((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')) {
			return "", false
		}
		b[i] = c
	}
	return string(b[:]), true
}

func normalizeClientID(raw, fallback string) string {
	if uid, ok := NormalizeUID(raw); ok {
		return uid
	}
	return sanitizeID(raw, fallback, 64)
}

func sanitizeID(raw string, fallback string, maxLen int) string {
	var b strings.Builder
	b.Grow(len(raw))
	for i := 0; i < len(raw) && b.Len() < maxLen; i++ {
		c := raw[i]
		if (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' {
			b.WriteByte(c)
		}
	}
	out := strings.Trim(b.String(), ".")
	if out == "" || strings.Contains(out, "..") {
		return fallback
	}
	return out
}

func sanitizeText(raw string, maxLen int) string {
	var b strings.Builder
	b.Grow(len(raw))
	for i := 0; i < len(raw) && b.Len() < maxLen; i++ {
		c := raw[i]
		if c >= 0x20 && c < 0x7f {
			b.WriteByte(c)
		}
	}
	return strings.TrimSpace(b.String())
}

func sessionKey(clientID, sessionID string) string {
	return clientID + "/" + sessionID
}

func (s *Store) loadExisting() error {
	clients, err := os.ReadDir(s.root)
	if err != nil {
		return fmt.Errorf("read sessions dir: %w", err)
	}
	for _, cEntry := range clients {
		if !cEntry.IsDir() {
			continue
		}
		clientID := sanitizeID(cEntry.Name(), "", 64)
		if clientID == "" {
			continue
		}
		cDir := filepath.Join(s.root, cEntry.Name())
		entries, err := os.ReadDir(cDir)
		if err != nil {
			continue
		}
		for _, entry := range entries {
			if entry.IsDir() || !strings.HasSuffix(entry.Name(), ".log") {
				continue
			}
			sessionID := sanitizeID(strings.TrimSuffix(entry.Name(), ".log"), "", 64)
			if sessionID == "" {
				continue
			}
			logPath := filepath.Join(cDir, entry.Name())
			metaPath := filepath.Join(cDir, sessionID+".meta.json")
			info, err := entry.Info()
			if err != nil {
				continue
			}
			meta := SessionMeta{
				ClientID:      clientID,
				SessionID:     sessionID,
				BytesReceived: uint64(info.Size()),
				FirstSeen:     info.ModTime().UTC(),
				LastSeen:      info.ModTime().UTC(),
			}
			if raw, err := os.ReadFile(metaPath); err == nil {
				var loaded SessionMeta
				if json.Unmarshal(raw, &loaded) == nil {
					loaded.ClientID = clientID
					loaded.SessionID = sessionID
					if loaded.Gaps == 0 && uint64(info.Size()) > loaded.BytesReceived {
						loaded.BytesReceived = uint64(info.Size())
					}
					meta = loaded
				}
			}
			s.sessions[sessionKey(clientID, sessionID)] = &sessionState{
				meta:     meta,
				logPath:  logPath,
				metaPath: metaPath,
			}
		}
	}
	if cEntries, err := os.ReadDir(s.clientsDir); err == nil {
		for _, entry := range cEntries {
			if entry.IsDir() || !strings.HasSuffix(entry.Name(), ".json") {
				continue
			}
			uid, ok := NormalizeUID(strings.TrimSuffix(entry.Name(), ".json"))
			if !ok {
				continue
			}
			raw, err := os.ReadFile(filepath.Join(s.clientsDir, entry.Name()))
			if err != nil {
				continue
			}
			var rec ClientRecord
			if json.Unmarshal(raw, &rec) == nil {
				rec.UID = uid
				s.clients[uid] = rec
			}
		}
	}
	return nil
}

func randomUID() (string, error) {
	var rnd [8]byte
	if _, err := rand.Read(rnd[:]); err != nil {
		return "", err
	}
	var out [9]byte
	for i := 0; i < 8; i++ {
		pos := i
		if i >= 4 {
			pos++
		}
		out[pos] = uidAlphabet[rnd[i]&31]
	}
	out[4] = '-'
	return string(out[:]), nil
}

// AllocateUID generates and persists a unique CCCC-CCCC console UID.
func (s *Store) AllocateUID(in UIDInput) (ClientRecord, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.closed {
		return ClientRecord{}, errors.New("store closed")
	}

	var uid string
	for attempt := 0; attempt < 32; attempt++ {
		candidate, err := randomUID()
		if err != nil {
			return ClientRecord{}, fmt.Errorf("generate uid: %w", err)
		}
		if _, exists := s.clients[candidate]; exists {
			continue
		}
		if _, err := os.Stat(filepath.Join(s.root, candidate)); err == nil {
			continue
		}
		uid = candidate
		break
	}
	if uid == "" {
		return ClientRecord{}, errors.New("could not allocate unique uid")
	}

	rec := ClientRecord{
		UID:          uid,
		CreatedAt:    time.Now().UTC(),
		Version:      sanitizeText(in.Version, 32),
		Firmware:     sanitizeText(in.Firmware, 32),
		RemoteIP:     sanitizeText(in.RemoteIP, 64),
		LegacyClient: sanitizeID(in.LegacyClient, "", 64),
	}

	raw, err := json.MarshalIndent(rec, "", "  ")
	if err != nil {
		return ClientRecord{}, err
	}
	finalPath := filepath.Join(s.clientsDir, uid+".json")
	tmpPath := finalPath + ".tmp"
	if err := os.WriteFile(tmpPath, raw, 0o644); err != nil {
		return ClientRecord{}, fmt.Errorf("write uid record: %w", err)
	}
	if err := os.Rename(tmpPath, finalPath); err != nil {
		_ = os.Remove(tmpPath)
		return ClientRecord{}, fmt.Errorf("save uid record: %w", err)
	}
	s.clients[uid] = rec
	return rec, nil
}

func (s *Store) getOrCreate(clientID, sessionID string) (*sessionState, error) {
	key := sessionKey(clientID, sessionID)
	s.mu.RLock()
	if s.closed {
		s.mu.RUnlock()
		return nil, errors.New("store closed")
	}
	st := s.sessions[key]
	s.mu.RUnlock()
	if st != nil {
		return st, nil
	}

	s.mu.Lock()
	defer s.mu.Unlock()
	if s.closed {
		return nil, errors.New("store closed")
	}
	if st = s.sessions[key]; st != nil {
		return st, nil
	}
	cDir := filepath.Join(s.root, clientID)
	if err := os.MkdirAll(cDir, 0o755); err != nil {
		return nil, fmt.Errorf("create client dir: %w", err)
	}
	now := time.Now().UTC()
	st = &sessionState{
		meta: SessionMeta{
			ClientID:  clientID,
			SessionID: sessionID,
			FirstSeen: now,
			LastSeen:  now,
		},
		logPath:  filepath.Join(cDir, sessionID+".log"),
		metaPath: filepath.Join(cDir, sessionID+".meta.json"),
	}
	s.sessions[key] = st
	return st, nil
}

func writeAll(f *os.File, b []byte) error {
	for len(b) > 0 {
		n, err := f.Write(b)
		if err != nil {
			return err
		}
		b = b[n:]
	}
	return nil
}

func (st *sessionState) persistMetaLocked(now time.Time, force bool) error {
	if !force && !st.dirtyAt.IsZero() && now.Sub(st.dirtyAt) < 5*time.Second {
		return nil
	}
	raw, err := json.MarshalIndent(st.meta, "", "  ")
	if err != nil {
		return err
	}
	tmp := st.metaPath + ".tmp"
	if err := os.WriteFile(tmp, raw, 0o644); err != nil {
		return err
	}
	if err := os.Rename(tmp, st.metaPath); err != nil {
		_ = os.Remove(tmp)
		return err
	}
	st.dirtyAt = now
	return nil
}

// AppendChunk appends a chunk of logs with offset-based deduplication and gap tracking.
func (s *Store) AppendChunk(in ChunkInput) (AppendResult, error) {
	clientID := normalizeClientID(in.ClientID, "unknown")
	sessionID := sanitizeID(in.SessionID, "default", 64)
	st, err := s.getOrCreate(clientID, sessionID)
	if err != nil {
		return AppendResult{}, err
	}

	st.mu.Lock()
	defer st.mu.Unlock()

	now := time.Now().UTC()
	if ver := sanitizeText(in.Version, 32); ver != "" {
		st.meta.Version = ver
	}
	if fw := sanitizeText(in.Firmware, 32); fw != "" {
		st.meta.Firmware = fw
	}
	if ip := sanitizeText(in.RemoteIP, 64); ip != "" {
		st.meta.RemoteIP = ip
	}
	if kind := sanitizeID(in.Kind, "", 16); kind != "" {
		st.meta.Kind = kind
	}
	if in.ClientDropped > st.meta.ClientDropped {
		st.meta.ClientDropped = in.ClientDropped
	}
	if st.meta.FirstSeen.IsZero() {
		st.meta.FirstSeen = now
	}
	st.meta.LastSeen = now

	if len(in.Data) == 0 {
		_ = st.persistMetaLocked(now, st.meta.Chunks == 0)
		return AppendResult{BytesReceived: st.meta.BytesReceived, Seq: in.Seq}, nil
	}

	data := in.Data
	res := AppendResult{Seq: in.Seq}

	if in.Offset < st.meta.BytesReceived {
		overlap := st.meta.BytesReceived - in.Offset
		if overlap >= uint64(len(data)) {
			res.BytesReceived = st.meta.BytesReceived
			res.Duplicate = true
			return res, nil
		}
		data = data[overlap:]
	} else if in.Offset > st.meta.BytesReceived {
		res.Gap = true
		st.meta.Gaps++
	}

	if st.file == nil {
		if s.openFiles.Load() >= maxOpenSessionFiles {
			st.mu.Unlock()
			s.CloseIdleFiles(10 * time.Second)
			st.mu.Lock()
		}
		if st.file == nil {
			f, err := os.OpenFile(st.logPath, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o644)
			if err != nil {
				return AppendResult{}, fmt.Errorf("open session log: %w", err)
			}
			st.file = f
			s.openFiles.Add(1)
		}
	}
	st.lastIO = now

	if res.Gap {
		gapBytes := in.Offset - st.meta.BytesReceived
		marker := fmt.Sprintf("\n# [server: gap of %d bytes before seq=%d offset=%d]\n", gapBytes, in.Seq, in.Offset)
		if err := writeAll(st.file, []byte(marker)); err != nil {
			return AppendResult{}, err
		}
		st.meta.BytesReceived = in.Offset
	}

	if err := writeAll(st.file, data); err != nil {
		return AppendResult{}, err
	}

	st.meta.BytesReceived += uint64(len(data))
	st.meta.Chunks++
	if in.Seq > st.meta.LastSeq {
		st.meta.LastSeq = in.Seq
	}
	res.BytesReceived = st.meta.BytesReceived

	forceMeta := st.meta.Chunks == 1 || res.Gap
	_ = st.persistMetaLocked(now, forceMeta)
	return res, nil
}

// ListSessions returns session metadata ordered most recently active first.
func (s *Store) ListSessions(limit int) []SessionMeta {
	s.mu.RLock()
	list := make([]*sessionState, 0, len(s.sessions))
	for _, st := range s.sessions {
		list = append(list, st)
	}
	s.mu.RUnlock()

	out := make([]SessionMeta, 0, len(list))
	for _, st := range list {
		st.mu.Lock()
		out = append(out, st.meta)
		st.mu.Unlock()
	}

	sort.Slice(out, func(i, j int) bool {
		if !out[i].LastSeen.Equal(out[j].LastSeen) {
			return out[i].LastSeen.After(out[j].LastSeen)
		}
		if out[i].ClientID != out[j].ClientID {
			return out[i].ClientID < out[j].ClientID
		}
		return out[i].SessionID < out[j].SessionID
	})

	if limit > 0 && len(out) > limit {
		out = out[:limit]
	}
	return out
}

// ReadLog reads up to the last tailBytes of a session log (or the entire log when tailBytes <= 0).
func (s *Store) ReadLog(clientID, sessionID string, tailBytes int64) ([]byte, SessionMeta, error) {
	c := normalizeClientID(clientID, "")
	sid := sanitizeID(sessionID, "", 64)
	if c == "" || sid == "" {
		return nil, SessionMeta{}, os.ErrNotExist
	}

	s.mu.RLock()
	st := s.sessions[sessionKey(c, sid)]
	s.mu.RUnlock()
	if st == nil {
		return nil, SessionMeta{}, os.ErrNotExist
	}

	st.mu.Lock()
	meta := st.meta
	logPath := st.logPath
	st.mu.Unlock()

	f, err := os.Open(logPath)
	if err != nil {
		return nil, meta, err
	}
	defer f.Close()

	if tailBytes > 0 {
		info, err := f.Stat()
		if err == nil && info.Size() > tailBytes {
			if _, err := f.Seek(-tailBytes, io.SeekEnd); err == nil {
				buf, err := io.ReadAll(f)
				if err != nil {
					return nil, meta, err
				}
				// Trim partial leading line when tailing mid-file.
				if idx := strings.IndexByte(string(buf), '\n'); idx >= 0 && idx+1 < len(buf) {
					buf = buf[idx+1:]
				}
				return buf, meta, nil
			}
		}
	}

	buf, err := io.ReadAll(f)
	return buf, meta, err
}

// CloseIdleFiles flushes dirty metadata and closes log file handles idle for at least maxIdle.
func (s *Store) CloseIdleFiles(maxIdle time.Duration) int {
	s.mu.RLock()
	list := make([]*sessionState, 0, len(s.sessions))
	for _, st := range s.sessions {
		list = append(list, st)
	}
	s.mu.RUnlock()

	now := time.Now().UTC()
	closedCount := 0
	for _, st := range list {
		st.mu.Lock()
		if st.file != nil && (maxIdle <= 0 || now.Sub(st.lastIO) >= maxIdle) {
			_ = st.persistMetaLocked(now, true)
			_ = st.file.Close()
			st.file = nil
			s.openFiles.Add(-1)
			closedCount++
		}
		st.mu.Unlock()
	}
	return closedCount
}

// Close stops background cleanup, flushes metadata, and closes open session log file handles.
func (s *Store) Close() {
	s.mu.Lock()
	if s.closed {
		s.mu.Unlock()
		return
	}
	s.closed = true
	close(s.stopCh)
	list := make([]*sessionState, 0, len(s.sessions))
	for _, st := range s.sessions {
		list = append(list, st)
	}
	s.mu.Unlock()

	<-s.doneCh

	now := time.Now().UTC()
	for _, st := range list {
		st.mu.Lock()
		_ = st.persistMetaLocked(now, true)
		if st.file != nil {
			_ = st.file.Close()
			st.file = nil
			s.openFiles.Add(-1)
		}
		st.mu.Unlock()
	}
}
