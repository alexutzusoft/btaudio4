package analytics

import (
	"bufio"
	"bytes"
	"errors"
	"fmt"
	"log"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"

	"btaudio4-server/internal/store"
)

const maxCatchUp = 24 * time.Hour

// Tracker records unique active users per local-time minute and logs hourly and daily peaks.
type Tracker struct {
	dir        string
	nowFn      func() time.Time
	mu         sync.Mutex
	closed     bool
	bucketAt   time.Time
	active     map[string]struct{}
	hourPeak   int
	hourPeakAt time.Time
	dayPeak    int
	dayPeakAt  time.Time
	openDate   string
	file       *os.File
	stopCh     chan struct{}
	doneCh     chan struct{}
}

// New creates an analytics Tracker writing daily files under <dataDir>/analytics/YYYY-MM-DD.log.
func New(dataDir string) (*Tracker, error) {
	return newTracker(dataDir, time.Now, true)
}

// NewWithClock creates a Tracker driven by a custom clock without a background timer; callers invoke Flush.
func NewWithClock(dataDir string, nowFn func() time.Time) (*Tracker, error) {
	return newTracker(dataDir, nowFn, false)
}

// Flush writes any completed minute, hourly-peak, and daily-peak entries up to the current clock time.
func (t *Tracker) Flush() {
	t.flushUntil(t.nowFn())
}

func newTracker(dataDir string, nowFn func() time.Time, startLoop bool) (*Tracker, error) {
	dir := filepath.Join(dataDir, "analytics")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return nil, fmt.Errorf("create analytics dir: %w", err)
	}
	now := nowFn()
	bucket := minuteStart(now)
	t := &Tracker{
		dir:      dir,
		nowFn:    nowFn,
		bucketAt: bucket,
		active:   make(map[string]struct{}),
		stopCh:   make(chan struct{}),
		doneCh:   make(chan struct{}),
	}
	if err := t.loadDayPeaks(bucket); err != nil {
		return nil, err
	}
	if startLoop {
		go t.loop()
	} else {
		close(t.doneCh)
	}
	return t, nil
}

// Dir returns the analytics directory path.
func (t *Tracker) Dir() string {
	return t.dir
}

// RecordActive marks a client as active during the current minute bucket.
func (t *Tracker) RecordActive(clientID, remoteIP string) {
	key := userKey(clientID, remoteIP)
	now := t.nowFn()
	t.mu.Lock()
	if !t.closed {
		if !now.Before(t.bucketAt.Add(time.Minute)) {
			t.flushUntilLocked(now)
		}
		t.active[key] = struct{}{}
	}
	t.mu.Unlock()
}

// Close stops the background ticker and closes any open analytics file handle.
func (t *Tracker) Close() {
	t.mu.Lock()
	if t.closed {
		t.mu.Unlock()
		return
	}
	t.closed = true
	close(t.stopCh)
	t.mu.Unlock()

	<-t.doneCh

	t.mu.Lock()
	if t.file != nil {
		_ = t.file.Close()
		t.file = nil
		t.openDate = ""
	}
	t.mu.Unlock()
}

func userKey(clientID, remoteIP string) string {
	cid := store.NormalizeClientID(clientID, "")
	if cid != "" && cid != "unknown" {
		return "uid:" + cid
	}
	if ip := strings.TrimSpace(remoteIP); ip != "" {
		return "ip:" + ip
	}
	return "unknown"
}

func minuteStart(t time.Time) time.Time {
	return time.Date(t.Year(), t.Month(), t.Day(), t.Hour(), t.Minute(), 0, 0, t.Location())
}

func dateKey(t time.Time) string {
	return t.Format("2006-01-02")
}

func formatAt(at time.Time, peak int) string {
	if peak <= 0 || at.IsZero() {
		return "none"
	}
	return at.Format(time.RFC3339)
}

func (t *Tracker) loop() {
	defer close(t.doneCh)
	for {
		t.mu.Lock()
		next := t.bucketAt.Add(time.Minute)
		t.mu.Unlock()

		wait := next.Sub(t.nowFn())
		if wait < time.Millisecond {
			wait = time.Millisecond
		}

		timer := time.NewTimer(wait)
		select {
		case <-t.stopCh:
			if !timer.Stop() {
				select {
				case <-timer.C:
				default:
				}
			}
			return
		case <-timer.C:
			t.flushUntil(t.nowFn())
		}
	}
}

func (t *Tracker) flushUntil(now time.Time) {
	t.mu.Lock()
	defer t.mu.Unlock()
	if t.closed {
		return
	}
	t.flushUntilLocked(now)
}

func (t *Tracker) flushUntilLocked(now time.Time) {

	var buf bytes.Buffer
	var bufDate string
	flushBuf := func() {
		if buf.Len() == 0 {
			return
		}
		if err := t.appendToDate(bufDate, buf.Bytes()); err != nil {
			log.Printf("analytics write error date=%s: %v", bufDate, err)
		}
		buf.Reset()
	}

	for !now.Before(t.bucketAt.Add(time.Minute)) {
		nextBucket := t.bucketAt.Add(time.Minute)
		day := dateKey(t.bucketAt)
		if bufDate != "" && bufDate != day {
			flushBuf()
		}
		bufDate = day

		count := len(t.active)
		if count > 0 {
			t.active = make(map[string]struct{})
		}
		if count > t.hourPeak {
			t.hourPeak = count
			t.hourPeakAt = t.bucketAt
		}
		if count > t.dayPeak {
			t.dayPeak = count
			t.dayPeakAt = t.bucketAt
		}

		fmt.Fprintf(&buf, "%s active_users=%d\n", t.bucketAt.Format(time.RFC3339), count)

		longGap := now.Sub(nextBucket) > maxCatchUp
		nextDay := dateKey(nextBucket)
		hourEnded := longGap || nextBucket.Hour() != t.bucketAt.Hour() || nextDay != day
		dayEnded := longGap || nextDay != day

		if hourEnded {
			fmt.Fprintf(&buf, "%s hourly_peak hour=%02d peak=%d at=%s\n",
				nextBucket.Format(time.RFC3339), t.bucketAt.Hour(), t.hourPeak, formatAt(t.hourPeakAt, t.hourPeak))
			t.hourPeak = 0
			t.hourPeakAt = time.Time{}
		}
		if dayEnded {
			fmt.Fprintf(&buf, "%s daily_peak date=%s peak=%d at=%s\n",
				nextBucket.Format(time.RFC3339), day, t.dayPeak, formatAt(t.dayPeakAt, t.dayPeak))
			t.dayPeak = 0
			t.dayPeakAt = time.Time{}
		}

		if longGap {
			t.bucketAt = minuteStart(now)
			break
		}
		t.bucketAt = nextBucket
	}

	flushBuf()
}

func (t *Tracker) appendToDate(day string, data []byte) error {
	if t.file != nil && t.openDate != day {
		_ = t.file.Close()
		t.file = nil
		t.openDate = ""
	}
	if t.file == nil {
		path := filepath.Join(t.dir, day+".log")
		f, err := os.OpenFile(path, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0o644)
		if err != nil {
			return err
		}
		t.file = f
		t.openDate = day
	}
	for len(data) > 0 {
		n, err := t.file.Write(data)
		if err != nil {
			_ = t.file.Close()
			t.file = nil
			t.openDate = ""
			return err
		}
		data = data[n:]
	}
	return nil
}

func (t *Tracker) loadDayPeaks(bucket time.Time) error {
	day := dateKey(bucket)
	path := filepath.Join(t.dir, day+".log")
	f, err := os.Open(path)
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	if err != nil {
		return fmt.Errorf("read analytics file %s: %w", path, err)
	}
	defer f.Close()

	loc := bucket.Location()
	scanner := bufio.NewScanner(f)
	for scanner.Scan() {
		fields := strings.Fields(scanner.Text())
		if len(fields) < 2 || !strings.HasPrefix(fields[1], "active_users=") {
			continue
		}
		ts, err := time.Parse(time.RFC3339, fields[0])
		if err != nil {
			continue
		}
		n, err := strconv.Atoi(strings.TrimPrefix(fields[1], "active_users="))
		if err != nil || n < 0 {
			continue
		}
		tsLocal := ts.In(loc)
		if n > t.dayPeak {
			t.dayPeak = n
			t.dayPeakAt = tsLocal
		}
		if dateKey(tsLocal) == day && tsLocal.Hour() == bucket.Hour() && n > t.hourPeak {
			t.hourPeak = n
			t.hourPeakAt = tsLocal
		}
	}
	return scanner.Err()
}
