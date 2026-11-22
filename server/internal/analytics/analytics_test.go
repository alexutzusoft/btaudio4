package analytics

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

type fakeClock struct {
	now time.Time
}

func (c *fakeClock) Now() time.Time {
	return c.now
}

func (c *fakeClock) Advance(d time.Duration) {
	c.now = c.now.Add(d)
}

func readLines(t *testing.T, path string) []string {
	t.Helper()
	raw, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read %s: %v", path, err)
	}
	trimmed := strings.TrimRight(string(raw), "\n")
	if trimmed == "" {
		return nil
	}
	return strings.Split(trimmed, "\n")
}

func TestPerMinuteDeduplicationAndZeroMinutes(t *testing.T) {
	loc := time.FixedZone("EET", 2*3600)
	clk := &fakeClock{now: time.Date(2026, 11, 22, 10, 15, 20, 0, loc)}
	dir := t.TempDir()

	tr, err := NewWithClock(dir, clk.Now)
	if err != nil {
		t.Fatalf("NewWithClock: %v", err)
	}
	defer tr.Close()

	// Same UID in different cases counts once; distinct UIDs and IP fallbacks count separately.
	tr.RecordActive("abcd-1234", "10.0.0.1")
	tr.RecordActive("ABCD-1234", "10.0.0.2")
	tr.RecordActive("WXYZ-9999", "10.0.0.1")
	tr.RecordActive("", "10.0.0.3")
	tr.RecordActive("unknown", "10.0.0.3")
	tr.RecordActive("", "10.0.0.4")

	// Advance 3 minutes: 10:15 had 4 users, 10:16 had 0, 10:17 had 0.
	clk.Advance(3 * time.Minute)
	tr.Flush()

	lines := readLines(t, filepath.Join(dir, "analytics", "2026-11-22.log"))
	want := []string{
		"2026-11-22T10:15:00+02:00 active_users=4",
		"2026-11-22T10:16:00+02:00 active_users=0",
		"2026-11-22T10:17:00+02:00 active_users=0",
	}
	if strings.Join(lines, "\n") != strings.Join(want, "\n") {
		t.Fatalf("lines mismatch:\ngot:\n%s\nwant:\n%s", strings.Join(lines, "\n"), strings.Join(want, "\n"))
	}
}

func TestHourlyAndDailyPeaksAndDayRollover(t *testing.T) {
	loc := time.FixedZone("EET", 2*3600)
	clk := &fakeClock{now: time.Date(2026, 11, 22, 22, 58, 10, 0, loc)}
	dir := t.TempDir()

	tr, err := NewWithClock(dir, clk.Now)
	if err != nil {
		t.Fatalf("NewWithClock: %v", err)
	}
	defer tr.Close()

	// 22:58 -> 3 users
	tr.RecordActive("AAAA-1111", "1.1.1.1")
	tr.RecordActive("BBBB-2222", "1.1.1.2")
	tr.RecordActive("CCCC-3333", "1.1.1.3")

	// 22:59 -> 1 user
	clk.Advance(1 * time.Minute)
	tr.Flush()
	tr.RecordActive("AAAA-1111", "1.1.1.1")

	// Cross into 23:00 -> flushes 22:59 and hour 22 peak (3 at 22:58).
	clk.Advance(1 * time.Minute)
	tr.Flush()

	// Advance to 23:59, recording 5 users at 23:10.
	clk.now = time.Date(2026, 11, 22, 23, 10, 5, 0, loc)
	tr.Flush()
	for i := 0; i < 5; i++ {
		tr.RecordActive(string(rune('A'+i))+"AAA-0000", "")
	}

	// Cross midnight to 2026-11-23 00:01 -> flushes rest of 2026-11-22, hour 23 peak, daily peak, and 00:00 of next day.
	clk.now = time.Date(2026, 11, 23, 0, 1, 0, 0, loc)
	tr.Flush()

	day1 := readLines(t, filepath.Join(dir, "analytics", "2026-11-22.log"))
	expectedDay1 := map[string]bool{
		"2026-11-22T22:58:00+02:00 active_users=3":                                              false,
		"2026-11-22T22:59:00+02:00 active_users=1":                                              false,
		"2026-11-22T23:00:00+02:00 hourly_peak hour=22 peak=3 at=2026-11-22T22:58:00+02:00":    false,
		"2026-11-22T23:10:00+02:00 active_users=5":                                              false,
		"2026-11-23T00:00:00+02:00 hourly_peak hour=23 peak=5 at=2026-11-22T23:10:00+02:00":    false,
		"2026-11-23T00:00:00+02:00 daily_peak date=2026-11-22 peak=5 at=2026-11-22T23:10:00+02:00": false,
	}
	for _, line := range day1 {
		if _, ok := expectedDay1[line]; ok {
			expectedDay1[line] = true
		}
	}
	for line, seen := range expectedDay1 {
		if !seen {
			t.Fatalf("missing expected line in day1 log: %q\nfull log:\n%s", line, strings.Join(day1, "\n"))
		}
	}

	day2 := readLines(t, filepath.Join(dir, "analytics", "2026-11-23.log"))
	if len(day2) != 1 || day2[0] != "2026-11-23T00:00:00+02:00 active_users=0" {
		t.Fatalf("unexpected day2 log: %v", day2)
	}
}

func TestReloadPreservesHourAndDayPeaks(t *testing.T) {
	loc := time.FixedZone("EET", 2*3600)
	clk := &fakeClock{now: time.Date(2026, 11, 22, 1, 10, 0, 0, loc)}
	dir := t.TempDir()

	tr1, err := NewWithClock(dir, clk.Now)
	if err != nil {
		t.Fatalf("NewWithClock 1: %v", err)
	}
	for i := 0; i < 4; i++ {
		tr1.RecordActive(string(rune('A'+i))+"BBB-1111", "")
	}
	clk.now = time.Date(2026, 11, 22, 2, 5, 0, 0, loc)
	tr1.Flush()
	for i := 0; i < 2; i++ {
		tr1.RecordActive(string(rune('C'+i))+"CCC-2222", "")
	}
	clk.now = time.Date(2026, 11, 22, 2, 6, 0, 0, loc)
	tr1.Flush()
	tr1.Close()

	// Restart at 02:20 and record 1 user, then flush past 03:00 and midnight.
	clk.now = time.Date(2026, 11, 22, 2, 20, 0, 0, loc)
	tr2, err := NewWithClock(dir, clk.Now)
	if err != nil {
		t.Fatalf("NewWithClock 2: %v", err)
	}
	defer tr2.Close()

	tr2.RecordActive("ZZZZ-9999", "")
	clk.now = time.Date(2026, 11, 23, 0, 0, 0, 0, loc)
	tr2.Flush()

	lines := readLines(t, filepath.Join(dir, "analytics", "2026-11-22.log"))
	wantHour02 := "2026-11-22T03:00:00+02:00 hourly_peak hour=02 peak=2 at=2026-11-22T02:05:00+02:00"
	wantDaily := "2026-11-23T00:00:00+02:00 daily_peak date=2026-11-22 peak=4 at=2026-11-22T01:10:00+02:00"

	var gotHour02, gotDaily bool
	for _, line := range lines {
		if line == wantHour02 {
			gotHour02 = true
		}
		if line == wantDaily {
			gotDaily = true
		}
	}
	if !gotHour02 || !gotDaily {
		t.Fatalf("reload peak mismatch: gotHour02=%v gotDaily=%v\nlog:\n%s",
			gotHour02, gotDaily, strings.Join(lines, "\n"))
	}
}

func TestRecordActiveAcrossMinuteBoundary(t *testing.T) {
	loc := time.FixedZone("EET", 2*3600)
	clk := &fakeClock{now: time.Date(2026, 11, 22, 12, 0, 30, 0, loc)}
	dir := t.TempDir()

	tr, err := NewWithClock(dir, clk.Now)
	if err != nil {
		t.Fatalf("NewWithClock: %v", err)
	}
	defer tr.Close()

	tr.RecordActive("AAAA-1111", "")
	// Boundary crossed before background tick fired: RecordActive flushes 12:00 and records into 12:01.
	clk.now = time.Date(2026, 11, 22, 12, 1, 0, 0, loc)
	tr.RecordActive("BBBB-2222", "")
	tr.RecordActive("CCCC-3333", "")

	clk.now = time.Date(2026, 11, 22, 12, 2, 0, 0, loc)
	tr.Flush()

	lines := readLines(t, filepath.Join(dir, "analytics", "2026-11-22.log"))
	want := []string{
		"2026-11-22T12:00:00+02:00 active_users=1",
		"2026-11-22T12:01:00+02:00 active_users=2",
	}
	if strings.Join(lines, "\n") != strings.Join(want, "\n") {
		t.Fatalf("lines mismatch:\ngot:\n%s\nwant:\n%s", strings.Join(lines, "\n"), strings.Join(want, "\n"))
	}
}

func TestZeroPeakLogsNone(t *testing.T) {
	loc := time.FixedZone("EET", 2*3600)
	clk := &fakeClock{now: time.Date(2026, 11, 22, 4, 59, 0, 0, loc)}
	dir := t.TempDir()

	tr, err := NewWithClock(dir, clk.Now)
	if err != nil {
		t.Fatalf("NewWithClock: %v", err)
	}
	defer tr.Close()

	clk.Advance(time.Minute)
	tr.Flush()

	lines := readLines(t, filepath.Join(dir, "analytics", "2026-11-22.log"))
	want := []string{
		"2026-11-22T04:59:00+02:00 active_users=0",
		"2026-11-22T05:00:00+02:00 hourly_peak hour=04 peak=0 at=none",
	}
	if strings.Join(lines, "\n") != strings.Join(want, "\n") {
		t.Fatalf("lines mismatch:\ngot:\n%s\nwant:\n%s", strings.Join(lines, "\n"), strings.Join(want, "\n"))
	}
}
