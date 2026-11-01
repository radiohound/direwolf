#!/bin/bash
# Step 2: puts the build from setup.sh on the air as the live K6ATV-10 (run on the hill Pi).
# What changes: K6ATV-10 now also decodes hybrid frames (native frame 3C FF 02 with PPM-coded content, spec 11), on by default.
# Transmit is NOT changed: beacons and everything else stay as they are now (add "LORAHYBRID 1" under the LCHANNEL later if you want hybrid transmit).
# It stops direwolf-chase for a few seconds and starts it again. The config is not edited.
# Read it first with:  less ~/direwolf-hybrid/scripts/hill-hybrid/install.sh      Undo with:  bash ~/direwolf-hybrid/scripts/hill-hybrid/rollback.sh BACKUPFOLDER
set -u
REPO=$(cd "$(dirname "$0")/../.." && pwd)
NEW=${NEW:-$REPO/build/src/direwolf}
LIVE=/usr/local/bin/direwolf
CONF=$HOME/direwolf.conf
SERVICE=direwolf-chase
KIT=$(cd "$(dirname "$0")" && pwd)
die() { echo; echo "STOP: $*" >&2; exit 1; }

[ -x "$NEW" ]  || die "$NEW not found; run setup.sh first"
[ -x "$LIVE" ] || die "$LIVE not found"
[ -f "$CONF" ] || die "$CONF not found"
for W in LORAHYBRID LORAHYBRIDRX LORAEVENTS LORACOMPRESS LORARXGAIN; do
    grep -aq "$W" "$NEW" || die "$NEW does not know $W; rebuild with setup.sh"
done
cmp -s "$NEW" "$LIVE" && die "the live binary is already this build"

D=$HOME/live-backup-hybrid-$(date +%F-%H%M%S)
mkdir "$D" || die "cannot make $D"
cp -p "$LIVE" "$D/direwolf.bin" && cp -p "$CONF" "$D/direwolf.conf" && systemctl cat "$SERVICE" > "$D/service.txt" 2>&1
cmp -s "$LIVE" "$D/direwolf.bin" || die "backup of the live binary did not copy correctly"
echo "Backed up the live binary and config to $D"

echo
echo "This replaces $LIVE with $NEW and restarts $SERVICE (K6ATV-10 is off the air for a few seconds)."
echo "Undo:  bash $KIT/rollback.sh $D"
read -r -p "Type yes to continue: " A; [ "$A" = yes ] || die "cancelled, nothing was changed"

sudo systemctl stop "$SERVICE" || die "cannot stop $SERVICE"
if ! sudo cp "$NEW" "$LIVE"; then
    echo "copy failed, putting the old binary back"; sudo cp "$D/direwolf.bin" "$LIVE"; sudo systemctl start "$SERVICE"; die "copy failed; old build restored"
fi
sudo systemctl start "$SERVICE"
sleep 20
echo; echo "=== status"; systemctl is-active "$SERVICE"
echo "=== first LoRa lines since the restart"
journalctl -u "$SERVICE" --since "-1 min" -a | grep -a -E "loraspi|LoRa channel|rror|rxgain|hybrid|Line [0-9]+:" | head -14
echo
echo "Backup is in $D.  Undo:  bash $KIT/rollback.sh $D"
echo "Later, look for hybrid frames heard:  journalctl -u $SERVICE --since '-1 hour' -a | grep -a 'hybrid frame'"
echo "Copy everything from the first line of this run and send it to Claude."
