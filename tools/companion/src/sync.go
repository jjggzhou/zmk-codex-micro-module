package companion

import (
	"context"
	"errors"
	"sync"
	"time"
)

func unix(v int64) time.Time         { return time.Unix(v, 0) }
func duration(v int64) time.Duration { return time.Duration(v) * time.Second }

type Event string

const (
	Startup         Event = "startup"
	Timer           Event = "timer"
	Manual          Event = "manual"
	Wake            Event = "wake"
	NetworkRestored Event = "network_restored"
	Reconnected     Event = "reconnected"
)

var ErrBusy = errors.New("refresh already in progress")

type Policy struct {
	Poll          time.Duration
	StaleAfter    time.Duration
	MinRefreshGap time.Duration
	FetchTimeout  time.Duration
	SendTimeout   time.Duration
}

func DefaultPolicy() Policy {
	return Policy{10 * time.Minute, 30 * time.Minute, 10 * time.Second, 5 * time.Second, 2 * time.Second}
}
func (p Policy) validate() error {
	if p.Poll < time.Second || p.StaleAfter < p.Poll || p.StaleAfter > 24*time.Hour || p.MinRefreshGap < 0 || p.FetchTimeout <= 0 || p.SendTimeout <= 0 {
		return errors.New("invalid sync policy")
	}
	return nil
}

type Outcome struct {
	Fetched     bool  `json:"fetched"`
	Published   bool  `json:"published"`
	View        View  `json:"view"`
	NextFetchAt int64 `json:"next_fetch_at"`
}
type Coordinator struct {
	mu           sync.Mutex
	provider     Provider
	store        *SnapshotStore
	device       *DeviceClient
	policy       Policy
	selectedPool string
	nextFetch    time.Time
	lastAttempt  time.Time
	failures     uint
	generation   uint64
	authBlocked  bool
}

func NewCoordinator(p Provider, s *SnapshotStore, d *DeviceClient, policy Policy, selectedPool string) (*Coordinator, error) {
	if p == nil || s == nil || d == nil {
		return nil, errors.New("nil dependency")
	}
	if err := policy.validate(); err != nil {
		return nil, err
	}
	return &Coordinator{provider: p, store: s, device: d, policy: policy, selectedPool: selectedPool}, nil
}
func (c *Coordinator) backoff() time.Duration {
	n := c.failures
	if n > 5 {
		n = 5
	}
	c.failures++
	delay := 30 * time.Second * time.Duration(uint64(1)<<n)
	if delay > c.policy.Poll {
		delay = c.policy.Poll
	}
	return delay
}

// Step is a single scheduler iteration. Calls during an active iteration are
// coalesced (ErrBusy); the running refresh already serves that event. A tray
// shell can invoke Timer once a second and forward wake/manual/network events.
func (c *Coordinator) Step(ctx context.Context, now time.Time, event Event) (Outcome, error) {
	if !c.mu.TryLock() {
		return Outcome{}, ErrBusy
	}
	defer c.mu.Unlock()
	if !c.device.Connected() {
		return Outcome{View: c.store.View(now, c.policy.StaleAfter)}, ErrDisconnected
	}
	started := time.Now()
	scope, gen := c.store.Scope()
	scopeChanged := gen != c.generation
	if scopeChanged {
		c.generation = gen
		c.authBlocked = false
		c.nextFetch = time.Time{}
		c.lastAttempt = time.Time{}
		c.failures = 0
	}
	due := c.nextFetch.IsZero() || !now.Before(c.nextFetch)
	forced := event != Timer && (c.lastAttempt.IsZero() || now.Sub(c.lastAttempt) >= c.policy.MinRefreshGap)
	if c.authBlocked && event != Manual && !scopeChanged {
		due, forced = false, false
	}
	out := Outcome{}
	var fetchErr error
	if due || forced {
		c.lastAttempt = now
		fetchCtx, cancel := context.WithTimeout(ctx, c.policy.FetchTimeout)
		raw, err := c.provider.Fetch(fetchCtx, scope)
		if err == nil {
			err = fetchCtx.Err()
		}
		cancel()
		out.Fetched = true
		if err == nil {
			var normalized Snapshot
			normalized, err = Normalize(raw, c.selectedPool)
			if err == nil && raw.ObservedAt-now.Add(time.Since(started)).Unix() > MaxClockSkewSeconds {
				err = errors.New("provider clock is ahead")
			}
			if err == nil {
				err = c.store.Accept(normalized, gen)
			}
		}
		if errors.Is(err, ErrUnauthorized) {
			// Authentication failures are not quota observations, but invalidate
			// the old account state immediately. No periodic login retry loop.
			err = c.store.Revoke(gen, now.Add(time.Since(started)))
			c.authBlocked = true
		}
		if err != nil {
			fetchErr = err
			c.nextFetch = now.Add(c.backoff())
		} else {
			current := c.store.View(now, c.policy.StaleAfter)
			c.authBlocked = current.Status == Unauthorized
			if current.Status == WaitingReset || current.Status == Stale || current.Status == ClockInvalid || current.Status == Unavailable {
				c.nextFetch = now.Add(c.backoff())
			} else {
				c.failures = 0
				c.nextFetch = now.Add(c.policy.Poll)
				if current.Weekly != nil && current.Weekly.ResetAt != nil {
					resetQuery := unix(*current.Weekly.ResetAt).Add(2 * time.Second)
					if resetQuery.After(now) && resetQuery.Before(c.nextFetch) {
						c.nextFetch = resetQuery
					}
				}
			}
		}
	}
	now = now.Add(time.Since(started)) // Do not send the stale pre-fetch clock anchor.
	out.View = c.store.View(now, c.policy.StaleAfter)
	out.NextFetchAt = c.nextFetch.Unix()
	sendCtx, cancel := context.WithTimeout(ctx, c.policy.SendTimeout)
	defer cancel()
	// Hardware opening is deliberate and outside Step; no hidden discovery.
	var fetchDeviceErr error
	fetchDeviceErr = c.store.withView(now, c.policy.StaleAfter, func(v View) error {
		out.View = v
		var err error
		out.Published, err = c.device.Publish(sendCtx, v)
		return err
	})
	return out, errors.Join(fetchErr, fetchDeviceErr)
}

// SwitchScope clears the old device state immediately, even while Fetch is
// still running. The late result will fail its generation check.
func (c *Coordinator) SwitchScope(ctx context.Context, scope string, now time.Time) error {
	if scope == "" {
		return errors.New("empty scope")
	}
	c.store.SetScope(scope)
	sendCtx, cancel := context.WithTimeout(ctx, c.policy.SendTimeout)
	defer cancel()
	return c.store.withView(now, c.policy.StaleAfter, func(v View) error { _, err := c.device.Publish(sendCtx, v); return err })
}
