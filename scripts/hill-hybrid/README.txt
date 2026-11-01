Hybrid frames on the hill Pi (branch feature/lora-hybrid)

The Direwolf that is on the hill now (APRS-PPM compression + LORAEVENTS + LORARXGAIN + CRC-on transmit) plus one change: it
decodes hybrid frames, i.e. the native frame 3C FF 02 with a PPM-coded content field (spec 11 of the APRS-PPM proposal, a DRAFT).
Transmit is NOT changed and the config is not edited. Built and unit-tested on a Mac; the Linux-only radio code (loraspi.c) is
compiled for the first time by setup.sh on the Pi. It has never run on a radio.

On the Pi (remote shell), one line each:
  git clone --branch feature/lora-hybrid https://github.com/radiohound/direwolf ~/direwolf-hybrid && bash ~/direwolf-hybrid/scripts/hill-hybrid/setup.sh
  bash ~/direwolf-hybrid/scripts/hill-hybrid/install.sh        (asks one question; type yes)
Undo:  bash ~/direwolf-hybrid/scripts/hill-hybrid/rollback.sh ~/live-backup-hybrid-YYYY-MM-DD-HHMMSS

Directives (apply to the LoRa channel above them in direwolf.conf; defaults in brackets)
  LORAHYBRIDRX 1|0 [1]   decode received hybrid frames
  LORAHYBRID   0|1 [0]   transmit hybrid frames when the packet can be encoded
Hybrid frames heard:  journalctl -u direwolf-chase --since '-1 hour' -a | grep -a 'hybrid frame'

Hybrid TRANSMIT test from the hill (optional; off after install). While it is on, stations running stock firmware do not hear K6ATV-10:
  cd ~/direwolf-hybrid && git pull && bash scripts/hill-hybrid/tx-on.sh      (adds LORAHYBRID 1 under the LoRa channel, backs up the config, restarts; asks yes)
  journalctl -u direwolf-chase --since '-30 min' -a | grep -a 'TX hybrid frame'
  bash ~/direwolf-hybrid/scripts/hill-hybrid/tx-off.sh                       (removes it again; receive stays on)
