#!/bin/bash
# Turns hybrid TRANSMIT off again on the hill: removes the LORAHYBRID line from ~/direwolf.conf (LORAHYBRIDRX, receive, is left alone)
# and restarts direwolf-chase. K6ATV-10 goes back to the frames it sent before. The config is backed up first.
set -u
CONF=${CONF:-$HOME/direwolf.conf}
SERVICE=${SERVICE:-direwolf-chase}
CTL=${CTL:-sudo systemctl}
JCTL=${JCTL:-journalctl}
WAIT=${WAIT:-20}
die() { echo; echo "STOP: $*" >&2; exit 1; }

[ -f "$CONF" ] || die "$CONF not found"
grep -q '^[[:space:]]*LORAHYBRID[[:space:]]' "$CONF" || die "$CONF has no LORAHYBRID line; nothing to do"
D=$HOME/hybrid-tx-backup-$(date +%F-%H%M%S)-$$
mkdir "$D" || die "cannot make $D"
cp -p "$CONF" "$D/direwolf.conf" || die "backup failed"
grep -v '^[[:space:]]*LORAHYBRID[[:space:]]' "$CONF" > "$D/direwolf.conf.new" || die "could not prepare the new config"
echo "The change to the config:"; diff "$CONF" "$D/direwolf.conf.new"
echo
read -r -p "Type yes to remove it and restart $SERVICE: " A; [ "$A" = yes ] || die "cancelled, nothing was changed"
cp "$D/direwolf.conf.new" "$CONF" || die "could not write $CONF"
$CTL restart "$SERVICE" || { cp "$D/direwolf.conf" "$CONF"; $CTL restart "$SERVICE"; die "restart failed; the previous config is back"; }
sleep "$WAIT"
echo; echo "=== status"; $CTL is-active "$SERVICE"
$JCTL -u "$SERVICE" --since "-2 min" -a | grep -a -E "hybrid frames|rror" | head -5
echo "Backup is in $D."
