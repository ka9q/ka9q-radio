#!/usr/bin/env bash
set -euo pipefail

# Configure Raspberry Pi for KA9Q-radio + Dire Wolf APRS receive pipeline.
# Tested conceptually against the setup established in this session:
#   RTL-SDR -> radiod -> aprs-pcm.local RTP/PCM -> pcmrecord -> Dire Wolf 1.7
#
# Safe behavior:
# - Installs required packages.
# - Enables Avahi.
# - Backs up existing config files before modifying them.
# - Preserves existing squelch values; only adds defaults if missing.
# - Creates a Dire Wolf config using stdin, 12 kHz, 2 PCM channels.
# - Uses AGW TCP 8002 because port 8000 is already used by ka9q-beacon-mon.
# - Creates a helper command to start the APRS decoder pipeline.
# - Does NOT enable a systemd service automatically.
#
# Optional environment overrides before running:
#   MYCALL=SM6XXX-10
#   RADIO_CONF=/etc/radio/rtlsdr-full.conf
#   APRS_OPEN=10 APRS_CLOSE=8
#   SIMPLEX_OPEN=10 SIMPLEX_CLOSE=8
#   REPEATER_OPEN=10 REPEATER_CLOSE=8
#
# Example:
#   sudo MYCALL=SM6XXX-10 bash configure-ka9q-direwolf.sh

if [[ ${EUID} -ne 0 ]]; then
  echo "Run as root, e.g.: sudo bash $0" >&2
  exit 1
fi

REAL_USER=${SUDO_USER:-pi}
REAL_HOME=$(getent passwd "$REAL_USER" | cut -d: -f6)
if [[ -z ${REAL_HOME} ]]; then
  REAL_HOME="/home/$REAL_USER"
fi

RADIO_CONF=${RADIO_CONF:-/etc/radio/rtlsdr-full.conf}
DW_CONF="$REAL_HOME/direwolf-ka9q.conf"
RUNNER=/usr/local/bin/ka9q-aprs-direwolf
MYCALL=${MYCALL:-NOCALL}

# Defaults are only inserted when the channel currently has no squelch setting.
APRS_OPEN=${APRS_OPEN:-10}
APRS_CLOSE=${APRS_CLOSE:-8}
SIMPLEX_OPEN=${SIMPLEX_OPEN:-10}
SIMPLEX_CLOSE=${SIMPLEX_CLOSE:-8}
REPEATER_OPEN=${REPEATER_OPEN:-10}
REPEATER_CLOSE=${REPEATER_CLOSE:-8}

AGWPORT=${AGWPORT:-8002}
KISSPORT=${KISSPORT:-8001}

stamp=$(date +%Y%m%d-%H%M%S)

backup_file() {
  local f=$1
  if [[ -f $f ]]; then
    cp -a "$f" "$f.bak-$stamp"
    echo "Backup: $f.bak-$stamp"
  fi
}

ensure_pkg() {
  local p=$1
  if ! dpkg -s "$p" >/dev/null 2>&1; then
    apt-get install -y "$p"
  fi
}

echo "==> Installing required packages"
apt-get update
ensure_pkg avahi-daemon
ensure_pkg avahi-utils
ensure_pkg direwolf
ensure_pkg tcpdump

echo "==> Enabling Avahi"
systemctl enable --now avahi-daemon

if [[ ! -f $RADIO_CONF ]]; then
  echo "ERROR: $RADIO_CONF does not exist." >&2
  echo "Install/configure ka9q-radio first, then rerun this script." >&2
  exit 1
fi

backup_file "$RADIO_CONF"

# Add squelch-open / squelch-close to a named INI section only if absent.
# Existing values are intentionally preserved because they may already be tuned.
add_squelch_if_missing() {
  local section=$1
  local open_val=$2
  local close_val=$3
  local tmp
  tmp=$(mktemp)

  awk -v sec="$section" -v op="$open_val" -v cl="$close_val" '
    function flush_section() {
      if (in_target) {
        if (!have_open)  print "squelch-open = " op
        if (!have_close) print "squelch-close = " cl
      }
    }
    /^\[[^]]+\][[:space:]]*$/ {
      flush_section()
      in_target = ($0 == "[" sec "]")
      have_open = 0
      have_close = 0
      print
      next
    }
    {
      if (in_target && $0 ~ /^[[:space:]]*squelch-open[[:space:]]*=/) have_open = 1
      if (in_target && $0 ~ /^[[:space:]]*squelch-close[[:space:]]*=/) have_close = 1
      print
    }
    END { flush_section() }
  ' "$RADIO_CONF" > "$tmp"

  cat "$tmp" > "$RADIO_CONF"
  rm -f "$tmp"
}

echo "==> Ensuring explicit squelch settings exist in KA9Q channel sections"
add_squelch_if_missing channel-aprs "$APRS_OPEN" "$APRS_CLOSE"
add_squelch_if_missing channel-simplex "$SIMPLEX_OPEN" "$SIMPLEX_CLOSE"
add_squelch_if_missing channel-repeater-1 "$REPEATER_OPEN" "$REPEATER_CLOSE"

# Verify that expected APRS channel exists and points at the multicast name used here.
if ! grep -q '^\[channel-aprs\][[:space:]]*$' "$RADIO_CONF"; then
  echo "WARNING: [channel-aprs] was not found in $RADIO_CONF" >&2
fi
if ! grep -q '^[[:space:]]*data[[:space:]]*=[[:space:]]*aprs-pcm\.local[[:space:]]*$' "$RADIO_CONF"; then
  echo "WARNING: aprs-pcm.local was not found as a data target in $RADIO_CONF" >&2
fi

backup_file "$DW_CONF"

echo "==> Writing Dire Wolf configuration: $DW_CONF"
cat > "$DW_CONF" <<EODW
# Dire Wolf 1.7 configuration for KA9Q pcmrecord raw stream.
# pcmrecord --catmode --raw provides two 16-bit PCM channels at ~12 kHz.

ADEVICE stdin null
ARATE 12000
ACHANNELS 2

CHANNEL 0
MYCALL $MYCALL
MODEM 1200

CHANNEL 1
MYCALL $MYCALL
MODEM 1200

# Port 8000 is commonly occupied on this Pi by ka9q-beacon-mon.
AGWPORT $AGWPORT
KISSPORT $KISSPORT
EODW

chown "$REAL_USER":"$REAL_USER" "$DW_CONF"
chmod 0644 "$DW_CONF"

echo "==> Creating APRS decoder helper: $RUNNER"
cat > "$RUNNER" <<'EORUN'
#!/usr/bin/env bash
set -euo pipefail

USER_NAME=${SUDO_USER:-${USER:-pi}}
USER_HOME=$(getent passwd "$USER_NAME" | cut -d: -f6)
DW_CONF=${DW_CONF:-$USER_HOME/direwolf-ka9q.conf}
SOURCE=${KA9Q_APRS_SOURCE:-aprs-pcm.local}
AUDIO_STATS_INTERVAL=${AUDIO_STATS_INTERVAL:-60}

exec bash -c 'pcmrecord --catmode --raw "$1" | direwolf -P A -D 1 -t 0 -a "$2" -c "$3" -' _ \
  "$SOURCE" "$AUDIO_STATS_INTERVAL" "$DW_CONF"
EORUN
chmod 0755 "$RUNNER"

# Optional systemd unit. It is installed but left disabled so that the user can
# first verify radiod startup order and avoid duplicate radiod/USB claims.
UNIT=/etc/systemd/system/ka9q-direwolf-aprs.service
backup_file "$UNIT"
cat > "$UNIT" <<EOUNIT
[Unit]
Description=KA9Q APRS PCM to Dire Wolf decoder
After=network-online.target avahi-daemon.service
Wants=network-online.target avahi-daemon.service

[Service]
Type=simple
User=$REAL_USER
Environment=HOME=$REAL_HOME
Environment=AUDIO_STATS_INTERVAL=60
ExecStart=$RUNNER
Restart=on-failure
RestartSec=3

[Install]
WantedBy=multi-user.target
EOUNIT

systemctl daemon-reload

echo
echo "==> Configuration complete"
echo "KA9Q config : $RADIO_CONF"
echo "Dire Wolf   : $DW_CONF"
echo "Run helper  : $RUNNER"
echo "AGW TCP     : $AGWPORT"
echo "KISS TCP    : $KISSPORT"
echo
echo "Important: this script did not start or enable ka9q-direwolf-aprs.service."
echo "First make sure exactly one radiod instance is running and that aprs-pcm.local is active."
echo
echo "Manual test:"
echo "  $RUNNER"
echo
echo "Useful checks:"
echo "  avahi-resolve -n aprs-pcm.local"
echo "  monitor -v aprs-pcm.local"
echo "  sudo tcpdump -pni any udp port 5004"
echo "  ss -ltnp | grep -E ':${AGWPORT}|:${KISSPORT}'"
echo
echo "After successful testing, enable at boot with:"
echo "  sudo systemctl enable --now ka9q-direwolf-aprs.service"
echo
echo "To suppress periodic ADEVICE0 status lines, use AUDIO_STATS_INTERVAL=0 manually:"
echo "  sudo -u $REAL_USER AUDIO_STATS_INTERVAL=0 $RUNNER"
