LACF (LoRa APRS Compact Frame, called hybrid in the directives) on the hill Pi (branch feature/lora-hybrid)

The Direwolf that is on the hill now (APRS-PPM compression + LORAEVENTS + LORARXGAIN + CRC-on transmit) plus changes: it
decodes LACF frames, i.e. the native frame 3C FF 02 with a PPM-coded content field (spec 11 of the APRS-PPM proposal, a DRAFT).
Transmit is NOT changed and the config is not edited. Built and unit-tested on a Mac; the Linux-only radio code (loraspi.c) is
compiled by setup.sh on the Pi. The LACF decode has run on the hill since 1 November 2026; the changes of 7e54e5c (WIDE rule), f2a2ce6 (private_path rule, raw bytes of
broken frames stay in the local events file only) and ccca510 (frame code synced with the reference: NUL, ':' and text-element rules) have run on the radio only as far as the 1 November 2026 hill restart (receive and transmit of LACF frames worked); the new strict rules have not seen a frame that triggers them.

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
