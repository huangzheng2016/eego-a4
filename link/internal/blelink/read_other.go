//go:build !darwin

package blelink

import "tinygo.org/x/bluetooth"

func readCharacteristic(c bluetooth.DeviceCharacteristic) ([]byte, error) {
	buf := make([]byte, 512)
	n, err := c.Read(buf)
	if err != nil {
		return nil, err
	}
	return buf[:n], nil
}
