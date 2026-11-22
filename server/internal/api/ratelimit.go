package api

import (
	"sync"
	"time"
)

const (
	uidRateWindow       = 1 * time.Minute
	uidRateMaxPerWindow = 120
)

type uidLimiter struct {
	mu      sync.Mutex
	buckets map[string][]time.Time
}

func newUIDLimiter() *uidLimiter {
	return &uidLimiter{buckets: make(map[string][]time.Time)}
}

func (l *uidLimiter) allow(ip string, now time.Time) bool {
	l.mu.Lock()
	defer l.mu.Unlock()
	cutoff := now.Add(-uidRateWindow)
	if len(l.buckets) > 1024 {
		for k, ts := range l.buckets {
			if len(ts) == 0 || ts[len(ts)-1].Before(cutoff) {
				delete(l.buckets, k)
			}
		}
	}
	ts := l.buckets[ip]
	keep := 0
	for _, t := range ts {
		if !t.Before(cutoff) {
			ts[keep] = t
			keep++
		}
	}
	ts = ts[:keep]
	if len(ts) >= uidRateMaxPerWindow {
		l.buckets[ip] = ts
		return false
	}
	l.buckets[ip] = append(ts, now)
	return true
}
