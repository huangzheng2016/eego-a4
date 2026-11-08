//go:build darwin

package blelink

import (
	"errors"

	"tinygo.org/x/bluetooth"
)

// The macOS backend has no characteristic read; the link falls back to the ATT MTU.
func readCharacteristic(bluetooth.DeviceCharacteristic) ([]byte, error) {
	return nil, errors.New("characteristic read not supported on macOS")
}
