#!/bin/bash
# Puts the previous Direwolf binary AND config back on the hill, from a folder made by hill-cad-install.sh.
#   bash rollback.sh ~/live-backup-hybrid-YYYY-MM-DD-HHMMSS
set -u
D=${1:-}
LIVE=/usr/local/bin/direwolf
CONF=$HOME/direwolf.conf
SERVICE=direwolf-chase
die() { echo; echo "STOP: $*" >&2; exit 1; }
[ -n "$D" ] && [ -f "$D/direwolf.bin" ] && [ -f "$D/direwolf.conf" ] || die "give the backup folder, e.g.  bash rollback.sh ~/live-backup-hybrid-2026-10-27-001500"
echo "This puts $D/direwolf.bin back as $LIVE, $D/direwolf.conf back as $CONF, and restarts $SERVICE."
read -r -p "Type yes to continue: " A; [ "$A" = yes ] || die "cancelled, nothing was changed"
sudo systemctl stop "$SERVICE" || die "cannot stop $SERVICE"
sudo cp "$D/direwolf.bin" "$LIVE" || { sudo systemctl start "$SERVICE"; die "copy of the binary failed"; }
cp "$D/direwolf.conf" "$CONF"    || { sudo systemctl start "$SERVICE"; die "copy of the config failed"; }
sudo systemctl start "$SERVICE"
sleep 10; echo; systemctl is-active "$SERVICE"
