package companion

import (
	"bytes"
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"sync"
)

const MaxMessageBytes = 1024

var ErrDisconnected = errors.New("device disconnected")
var ErrIncompatible = errors.New("isolated usage-v1 transport not available")

type Transport interface {
	Open(context.Context) error
	Connected() bool
	RoundTrip(context.Context, []byte) ([]byte, error)
	Close() error
}
type Capabilities struct {
	Schema          int    `json:"schema_version"`
	DeviceID        string `json:"device_id"`
	Session         string `json:"session"`
	Isolated        bool   `json:"isolated"`
	MaxMessageBytes int    `json:"max_message_bytes"`
}
type UsageParams struct {
	Schema   int    `json:"schema_version"`
	Session  string `json:"session"`
	Sequence uint64 `json:"sequence"`
	View     View   `json:"view"`
}
type Acknowledgement struct {
	Session  string `json:"session"`
	Sequence uint64 `json:"sequence"`
	Applied  bool   `json:"applied"`
}
type request struct {
	JSONRPC string          `json:"jsonrpc"`
	ID      uint64          `json:"id"`
	Method  string          `json:"method"`
	Params  json.RawMessage `json:"params"`
}
type response struct {
	JSONRPC string          `json:"jsonrpc"`
	ID      uint64          `json:"id"`
	Result  json.RawMessage `json:"result"`
	Error   *struct {
		Code int `json:"code"`
	} `json:"error,omitempty"`
}

// DeviceClient serializes our requests, checks replies, and only remembers an
// acknowledged state. This is NOT a lock shared with Codex Desktop.
type DeviceClient struct {
	boundDevice   string
	mu            sync.Mutex
	transport     Transport
	caps          Capabilities
	connected     bool
	requestID     uint64
	sequence      uint64
	lastSignature []byte
}

func NewDeviceClient(t Transport) *DeviceClient { return &DeviceClient{transport: t} }

// BindDevice prevents a reconnect from silently switching to a different unit.
func (d *DeviceClient) BindDevice(id string) error {
	d.mu.Lock()
	defer d.mu.Unlock()
	if d.connected {
		return errors.New("close device before rebinding")
	}
	d.boundDevice = id
	return nil
}
func (d *DeviceClient) call(ctx context.Context, method string, params any, dst any) error {
	if d.requestID == ^uint64(0) {
		return errors.New("request id exhausted")
	}
	d.requestID++
	p, err := json.Marshal(params)
	if err != nil {
		return err
	}
	wire, err := json.Marshal(request{"2.0", d.requestID, method, p})
	if err != nil {
		return err
	}
	if len(wire) > MaxMessageBytes {
		return errors.New("request too large")
	}
	reply, err := d.transport.RoundTrip(ctx, wire)
	if err != nil {
		return err
	}
	if err = ctx.Err(); err != nil {
		return err
	}
	if len(reply) > MaxMessageBytes {
		return errors.New("reply too large")
	}
	if err := uniqueJSON(reply); err != nil {
		return err
	}
	var r response
	if json.Unmarshal(reply, &r) != nil || r.JSONRPC != "2.0" || r.ID != d.requestID {
		return errors.New("mismatched RPC reply")
	}
	if r.Error != nil {
		return errors.New("device rejected request")
	}
	if len(r.Result) == 0 || bytes.Equal(r.Result, []byte("null")) {
		return errors.New("missing RPC result")
	}
	if err = json.Unmarshal(r.Result, dst); err != nil {
		return errors.New("invalid RPC result")
	}
	return nil
}
func (d *DeviceClient) Connect(ctx context.Context) error {
	d.mu.Lock()
	defer d.mu.Unlock()
	d.connected = false
	d.lastSignature = nil
	d.sequence = 0
	if err := d.transport.Close(); err != nil {
		return err
	}
	if err := d.transport.Open(ctx); err != nil {
		return err
	}
	var c Capabilities
	err := d.call(ctx, "micro.capabilities", nil, &c)
	if err != nil {
		d.transport.Close()
		return err
	}
	token, tokenErr := hex.DecodeString(c.Session)
	if (d.boundDevice != "" && c.DeviceID != d.boundDevice) || c.Schema != SchemaVersion || !c.Isolated || c.DeviceID == "" || tokenErr != nil || len(token) != 16 || c.MaxMessageBytes < MaxMessageBytes {
		d.transport.Close()
		return ErrIncompatible
	}
	d.caps = c
	d.connected = true
	return nil
}
func (d *DeviceClient) Publish(ctx context.Context, v View) (bool, error) {
	d.mu.Lock()
	defer d.mu.Unlock()
	if !d.connected || !d.transport.Connected() {
		d.connected = false
		d.lastSignature = nil
		return false, ErrDisconnected
	}
	if err := v.Validate(); err != nil {
		return false, err
	}
	sigView := v
	sigView.HostNow = 0 // Clock ticks are not new quota observations.
	sig, err := json.Marshal(sigView)
	if err != nil {
		return false, err
	}
	if bytes.Equal(sig, d.lastSignature) {
		return false, nil
	}
	if d.sequence == ^uint64(0) {
		return false, errors.New("sequence exhausted")
	}
	// On a retry use a NEW sequence and a newly derived view, but retain the
	// original observed_at. Never replay a queued time anchor as a fresh sample.
	d.sequence++
	p := UsageParams{SchemaVersion, d.caps.Session, d.sequence, v}
	var ack Acknowledgement
	if err = d.call(ctx, "micro.usage.set", p, &ack); err != nil {
		if errors.Is(err, ErrDisconnected) {
			d.connected = false
			d.lastSignature = nil
		}
		return false, err
	}
	if ack.Session != p.Session || ack.Sequence != p.Sequence || !ack.Applied {
		return false, errors.New("invalid usage acknowledgement")
	}
	d.lastSignature = sig
	return true, nil
}
func (d *DeviceClient) Connected() bool {
	d.mu.Lock()
	defer d.mu.Unlock()
	return d.connected && d.transport.Connected()
}
func (d *DeviceClient) Close() error {
	d.mu.Lock()
	defer d.mu.Unlock()
	d.connected = false
	d.lastSignature = nil
	return d.transport.Close()
}

// FakeDevice is a protocol simulator, NEVER a native HID implementation.
// Its mutex, session and sequence logic model one isolated device connection.
type FakeDevice struct {
	mu         sync.Mutex
	connected  bool
	session    string
	sequence   uint64
	lastParams []byte
	lastWire   []byte
	view       *View
	applied    int
}

func (f *FakeDevice) Open(ctx context.Context) error {
	if err := ctx.Err(); err != nil {
		return err
	}
	f.mu.Lock()
	defer f.mu.Unlock()
	nonce := make([]byte, 16)
	if _, err := rand.Read(nonce); err != nil {
		return err
	}
	f.connected = true
	f.session = hex.EncodeToString(nonce)
	f.sequence = 0
	f.lastParams = nil
	f.view = nil
	return nil
}
func (f *FakeDevice) Connected() bool { f.mu.Lock(); defer f.mu.Unlock(); return f.connected }
func (f *FakeDevice) Close() error    { f.mu.Lock(); defer f.mu.Unlock(); f.connected = false; return nil }
func (f *FakeDevice) RoundTrip(ctx context.Context, wire []byte) ([]byte, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	f.mu.Lock()
	defer f.mu.Unlock()
	if !f.connected {
		return nil, ErrDisconnected
	}
	if len(wire) > MaxMessageBytes {
		return nil, errors.New("request too large")
	}
	if err := uniqueJSON(wire); err != nil {
		return nil, err
	}
	var r request
	if json.Unmarshal(wire, &r) != nil || r.JSONRPC != "2.0" || r.ID == 0 {
		return nil, errors.New("invalid request")
	}
	var result any
	switch r.Method {
	case "micro.capabilities":
		result = Capabilities{SchemaVersion, "FAKE-DEVICE-ONLY", f.session, true, MaxMessageBytes}
	case "micro.usage.set":
		var p UsageParams
		if json.Unmarshal(r.Params, &p) != nil || p.Schema != SchemaVersion || p.Session != f.session || p.Sequence == 0 || p.View.Validate() != nil {
			return nil, errors.New("invalid usage update")
		}
		if p.Sequence < f.sequence || (p.Sequence == f.sequence && !bytes.Equal(r.Params, f.lastParams)) {
			return nil, errors.New("old or conflicting update")
		}
		if p.Sequence > f.sequence {
			f.sequence = p.Sequence
			f.lastParams = append([]byte(nil), r.Params...)
			f.lastWire = append([]byte(nil), wire...)
			copyView := p.View
			f.view = &copyView
			f.applied++
		}
		result = Acknowledgement{f.session, f.sequence, true}
	default:
		return nil, errors.New("unknown method")
	}
	data, err := json.Marshal(result)
	if err != nil {
		return nil, err
	}
	return json.Marshal(response{JSONRPC: "2.0", ID: r.ID, Result: data})
}
func (f *FakeDevice) Applied() int { f.mu.Lock(); defer f.mu.Unlock(); return f.applied }
func (f *FakeDevice) LastWire() []byte {
	f.mu.Lock()
	defer f.mu.Unlock()
	return append([]byte(nil), f.lastWire...)
}

// ViewAt models firmware aging without an MCU RTC: advance the received UTC
// anchor by a trustworthy monotonic elapsed duration. On reboot discard it.
func (f *FakeDevice) ViewAt(elapsedSeconds int64) View {
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.view == nil || elapsedSeconds < 0 || elapsedSeconds > 86400*366 {
		return View{Status: Unavailable}
	}
	v := *f.view
	v.Weekly = cloneWeekly(v.Weekly)
	v.HostNow += elapsedSeconds
	if v.Status == Ready || v.Status == NoWeekly || v.Status == SelectionRequired {
		s := Snapshot{Schema: SchemaVersion, Scope: "wire", Source: "wire", Status: v.Status, ObservedAt: v.ObservedAt, Weekly: v.Weekly}
		return s.ViewAt(unix(v.HostNow), duration(v.StaleAfterSeconds))
	}
	return v
}
