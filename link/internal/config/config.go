// Package config stores the paired device.
package config

import (
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
)

// Config is saved as <user config dir>/eego-link/config.json.
type Config struct {
	Name    string `json:"name"`
	Address string `json:"address"`
}

// Path returns the config file location.
func Path() (string, error) {
	dir, err := os.UserConfigDir()
	if err != nil {
		return "", err
	}
	return filepath.Join(dir, "eego-link", "config.json"), nil
}

// Load reads the config; os.ErrNotExist when nothing has been paired yet.
func Load() (Config, error) {
	var c Config
	p, err := Path()
	if err != nil {
		return c, err
	}
	raw, err := os.ReadFile(p)
	if err != nil {
		return c, err
	}
	if err := json.Unmarshal(raw, &c); err != nil {
		return c, err
	}
	if c.Name == "" && c.Address == "" {
		return c, errors.New("config has no device")
	}
	return c, nil
}

// Save writes the config with owner-only permissions.
func Save(c Config) error {
	p, err := Path()
	if err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(p), 0o700); err != nil {
		return err
	}
	raw, _ := json.MarshalIndent(c, "", "  ")
	return os.WriteFile(p, append(raw, '\n'), 0o600)
}
