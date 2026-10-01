// A development-only simulator. There is intentionally no live/HID mode.
package main

import (
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"strconv"
	"strings"
	"time"

	core "github.com/jjggzhou/zmk-codex-micro-module/tools/companion/src"
)

type fixedProvider struct{ value core.RawSnapshot }

func (p fixedProvider) Fetch(ctx context.Context, scope string) (core.RawSnapshot, error) {
	if err := ctx.Err(); err != nil {
		return core.RawSnapshot{}, err
	}
	return p.value, nil
}
func sample(now int64, scenario string, scope string) (core.RawSnapshot, error) {
	week, short, used, reset := core.WeekMinutes, 300, 37.0, now+3*86400
	r := core.RawSnapshot{Schema: 1, Scope: scope, Source: "SIMULATED", Mode: "chatgpt", Plan: "plus", ObservedAt: now,
		Pools: []core.Pool{{ID: "codex", Windows: []core.Window{{DurationMinutes: &week, UsedPercent: &used, ResetAt: &reset}}}}}
	switch scenario {
	case "weekly":
	case "free":
		r.Plan = "free"
	case "pro":
		r.Plan = "pro"
	case "unknown-plan":
		r.Plan = "future-plan"
	case "exhausted":
		used = 100
	case "near-zero":
		used = 99.6
	case "no-weekly":
		r.Pools[0].Windows[0].DurationMinutes = &short
	case "missing-percent":
		r.Pools[0].Windows[0].UsedPercent = nil
	case "missing-reset":
		r.Pools[0].Windows[0].ResetAt = nil
	case "reset":
		reset = now + 30
	case "api-key":
		r.Mode = "api_key"
	case "unauthorized":
		r.Mode = "unauthenticated"
	case "multi-pool":
		p := r.Pools[0]
		p.ID = "other"
		r.Pools = append(r.Pools, p)
	default:
		return r, fmt.Errorf("unknown demo scenario %q", scenario)
	}
	return r, nil
}
func run(args []string) error {
	if len(args) == 0 || args[0] != "demo" {
		return fmt.Errorf("usage: codex-micro-runtime demo [--scenario weekly] [--fixture file.json] [--scope demo] [--pool codex] [--offsets 0,60,1801]\nSimulation only; real account access and HID are not implemented")
	}
	f := flag.NewFlagSet("demo", flag.ContinueOnError)
	scenario := f.String("scenario", "weekly", "simulation scenario")
	fixture := f.String("fixture", "", "explicit provider fixture, never auto-discovered")
	scope := f.String("scope", "demo", "opaque simulated account scope")
	pool := f.String("pool", "", "explicit quota pool selection")
	at := f.Int64("at", 1790856000, "simulation start UTC Unix seconds")
	offsets := f.String("offsets", "0,60,1801", "nondecreasing simulated elapsed seconds")
	if err := f.Parse(args[1:]); err != nil {
		return err
	}
	if f.NArg() != 0 || *scope == "" || *at <= 0 || *at > 253402300799-366*86400 {
		return fmt.Errorf("invalid arguments")
	}
	var steps []int64
	previous := int64(-1)
	for _, text := range strings.Split(*offsets, ",") {
		n, err := strconv.ParseInt(strings.TrimSpace(text), 10, 64)
		if err != nil || n < 0 || n < previous || n > 366*86400 {
			return fmt.Errorf("invalid offsets")
		}
		steps = append(steps, n)
		previous = n
	}
	raw, err := sample(*at, *scenario, *scope)
	if err != nil {
		return err
	}
	var provider core.Provider = fixedProvider{raw}
	if *fixture != "" {
		provider = core.JSONFileProvider{Path: *fixture}
	}
	fake := &core.FakeDevice{}
	device := core.NewDeviceClient(fake)
	ctx := context.Background()
	if err = device.Connect(ctx); err != nil {
		return err
	}
	defer device.Close()
	store := core.NewStore(*scope)
	engine, err := core.NewCoordinator(provider, store, device, core.DefaultPolicy(), *pool)
	if err != nil {
		return err
	}
	fmt.Fprintln(os.Stderr, "SIMULATION ONLY: fake account + fake device; no hooks, network, credentials, HID or background installation")
	enc := json.NewEncoder(os.Stdout)
	for i, offset := range steps {
		event := core.Timer
		if i == 0 {
			event = core.Startup
		}
		outcome, stepErr := engine.Step(ctx, time.Unix(*at+offset, 0), event)
		message := struct {
			Simulation bool         `json:"simulation"`
			Elapsed    int64        `json:"elapsed_s"`
			Label      string       `json:"label"`
			ResetIn    *int64       `json:"reset_in_s"`
			Outcome    core.Outcome `json:"outcome"`
		}{true, offset, outcome.View.Label(), outcome.View.ResetIn(), outcome}
		if err := enc.Encode(message); err != nil {
			return err
		}
		if stepErr != nil {
			return stepErr
		}
	}
	return nil
}
func main() {
	if err := run(os.Args[1:]); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
