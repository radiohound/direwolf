#!/bin/bash
# Turns on hybrid TRANSMIT on the hill for a test: adds the line  LORAHYBRID 1  under the LoRa channel in ~/direwolf.conf and
# restarts direwolf-chase. K6ATV-10 then sends hybrid frames when the packet can be encoded (stations running stock firmware will not hear them).
# The config is backed up first, and put back if the service does not come up. Undo with:  bash ~/direwolf-hybrid/scripts/hill-hybrid/tx-off.sh
# Read it first with:  less ~/direwolf-hybrid/scripts/hill-hybrid/tx-on.sh
set -u
CONF=${CONF:-$HOME/direwolf.conf}
SERVICE=${SERVICE:-direwolf-chase}
CTL=${CTL:-sudo systemctl}
JCTL=${JCTL:-journalctl}
WAIT=${WAIT:-25}
die() { echo; echo "STOP: $*" >&2; exit 1; }

[ -f "$CONF" ] || die "$CONF not found"
grep -q '^[[:space:]]*LCHANNEL[[:space:]]' "$CONF" || die "no LCHANNEL line in $CONF"
if grep -q '^[[:space:]]*LORAHYBRID[[:space:]]' "$CONF"; then
    grep '^[[:space:]]*LORAHYBRID[[:space:]]' "$CONF"; die "$CONF already has a LORAHYBRID line; nothing was changed"
fi
D=$HOME/hybrid-tx-backup-$(date +%F-%H%M%S)-$$
mkdir "$D" || die "cannot make $D"
cp -p "$CONF" "$D/direwolf.conf" || die "backup failed"
echo "Backed up the config to $D"

# insert right after the last LCHANNEL / LORA... line, because the directive applies to the channel above it
LAST=$(awk '/^[[:space:]]*(LCHANNEL|LORA[A-Z0-9]*)([[:space:]]|$)/ { n = NR } END { print n + 0 }' "$CONF")
[ "$LAST" -gt 0 ] || die "no LCHANNEL or LORA line found in $CONF; nothing was changed"
awk -v last="$LAST" '{ print } NR == last { print "LORAHYBRID 1" }' "$CONF" > "$D/direwolf.conf.new" || die "could not prepare the new config"
echo; echo "The change to the config:"; diff "$CONF" "$D/direwolf.conf.new"
echo
echo "This writes the change and restarts $SERVICE (K6ATV-10 is off the air for a few seconds), then K6ATV-10 transmits hybrid frames."
echo "Undo:  bash $(cd "$(dirname "$0")" && pwd)/tx-off.sh"
read -r -p "Type yes to continue: " A; [ "$A" = yes ] || die "cancelled, nothing was changed"

cp "$D/direwolf.conf.new" "$CONF" || die "could not write $CONF"
$CTL restart "$SERVICE" || { cp "$D/direwolf.conf" "$CONF"; $CTL restart "$SERVICE"; die "restart failed; the old config is back"; }
sleep "$WAIT"
if ! $CTL is-active --quiet "$SERVICE"; then
    echo "The service is not active; putting the old config back"; cp "$D/direwolf.conf" "$CONF"; $CTL restart "$SERVICE"; die "service did not come up; old config restored"
fi
echo; echo "=== status"; $CTL is-active "$SERVICE"
echo "=== hybrid lines since the restart"
$JCTL -u "$SERVICE" --since "-2 min" -a | grep -a -E "hybrid|rror|Line [0-9]+:" | head -10
echo
echo "A beacon goes out about a minute after a restart. Look for:  TX hybrid frame: N B instead of M B"
echo "Later:  $JCTL -u $SERVICE --since '-30 min' -a | grep -a 'TX hybrid frame'"
echo "Backup is in $D.  Turn it off again with:  bash $(cd "$(dirname "$0")" && pwd)/tx-off.sh"
