// Package companion is the hook-free, platform-neutral quota synchronization core.
// It has no HID backend and never reads Codex credentials or conversation files.
package companion

import (
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"time"
)

const SchemaVersion = 1
const WeekMinutes = 7 * 24 * 60
const MaxClockSkewSeconds int64 = 60

type Status string

const (
	Ready             Status = "ready"
	Unavailable       Status = "unavailable"
	Unauthorized      Status = "unauthorized"
	Unsupported       Status = "unsupported"
	NoWeekly          Status = "no_weekly"
	SelectionRequired Status = "selection_required"
	Stale             Status = "stale"
	WaitingReset      Status = "waiting_reset"
	ClockInvalid      Status = "clock_invalid"
)

// RawSnapshot is OUR provider contract, not the Codex app-server wire schema.
// Scope is a local opaque account/workspace identity; it must never be a token.
type RawSnapshot struct {
	Schema     int    `json:"schema_version"`
	Scope      string `json:"scope"`
	Source     string `json:"source"`
	Mode       string `json:"mode"`           // chatgpt, api_key, external, unauthenticated
	Plan       string `json:"plan,omitempty"` // diagnostic metadata, NEVER a selection rule
	ObservedAt int64  `json:"observed_at"`
	Pools      []Pool `json:"pools"`
}
type Pool struct {
	ID      string   `json:"id"`
	Windows []Window `json:"windows"`
	Credits *Credits `json:"credits,omitempty"`
}
type Credits struct {
	Available *bool   `json:"available,omitempty"`
	Unlimited *bool   `json:"unlimited,omitempty"`
	Balance   *string `json:"balance,omitempty"`
}
type Window struct {
	DurationMinutes *int     `json:"duration_minutes"`
	UsedPercent     *float64 `json:"used_percent"`
	ResetAt         *int64   `json:"reset_at"`
}
type Weekly struct {
	// 0..10000 basis points of remaining quota. Positive sub-1% is not exhaustion.
	RemainingBPS int    `json:"remaining_bps"`
	ResetAt      *int64 `json:"reset_at"`
}
type Snapshot struct {
	Schema     int     `json:"schema_version"`
	Scope      string  `json:"scope"`
	Source     string  `json:"source"`
	ObservedAt int64   `json:"observed_at"`
	Status     Status  `json:"status"`
	PoolID     string  `json:"pool_id,omitempty"`
	Weekly     *Weekly `json:"weekly"`
}

// View is the only quota state sent to the device; no account, plan or credits.
type View struct {
	Status            Status  `json:"status"`
	ObservedAt        int64   `json:"observed_at"`
	HostNow           int64   `json:"host_now"`
	StaleAfterSeconds int64   `json:"stale_after_s"`
	Weekly            *Weekly `json:"weekly"`
}

func baseStatus(s Status) bool {
	switch s {
	case Ready, Unavailable, Unauthorized, Unsupported, NoWeekly, SelectionRequired:
		return true
	}
	return false
}
func validUnix(v int64) bool { return v > 0 && v <= 253402300799 } // year 9999 bound
func cloneWeekly(w *Weekly) *Weekly {
	if w == nil {
		return nil
	}
	c := *w
	if w.ResetAt != nil {
		v := *w.ResetAt
		c.ResetAt = &v
	}
	return &c
}
func cloneSnapshot(s Snapshot) Snapshot { s.Weekly = cloneWeekly(s.Weekly); return s }

// Normalize selects by the observed window duration, NEVER by the plan name or
// by assuming primary/secondary means short/weekly. Ambiguity is fail-closed.
func Normalize(raw RawSnapshot, selectedPool string) (Snapshot, error) {
	s := Snapshot{Schema: SchemaVersion, Scope: raw.Scope, Source: raw.Source,
		ObservedAt: raw.ObservedAt, Status: Unavailable}
	bad := func() (Snapshot, error) { return s, errors.New("invalid provider snapshot") }
	if raw.Schema != SchemaVersion || raw.Scope == "" || raw.Source == "" || !validUnix(raw.ObservedAt) {
		return bad()
	}
	switch raw.Mode {
	case "unauthenticated":
		s.Status = Unauthorized
		return s, nil
	case "api_key", "external":
		s.Status = Unsupported
		return s, nil
	case "chatgpt":
	default:
		return bad()
	}
	seen := map[string]bool{}
	type candidate struct {
		pool   string
		window Window
	}
	var candidates []candidate
	selectedFound := selectedPool == ""
	for _, p := range raw.Pools {
		if p.ID == "" || seen[p.ID] {
			return bad()
		}
		seen[p.ID] = true
		if selectedPool != "" && p.ID != selectedPool {
			continue
		}
		selectedFound = true
		for _, w := range p.Windows {
			if w.DurationMinutes != nil && *w.DurationMinutes == WeekMinutes {
				candidates = append(candidates, candidate{p.ID, w})
			}
		}
	}
	if !selectedFound || len(candidates) > 1 {
		s.Status = SelectionRequired
		return s, nil
	}
	if len(candidates) == 0 {
		s.Status = NoWeekly
		return s, nil
	}
	c := candidates[0]
	w := c.window
	if w.UsedPercent == nil || math.IsNaN(*w.UsedPercent) || math.IsInf(*w.UsedPercent, 0) || *w.UsedPercent < 0 || *w.UsedPercent > 100 {
		return bad()
	}
	if w.ResetAt != nil && !validUnix(*w.ResetAt) {
		return bad()
	}
	remaining := 100 - *w.UsedPercent
	bps := int(math.Round(remaining * 100))
	if remaining > 0 && bps == 0 {
		bps = 1
	}
	s.Status, s.PoolID = Ready, c.pool
	s.Weekly = cloneWeekly(&Weekly{RemainingBPS: bps, ResetAt: w.ResetAt})
	return s, nil
}

func (s Snapshot) Validate() error {
	if s.Schema != SchemaVersion || s.Scope == "" || s.Source == "" || !validUnix(s.ObservedAt) || !baseStatus(s.Status) {
		return errors.New("invalid cached snapshot")
	}
	if s.Status != Ready {
		if s.Weekly != nil {
			return errors.New("unexpected weekly data")
		}
		return nil
	}
	if s.Weekly == nil || s.Weekly.RemainingBPS < 0 || s.Weekly.RemainingBPS > 10000 {
		return errors.New("invalid remaining quota")
	}
	if s.Weekly.ResetAt != nil && !validUnix(*s.Weekly.ResetAt) {
		return errors.New("invalid reset time")
	}
	return nil
}

// ViewAt is deliberately NOT cached: time-dependent states must advance even
// when the underlying file, quota value and connection have not changed.
func (s Snapshot) ViewAt(now time.Time, staleAfter time.Duration) View {
	v := View{Status: s.Status, ObservedAt: s.ObservedAt, HostNow: now.Unix(), StaleAfterSeconds: int64(staleAfter / time.Second)}
	if s.Validate() != nil || staleAfter < time.Second || staleAfter > 24*time.Hour {
		v.Status = Unavailable
		return v
	}
	if !validUnix(now.Unix()) || s.ObservedAt-now.Unix() > MaxClockSkewSeconds {
		v.Status = ClockInvalid
		return v
	}
	if s.Status == Unauthorized || s.Status == Unsupported {
		return v
	}
	if s.Status == Ready && s.Weekly.ResetAt != nil && now.Unix() >= *s.Weekly.ResetAt {
		v.Status = WaitingReset
		return v
	}
	if now.Unix()-s.ObservedAt >= v.StaleAfterSeconds {
		v.Status = Stale
		return v
	}
	v.Weekly = cloneWeekly(s.Weekly)
	return v
}

func (v View) Validate() error {
	if !validUnix(v.HostNow) || v.StaleAfterSeconds < 1 || v.StaleAfterSeconds > 86400 {
		return errors.New("invalid view time")
	}
	if v.Status != Ready {
		switch v.Status {
		case Unavailable, Unauthorized, Unsupported, NoWeekly, SelectionRequired, Stale, WaitingReset, ClockInvalid:
			if v.Weekly == nil {
				return nil
			}
		}
		return errors.New("invalid unavailable view")
	}
	s := Snapshot{Schema: SchemaVersion, Scope: "wire", Source: "wire", Status: Ready, ObservedAt: v.ObservedAt, Weekly: v.Weekly}
	if s.Validate() != nil || s.ViewAt(time.Unix(v.HostNow, 0), time.Duration(v.StaleAfterSeconds)*time.Second).Status != Ready {
		return errors.New("invalid ready view")
	}
	return nil
}

func (v View) Label() string {
	if v.Status != Ready || v.Weekly == nil {
		return "WEEK --"
	}
	if v.Weekly.RemainingBPS > 0 && v.Weekly.RemainingBPS < 100 {
		return "WEEK <1%"
	}
	return fmt.Sprintf("WEEK %d%%", v.Weekly.RemainingBPS/100)
}
func (v View) ResetIn() *int64 {
	if v.Status != Ready || v.Weekly == nil || v.Weekly.ResetAt == nil {
		return nil
	}
	remaining := *v.Weekly.ResetAt - v.HostNow
	if remaining <= 0 {
		return nil
	}
	return &remaining
}

// A missing numeric field must never decode as a real zero-percent quota.
func (w *Weekly) UnmarshalJSON(data []byte) error {
	var raw struct {
		Remaining *int   `json:"remaining_bps"`
		Reset     *int64 `json:"reset_at"`
	}
	if err := json.Unmarshal(data, &raw); err != nil {
		return err
	}
	if raw.Remaining == nil || *raw.Remaining < 0 || *raw.Remaining > 10000 {
		return errors.New("missing or invalid remaining_bps")
	}
	if raw.Reset != nil && !validUnix(*raw.Reset) {
		return errors.New("invalid reset time")
	}
	w.RemainingBPS, w.ResetAt = *raw.Remaining, raw.Reset
	return nil
}
