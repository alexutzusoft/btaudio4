package store

import "testing"

func TestIdleFileHandleEvictionAndReopen(t *testing.T) {
	dir := t.TempDir()
	st, err := New(dir)
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	defer st.Close()

	line1 := "first chunk\n"
	line2 := "second chunk after idle close\n"
	if _, err := st.AppendChunk(ChunkInput{
		ClientID:  "c1",
		SessionID: "s1",
		Seq:       1,
		Offset:    0,
		Data:      []byte(line1),
	}); err != nil {
		t.Fatalf("AppendChunk 1: %v", err)
	}
	if st.openFiles.Load() != 1 {
		t.Fatalf("openFiles=%d, want 1", st.openFiles.Load())
	}

	closed := st.CloseIdleFiles(0)
	if closed != 1 || st.openFiles.Load() != 0 {
		t.Fatalf("closed=%d openFiles=%d, want 1 and 0", closed, st.openFiles.Load())
	}

	if _, err := st.AppendChunk(ChunkInput{
		ClientID:  "c1",
		SessionID: "s1",
		Seq:       2,
		Offset:    uint64(len(line1)),
		Data:      []byte(line2),
	}); err != nil {
		t.Fatalf("AppendChunk 2: %v", err)
	}
	if st.openFiles.Load() != 1 {
		t.Fatalf("openFiles=%d after reopen, want 1", st.openFiles.Load())
	}

	data, _, err := st.ReadLog("c1", "s1", 0)
	if err != nil {
		t.Fatalf("ReadLog: %v", err)
	}
	if string(data) != line1+line2 {
		t.Fatalf("got %q, want %q", string(data), line1+line2)
	}
}
