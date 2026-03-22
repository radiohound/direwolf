/* loratnc.h */

/*--------------------------------------------------------------------
 *
 * Name:    loratnc.h
 *
 * Purpose: Interface to the LoRa APRS bridge (lora_kiss_bridge.py).
 *
 *          Dire Wolf listens on LORAPORT (TCP).  The bridge connects,
 *          sends received LoRa packets as TNC2 text lines, and receives
 *          outbound packets as TNC2 text lines for transmission.
 *
 *          Dire Wolf calls ax25_from_text() on each incoming line and
 *          injects the result into its normal packet pipeline (iGate,
 *          digipeater, decoder, etc.).
 *
 *--------------------------------------------------------------------*/

#ifndef LORATNC_H
#define LORATNC_H 1

#include "audio.h"
#include "config.h"

/*
 * Channel number assigned to the LoRa interface.
 * Set during loratnc_init(); -1 if not configured.
 */
extern int g_lora_chan;

void loratnc_init (struct audio_s *pa, struct misc_config_s *mc);

void loratnc_send_packet (int chan, packet_t pp);

#endif  /* LORATNC_H */
