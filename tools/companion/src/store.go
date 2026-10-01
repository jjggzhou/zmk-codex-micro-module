package companion

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"io"
	"os"
	"path/filepath"
	"sync"
	"time"
)

var ErrScopeChanged = errors.New("account scope changed; result discarded")
var ErrOldSnapshot = errors.New("out-of-order snapshot discarded")

const MaxSnapshotBytes = 64 * 1024

type Provider interface {
	Fetch(context.Context, string) (RawSnapshot, error)
}

var ErrUnauthorized = errors.New("authorization required")

// JSONFileProvider is an EXPLICIT fixture/import provider. It neither discovers
// nor reads ~/.codex, auth.json, transcripts, or any OS credential store.
type JSONFileProvider struct{ Path string }

func decodeBounded(r io.Reader, dst any) error {
	data, err := io.ReadAll(io.LimitReader(r, MaxSnapshotBytes+1))
	if err != nil {
		return err
	}
	if len(data) > MaxSnapshotBytes {
		return errors.New("snapshot too large")
	}
	if err := uniqueJSON(data); err != nil {
		return err
	}
	dec := json.NewDecoder(bytes.NewReader(data))
	dec.DisallowUnknownFields()
	if err := dec.Decode(dst); err != nil {
		return errors.New("invalid snapshot JSON")
	}
	var extra any
	if dec.Decode(&extra) != io.EOF {
		return errors.New("trailing snapshot data")
	}
	return nil
}
func (p JSONFileProvider) Fetch(ctx context.Context, scope string) (RawSnapshot, error) {
	if err := ctx.Err(); err != nil {
		return RawSnapshot{}, err
	}
	info, err := os.Stat(p.Path)
	if err != nil || !info.Mode().IsRegular() || info.Size() > MaxSnapshotBytes {
		return RawSnapshot{}, errors.New("fixture must be a bounded regular file")
	}
	f, err := os.Open(p.Path)
	if err != nil {
		return RawSnapshot{}, errors.New("cannot open fixture")
	}
	defer f.Close()
	var raw RawSnapshot
	if err := decodeBounded(f, &raw); err != nil {
		return raw, err
	}
	if raw.Scope != scope {
		return RawSnapshot{}, ErrScopeChanged
	}
	if err := ctx.Err(); err != nil {
		return RawSnapshot{}, err
	}
	return raw, nil // Do not replace observed_at with the file-read time.
}

type cacheFile struct {
	Schema   int       `json:"schema_version"`
	Scope    string    `json:"scope"`
	Snapshot *Snapshot `json:"snapshot"`
}

type SnapshotStore struct {
	mu         sync.RWMutex
	scope      string
	generation uint64
	last       *Snapshot
}

func NewStore(scope string) *SnapshotStore { return &SnapshotStore{scope: scope, generation: 1} }
func (s *SnapshotStore) Scope() (string, uint64) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.scope, s.generation
}
func (s *SnapshotStore) SetScope(scope string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.scope != scope {
		s.scope = scope
		s.generation++
		s.last = nil
	}
}
func (s *SnapshotStore) Accept(value Snapshot, generation uint64) error {
	if err := value.Validate(); err != nil {
		return err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if value.Scope != s.scope || generation != s.generation {
		return ErrScopeChanged
	}
	if s.last != nil && value.ObservedAt < s.last.ObservedAt {
		return ErrOldSnapshot
	}
	c := cloneSnapshot(value)
	s.last = &c
	return nil
}
func (s *SnapshotStore) Last() (Snapshot, bool) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	if s.last == nil {
		return Snapshot{}, false
	}
	return cloneSnapshot(*s.last), true
}
func (s *SnapshotStore) View(now time.Time, ttl time.Duration) View {
	if last, ok := s.Last(); ok {
		return last.ViewAt(now, ttl)
	}
	return View{Status: Unavailable, HostNow: now.Unix(), StaleAfterSeconds: int64(ttl / time.Second)}
}

// Clear invalidates in-flight fetches as well as the visible cache. Persist the
// resulting state before any later production process restores a cache.
// Revoke is authoritative even if an earlier timestamp was in the future.
func (s *SnapshotStore) Revoke(generation uint64, now time.Time) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if generation != s.generation {
		return ErrScopeChanged
	}
	s.last = &Snapshot{Schema: SchemaVersion, Scope: s.scope, Source: "auth-status", ObservedAt: now.Unix(), Status: Unauthorized}
	return nil
}

// withView makes publication and account switching linearizable: SetScope
// cannot return while an old account snapshot is still being sent.
func (s *SnapshotStore) withView(now time.Time, ttl time.Duration, fn func(View) error) error {
	s.mu.RLock()
	defer s.mu.RUnlock()
	v := View{Status: Unavailable, HostNow: now.Unix(), StaleAfterSeconds: int64(ttl / time.Second)}
	if s.last != nil {
		v = s.last.ViewAt(now, ttl)
	}
	return fn(v)
}
func (s *SnapshotStore) Clear() { s.mu.Lock(); defer s.mu.Unlock(); s.generation++; s.last = nil }
func (s *SnapshotStore) Save(path string) error {
	// Serialize account switching with persistence. Never use this on a shared,
	// world-writable directory; production Windows builds must set a user ACL.
	s.mu.RLock()
	defer s.mu.RUnlock()
	data, err := json.Marshal(cacheFile{Schema: SchemaVersion, Scope: s.scope, Snapshot: s.last})
	if err != nil {
		return err
	}
	if err = os.MkdirAll(filepath.Dir(path), 0700); err != nil {
		return err
	}
	f, err := os.CreateTemp(filepath.Dir(path), ".usage-*.tmp")
	if err != nil {
		return err
	}
	tmp := f.Name()
	defer os.Remove(tmp)
	if _, err = f.Write(data); err != nil {
		f.Close()
		return err
	}
	if err = f.Sync(); err != nil {
		f.Close()
		return err
	}
	if err = f.Close(); err != nil {
		return err
	}
	// On replacement failure retain the previous file; do NOT delete it first.
	// os.Rename is not promised to be atomic on every supported platform.
	return os.Rename(tmp, path)
}
func (s *SnapshotStore) Load(path string) error { return s.LoadAt(path, time.Now()) }
func (s *SnapshotStore) LoadAt(path string, now time.Time) error {
	_, generation := s.Scope()
	f, err := os.Open(path)
	if err != nil {
		return err
	}
	defer f.Close()
	var envelope cacheFile
	if err = decodeBounded(f, &envelope); err != nil {
		return err
	}
	s.mu.RLock()
	matches := envelope.Scope == s.scope && generation == s.generation
	s.mu.RUnlock()
	if envelope.Schema != SchemaVersion || !matches {
		return ErrScopeChanged
	}
	if envelope.Snapshot != nil {
		if envelope.Snapshot.ObservedAt-now.Unix() > MaxClockSkewSeconds {
			return errors.New("cached timestamp is ahead of clock")
		}
		if envelope.Snapshot.Scope != envelope.Scope {
			return ErrScopeChanged
		}
		return s.Accept(*envelope.Snapshot, generation)
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if generation != s.generation {
		return ErrScopeChanged
	}
	s.last = nil
	s.generation++
	return nil
}

// Reject duplicate object members and deep nesting before typed decoding. The
// standard JSON decoder otherwise accepts the last occurrence of a member.
func uniqueJSON(data []byte) error {
	dec := json.NewDecoder(bytes.NewReader(data))
	dec.UseNumber()
	var walk func(int) error
	walk = func(depth int) error {
		if depth > 32 {
			return errors.New("JSON nesting limit")
		}
		token, err := dec.Token()
		if err != nil {
			return errors.New("invalid JSON")
		}
		delim, ok := token.(json.Delim)
		if !ok {
			return nil
		}
		switch delim {
		case '{':
			seen := map[string]bool{}
			for dec.More() {
				key, err := dec.Token()
				if err != nil {
					return errors.New("invalid JSON key")
				}
				name, ok := key.(string)
				if !ok || seen[name] {
					return errors.New("duplicate or invalid JSON key")
				}
				seen[name] = true
				if err = walk(depth + 1); err != nil {
					return err
				}
			}
			end, err := dec.Token()
			if err != nil || end != json.Delim('}') {
				return errors.New("invalid JSON object")
			}
		case '[':
			for dec.More() {
				if err := walk(depth + 1); err != nil {
					return err
				}
			}
			end, err := dec.Token()
			if err != nil || end != json.Delim(']') {
				return errors.New("invalid JSON array")
			}
		default:
			return errors.New("invalid JSON delimiter")
		}
		return nil
	}
	if err := walk(0); err != nil {
		return err
	}
	if _, err := dec.Token(); err != io.EOF {
		return errors.New("trailing JSON")
	}
	return nil
}
