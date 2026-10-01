package tests

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"math"
	"os"
	"path/filepath"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	c "github.com/jjggzhou/zmk-codex-micro-module/tools/companion/src"
)

const epoch int64 = 1790856000

var ctx = context.Background()

func ptr[T any](v T) *T    { return &v }
func at(s int64) time.Time { return time.Unix(epoch+s, 0) }
func raw() c.RawSnapshot {
	return c.RawSnapshot{Schema: 1, Scope: "scope-A", Source: "fixture", Mode: "chatgpt", Plan: "plus", ObservedAt: epoch,
		Pools: []c.Pool{{ID: "codex", Windows: []c.Window{{DurationMinutes: ptr(c.WeekMinutes), UsedPercent: ptr(37.0), ResetAt: ptr(epoch + 3*86400)}}}}}
}
func normalized(t *testing.T, r c.RawSnapshot) c.Snapshot {
	t.Helper()
	s, e := c.Normalize(r, "")
	if e != nil {
		t.Fatal(e)
	}
	return s
}
func mustAccept(t *testing.T, store *c.SnapshotStore, s c.Snapshot) {
	t.Helper()
	_, g := store.Scope()
	if e := store.Accept(s, g); e != nil {
		t.Fatal(e)
	}
}
func connected(t *testing.T) (*c.DeviceClient, *c.FakeDevice) {
	t.Helper()
	f := &c.FakeDevice{}
	d := c.NewDeviceClient(f)
	if e := d.Connect(ctx); e != nil {
		t.Fatal(e)
	}
	t.Cleanup(func() { d.Close() })
	return d, f
}

type providerFunc func(context.Context, string) (c.RawSnapshot, error)

func (f providerFunc) Fetch(ctx context.Context, s string) (c.RawSnapshot, error) { return f(ctx, s) }
func engine(t *testing.T, p c.Provider, store *c.SnapshotStore, d *c.DeviceClient) *c.Coordinator {
	t.Helper()
	e, err := c.NewCoordinator(p, store, d, c.DefaultPolicy(), "")
	if err != nil {
		t.Fatal(err)
	}
	return e
}

func TestPlanNamesDoNotDetermineQuota(t *testing.T) {
	for _, plan := range []string{"free", "go", "plus", "pro", "pro-100", "pro-200", "business", "enterprise", "edu", "future-plan", ""} {
		t.Run(plan, func(t *testing.T) {
			r := raw()
			r.Plan = plan
			s := normalized(t, r)
			v := s.ViewAt(at(0), 30*time.Minute)
			if v.Label() != "WEEK 63%" || v.Status != c.Ready {
				t.Fatalf("%+v", v)
			}
		})
	}
}
func TestUnknownIsNotZero(t *testing.T) {
	cases := []struct {
		name    string
		change  func(*c.RawSnapshot)
		state   c.Status
		wantErr bool
	}{
		{"unauthenticated", func(r *c.RawSnapshot) { r.Mode = "unauthenticated" }, c.Unauthorized, false},
		{"API key", func(r *c.RawSnapshot) { r.Mode = "api_key" }, c.Unsupported, false},
		{"external provider", func(r *c.RawSnapshot) { r.Mode = "external" }, c.Unsupported, false},
		{"unknown mode", func(r *c.RawSnapshot) { r.Mode = "new-provider" }, c.Unavailable, true},
		{"no windows", func(r *c.RawSnapshot) { r.Pools = nil }, c.NoWeekly, false},
		{"short window", func(r *c.RawSnapshot) { r.Pools[0].Windows[0].DurationMinutes = ptr(300) }, c.NoWeekly, false},
		{"missing duration", func(r *c.RawSnapshot) { r.Pools[0].Windows[0].DurationMinutes = nil }, c.NoWeekly, false},
		{"missing percent", func(r *c.RawSnapshot) { r.Pools[0].Windows[0].UsedPercent = nil }, c.Unavailable, true},
		{"wrong schema", func(r *c.RawSnapshot) { r.Schema = 99 }, c.Unavailable, true},
		{"missing timestamp", func(r *c.RawSnapshot) { r.ObservedAt = 0 }, c.Unavailable, true},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			r := raw()
			tc.change(&r)
			s, e := c.Normalize(r, "")
			if (e != nil) != tc.wantErr || s.Status != tc.state || s.Weekly != nil {
				t.Fatalf("%+v %v", s, e)
			}
			if s.ViewAt(at(0), 30*time.Minute).Label() != "WEEK --" {
				t.Fatal("unknown became a number")
			}
		})
	}
}
func TestPercentageEdges(t *testing.T) {
	for _, tc := range []struct {
		used  float64
		label string
	}{{0, "WEEK 100%"}, {37, "WEEK 63%"}, {100, "WEEK 0%"}, {99.6, "WEEK <1%"}, {99.99999, "WEEK <1%"}} {
		t.Run(tc.label, func(t *testing.T) {
			r := raw()
			r.Pools[0].Windows[0].UsedPercent = ptr(tc.used)
			if got := normalized(t, r).ViewAt(at(0), 30*time.Minute).Label(); got != tc.label {
				t.Fatal(got)
			}
		})
	}
	for _, v := range []float64{-1, 101, math.NaN(), math.Inf(1), math.Inf(-1)} {
		r := raw()
		r.Pools[0].Windows[0].UsedPercent = ptr(v)
		if _, e := c.Normalize(r, ""); e == nil {
			t.Fatalf("accepted %v", v)
		}
	}
}
func TestWindowAndPoolSelection(t *testing.T) {
	r := raw()
	r.Pools[0].Windows = append([]c.Window{{DurationMinutes: ptr(300), UsedPercent: ptr(91.0), ResetAt: ptr(epoch + 20)}}, r.Pools[0].Windows...)
	s := normalized(t, r)
	if s.Weekly.RemainingBPS != 6300 || *s.Weekly.ResetAt != epoch+3*86400 {
		t.Fatal("mixed two windows")
	}
	r.Pools = append(r.Pools, c.Pool{ID: "other", Windows: r.Pools[0].Windows})
	for _, selection := range []string{"", "missing"} {
		s, e := c.Normalize(r, selection)
		if e != nil || s.Status != c.SelectionRequired {
			t.Fatalf("%+v %v", s, e)
		}
	}
	s, e := c.Normalize(r, "other")
	if e != nil || s.PoolID != "other" || s.Weekly.RemainingBPS != 6300 {
		t.Fatal(s, e)
	}
	r.Pools[1].ID = "codex"
	if _, e = c.Normalize(r, ""); e == nil {
		t.Fatal("duplicate pool IDs accepted")
	}
	r = raw()
	r.Pools[0].Windows = append(r.Pools[0].Windows, r.Pools[0].Windows[0])
	if normalized(t, r).Status != c.SelectionRequired {
		t.Fatal("duplicate weekly windows selected arbitrarily")
	}
}
func TestCreditsDoNotBecomeWeeklyAllowance(t *testing.T) {
	r := raw()
	r.Pools[0].Credits = &c.Credits{Available: ptr(true), Unlimited: ptr(true), Balance: ptr("5000")}
	r.Pools[0].Windows[0].UsedPercent = ptr(100.0)
	if normalized(t, r).ViewAt(at(0), 30*time.Minute).Label() != "WEEK 0%" {
		t.Fatal("credits changed percentage")
	}
	r.Pools[0].Windows = nil
	if normalized(t, r).Status != c.NoWeekly {
		t.Fatal("invented weekly window")
	}
}
func TestTimeDerivedEveryRead(t *testing.T) {
	s := normalized(t, raw())
	for _, tc := range []struct {
		offset int64
		status c.Status
	}{{0, c.Ready}, {1799, c.Ready}, {1800, c.Stale}, {2000, c.Stale}, {-61, c.ClockInvalid}} {
		v := s.ViewAt(at(tc.offset), 30*time.Minute)
		if v.Status != tc.status {
			t.Fatalf("%d %+v", tc.offset, v)
		}
		if tc.status != c.Ready && v.Weekly != nil {
			t.Fatal("invalid state retained percentage")
		}
	}
	s.Weekly.ResetAt = ptr(epoch + 30)
	if s.ViewAt(at(29), 30*time.Minute).Status != c.Ready || s.ViewAt(at(30), 30*time.Minute).Status != c.WaitingReset {
		t.Fatal("reset boundary")
	}
	if s.ViewAt(at(31), 30*time.Minute).Label() != "WEEK --" {
		t.Fatal("reset invented 100 percent")
	}
	s.Weekly.ResetAt = nil
	if v := s.ViewAt(at(0), 30*time.Minute); v.Label() != "WEEK 63%" || v.ResetIn() != nil {
		t.Fatal("missing reset invalidated percentage")
	}
	for _, zone := range []int{-7 * 3600, 0, 8 * 3600} {
		v := s.ViewAt(at(0).In(time.FixedZone("test", zone)), 30*time.Minute)
		if v.Label() != "WEEK 63%" {
			t.Fatal("timezone changed quota")
		}
	}
	for _, bad := range []int64{0, -1, 253402300800} {
		r := raw()
		r.Pools[0].Windows[0].ResetAt = ptr(bad)
		if _, e := c.Normalize(r, ""); e == nil {
			t.Fatal("invalid reset accepted")
		}
	}
}
func TestStoreAccountGenerationAndOwnership(t *testing.T) {
	store := c.NewStore("scope-A")
	_, g := store.Scope()
	s := normalized(t, raw())
	mustAccept(t, store, s)
	s.Weekly.RemainingBPS = 123
	last, _ := store.Last()
	last.Weekly.RemainingBPS = 456
	if store.View(at(0), 30*time.Minute).Label() != "WEEK 63%" {
		t.Fatal("mutable cache alias")
	}
	old := normalized(t, raw())
	old.ObservedAt--
	if !errors.Is(store.Accept(old, g), c.ErrOldSnapshot) {
		t.Fatal("accepted old observation")
	}
	store.SetScope("scope-B")
	if store.View(at(0), 30*time.Minute).Label() != "WEEK --" {
		t.Fatal("account leaked")
	}
	if !errors.Is(store.Accept(normalized(t, raw()), g), c.ErrScopeChanged) {
		t.Fatal("late response accepted")
	}
	store.SetScope("scope-A")
	if !errors.Is(store.Accept(normalized(t, raw()), g), c.ErrScopeChanged) {
		t.Fatal("A-B-A revived old generation")
	}
}
func TestPersistenceKeepsTimestampAndTombstone(t *testing.T) {
	file := filepath.Join(t.TempDir(), "snapshot.json")
	store := c.NewStore("scope-A")
	mustAccept(t, store, normalized(t, raw()))
	if e := store.Save(file); e != nil {
		t.Fatal(e)
	}
	loaded := c.NewStore("scope-A")
	if e := loaded.LoadAt(file, at(0)); e != nil {
		t.Fatal(e)
	}
	if loaded.View(at(1800), 30*time.Minute).Status != c.Stale {
		t.Fatal("cache load renewed freshness")
	}
	if e := c.NewStore("scope-B").LoadAt(file, at(0)); !errors.Is(e, c.ErrScopeChanged) {
		t.Fatal("loaded wrong account", e)
	}
	store.Clear()
	if e := store.Save(file); e != nil {
		t.Fatal(e)
	}
	if e := loaded.LoadAt(file, at(0)); e != nil {
		t.Fatal(e)
	}
	if _, ok := loaded.Last(); ok {
		t.Fatal("clear restored prior snapshot")
	}
	content, _ := os.ReadFile(file)
	if strings.Contains(string(content), "remaining_bps") {
		t.Fatal("tombstone retained old quota")
	}
}
func TestFixtureIsBoundedAndNeverRestamped(t *testing.T) {
	dir := t.TempDir()
	file := filepath.Join(dir, "fixture.json")
	data, _ := json.Marshal(raw())
	if e := os.WriteFile(file, data, 0600); e != nil {
		t.Fatal(e)
	}
	p := c.JSONFileProvider{Path: file}
	r, e := p.Fetch(ctx, "scope-A")
	if e != nil || r.ObservedAt != epoch {
		t.Fatal(r, e)
	}
	if _, e = p.Fetch(ctx, "scope-B"); !errors.Is(e, c.ErrScopeChanged) {
		t.Fatal("wrong scope accepted")
	}
	for _, bad := range []string{"{", string(data) + "{}", `{"unexpected":true}`, strings.Repeat(" ", c.MaxSnapshotBytes+1)} {
		os.WriteFile(file, []byte(bad), 0600)
		if _, e = p.Fetch(ctx, "scope-A"); e == nil {
			t.Fatal("invalid fixture accepted")
		}
	}
	canceled, cancel := context.WithCancel(ctx)
	cancel()
	if _, e = p.Fetch(canceled, "scope-A"); !errors.Is(e, context.Canceled) {
		t.Fatal(e)
	}
	if _, e = (c.JSONFileProvider{Path: dir}).Fetch(ctx, "scope-A"); e == nil {
		t.Fatal("directory accepted")
	}
}
func TestClientDedupDisconnectAndRestore(t *testing.T) {
	d, f := connected(t)
	s := normalized(t, raw())
	v := s.ViewAt(at(0), 30*time.Minute)
	if sent, e := d.Publish(ctx, v); e != nil || !sent {
		t.Fatal(sent, e)
	}
	if sent, e := d.Publish(ctx, s.ViewAt(at(60), 30*time.Minute)); e != nil || sent {
		t.Fatal("clock-only re-send", e)
	}
	if f.ViewAt(1800).Status != c.Stale {
		t.Fatal("device did not expire while host was absent")
	}
	wire := f.LastWire()
	for _, secret := range []string{"scope-A", "fixture", "plus", "credits"} {
		if bytes.Contains(wire, []byte(secret)) {
			t.Fatal("host metadata on wire")
		}
	}
	f.Close()
	if _, e := d.Publish(ctx, v); !errors.Is(e, c.ErrDisconnected) {
		t.Fatal("dedup masked disconnect", e)
	}
	if e := d.Connect(ctx); e != nil {
		t.Fatal(e)
	}
	if sent, e := d.Publish(ctx, v); e != nil || !sent {
		t.Fatal("reconnect did not resend", e)
	}
	if _, e := f.RoundTrip(ctx, wire); e == nil {
		t.Fatal("previous session replay accepted")
	}
	if sent, e := d.Publish(ctx, s.ViewAt(at(1800), 30*time.Minute)); e != nil || !sent {
		t.Fatal("expiry not published", e)
	}
}
func TestReplayAndSequence(t *testing.T) {
	d, f := connected(t)
	s := normalized(t, raw())
	d.Publish(ctx, s.ViewAt(at(0), 30*time.Minute))
	first := f.LastWire()
	count := f.Applied()
	if _, e := f.RoundTrip(ctx, first); e != nil || f.Applied() != count {
		t.Fatal("replay was not idempotent", e)
	}
	s.ObservedAt++
	d.Publish(ctx, s.ViewAt(at(1), 30*time.Minute))
	if _, e := f.RoundTrip(ctx, first); e == nil {
		t.Fatal("old sequence accepted")
	}
	var obj map[string]any
	json.Unmarshal(f.LastWire(), &obj)
	params := obj["params"].(map[string]any)
	view := params["view"].(map[string]any)
	view["observed_at"] = float64(epoch + 2)
	view["host_now"] = float64(epoch + 2)
	changed, _ := json.Marshal(obj)
	if _, e := f.RoundTrip(ctx, changed); e == nil {
		t.Fatal("same sequence changed data")
	}
}

type alteredTransport struct {
	*c.FakeDevice
	edit func(string, []byte) ([]byte, error)
}

func (a *alteredTransport) RoundTrip(ctx context.Context, b []byte) ([]byte, error) {
	r, e := a.FakeDevice.RoundTrip(ctx, b)
	if e != nil {
		return r, e
	}
	var q struct {
		Method string `json:"method"`
	}
	json.Unmarshal(b, &q)
	return a.edit(q.Method, r)
}
func editJSON(b []byte, edit func(map[string]any)) []byte {
	var m map[string]any
	json.Unmarshal(b, &m)
	edit(m)
	out, _ := json.Marshal(m)
	return out
}
func TestReplyValidation(t *testing.T) {
	for _, name := range []string{"wrong-id", "wrong-session", "wrong-sequence", "not-applied", "oversized", "invalid-json"} {
		t.Run(name, func(t *testing.T) {
			tr := &alteredTransport{FakeDevice: &c.FakeDevice{}, edit: func(method string, b []byte) ([]byte, error) {
				if method == "micro.capabilities" {
					return b, nil
				}
				switch name {
				case "oversized":
					return bytes.Repeat([]byte(" "), c.MaxMessageBytes+1), nil
				case "invalid-json":
					return []byte("{"), nil
				default:
					return editJSON(b, func(m map[string]any) {
						r := m["result"].(map[string]any)
						switch name {
						case "wrong-id":
							m["id"] = 999
						case "wrong-session":
							r["session"] = "other"
						case "wrong-sequence":
							r["sequence"] = 999
						case "not-applied":
							r["applied"] = false
						}
					}), nil
				}
			}}
			d := c.NewDeviceClient(tr)
			defer d.Close()
			if e := d.Connect(ctx); e != nil {
				t.Fatal(e)
			}
			if _, e := d.Publish(ctx, normalized(t, raw()).ViewAt(at(0), 30*time.Minute)); e == nil {
				t.Fatal("accepted bad reply")
			}
		})
	}
}
func TestHandshakeAndDeviceBinding(t *testing.T) {
	for _, key := range []string{"isolated", "schema_version", "session", "max_message_bytes"} {
		t.Run(key, func(t *testing.T) {
			tr := &alteredTransport{FakeDevice: &c.FakeDevice{}, edit: func(method string, b []byte) ([]byte, error) {
				return editJSON(b, func(m map[string]any) {
					r := m["result"].(map[string]any)
					switch key {
					case "isolated":
						r[key] = false
					case "schema_version":
						r[key] = 99
					case "session":
						r[key] = ""
					case "max_message_bytes":
						r[key] = 64
					}
				}), nil
			}}
			d := c.NewDeviceClient(tr)
			if e := d.Connect(ctx); !errors.Is(e, c.ErrIncompatible) {
				t.Fatal(e)
			}
			if tr.Connected() {
				t.Fatal("failed handshake leaked connection")
			}
		})
	}
	d := c.NewDeviceClient(&c.FakeDevice{})
	d.BindDevice("not-this-device")
	if e := d.Connect(ctx); !errors.Is(e, c.ErrIncompatible) {
		t.Fatal("bound identity ignored", e)
	}
}
func TestLostAckDoesNotSuppressRetry(t *testing.T) {
	dropped := false
	tr := &alteredTransport{FakeDevice: &c.FakeDevice{}, edit: func(m string, b []byte) ([]byte, error) {
		if m == "micro.usage.set" && !dropped {
			dropped = true
			return nil, context.DeadlineExceeded
		}
		return b, nil
	}}
	d := c.NewDeviceClient(tr)
	defer d.Close()
	d.Connect(ctx)
	s := normalized(t, raw())
	if _, e := d.Publish(ctx, s.ViewAt(at(0), 30*time.Minute)); e == nil {
		t.Fatal("lost ack succeeded")
	}
	if sent, e := d.Publish(ctx, s.ViewAt(at(10), 30*time.Minute)); e != nil || !sent {
		t.Fatal(sent, e)
	}
	if tr.ViewAt(1790).Status != c.Stale {
		t.Fatal("retry renewed quota age")
	}
}
func TestSchedulerAndNoDevicePause(t *testing.T) {
	calls := 0
	p := providerFunc(func(_ context.Context, _ string) (c.RawSnapshot, error) { calls++; return raw(), nil })
	store := c.NewStore("scope-A")
	d, f := connected(t)
	e := engine(t, p, store, d)
	if o, err := e.Step(ctx, at(0), c.Startup); err != nil || !o.Fetched || !o.Published {
		t.Fatal(o, err)
	}
	if o, err := e.Step(ctx, at(1), c.Manual); err != nil || o.Fetched || o.Published {
		t.Fatal("burst not deduped", o, err)
	}
	if o, err := e.Step(ctx, at(599), c.Timer); err != nil || o.Fetched {
		t.Fatal(o, err)
	}
	if o, err := e.Step(ctx, at(600), c.Timer); err != nil || !o.Fetched || o.Published {
		t.Fatal("same observation resent", o, err)
	}
	if calls != 2 {
		t.Fatal(calls)
	}
	f.Close()
	if _, err := e.Step(ctx, at(2000), c.Timer); !errors.Is(err, c.ErrDisconnected) {
		t.Fatal(err)
	}
	if calls != 2 {
		t.Fatal("fetched without device")
	}
}
func TestResetBoundaryRefreshAndBackoff(t *testing.T) {
	r := raw()
	r.Pools[0].Windows[0].ResetAt = ptr(epoch + 30)
	p := providerFunc(func(context.Context, string) (c.RawSnapshot, error) { return r, nil })
	d, _ := connected(t)
	e := engine(t, p, c.NewStore("scope-A"), d)
	o, err := e.Step(ctx, at(0), c.Startup)
	if err != nil || o.NextFetchAt != epoch+32 {
		t.Fatal(o, err)
	}
	o, err = e.Step(ctx, at(30), c.Timer)
	if err != nil || o.Fetched || o.View.Status != c.WaitingReset {
		t.Fatal(o, err)
	}
	o, err = e.Step(ctx, at(32), c.Timer)
	if err != nil || !o.Fetched || o.View.Status != c.WaitingReset || o.NextFetchAt != epoch+62 {
		t.Fatal(o, err)
	}
	o, err = e.Step(ctx, at(33), c.Timer)
	if err != nil || o.Fetched {
		t.Fatal("reset causes busy retry", o, err)
	}
	r.ObservedAt = epoch + 62
	r.Pools[0].Windows[0].ResetAt = ptr(epoch + 7*86400)
	r.Pools[0].Windows[0].UsedPercent = ptr(0.0)
	o, err = e.Step(ctx, at(62), c.Timer)
	if err != nil || o.View.Label() != "WEEK 100%" {
		t.Fatal(o, err)
	}
}
func TestFailuresAndUnauthorized(t *testing.T) {
	calls := 0
	failure := error(nil)
	p := providerFunc(func(context.Context, string) (c.RawSnapshot, error) { calls++; return raw(), failure })
	store := c.NewStore("scope-A")
	d, _ := connected(t)
	e := engine(t, p, store, d)
	e.Step(ctx, at(0), c.Startup)
	failure = errors.New("network offline")
	o, err := e.Step(ctx, at(600), c.Timer)
	if err == nil || o.NextFetchAt != epoch+630 || o.View.Status != c.Ready {
		t.Fatal(o, err)
	}
	o, _ = e.Step(ctx, at(1801), c.Timer)
	if o.View.Status != c.Stale || o.View.Weekly != nil {
		t.Fatal("offline cache remained current")
	}
	failure = c.ErrUnauthorized
	o, err = e.Step(ctx, at(2000), c.Manual)
	if err != nil || o.View.Status != c.Unauthorized {
		t.Fatal(o, err)
	}
	count := calls
	o, err = e.Step(ctx, at(9000), c.Timer)
	if err != nil || o.Fetched || calls != count {
		t.Fatal("auth retry loop", o, err)
	}
}
func TestFutureSnapshotDoesNotPoisonStore(t *testing.T) {
	r := raw()
	r.ObservedAt = epoch + 10000
	p := providerFunc(func(context.Context, string) (c.RawSnapshot, error) { return r, nil })
	store := c.NewStore("scope-A")
	d, _ := connected(t)
	e := engine(t, p, store, d)
	if _, err := e.Step(ctx, at(0), c.Startup); err == nil {
		t.Fatal("future sample accepted")
	}
	if _, ok := store.Last(); ok {
		t.Fatal("poisoned cache")
	}
	r = raw()
	if o, err := e.Step(ctx, at(30), c.Timer); err != nil || o.View.Status != c.Ready {
		t.Fatal(o, err)
	}
}
func TestConcurrentRefreshAndAccountSwitch(t *testing.T) {
	entered := make(chan struct{})
	release := make(chan struct{})
	var calls atomic.Int32
	p := providerFunc(func(callCtx context.Context, _ string) (c.RawSnapshot, error) {
		calls.Add(1)
		close(entered)
		select {
		case <-release:
			return raw(), nil
		case <-callCtx.Done():
			return c.RawSnapshot{}, callCtx.Err()
		}
	})
	store := c.NewStore("scope-A")
	d, f := connected(t)
	mustAccept(t, store, normalized(t, raw()))
	d.Publish(ctx, store.View(at(0), 30*time.Minute))
	e := engine(t, p, store, d)
	done := make(chan error, 1)
	go func() { _, err := e.Step(ctx, at(0), c.Startup); done <- err }()
	<-entered
	if _, err := e.Step(ctx, at(0), c.Manual); !errors.Is(err, c.ErrBusy) {
		t.Fatal("overlapping fetch", err)
	}
	if err := e.SwitchScope(ctx, "scope-B", at(0)); err != nil {
		t.Fatal(err)
	}
	if f.ViewAt(0).Label() != "WEEK --" {
		t.Fatal("account switch did not clear device")
	}
	close(release)
	if err := <-done; !errors.Is(err, c.ErrScopeChanged) {
		t.Fatal("late result accepted", err)
	}
	if calls.Load() != 1 || store.View(at(0), 30*time.Minute).Label() != "WEEK --" {
		t.Fatal("old account leaked")
	}
}
func TestCooperativeFetchTimeout(t *testing.T) {
	p := providerFunc(func(ctx context.Context, _ string) (c.RawSnapshot, error) {
		<-ctx.Done()
		return c.RawSnapshot{}, ctx.Err()
	})
	d, _ := connected(t)
	policy := c.DefaultPolicy()
	policy.FetchTimeout = time.Millisecond
	e, err := c.NewCoordinator(p, c.NewStore("scope-A"), d, policy, "")
	if err != nil {
		t.Fatal(err)
	}
	if _, err = e.Step(ctx, at(0), c.Startup); !errors.Is(err, context.DeadlineExceeded) {
		t.Fatal(err)
	}
}

func TestMissingNumericAndDuplicateMembers(t *testing.T) {
	for _, raw := range []string{`{}`, `{"remaining_bps":null}`, `{"remaining_bps":-1}`} {
		var w c.Weekly
		if json.Unmarshal([]byte(raw), &w) == nil {
			t.Fatal("missing/invalid numeric value became quota", raw)
		}
	}
	file := filepath.Join(t.TempDir(), "duplicate.json")
	b, _ := json.Marshal(raw())
	b = bytes.Replace(b, []byte(`"used_percent":37`), []byte(`"used_percent":37,"used_percent":100`), 1)
	os.WriteFile(file, b, 0600)
	if _, err := (c.JSONFileProvider{Path: file}).Fetch(ctx, "scope-A"); err == nil {
		t.Fatal("accepted ambiguous fixture")
	}
	fake := &c.FakeDevice{}
	fake.Open(ctx)
	defer fake.Close()
	if _, err := fake.RoundTrip(ctx, []byte(`{"jsonrpc":"2.0","id":1,"id":2,"method":"micro.capabilities","params":null}`)); err == nil {
		t.Fatal("accepted duplicate RPC ID")
	}
}
func TestFutureCacheRejectedAndRevocationWins(t *testing.T) {
	store := c.NewStore("scope-A")
	r := raw()
	r.ObservedAt = epoch + 10000
	mustAccept(t, store, normalized(t, r))
	file := filepath.Join(t.TempDir(), "future.json")
	if err := store.Save(file); err != nil {
		t.Fatal(err)
	}
	loaded := c.NewStore("scope-A")
	if loaded.LoadAt(file, at(0)) == nil {
		t.Fatal("future cache poisoned store")
	}
	_, gen := store.Scope()
	if err := store.Revoke(gen, at(0)); err != nil {
		t.Fatal(err)
	}
	if store.View(at(0), 30*time.Minute).Status != c.Unauthorized {
		t.Fatal("timestamp blocked revocation")
	}
}
