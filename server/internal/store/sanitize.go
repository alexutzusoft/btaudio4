package store

import "strings"

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

// NormalizeClientID normalizes a CCCC-CCCC UID or sanitizes a legacy client ID.
func NormalizeClientID(raw, fallback string) string {
	if uid, ok := NormalizeUID(raw); ok {
		return uid
	}
	return SanitizeID(raw, fallback, 64)
}

// SanitizeID strips disallowed characters from an identifier and rejects path traversal.
func SanitizeID(raw string, fallback string, maxLen int) string {
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

// SanitizeText keeps printable ASCII characters up to maxLen and trims surrounding whitespace.
func SanitizeText(raw string, maxLen int) string {
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
