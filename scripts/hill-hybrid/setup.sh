#!/bin/bash
# Step 1 of putting hybrid frames on the hill Pi. Run it from the clone of branch feature/lora-hybrid:
#   git clone --branch feature/lora-hybrid https://github.com/radiohound/direwolf ~/direwolf-hybrid && bash ~/direwolf-hybrid/scripts/hill-hybrid/setup.sh
# It backs up the live binary and config, builds this clone in <clone>/build and runs the unit tests.
# It does NOT stop or change any service, and does not touch /usr/local, ~/direwolf or ~/direwolf.conf.
# Read it first with:  less ~/direwolf-hybrid/scripts/hill-hybrid/setup.sh
set -u
DW_BIN=${DW_BIN:-/usr/local/bin/direwolf}
DW_CONF=${DW_CONF:-$HOME/direwolf.conf}
DW_SERVICE=${DW_SERVICE:-/etc/systemd/system/direwolf-chase.service}
REPO=$(cd "$(dirname "$0")/../.." && pwd)
CODE_COMMIT=1c5a4c407f112d67a806082ff24198e79d098d62    # the last code change (the LACF version in the status event, and decoded or sent LACF frames are class hybrid; before: protocol version 2, a7d8467)

step() { echo; echo "=== $*"; }
die()  { echo; echo "STOP: $*" >&2; exit 1; }

step "1. Back up"
[ -x "$DW_BIN" ]      || die "$DW_BIN not found"
[ -f "$DW_CONF" ]     || die "$DW_CONF not found"
[ -f "$DW_SERVICE" ]  || die "$DW_SERVICE not found"
D=$HOME/backup-hybrid-$(date +%F-%H%M%S)
mkdir "$D" || die "cannot create $D"
cp -a "$DW_BIN" "$D/direwolf.bin"     || die "copy of the binary failed"
cp -a "$DW_CONF" "$D/"                || die "copy of the config failed"
cp -a "$DW_SERVICE" "$D/"             || die "copy of the service file failed"
A=$(sha256sum "$DW_BIN" | cut -d' ' -f1); B=$(sha256sum "$D/direwolf.bin" | cut -d' ' -f1)
[ "$A" = "$B" ] || die "backup checksum does not match"
echo "Backed up to $D"; ls -la "$D"

step "2. Check the source in $REPO"
cd "$REPO" || die "cannot enter $REPO"
[ "$(git rev-parse --abbrev-ref HEAD)" = feature/lora-hybrid ] || die "this clone is not on branch feature/lora-hybrid"
git merge-base --is-ancestor "$CODE_COMMIT" HEAD || die "this clone does not contain the code commit ${CODE_COMMIT:0:7}"
OTHER=$(git diff --name-only "$CODE_COMMIT" HEAD | grep -v '^scripts/hill-hybrid/\|^doc/LoRa-Hybrid.md$\|^\.github/' || true)
[ -z "$OTHER" ] || die "the branch changed files other than these scripts since ${CODE_COMMIT:0:7}: $OTHER"
[ -z "$(git status --porcelain --untracked-files=no)" ] || die "the clone has local changes"
echo "Source OK: $(git log --format=%h -1) = code commit ${CODE_COMMIT:0:7} plus scripts and docs only"

step "3. Build in $REPO/build"
cmake -S . -B build -DUNITTEST=ON > build.log 2>&1 || { tail -20 build.log; die "cmake failed"; }
make -C build -j2 direwolf lctest loraevents_test lorahybrid_test >> build.log 2>&1 || { tail -30 build.log; die "build failed"; }

step "4. Unit tests"
(cd build && ctest -R 'lctest|loraevents_test|lorahybrid_test' --output-on-failure 2>&1 | tail -12) | tee test.out
grep -q "100% tests passed" test.out || die "unit tests did not pass"
for W in LORAHYBRID LORAHYBRIDRX LORAEVENTS LORACOMPRESS LORARXGAIN; do
    grep -aq "$W" build/src/direwolf || die "the new binary does not know $W"
done
echo "The new binary knows LORAHYBRID, LORAHYBRIDRX, LORAEVENTS, LORACOMPRESS and LORARXGAIN."

echo
echo "ALL GOOD: backup in $D, new build at $REPO/build/src/direwolf, unit tests passed."
echo "Nothing was stopped or changed. Copy everything from the '=== 1.' line down to here and send it to Claude."
