package store

import (
	"crypto/rand"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"
)

const uidAlphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"

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

func (s *Store) loadClients() {
	cEntries, err := os.ReadDir(s.clientsDir)
	if err != nil {
		return
	}
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
		Version:      SanitizeText(in.Version, 32),
		Firmware:     SanitizeText(in.Firmware, 32),
		RemoteIP:     SanitizeText(in.RemoteIP, 64),
		LegacyClient: SanitizeID(in.LegacyClient, "", 64),
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

// Client returns a persisted client record by UID.
func (s *Store) Client(uid string) (ClientRecord, bool) {
	norm, ok := NormalizeUID(uid)
	if !ok {
		return ClientRecord{}, false
	}
	s.mu.RLock()
	defer s.mu.RUnlock()
	rec, exists := s.clients[norm]
	return rec, exists
}
