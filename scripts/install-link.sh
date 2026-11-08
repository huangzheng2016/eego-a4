#!/bin/sh
# Install eego-link (Linux/macOS).
#   curl -fsSL https://raw.githubusercontent.com/huangzheng2016/eego-a4/main/scripts/install-link.sh | sh
#   ... | sh -s -- --service       also start it at login (systemd --user / launchd)
#   ... | sh -s -- --system        install to /usr/local/bin (uses sudo)
set -eu

BASE="https://github.com/huangzheng2016/eego-a4/releases/download/link"
BIN_DIR="$HOME/.local/bin"
SERVICE=0
SUDO=""
for arg in "$@"; do
  case "$arg" in
    --service) SERVICE=1 ;;
    --system) BIN_DIR=/usr/local/bin; SUDO=sudo ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
  esac
done

case "$(uname -s)" in
  Linux) os=linux ;;
  Darwin) os=darwin ;;
  *) echo "unsupported OS: $(uname -s) (use install-link.ps1 on Windows)" >&2; exit 1 ;;
esac
case "$(uname -m)" in
  x86_64 | amd64) arch=amd64 ;;
  aarch64 | arm64) arch=arm64 ;;
  *) echo "unsupported CPU: $(uname -m)" >&2; exit 1 ;;
esac
asset="eego-link-$os-$arch"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
echo "downloading $asset"
curl -fsSL -o "$tmp/$asset" "$BASE/$asset"
curl -fsSL -o "$tmp/SHA256SUMS" "$BASE/SHA256SUMS"
want=$(grep " $asset\$" "$tmp/SHA256SUMS" | cut -d' ' -f1)
if command -v sha256sum >/dev/null 2>&1; then
  got=$(sha256sum "$tmp/$asset" | cut -d' ' -f1)
else
  got=$(shasum -a 256 "$tmp/$asset" | cut -d' ' -f1)
fi
if [ -z "$want" ] || [ "$want" != "$got" ]; then
  echo "checksum mismatch for $asset" >&2
  exit 1
fi

$SUDO mkdir -p "$BIN_DIR"
$SUDO install -m 0755 "$tmp/$asset" "$BIN_DIR/eego-link"
echo "installed $BIN_DIR/eego-link"
case ":$PATH:" in *":$BIN_DIR:"*) ;; *) echo "note: add $BIN_DIR to PATH" ;; esac

if [ "$SERVICE" = 1 ]; then
  if [ "$os" = linux ]; then
    unit="$HOME/.config/systemd/user/eego-link.service"
    mkdir -p "$(dirname "$unit")"
    cat >"$unit" <<UNIT
[Unit]
Description=EEGO A4 BLE network uplink
After=bluetooth.target

[Service]
ExecStart=$BIN_DIR/eego-link run
Restart=always
RestartSec=5

[Install]
WantedBy=default.target
UNIT
    systemctl --user daemon-reload
    systemctl --user enable --now eego-link.service
    echo "service: systemctl --user status eego-link"
  else
    plist="$HOME/Library/LaunchAgents/com.eego.eego-link.plist"
    mkdir -p "$(dirname "$plist")"
    cat >"$plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key><string>com.eego.eego-link</string>
  <key>ProgramArguments</key><array><string>$BIN_DIR/eego-link</string><string>run</string></array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>StandardErrorPath</key><string>$HOME/Library/Logs/eego-link.log</string>
</dict></plist>
PLIST
    launchctl unload "$plist" 2>/dev/null || true
    launchctl load "$plist"
    echo "service: log at ~/Library/Logs/eego-link.log"
  fi
fi

echo "next: eego-link pair   (enter the 6-digit code shown on the device)"
