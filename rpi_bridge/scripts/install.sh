#!/usr/bin/env bash
# One-shot installer for the Tivvy voice bridge on Raspberry Pi OS (Bookworm+).
#
#   sudo ./scripts/install.sh
#
# Creates:
#   /opt/tivvy/bridge        the package
#   /opt/tivvy/venv          virtualenv with the runtime dependencies
#   /opt/tivvy/models/...    the Vosk model
#   /etc/tivvy/bridge.toml   config (only written if missing)
#   /var/log/tivvy           transcript log directory
#   tivvy-bridge.service     systemd unit, enabled but not started
set -euo pipefail

if [[ "${EUID}" -ne 0 ]]; then
  echo "Run with sudo." >&2
  exit 1
fi

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PREFIX=/opt/tivvy
SERVICE_USER=tivvy

echo "==> Installing system packages"
apt-get update -qq
apt-get install -y --no-install-recommends \
  python3-venv python3-dev build-essential \
  libportaudio2 portaudio19-dev \
  bluez curl unzip

echo "==> Creating the ${SERVICE_USER} service account"
if ! id -u "${SERVICE_USER}" >/dev/null 2>&1; then
  useradd --system --create-home --home-dir /var/lib/tivvy --shell /usr/sbin/nologin "${SERVICE_USER}"
fi
usermod -aG audio,bluetooth,dialout "${SERVICE_USER}"

echo "==> Copying the bridge to ${PREFIX}/bridge"
mkdir -p "${PREFIX}/bridge" /etc/tivvy /var/log/tivvy
cp -r "${SRC}/tivvy_bridge" "${SRC}/requirements.txt" "${SRC}/pyproject.toml" "${PREFIX}/bridge/"
cp "${SRC}/README.md" "${PREFIX}/bridge/" 2>/dev/null || true
chown -R "${SERVICE_USER}:${SERVICE_USER}" /var/log/tivvy

echo "==> Building the virtualenv"
python3 -m venv "${PREFIX}/venv"
"${PREFIX}/venv/bin/pip" install --upgrade pip wheel
"${PREFIX}/venv/bin/pip" install -r "${PREFIX}/bridge/requirements.txt"

echo "==> Fetching the Vosk model"
bash "${SRC}/scripts/download_vosk_model.sh" "${PREFIX}/models"

if [[ ! -f /etc/tivvy/bridge.toml ]]; then
  echo "==> Writing /etc/tivvy/bridge.toml"
  cp "${SRC}/config.example.toml" /etc/tivvy/bridge.toml
else
  echo "==> Keeping the existing /etc/tivvy/bridge.toml"
fi

echo "==> Installing the systemd unit"
install -m 0644 "${SRC}/scripts/tivvy-bridge.service" /etc/systemd/system/tivvy-bridge.service
systemctl daemon-reload
systemctl enable tivvy-bridge.service

cat <<'EOF'

Installed. Next steps:

  1. Find your microphone:
       /opt/tivvy/venv/bin/python -m tivvy_bridge --list-audio
     and set [audio] device in /etc/tivvy/bridge.toml.

  2. Find the Qualia (power it on first):
       /opt/tivvy/venv/bin/python -m tivvy_bridge --scan-ble
     and paste its address into [link] device_address.

  3. Verify everything loads:
       /opt/tivvy/venv/bin/python -m tivvy_bridge --config /etc/tivvy/bridge.toml --check

  4. Send a command without speaking:
       /opt/tivvy/venv/bin/python -m tivvy_bridge --config /etc/tivvy/bridge.toml \
           --say "set a baking timer for two minutes"

  5. Start it for real:
       sudo systemctl start tivvy-bridge
       journalctl -u tivvy-bridge -f

EOF
