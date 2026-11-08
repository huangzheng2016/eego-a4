//go:build linux

package blelink

import (
	"errors"
	"fmt"
	"strings"
	"time"

	"github.com/godbus/dbus/v5"
	"tinygo.org/x/bluetooth"
)

const agentPath = dbus.ObjectPath("/com/eego/link")

// bluezAgent answers BlueZ pairing prompts; the passkey comes from the terminal.
type bluezAgent struct {
	prompt func(device string) (uint32, error)
}

func (a *bluezAgent) Release() *dbus.Error { return nil }
func (a *bluezAgent) RequestPinCode(dbus.ObjectPath) (string, *dbus.Error) {
	return "", dbus.NewError("org.bluez.Error.Rejected", nil)
}
func (a *bluezAgent) DisplayPinCode(dbus.ObjectPath, string) *dbus.Error { return nil }
func (a *bluezAgent) RequestPasskey(dev dbus.ObjectPath) (uint32, *dbus.Error) {
	pk, err := a.prompt(string(dev))
	if err != nil {
		return 0, dbus.NewError("org.bluez.Error.Canceled", nil)
	}
	return pk, nil
}
func (a *bluezAgent) DisplayPasskey(dbus.ObjectPath, uint32, uint16) *dbus.Error { return nil }
func (a *bluezAgent) RequestConfirmation(dbus.ObjectPath, uint32) *dbus.Error    { return nil }
func (a *bluezAgent) RequestAuthorization(dbus.ObjectPath) *dbus.Error           { return nil }
func (a *bluezAgent) AuthorizeService(dbus.ObjectPath, string) *dbus.Error       { return nil }
func (a *bluezAgent) Cancel() *dbus.Error                                        { return nil }

var pairBus *dbus.Conn

// PreparePairing registers the default BlueZ agent (capability KeyboardOnly) so that the
// device's passkey can be typed in the terminal. The returned func unregisters it.
func PreparePairing(prompt func(device string) (uint32, error)) (func(), error) {
	bus, err := dbus.SystemBus()
	if err != nil {
		return nil, fmt.Errorf("system bus: %w", err)
	}
	if err := bus.Export(&bluezAgent{prompt: prompt}, agentPath, "org.bluez.Agent1"); err != nil {
		return nil, err
	}
	mgr := bus.Object("org.bluez", "/org/bluez")
	if call := mgr.Call("org.bluez.AgentManager1.RegisterAgent", 0, agentPath, "KeyboardOnly"); call.Err != nil {
		return nil, fmt.Errorf("register agent: %w", call.Err)
	}
	if call := mgr.Call("org.bluez.AgentManager1.RequestDefaultAgent", 0, agentPath); call.Err != nil {
		return nil, fmt.Errorf("default agent: %w", call.Err)
	}
	pairBus = bus
	return func() {
		mgr.Call("org.bluez.AgentManager1.UnregisterAgent", 0, agentPath)
		pairBus = nil
	}, nil
}

// afterConnect pairs explicitly (bonding + Trusted) while the agent is registered.
func afterConnect(_ bluetooth.Device, f Found) error {
	if pairBus == nil {
		return nil
	}
	path := dbus.ObjectPath("/org/bluez/hci0/dev_" + strings.ReplaceAll(strings.ToUpper(f.Address), ":", "_"))
	dev := pairBus.Object("org.bluez", path)
	if v, err := dev.GetProperty("org.bluez.Device1.Paired"); err == nil {
		if paired, _ := v.Value().(bool); paired {
			return nil
		}
	}
	call := dev.Call("org.bluez.Device1.Pair", 0)
	if call.Err != nil {
		var de dbus.Error
		if !errors.As(call.Err, &de) || (de.Name != "org.bluez.Error.AlreadyExists" && de.Name != "org.bluez.Error.InProgress") {
			return fmt.Errorf("pair: %w", call.Err)
		}
	}
	deadline := time.Now().Add(60 * time.Second)
	for time.Now().Before(deadline) {
		if v, err := dev.GetProperty("org.bluez.Device1.Paired"); err == nil {
			if paired, _ := v.Value().(bool); paired {
				_ = dev.SetProperty("org.bluez.Device1.Trusted", dbus.MakeVariant(true))
				return nil
			}
		}
		time.Sleep(500 * time.Millisecond)
	}
	return errors.New("pairing timed out")
}

func pairingHint() string {
	return "\nRun `eego-link pair` and type the 6-digit code shown on the device screen. " +
		"BlueZ must be running and your user needs access to it (bluetooth group)."
}
