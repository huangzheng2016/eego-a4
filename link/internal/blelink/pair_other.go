//go:build !linux

package blelink

import (
	"runtime"

	"tinygo.org/x/bluetooth"
)

// PreparePairing is a no-op: macOS and Windows show their own pairing UI.
func PreparePairing(func(device string) (uint32, error)) (func(), error) { return func() {}, nil }

func afterConnect(bluetooth.Device, Found) error { return nil }

func pairingHint() string {
	if runtime.GOOS == "windows" {
		return "\nPair once in Settings > Bluetooth & devices > Add device > Bluetooth, choose the " +
			"EEGO-A4 device and enter the 6-digit code shown on its screen, then run this again."
	}
	return "\nAccept the pairing dialog and enter the 6-digit code shown on the device screen. If the " +
		"device was paired before, remove it in System Settings > Bluetooth first."
}
