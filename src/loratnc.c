/*--------------------------------------------------------------------
 *
 * Module:      loratnc.c
 *
 * Purpose:     Interface to the LoRa APRS bridge (lora_kiss_bridge.py).
 *
 * Description: Dire Wolf listens on LORAPORT (TCP).  The bridge connects
 *              and exchanges raw TNC2 text lines — one APRS packet per line.
 *
 *              Incoming lines (bridge → Dire Wolf):
 *                  ax25_from_text() converts TNC2 text to a packet object
 *                  which is then fed into the normal received-frame queue.
 *                  Dire Wolf handles decoding, iGate, digipeating, etc.
 *
 *              Outgoing lines (Dire Wolf → bridge):
 *                  Packet objects are formatted as TNC2 text and sent to
 *                  the bridge for transmission over LoRa.
 *
 * Configuration:
 *              Add to direwolf.conf:
 *                  LORAPORT 8002
 *
 *              The bridge connects to this port.  Start lora_kiss_bridge.py
 *              before starting Dire Wolf.
 *
 *--------------------------------------------------------------------*/

#include "direwolf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#if __WIN32__
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

#include "textcolor.h"
#include "audio.h"
#include "config.h"
#include "ax25_pad.h"
#include "dlq.h"
#include "dwsock.h"
#include "loratnc.h"

#if __WIN32__
#define THREAD_F unsigned __stdcall
#else
#define THREAD_F void *
#endif

/* Channel number assigned to the LoRa interface. -1 = not configured. */
int g_lora_chan = -1;

/* Socket to the bridge.  -1 = not connected. */
static volatile int s_sock = -1;

#if __WIN32__
static unsigned __stdcall lora_listen_thread (void *arg);
#else
static void * lora_listen_thread (void *arg);
#endif


/*-------------------------------------------------------------------
 *
 * Name:        loratnc_init
 *
 * Purpose:     Start the LoRa TNC listener if LORAPORT is configured.
 *
 * Inputs:      pa  - Audio/channel configuration (for assigning channel).
 *              mc  - Misc configuration (contains lora_port).
 *
 *--------------------------------------------------------------------*/

void loratnc_init (struct audio_s *pa, struct misc_config_s *mc)
{
	if (mc->lora_port == 0) {
	    return;   /* LORAPORT not configured */
	}

	/* Assign the next available channel number above the radio channels. */
	int chan = MAX_RADIO_CHANS;
	while (chan < MAX_TOTAL_CHANS && pa->chan_medium[chan] != MEDIUM_NONE) {
	    chan++;
	}
	if (chan >= MAX_TOTAL_CHANS) {
	    text_color_set(DW_COLOR_ERROR);
	    dw_printf ("loratnc_init: No free channel slots for LORAPORT.\n");
	    return;
	}

	pa->chan_medium[chan] = MEDIUM_NETTNC;  /* Closest existing type */
	g_lora_chan = chan;

	text_color_set(DW_COLOR_INFO);
	dw_printf ("LoRa APRS bridge: channel %d, listening on port %d\n",
	           g_lora_chan, mc->lora_port);

	/* Start the listener thread, pass port number as argument. */
	long port = mc->lora_port;

#if __WIN32__
	HANDLE tid;
	tid = (HANDLE)_beginthreadex(NULL, 0, lora_listen_thread,
	                              (void *)(ptrdiff_t)port, 0, NULL);
	if (tid == NULL) {
	    text_color_set(DW_COLOR_ERROR);
	    dw_printf ("loratnc_init: Failed to create listener thread.\n");
	}
#else
	pthread_t tid;
	int e = pthread_create(&tid, NULL, lora_listen_thread, (void *)(ptrdiff_t)port);
	if (e != 0) {
	    text_color_set(DW_COLOR_ERROR);
	    dw_printf ("loratnc_init: pthread_create failed: %s\n", strerror(e));
	}
#endif
}


/*-------------------------------------------------------------------
 *
 * Name:        lora_listen_thread
 *
 * Purpose:     TCP server thread.  Waits for the bridge to connect,
 *              reads TNC2 text lines, and injects them into Dire Wolf's
 *              received-frame queue.  Re-listens if bridge disconnects.
 *
 *--------------------------------------------------------------------*/

#if __WIN32__
static unsigned __stdcall lora_listen_thread (void *arg)
#else
static void * lora_listen_thread (void *arg)
#endif
{
	int port = (int)(ptrdiff_t)arg;

#if __WIN32__
	SOCKET srv_sock;
	struct sockaddr_in srv_addr;
	srv_sock = socket(AF_INET, SOCK_STREAM, 0);
	int opt = 1;
	setsockopt(srv_sock, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
	memset(&srv_addr, 0, sizeof(srv_addr));
	srv_addr.sin_family = AF_INET;
	srv_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	srv_addr.sin_port = htons(port);
	bind(srv_sock, (struct sockaddr *)&srv_addr, sizeof(srv_addr));
	listen(srv_sock, 1);
#else
	int srv_sock;
	struct sockaddr_in srv_addr;
	srv_sock = socket(AF_INET, SOCK_STREAM, 0);
	int opt = 1;
	setsockopt(srv_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
	memset(&srv_addr, 0, sizeof(srv_addr));
	srv_addr.sin_family = AF_INET;
	srv_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	srv_addr.sin_port = htons(port);
	bind(srv_sock, (struct sockaddr *)&srv_addr, sizeof(srv_addr));
	listen(srv_sock, 1);
#endif

	while (1) {

	    text_color_set(DW_COLOR_INFO);
	    dw_printf ("LoRa bridge: waiting for connection on port %d\n", port);

	    int conn = accept(srv_sock, NULL, NULL);
	    if (conn < 0) {
	        SLEEP_SEC(1);
	        continue;
	    }

	    text_color_set(DW_COLOR_INFO);
	    dw_printf ("LoRa bridge: connected.\n");
	    s_sock = conn;

	    /* Read TNC2 text lines until bridge disconnects. */
	    char buf[512];
	    int  pos = 0;

	    while (1) {
	        char ch;
#if __WIN32__
	        int n = recv(conn, &ch, 1, 0);
#else
	        int n = read(conn, &ch, 1);
#endif
	        if (n <= 0) break;

	        if (ch == '\r') continue;   /* ignore CR */

	        if (ch == '\n' || pos >= (int)sizeof(buf) - 1) {
	            buf[pos] = '\0';
	            pos = 0;

	            if (buf[0] == '\0') continue;    /* skip blank lines */
	            if (buf[0] == '#')  continue;    /* skip comment lines */

	            /* Convert TNC2 text to packet object. */
	            packet_t pp = ax25_from_text(buf, 1);
	            if (pp == NULL) {
	                text_color_set(DW_COLOR_ERROR);
	                dw_printf ("LoRa bridge: failed to parse: %s\n", buf);
	                continue;
	            }

	            /* Inject into Dire Wolf's received-frame queue. */
	            alevel_t alevel;
	            memset(&alevel, 0, sizeof(alevel));
	            fec_type_t fec_type = fec_type_none;
	            retry_t retries;
	            memset(&retries, 0, sizeof(retries));
	            char spectrum[] = "LoRa";

	            dlq_rec_frame(g_lora_chan, -3, 0, pp, alevel,
	                          fec_type, retries, spectrum);
	        }
	        else {
	            buf[pos++] = ch;
	        }
	    }

	    text_color_set(DW_COLOR_INFO);
	    dw_printf ("LoRa bridge: disconnected.\n");
	    s_sock = -1;
#if __WIN32__
	    closesocket(conn);
#else
	    close(conn);
#endif
	}

	return (0);  /* unreachable */
}


/*-------------------------------------------------------------------
 *
 * Name:        loratnc_send_packet
 *
 * Purpose:     Send a packet to the LoRa bridge for transmission.
 *
 * Inputs:      chan  - Must equal g_lora_chan.
 *              pp    - Packet object.  Caller retains ownership.
 *
 * Description: Formats the packet as a TNC2 text line and writes it
 *              to the bridge TCP connection.
 *
 *--------------------------------------------------------------------*/

void loratnc_send_packet (int chan, packet_t pp)
{
	(void)chan;   /* only one LoRa channel */

	if (s_sock < 0) {
	    text_color_set(DW_COLOR_INFO);
	    dw_printf ("LoRa bridge: not connected — packet dropped.\n");
	    return;
	}

	/* Build TNC2 line: "SRC>DST,PATH:info\n" */
	char addrs[256];
	ax25_format_addrs(pp, addrs);

	unsigned char *info_ptr;
	int info_len = ax25_get_info(pp, &info_ptr);

	char line[AX25_MAX_PACKET_LEN + 4];
	int  llen = snprintf(line, sizeof(line), "%s%.*s\n",
	                     addrs, info_len, (char *)info_ptr);

	if (llen <= 0 || llen >= (int)sizeof(line)) return;

#if __WIN32__
	int err = send(s_sock, line, llen, 0);
	if (err == SOCKET_ERROR) {
#else
	int err = write(s_sock, line, llen);
	if (err <= 0) {
#endif
	    text_color_set(DW_COLOR_ERROR);
	    dw_printf ("LoRa bridge: send failed — connection lost.\n");
	    s_sock = -1;
	}
}
