/*
 * loraspi.c  —  Native SPI LoRa driver for Dire Wolf.
 *
 * Supports SX1276/SX1278 (RFM95W) and SX1262/SX1268 LoRa modules wired
 * directly to a Raspberry Pi via SPI and GPIO.
 *
 * Hardware is selected by the LORAHW directive in direwolf.conf, which
 * maps to a built-in profile table (mirrors hardware_profiles.yaml).
 *
 * RF parameters (LORAFREQ, LORASF, LORABW, LORACR, LORASW, LORATXPOWER)
 * are read from the per-channel config populated by config.c.
 *
 * LoRa APRS air format:
 *   0x3C 0xFF 0x01 <TNC2 text>
 * The preamble is stripped on receive and prepended on transmit.
 *
 * Architecture:
 *   loraspi_init()  —  called once from direwolf.c at startup
 *     for each MEDIUM_LORA channel:
 *       open spidev, export/configure GPIO pins
 *       initialise LoRa chip (reset, configure modem params)
 *       start rx_thread  —  polls for received packets
 *       start tx_thread  —  drains the tx_queue
 *
 *   loraspi_send_packet()  —  called from tq.c
 *     converts AX.25 packet → TNC2 text
 *     pushes to per-channel tx_queue
 *
 * GPIO is accessed via Linux sysfs (/sys/class/gpio/) — no extra
 * library dependency beyond a standard Linux kernel.
 */

#include "direwolf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>
#include <dirent.h>

#include <math.h>
#include "textcolor.h"
#include "audio.h"
#include "ax25_pad.h"
#include "dlq.h"
#include "loraspi.h"

/* =========================================================================
 * Compile guard — Linux SPI/GPIO only
 * ========================================================================= */
#ifndef __linux__
void loraspi_init (struct audio_s *pa) { (void)pa; }
void loraspi_send_packet (int chan, packet_t pp) { (void)chan; (void)pp; }
int  loraspi_apply_profile (int chan, const char *name, struct audio_s *pa) { (void)chan; (void)name; (void)pa; return -1; }
#else

/* =========================================================================
 * TX queue
 * ========================================================================= */
#define TX_QUEUE_DEPTH 8

typedef struct {
    uint8_t  data[256];
    int      len;
} tx_item_t;

typedef struct {
    tx_item_t   items[TX_QUEUE_DEPTH];
    int         head, tail, count;
    pthread_mutex_t lock;
    pthread_cond_t  cond;
} tx_queue_t;

static void txq_init  (tx_queue_t *q) {
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init (&q->cond, NULL);
}

static bool txq_push (tx_queue_t *q, const uint8_t *data, int len) {
    pthread_mutex_lock(&q->lock);
    if (q->count >= TX_QUEUE_DEPTH) {
        pthread_mutex_unlock(&q->lock);
        return false;
    }
    tx_item_t *it = &q->items[q->tail];
    memcpy(it->data, data, len);
    it->len = len;
    q->tail = (q->tail + 1) % TX_QUEUE_DEPTH;
    q->count++;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->lock);
    return true;
}

static bool txq_pop (tx_queue_t *q, uint8_t *data, int *len) {
    pthread_mutex_lock(&q->lock);
    while (q->count == 0)
        pthread_cond_wait(&q->cond, &q->lock);
    tx_item_t *it = &q->items[q->head];
    memcpy(data, it->data, it->len);
    *len = it->len;
    q->head = (q->head + 1) % TX_QUEUE_DEPTH;
    q->count--;
    pthread_mutex_unlock(&q->lock);
    return true;
}

/* =========================================================================
 * Per-channel state
 * ========================================================================= */
typedef struct {
    int         chan;           /* Dire Wolf channel number */
    int         chip;           /* LORA_CHIP_SX1276 or LORA_CHIP_SX1262 */
    int         spi_fd;         /* /dev/spidev<bus>.<dev> */
    /* GPIO pin numbers (-1 = not used) */
    int         pin_cs;
    int         pin_reset;
    int         pin_irq;        /* DIO0 (SX1276) or DIO1 (SX1262) */
    int         pin_busy;       /* SX1262 only */
    int         pin_tx_en;
    int         pin_rx_en;
    /* RF config */
    float       freq_mhz;
    int         sf;
    int         bw_khz;
    int         cr;             /* coding rate denominator: 5..8 */
    int         sw;             /* sync word */
    int         txpower;        /* dBm */
    bool        pa_boost;       /* SX1276: PA_BOOST vs RFO */
    bool        tcxo;
    float       tcxo_voltage;
    /* Mutex protecting SPI bus — shared by rx_thread and tx_thread */
    pthread_mutex_t spi_lock;
    /* Threads */
    pthread_t   rx_thread;
    pthread_t   tx_thread;
    tx_queue_t  txq;
    volatile bool running;
} lora_chan_t;

#define MAX_LORA_CHANS 4
static lora_chan_t s_lora[MAX_LORA_CHANS];
static int         s_lora_count = 0;

/* =========================================================================
 * Hardware profile table  (mirrors hardware_profiles.yaml)
 * ========================================================================= */
typedef struct {
    const char *name;
    int  chip;          /* LORA_CHIP_* */
    int  spi_bus;
    int  spi_dev;
    int  pin_cs;
    int  pin_reset;
    int  pin_irq;
    int  pin_busy;
    int  pin_tx_en;
    int  pin_rx_en;
    bool pa_boost;
    bool tcxo;
    float tcxo_voltage;
} hw_profile_t;

static const hw_profile_t s_profiles[] = {
    /* name               chip            bus dev  cs  rst  irq busy txen rxen  pa_boost tcxo  tcxo_v */
    { "meshadv",          LORA_CHIP_SX1262, 0, 0, 21,  18,  16,  20,  13,  12,  false, true,  1.8f },
    { "e22_900m30s",      LORA_CHIP_SX1262, 0, 0,  8,  25,  24,  23,  -1,  22,  false, true,  1.8f },
    { "e22_400m30s",      LORA_CHIP_SX1262, 0, 0,  8,  25,  24,  23,  17,  27,  false, true,  1.8f },
    { "ebyte_e22",        LORA_CHIP_SX1262, 0, 0,  8,  25,  24,  23,  -1,  -1,  false, true,  1.8f },
    { "lorapi_rfm95w",    LORA_CHIP_SX1276, 0, 1,  7,  22,  -1,  -1,  -1,  -1,  true,  false, 0.0f },
    { "lorapi_rfm98w",    LORA_CHIP_SX1276, 0, 1,  7,  22,  -1,  -1,  -1,  -1,  true,  false, 0.0f },
    { "generic_sx1276",   LORA_CHIP_SX1276, 0, 0,  8,  25,  24,  -1,  -1,  -1,  true,  false, 0.0f },
    { NULL, 0, 0, 0, 0, 0, 0, 0, 0, 0, false, false, 0.0f }
};

static const hw_profile_t *find_profile (const char *name) {
    for (const hw_profile_t *p = s_profiles; p->name; p++)
        if (strcasecmp(p->name, name) == 0)
            return p;
    return NULL;
}

/* =========================================================================
 * GPIO via sysfs  (/sys/class/gpio/)
 *
 * On Raspberry Pi 3/4 the GPIO chip is registered at sysfs base 0, so
 * BCM pin N appears as /sys/class/gpio/gpioN.
 * On Raspberry Pi 5 the RP1 chip registers at a non-zero base (typically
 * 512), so BCM pin N appears as /sys/class/gpio/gpio(512+N).
 * gpio_sysfs_num() maps a BCM pin number to the correct sysfs number.
 * ========================================================================= */
static int s_gpio_base = -1;   /* -1 = not yet detected */

static int gpio_sysfs_num (int bcm_pin) {
    if (s_gpio_base < 0) {
        /* Scan /sys/class/gpio/ for gpiochipN; pick the chip with the
         * smallest base that has ngpio >= 28 (covers the Pi header GPIO). */
        int best = 0;
        bool found = false;
        DIR *d = opendir("/sys/class/gpio");
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                if (strncmp(de->d_name, "gpiochip", 8) != 0) continue;
                char p[128];
                snprintf(p, sizeof(p), "/sys/class/gpio/%s/base", de->d_name);
                int fd = open(p, O_RDONLY);
                if (fd < 0) continue;
                char buf[16]; int n = read(fd, buf, sizeof(buf)-1); close(fd);
                if (n <= 0) continue;
                buf[n] = '\0';
                int base = atoi(buf);
                snprintf(p, sizeof(p), "/sys/class/gpio/%s/ngpio", de->d_name);
                fd = open(p, O_RDONLY);
                if (fd < 0) continue;
                n = read(fd, buf, sizeof(buf)-1); close(fd);
                if (n <= 0) continue;
                buf[n] = '\0';
                int ngpio = atoi(buf);
                if (ngpio >= 28 && (!found || base < best)) {
                    best = base; found = true;
                }
            }
            closedir(d);
        }
        s_gpio_base = found ? best : 0;
        text_color_set(DW_COLOR_INFO);
        dw_printf ("loraspi: GPIO chip base offset = %d\n", s_gpio_base);
    }
    return s_gpio_base + bcm_pin;
}

static void gpio_export (int bcm_pin) {
    int sysfs_pin = gpio_sysfs_num(bcm_pin);
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d", sysfs_pin);
    if (access(path, F_OK) == 0) return;   /* already exported */
    int fd = open("/sys/class/gpio/export", O_WRONLY);
    if (fd < 0) return;
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", sysfs_pin);
    (void)write(fd, buf, strlen(buf));
    close(fd);
    usleep(50000);  /* wait for udev to set permissions */
}

static void gpio_direction (int bcm_pin, const char *dir) {
    int sysfs_pin = gpio_sysfs_num(bcm_pin);
    char path[80];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", sysfs_pin);
    int fd = open(path, O_WRONLY);
    if (fd < 0) return;
    (void)write(fd, dir, strlen(dir));
    close(fd);
}

static void gpio_write (int bcm_pin, int val) {
    int sysfs_pin = gpio_sysfs_num(bcm_pin);
    char path[80];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", sysfs_pin);
    int fd = open(path, O_WRONLY);
    if (fd < 0) return;
    (void)write(fd, val ? "1" : "0", 1);
    close(fd);
}

static int gpio_read (int bcm_pin) {
    int sysfs_pin = gpio_sysfs_num(bcm_pin);
    char path[80];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", sysfs_pin);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    char c = '0';
    (void)read(fd, &c, 1);
    close(fd);
    return c == '1';
}

static void gpio_setup_out (int pin, int initial) {
    if (pin < 0) return;
    gpio_export(pin);
    gpio_direction(pin, "out");
    gpio_write(pin, initial);
}

static void gpio_setup_in (int pin) {
    if (pin < 0) return;
    gpio_export(pin);
    gpio_direction(pin, "in");
}

/* =========================================================================
 * SPI via spidev
 * ========================================================================= */
static int spi_open (int bus, int dev, uint32_t speed_hz) {
    char path[32];
    snprintf(path, sizeof(path), "/dev/spidev%d.%d", bus, dev);
    int fd = open(path, O_RDWR);
    if (fd < 0) {
        text_color_set(DW_COLOR_ERROR);
        dw_printf ("loraspi: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }
    uint8_t mode = SPI_MODE_0;
    uint8_t bits = 8;
    ioctl(fd, SPI_IOC_WR_MODE,           &mode);
    ioctl(fd, SPI_IOC_WR_BITS_PER_WORD,  &bits);
    ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ,   &speed_hz);
    return fd;
}

static void spi_transfer (int fd, int cs_pin, const uint8_t *tx, uint8_t *rx, int len) {
    if (cs_pin >= 0) gpio_write(cs_pin, 0);
    struct spi_ioc_transfer t = {
        .tx_buf        = (unsigned long)tx,
        .rx_buf        = (unsigned long)rx,
        .len           = (uint32_t)len,
        .speed_hz      = 0,
        .delay_usecs   = 0,
        .bits_per_word = 8,
    };
    int ret = ioctl(fd, SPI_IOC_MESSAGE(1), &t);
    if (ret < 0) {
        text_color_set(DW_COLOR_ERROR);
        dw_printf ("loraspi: spi_transfer ioctl failed: fd=%d ret=%d errno=%d (%s)\n",
                   fd, ret, errno, strerror(errno));
    }
    if (cs_pin >= 0) gpio_write(cs_pin, 1);
}

/* =========================================================================
 * SX1276 register-level driver
 * ========================================================================= */

/* Register addresses */
#define SX1276_REG_FIFO             0x00
#define SX1276_REG_OP_MODE          0x01
#define SX1276_REG_FRF_MSB          0x06
#define SX1276_REG_FRF_MID          0x07
#define SX1276_REG_FRF_LSB          0x08
#define SX1276_REG_PA_CONFIG        0x09
#define SX1276_REG_PA_DAC           0x4D
#define SX1276_REG_LNA              0x0C
#define SX1276_REG_FIFO_ADDR_PTR    0x0D
#define SX1276_REG_FIFO_TX_BASE     0x0E
#define SX1276_REG_FIFO_RX_BASE     0x0F
#define SX1276_REG_FIFO_RX_CURRENT  0x10
#define SX1276_REG_IRQ_FLAGS_MASK   0x11
#define SX1276_REG_IRQ_FLAGS        0x12
#define SX1276_REG_RX_NB_BYTES      0x13
#define SX1276_REG_MODEM_CONFIG1    0x1D
#define SX1276_REG_MODEM_CONFIG2    0x1E
#define SX1276_REG_SYMB_TIMEOUT_LSB 0x1F
#define SX1276_REG_PREAMBLE_MSB     0x20
#define SX1276_REG_PREAMBLE_LSB     0x21
#define SX1276_REG_PAYLOAD_LENGTH   0x22
#define SX1276_REG_MAX_PAYLOAD      0x23
#define SX1276_REG_MODEM_CONFIG3    0x26
#define SX1276_REG_RSSI_VALUE       0x1A
#define SX1276_REG_PKT_SNR          0x19
#define SX1276_REG_PKT_RSSI         0x1A
#define SX1276_REG_DETECT_OPTIMIZE  0x31
#define SX1276_REG_INVERT_IQ        0x33
#define SX1276_REG_DETECT_THRESH    0x37
#define SX1276_REG_SYNC_WORD        0x39
#define SX1276_REG_DIO_MAPPING1     0x40
#define SX1276_REG_VERSION          0x42

/* OP_MODE values (LoRa mode bit 7 set) */
#define SX1276_MODE_SLEEP       0x80
#define SX1276_MODE_STDBY       0x81
#define SX1276_MODE_TX          0x83
#define SX1276_MODE_RX_CONT     0x85
#define SX1276_MODE_RX_SINGLE   0x86

/* IRQ flags */
#define SX1276_IRQ_RX_DONE      0x40
#define SX1276_IRQ_TX_DONE      0x08
#define SX1276_IRQ_CRC_ERR      0x20
#define SX1276_IRQ_VALID_HDR    0x10

static uint8_t sx1276_read_reg (lora_chan_t *lc, uint8_t reg) {
    uint8_t tx[2] = { reg & 0x7F, 0x00 };
    uint8_t rx[2] = { 0, 0 };
    spi_transfer(lc->spi_fd, lc->pin_cs, tx, rx, 2);
    return rx[1];
}

static void sx1276_write_reg (lora_chan_t *lc, uint8_t reg, uint8_t val) {
    uint8_t tx[2] = { reg | 0x80, val };
    uint8_t rx[2];
    spi_transfer(lc->spi_fd, lc->pin_cs, tx, rx, 2);
}

static void sx1276_reset (lora_chan_t *lc) {
    if (lc->pin_reset < 0) return;
    gpio_write(lc->pin_reset, 0);
    usleep(10000);
    gpio_write(lc->pin_reset, 1);
    usleep(10000);
}

static bool sx1276_init (lora_chan_t *lc) {
    sx1276_reset(lc);

    uint8_t ver = sx1276_read_reg(lc, SX1276_REG_VERSION);
    if (ver != 0x12) {
        text_color_set(DW_COLOR_ERROR);
        dw_printf ("loraspi: SX1276 not found on channel %d (version=0x%02X, expected 0x12)\n",
                   lc->chan, ver);
        return false;
    }

    /* Switch to LoRa sleep mode */
    sx1276_write_reg(lc, SX1276_REG_OP_MODE, SX1276_MODE_SLEEP);
    usleep(10000);

    /* Set frequency */
    uint64_t frf = (uint64_t)((double)lc->freq_mhz * 1e6 / 32e6 * (double)(1ULL << 19));
    sx1276_write_reg(lc, SX1276_REG_FRF_MSB, (frf >> 16) & 0xFF);
    sx1276_write_reg(lc, SX1276_REG_FRF_MID, (frf >>  8) & 0xFF);
    sx1276_write_reg(lc, SX1276_REG_FRF_LSB,  frf        & 0xFF);

    /* BW: 7.8=0 10.4=1 15.6=2 20.8=3 31.25=4 41.7=5 62.5=6 125=7 250=8 500=9 */
    int bw_code;
    if      (lc->bw_khz <= 8)   bw_code = 0;
    else if (lc->bw_khz <= 11)  bw_code = 1;
    else if (lc->bw_khz <= 16)  bw_code = 2;
    else if (lc->bw_khz <= 21)  bw_code = 3;
    else if (lc->bw_khz <= 32)  bw_code = 4;
    else if (lc->bw_khz <= 42)  bw_code = 5;
    else if (lc->bw_khz <= 63)  bw_code = 6;
    else if (lc->bw_khz <= 125) bw_code = 7;
    else if (lc->bw_khz <= 250) bw_code = 8;
    else                         bw_code = 9;

    int cr_code = lc->cr - 4;   /* 4/5=1 4/6=2 4/7=3 4/8=4 */
    sx1276_write_reg(lc, SX1276_REG_MODEM_CONFIG1,
        (bw_code << 4) | (cr_code << 1));

    /* SF, CRC on, RX timeout MSB */
    sx1276_write_reg(lc, SX1276_REG_MODEM_CONFIG2,
        (lc->sf << 4) | 0x04);   /* 0x04 = RX payload CRC on */

    /* Low data rate optimise for SF11/SF12 at BW125 */
    bool ldro = (lc->sf >= 11 && lc->bw_khz <= 125);
    sx1276_write_reg(lc, SX1276_REG_MODEM_CONFIG3, ldro ? 0x08 : 0x00);

    /* Preamble length = 8 */
    sx1276_write_reg(lc, SX1276_REG_PREAMBLE_MSB, 0x00);
    sx1276_write_reg(lc, SX1276_REG_PREAMBLE_LSB, 0x08);

    /* Max payload */
    sx1276_write_reg(lc, SX1276_REG_MAX_PAYLOAD, 0xFF);

    /* Sync word */
    sx1276_write_reg(lc, SX1276_REG_SYNC_WORD, (uint8_t)lc->sw);

    /* LoRa detection optimisation */
    sx1276_write_reg(lc, SX1276_REG_DETECT_OPTIMIZE, 0xC3);
    sx1276_write_reg(lc, SX1276_REG_DETECT_THRESH,   0x0A);

    /* Boosted LNA */
    sx1276_write_reg(lc, SX1276_REG_LNA, 0x23);

    /* TX power */
    if (lc->pa_boost) {
        if (lc->txpower > 17) {
            sx1276_write_reg(lc, SX1276_REG_PA_DAC, 0x87);
            sx1276_write_reg(lc, SX1276_REG_PA_CONFIG, 0xFF);
        } else {
            sx1276_write_reg(lc, SX1276_REG_PA_DAC, 0x84);
            sx1276_write_reg(lc, SX1276_REG_PA_CONFIG,
                0x80 | (uint8_t)(lc->txpower - 2));
        }
    } else {
        /* RFO path: max 14 dBm */
        int pwr = lc->txpower < 0 ? 0 : (lc->txpower > 14 ? 14 : lc->txpower);
        sx1276_write_reg(lc, SX1276_REG_PA_CONFIG, (uint8_t)(0x70 | pwr));
    }

    /* FIFO base addresses */
    sx1276_write_reg(lc, SX1276_REG_FIFO_TX_BASE, 0x00);
    sx1276_write_reg(lc, SX1276_REG_FIFO_RX_BASE, 0x00);

    /* Standby */
    sx1276_write_reg(lc, SX1276_REG_OP_MODE, SX1276_MODE_STDBY);
    usleep(10000);

    /* Read back actual frequency from registers as a sanity check */
    uint32_t frf_check = ((uint32_t)sx1276_read_reg(lc, SX1276_REG_FRF_MSB) << 16) |
                         ((uint32_t)sx1276_read_reg(lc, SX1276_REG_FRF_MID) <<  8) |
                          (uint32_t)sx1276_read_reg(lc, SX1276_REG_FRF_LSB);
    double freq_actual = (double)frf_check * 32e6 / (double)(1ULL << 19);
    if (fabs(freq_actual - (double)lc->freq_mhz * 1e6) > 1000.0) {
        text_color_set(DW_COLOR_ERROR);
        dw_printf ("loraspi ch%d: WARNING frequency mismatch — config %.3f MHz, chip %.3f MHz\n",
                   lc->chan, lc->freq_mhz, freq_actual / 1e6);
    }

    return true;
}

static int sx1276_receive (lora_chan_t *lc, uint8_t *buf, int maxlen,
                           int *rssi_out, float *snr_out) {
    uint8_t irq = sx1276_read_reg(lc, SX1276_REG_IRQ_FLAGS);
    sx1276_write_reg(lc, SX1276_REG_IRQ_FLAGS, 0xFF);   /* clear all */

    if (!(irq & SX1276_IRQ_RX_DONE)) return 0;
    if   (irq & SX1276_IRQ_CRC_ERR)  return -1;

    int nb  = sx1276_read_reg(lc, SX1276_REG_RX_NB_BYTES);
    int ptr = sx1276_read_reg(lc, SX1276_REG_FIFO_RX_CURRENT);
    if (nb <= 0 || nb > maxlen) return -1;

    sx1276_write_reg(lc, SX1276_REG_FIFO_ADDR_PTR, (uint8_t)ptr);

    /* Burst-read FIFO */
    uint8_t tx_buf[257];
    uint8_t rx_buf[257];
    tx_buf[0] = SX1276_REG_FIFO & 0x7F;
    memset(tx_buf + 1, 0, nb);
    spi_transfer(lc->spi_fd, lc->pin_cs, tx_buf, rx_buf, nb + 1);
    memcpy(buf, rx_buf + 1, nb);

    /* SNR (signed byte, units = 0.25 dB) */
    int8_t snr_raw = (int8_t)sx1276_read_reg(lc, SX1276_REG_PKT_SNR);
    *snr_out  = snr_raw / 4.0f;

    /* RSSI (HF port 868/915 MHz uses -157, LF port 433 MHz uses -164) */
    int rssi_offset = (lc->freq_mhz < 600.0f) ? -164 : -157;
    *rssi_out = rssi_offset + sx1276_read_reg(lc, SX1276_REG_PKT_RSSI);

    return nb;
}

static bool sx1276_transmit (lora_chan_t *lc, const uint8_t *data, int len) {
    sx1276_write_reg(lc, SX1276_REG_OP_MODE, SX1276_MODE_STDBY);
    usleep(1000);

    sx1276_write_reg(lc, SX1276_REG_FIFO_ADDR_PTR, 0x00);
    sx1276_write_reg(lc, SX1276_REG_PAYLOAD_LENGTH, (uint8_t)len);

    /* Burst-write FIFO */
    uint8_t tx_buf[257];
    uint8_t rx_buf[257];
    tx_buf[0] = SX1276_REG_FIFO | 0x80;
    memcpy(tx_buf + 1, data, len);
    spi_transfer(lc->spi_fd, lc->pin_cs, tx_buf, rx_buf, len + 1);

    sx1276_write_reg(lc, SX1276_REG_OP_MODE, SX1276_MODE_TX);

    /* Poll TX_DONE (timeout 10 s) */
    for (int i = 0; i < 10000; i++) {
        usleep(1000);
        if (sx1276_read_reg(lc, SX1276_REG_IRQ_FLAGS) & SX1276_IRQ_TX_DONE) {
            sx1276_write_reg(lc, SX1276_REG_IRQ_FLAGS, 0xFF);
            return true;
        }
    }
    sx1276_write_reg(lc, SX1276_REG_IRQ_FLAGS, 0xFF);
    return false;
}

static void sx1276_start_rx (lora_chan_t *lc) {
    sx1276_write_reg(lc, SX1276_REG_FIFO_RX_BASE,  0x00);
    sx1276_write_reg(lc, SX1276_REG_FIFO_ADDR_PTR, 0x00);
    sx1276_write_reg(lc, SX1276_REG_IRQ_FLAGS,      0xFF);
    sx1276_write_reg(lc, SX1276_REG_OP_MODE, SX1276_MODE_RX_CONT);
}

/* =========================================================================
 * SX1262 command-level driver
 * ========================================================================= */

/* SX1262 command opcodes */
#define SX1262_CMD_SET_SLEEP             0x84
#define SX1262_CMD_SET_STANDBY           0x80
#define SX1262_CMD_SET_TX                0x83
#define SX1262_CMD_SET_RX                0x82
#define SX1262_CMD_SET_PACKET_TYPE       0x8A
#define SX1262_CMD_SET_RF_FREQUENCY      0x86
#define SX1262_CMD_SET_TX_PARAMS         0x8E
#define SX1262_CMD_SET_PA_CONFIG         0x95
#define SX1262_CMD_SET_MODULATION_PARAMS 0x8B
#define SX1262_CMD_SET_PACKET_PARAMS     0x8C
#define SX1262_CMD_SET_BUFFER_BASE_ADDR  0x8F
#define SX1262_CMD_GET_RX_BUFFER_STATUS  0x13
#define SX1262_CMD_GET_PACKET_STATUS     0x14
#define SX1262_CMD_READ_BUFFER           0x1E
#define SX1262_CMD_WRITE_BUFFER          0x0E
#define SX1262_CMD_WRITE_REGISTER        0x0D
#define SX1262_CMD_READ_REGISTER         0x1D
#define SX1262_CMD_SET_DIO2_RF_SWITCH    0x9D
#define SX1262_CMD_SET_DIO3_TCXO         0x97
#define SX1262_CMD_CALIBRATE_IMAGE       0x98
#define SX1262_CMD_CLR_IRQ_STATUS        0x02
#define SX1262_CMD_SET_IRQ_PARAMS        0x08
#define SX1262_CMD_GET_IRQ_STATUS        0x12

#define SX1262_IRQ_RX_DONE  0x0002
#define SX1262_IRQ_TX_DONE  0x0001
#define SX1262_IRQ_CRC_ERR  0x0040

static void sx1262_wait_busy (lora_chan_t *lc) {
    if (lc->pin_busy < 0) return;
    for (int i = 0; i < 10000; i++) {
        if (!gpio_read(lc->pin_busy)) return;
        usleep(100);
    }
    text_color_set(DW_COLOR_ERROR);
    dw_printf ("loraspi: SX1262 BUSY timeout on channel %d\n", lc->chan);
}

static void sx1262_cmd (lora_chan_t *lc, const uint8_t *tx, uint8_t *rx, int len) {
    sx1262_wait_busy(lc);
    spi_transfer(lc->spi_fd, lc->pin_cs, tx, rx, len);
}

static void sx1262_write_reg (lora_chan_t *lc, uint16_t addr, uint8_t val) {
    uint8_t tx[4] = { SX1262_CMD_WRITE_REGISTER,
                      (addr >> 8) & 0xFF, addr & 0xFF, val };
    uint8_t rx[4];
    sx1262_cmd(lc, tx, rx, 4);
}

static void sx1262_reset (lora_chan_t *lc) {
    if (lc->pin_reset < 0) return;
    gpio_write(lc->pin_reset, 0);
    usleep(200);
    gpio_write(lc->pin_reset, 1);
    usleep(10000);
}

static bool sx1262_init (lora_chan_t *lc) {
    sx1262_reset(lc);

    /* Standby XOSC */
    uint8_t cmd[2] = { SX1262_CMD_SET_STANDBY, 0x01 };
    uint8_t rx[2];
    sx1262_cmd(lc, cmd, rx, 2);
    usleep(10000);

    /* TCXO control on DIO3 */
    if (lc->tcxo) {
        /* Voltage codes: 1.6=0 1.7=1 1.8=2 2.2=3 2.4=4 2.7=5 3.0=6 3.3=7 */
        uint8_t vcode = 2;  /* default 1.8 V */
        if      (lc->tcxo_voltage < 1.65f) vcode = 0;
        else if (lc->tcxo_voltage < 1.75f) vcode = 1;
        else if (lc->tcxo_voltage < 2.0f)  vcode = 2;
        else if (lc->tcxo_voltage < 2.3f)  vcode = 3;
        else if (lc->tcxo_voltage < 2.55f) vcode = 4;
        else if (lc->tcxo_voltage < 2.85f) vcode = 5;
        else if (lc->tcxo_voltage < 3.15f) vcode = 6;
        else                                vcode = 7;
        uint8_t tcxo_cmd[5] = { SX1262_CMD_SET_DIO3_TCXO, vcode, 0x00, 0x01, 0x40 }; /* 320 * 15.625 us = 5 ms */
        uint8_t tcxo_rx[5];
        sx1262_cmd(lc, tcxo_cmd, tcxo_rx, 5);
        usleep(5000);
    }

    /* DIO2 as RF switch (handles TXEN/RXEN internally on most modules) */
    uint8_t dio2_cmd[2] = { SX1262_CMD_SET_DIO2_RF_SWITCH, 0x01 };
    uint8_t dio2_rx[2];
    sx1262_cmd(lc, dio2_cmd, dio2_rx, 2);

    /* Image calibration for the frequency band */
    uint8_t cal_cmd[3] = { SX1262_CMD_CALIBRATE_IMAGE, 0, 0 };
    if (lc->freq_mhz < 450.0f)       { cal_cmd[1] = 0x6B; cal_cmd[2] = 0x6F; }
    else if (lc->freq_mhz < 900.0f)  { cal_cmd[1] = 0xC1; cal_cmd[2] = 0xC5; }
    else                              { cal_cmd[1] = 0xD7; cal_cmd[2] = 0xDB; }
    uint8_t cal_rx[3];
    sx1262_cmd(lc, cal_cmd, cal_rx, 3);
    usleep(5000);

    /* LoRa packet type */
    uint8_t pt[2] = { SX1262_CMD_SET_PACKET_TYPE, 0x01 };
    uint8_t pt_rx[2];
    sx1262_cmd(lc, pt, pt_rx, 2);

    /* RF frequency */
    uint32_t frf = (uint32_t)((double)lc->freq_mhz * 1e6 / 0.95367431640625);
    uint8_t freq_cmd[5] = { SX1262_CMD_SET_RF_FREQUENCY,
        (frf >> 24) & 0xFF, (frf >> 16) & 0xFF,
        (frf >>  8) & 0xFF,  frf        & 0xFF };
    uint8_t freq_rx[5];
    sx1262_cmd(lc, freq_cmd, freq_rx, 5);

    /* PA config for SX1262 (22 dBm max) */
    uint8_t pa_cmd[5] = { SX1262_CMD_SET_PA_CONFIG, 0x04, 0x07, 0x00, 0x01 };
    uint8_t pa_rx[5];
    sx1262_cmd(lc, pa_cmd, pa_rx, 5);

    /* TX params: power, ramp time (40 us = 0x04) */
    int8_t pwr = (int8_t)(lc->txpower > 22 ? 22 : lc->txpower);
    uint8_t tx_params[3] = { SX1262_CMD_SET_TX_PARAMS, (uint8_t)pwr, 0x04 };
    uint8_t tx_rx[3];
    sx1262_cmd(lc, tx_params, tx_rx, 3);

    /* Modulation params: SF, BW, CR, LDRO */
    /* BW codes: 7.81=0x00 10.42=0x08 15.63=0x01 20.83=0x09 31.25=0x02
                 41.67=0x0A 62.5=0x03 125=0x04 250=0x05 500=0x06 */
    uint8_t bw_code;
    if      (lc->bw_khz <= 8)   bw_code = 0x00;
    else if (lc->bw_khz <= 11)  bw_code = 0x08;
    else if (lc->bw_khz <= 16)  bw_code = 0x01;
    else if (lc->bw_khz <= 21)  bw_code = 0x09;
    else if (lc->bw_khz <= 32)  bw_code = 0x02;
    else if (lc->bw_khz <= 42)  bw_code = 0x0A;
    else if (lc->bw_khz <= 63)  bw_code = 0x03;
    else if (lc->bw_khz <= 125) bw_code = 0x04;
    else if (lc->bw_khz <= 250) bw_code = 0x05;
    else                         bw_code = 0x06;

    uint8_t cr_code = (uint8_t)(lc->cr - 4);   /* 4/5=1 4/6=2 4/7=3 4/8=4 */
    bool ldro = (lc->sf >= 11 && lc->bw_khz <= 125);
    uint8_t mod[5] = { SX1262_CMD_SET_MODULATION_PARAMS,
        (uint8_t)lc->sf, bw_code, cr_code, ldro ? 0x01 : 0x00 };
    uint8_t mod_rx[5];
    sx1262_cmd(lc, mod, mod_rx, 5);

    /* Packet params: preamble=8, explicit header, max payload=255, CRC off, IQ normal */
    uint8_t pkt[7] = { SX1262_CMD_SET_PACKET_PARAMS,
        0x00, 0x08,   /* preamble = 8 */
        0x00,         /* explicit header */
        0xFF,         /* max payload */
        0x00,         /* CRC off (LoRa APRS uses AX.25 FCS) */
        0x00 };       /* standard IQ */
    uint8_t pkt_rx[7];
    sx1262_cmd(lc, pkt, pkt_rx, 7);

    /* Sync word: SX1262 uses 2 bytes at 0x0740/0x0741.
     * The SX1276-style single byte (e.g. 0x12) maps to SX1262 format by
     * expanding each nibble: 0x12 -> 0x1424, 0x34 -> 0x3444.
     * Formula: high = (sw >> 4 & 0x0F) << 4 | 0x04
     *          low  = (sw      & 0x0F) << 4 | 0x04  */
    uint8_t sw_hi = (uint8_t)(((lc->sw >> 4) & 0x0F) << 4 | 0x04);
    uint8_t sw_lo = (uint8_t)(( lc->sw       & 0x0F) << 4 | 0x04);
    sx1262_write_reg(lc, 0x0740, sw_hi);
    sx1262_write_reg(lc, 0x0741, sw_lo);

    /* Buffer base addresses: TX=0x00, RX=0x80 (separate halves to prevent overlap) */
    uint8_t buf_base[3] = { SX1262_CMD_SET_BUFFER_BASE_ADDR, 0x00, 0x80 };
    uint8_t buf_rx[3];
    sx1262_cmd(lc, buf_base, buf_rx, 3);

    /* Enable RX_DONE and TX_DONE IRQs on DIO1 */
    uint8_t irq_cmd[9] = { SX1262_CMD_SET_IRQ_PARAMS,
        0x00, 0x43,   /* IRQ mask: RX_DONE | TX_DONE | CRC_ERR */
        0x00, 0x43,   /* DIO1 mask */
        0x00, 0x00,   /* DIO2 mask */
        0x00, 0x00 }; /* DIO3 mask */
    uint8_t irq_rx[9];
    sx1262_cmd(lc, irq_cmd, irq_rx, 9);

    return true;
}

static int sx1262_receive (lora_chan_t *lc, uint8_t *buf, int maxlen,
                           int *rssi_out, float *snr_out) {
    /* Read and clear IRQ status */
    uint8_t get_irq[4] = { SX1262_CMD_GET_IRQ_STATUS, 0, 0, 0 };
    uint8_t irq_rx[4];
    sx1262_cmd(lc, get_irq, irq_rx, 4);
    /* irq_rx[0]=status, [1]=echoed status, [2]=IRQ[15:8], [3]=IRQ[7:0] */
    uint16_t irq = ((uint16_t)irq_rx[2] << 8) | irq_rx[3];

    uint8_t clr[3] = { SX1262_CMD_CLR_IRQ_STATUS, 0xFF, 0xFF };
    uint8_t clr_rx[3];
    sx1262_cmd(lc, clr, clr_rx, 3);

    if (!(irq & SX1262_IRQ_RX_DONE)) return 0;
    if   (irq & SX1262_IRQ_CRC_ERR)  return -1;

    /* Get payload length and buffer offset */
    uint8_t get_buf[4] = { SX1262_CMD_GET_RX_BUFFER_STATUS, 0, 0, 0 };
    uint8_t buf_rx[4];
    sx1262_cmd(lc, get_buf, buf_rx, 4);
    /* SX1262 echoes status on both byte[0] and byte[1]; data starts at byte[2] */
    /* buf_rx[0]=status, [1]=status(echo), [2]=payloadLength, [3]=rxStartBufferPointer */
    int nb     = buf_rx[2];
    int offset = buf_rx[3];
    if (nb <= 0 || nb > maxlen) return -1;

    /* Read buffer (fixed max 256 bytes payload) */
    uint8_t rd_cmd[256 + 3];
    uint8_t rd_rx[256 + 3];
    rd_cmd[0] = SX1262_CMD_READ_BUFFER;
    rd_cmd[1] = (uint8_t)offset;
    rd_cmd[2] = 0x00;   /* NOP status byte */
    memset(rd_cmd + 3, 0, nb);
    sx1262_cmd(lc, rd_cmd, rd_rx, nb + 3);
    memcpy(buf, rd_rx + 3, nb);

    /* Packet status: RSSI, SNR */
    uint8_t ps_cmd[4] = { SX1262_CMD_GET_PACKET_STATUS, 0, 0, 0 };
    uint8_t ps_rx[4];
    sx1262_cmd(lc, ps_cmd, ps_rx, 4);
    *rssi_out = -(ps_rx[1] / 2);
    *snr_out  = (int8_t)ps_rx[2] / 4.0f;

    return nb;
}

static bool sx1262_transmit (lora_chan_t *lc, const uint8_t *data, int len) {
    if (len > 256) return false;

    /* Go to STBY_RC before TX */
    uint8_t stby[2] = { SX1262_CMD_SET_STANDBY, 0x00 };   /* STBY_RC */
    uint8_t stby_rx[2];
    sx1262_cmd(lc, stby, stby_rx, 2);
    usleep(50000);  /* 50 ms — let chip settle */

    /* Enable TX path, disable RX path */
    if (lc->pin_tx_en >= 0) gpio_write(lc->pin_tx_en, 1);
    if (lc->pin_rx_en >= 0) gpio_write(lc->pin_rx_en, 0);

    /* Write buffer */
    uint8_t wr_cmd[256 + 2];
    uint8_t wr_rx[256 + 2];
    wr_cmd[0] = SX1262_CMD_WRITE_BUFFER;
    wr_cmd[1] = 0x00;   /* offset */
    memcpy(wr_cmd + 2, data, len);
    sx1262_cmd(lc, wr_cmd, wr_rx, len + 2);

    /* Packet params: set actual payload length */
    uint8_t pkt[7] = { SX1262_CMD_SET_PACKET_PARAMS,
        0x00, 0x08, 0x00, (uint8_t)len, 0x00, 0x00 };
    uint8_t pkt_rx[7];
    sx1262_cmd(lc, pkt, pkt_rx, 7);

    /* SetTx: timeout=0 (no timeout) */
    uint8_t tx_cmd[4] = { SX1262_CMD_SET_TX, 0x00, 0x00, 0x00 };
    uint8_t tx_rx[4];
    sx1262_cmd(lc, tx_cmd, tx_rx, 4);

    /* Poll TX_DONE via IRQ (timeout 12 s) */
    bool ok = false;
    for (int i = 0; i < 12000; i++) {
        usleep(1000);
        uint8_t gi[4] = { SX1262_CMD_GET_IRQ_STATUS, 0, 0, 0 };
        uint8_t gr[4];
        sx1262_cmd(lc, gi, gr, 4);
        /* gr[0]=status, [1]=echoed status, [2]=IRQ[15:8], [3]=IRQ[7:0] */
        uint16_t irq = ((uint16_t)gr[2] << 8) | gr[3];
        if (irq & SX1262_IRQ_TX_DONE) {
            uint8_t clr[3] = { SX1262_CMD_CLR_IRQ_STATUS, 0xFF, 0xFF };
            uint8_t clr_rx[3];
            sx1262_cmd(lc, clr, clr_rx, 3);
            ok = true;
            break;
        }
    }

    /* Restore RX path */
    if (lc->pin_tx_en >= 0) gpio_write(lc->pin_tx_en, 0);
    if (lc->pin_rx_en >= 0) gpio_write(lc->pin_rx_en, 1);

    return ok;
}

static void sx1262_start_rx (lora_chan_t *lc) {
    /* Enable RX path, disable TX path */
    if (lc->pin_tx_en >= 0) gpio_write(lc->pin_tx_en, 0);
    if (lc->pin_rx_en >= 0) gpio_write(lc->pin_rx_en, 1);

    uint8_t clr[3] = { SX1262_CMD_CLR_IRQ_STATUS, 0xFF, 0xFF };
    uint8_t clr_rx[3];
    sx1262_cmd(lc, clr, clr_rx, 3);
    /* SetRx: timeout=0xFFFFFF = continuous */
    uint8_t rx_cmd[4] = { SX1262_CMD_SET_RX, 0xFF, 0xFF, 0xFF };
    uint8_t rx_rx[4];
    sx1262_cmd(lc, rx_cmd, rx_rx, 4);
}

/* =========================================================================
 * Chip-agnostic wrappers
 * ========================================================================= */

static bool chip_init (lora_chan_t *lc) {
    if (lc->chip == LORA_CHIP_SX1276) return sx1276_init(lc);
    if (lc->chip == LORA_CHIP_SX1262) return sx1262_init(lc);
    return false;
}

static void chip_start_rx (lora_chan_t *lc) {
    if (lc->chip == LORA_CHIP_SX1276) sx1276_start_rx(lc);
    if (lc->chip == LORA_CHIP_SX1262) sx1262_start_rx(lc);
}

static int chip_receive (lora_chan_t *lc, uint8_t *buf, int maxlen,
                         int *rssi, float *snr) {
    if (lc->chip == LORA_CHIP_SX1276)
        return sx1276_receive(lc, buf, maxlen, rssi, snr);
    if (lc->chip == LORA_CHIP_SX1262)
        return sx1262_receive(lc, buf, maxlen, rssi, snr);
    return 0;
}

static bool chip_transmit (lora_chan_t *lc, const uint8_t *data, int len) {
    bool ok;
    if (lc->chip == LORA_CHIP_SX1276) ok = sx1276_transmit(lc, data, len);
    else if (lc->chip == LORA_CHIP_SX1262) ok = sx1262_transmit(lc, data, len);
    else ok = false;
    /* Return to continuous RX after TX */
    chip_start_rx(lc);
    return ok;
}

/* =========================================================================
 * TNC2 <-> AX.25 helpers
 * =========================================================================
 *
 * LoRa APRS uses TNC2 ASCII text on the air.  Dire Wolf works with binary
 * AX.25 packet objects internally.  These helpers bridge the two.
 *
 * On receive: strip 3-byte preamble, pass TNC2 text to ax25_from_text().
 * On transmit: ax25_to_text() → prepend preamble → send over LoRa.
 */

#define LORA_PREAMBLE_LEN 3
static const uint8_t LORA_PREAMBLE[LORA_PREAMBLE_LEN] = { 0x3C, 0xFF, 0x01 };

/* =========================================================================
 * RX thread
 * ========================================================================= */
static void *rx_thread (void *arg) {
    lora_chan_t *lc = (lora_chan_t *)arg;
    uint8_t buf[256];

    pthread_mutex_lock(&lc->spi_lock);
    chip_start_rx(lc);
    pthread_mutex_unlock(&lc->spi_lock);

    while (lc->running) {
        int rssi = 0;
        float snr = 0.0f;
        pthread_mutex_lock(&lc->spi_lock);
        int nb = chip_receive(lc, buf, sizeof(buf), &rssi, &snr);
        pthread_mutex_unlock(&lc->spi_lock);

        if (nb > 0) {
            /* Strip LoRa APRS preamble (0x3C 0xFF 0x01) if present */
            uint8_t *payload = buf;
            int plen = nb;
            if (plen >= LORA_PREAMBLE_LEN &&
                memcmp(payload, LORA_PREAMBLE, LORA_PREAMBLE_LEN) == 0) {
                payload += LORA_PREAMBLE_LEN;
                plen    -= LORA_PREAMBLE_LEN;
            }
            if (plen <= 0) goto next;

            /* Null-terminate and convert to TNC2 string */
            char tnc2[256];
            if (plen >= (int)sizeof(tnc2)) plen = (int)sizeof(tnc2) - 1;
            memcpy(tnc2, payload, plen);
            tnc2[plen] = '\0';

            /* Strip trailing whitespace */
            for (int i = plen - 1; i >= 0 && (tnc2[i] == '\r' || tnc2[i] == '\n' || tnc2[i] == ' '); i--)
                tnc2[i] = '\0';

            /* Parse TNC2 into AX.25 packet object */
            packet_t pp = ax25_from_text(tnc2, 1);
            if (pp == NULL) {
                text_color_set(DW_COLOR_ERROR);
                dw_printf ("loraspi: failed to parse: %s\n", tnc2);
                goto next;
            }

            /* Build signal level struct for display */
            alevel_t alevel;
            memset(&alevel, 0, sizeof(alevel));
            alevel.rec = rssi;

            /* Build spectrum string: "LoRa SNR=X.XdB" */
            char spectrum[32];
            snprintf(spectrum, sizeof(spectrum), "LoRa SNR=%.1fdB", snr);

            /* Inject into Dire Wolf frame queue */
            dlq_rec_frame(lc->chan, -3, 0, pp, alevel, fec_type_none, RETRY_NONE, spectrum);

            text_color_set(DW_COLOR_REC);
            dw_printf ("%s  RSSI=%d dBm  SNR=%.1f dB\n", tnc2, rssi, snr);
        }

    next:
        /* 10 ms poll interval — low CPU, ~100 Hz check rate */
        usleep(10000);
    }
    return NULL;
}

/* =========================================================================
 * TX thread
 * ========================================================================= */
static void *tx_thread (void *arg) {
    lora_chan_t *lc = (lora_chan_t *)arg;
    uint8_t data[256];
    int len;

    while (lc->running) {
        txq_pop(&lc->txq, data, &len);
        if (!lc->running) break;

        pthread_mutex_lock(&lc->spi_lock);
        bool ok = chip_transmit(lc, data, len);
        /* SX1276/SX1262 drops to STDBY after TX_DONE — re-arm RX immediately */
        chip_start_rx(lc);
        pthread_mutex_unlock(&lc->spi_lock);

        text_color_set(ok ? DW_COLOR_XMIT : DW_COLOR_ERROR);
        dw_printf ("loraspi: TX %s (%d bytes)\n", ok ? "OK" : "FAILED", len);
    }
    return NULL;
}

/* =========================================================================
 * Public API
 * ========================================================================= */

void loraspi_init (struct audio_s *pa) {
    for (int chan = 0; chan < MAX_TOTAL_CHANS; chan++) {
        if (pa->chan_medium[chan] != MEDIUM_LORA) continue;

        if (s_lora_count >= MAX_LORA_CHANS) {
            text_color_set(DW_COLOR_ERROR);
            dw_printf ("loraspi: too many LORA channels (max %d)\n", MAX_LORA_CHANS);
            continue;
        }

        lora_chan_t *lc = &s_lora[s_lora_count++];
        memset(lc, 0, sizeof(*lc));
        lc->chan = chan;
        lc->chip = pa->lora_chip[chan];

        /* RF parameters */
        lc->freq_mhz = pa->lora_freq_mhz[chan];
        lc->sf       = pa->lora_sf[chan];
        lc->bw_khz   = pa->lora_bw_khz[chan];
        lc->cr       = pa->lora_cr[chan];
        lc->sw       = pa->lora_sw[chan];
        lc->txpower  = pa->lora_txpower[chan];

        /* Hardware pins */
        lc->pin_cs    = pa->lora_pin_cs[chan];
        lc->pin_reset = pa->lora_pin_reset[chan];
        lc->pin_irq   = pa->lora_pin_irq[chan];
        lc->pin_busy  = pa->lora_pin_busy[chan];
        lc->pin_tx_en = pa->lora_pin_tx_en[chan];
        lc->pin_rx_en = pa->lora_pin_rx_en[chan];
        lc->pa_boost  = pa->lora_pa_boost[chan];
        lc->tcxo      = pa->lora_tcxo[chan];
        lc->tcxo_voltage = pa->lora_tcxo_voltage[chan];

        /* Set up GPIO */
        gpio_setup_out(lc->pin_cs,    1);
        gpio_setup_out(lc->pin_reset, 1);
        gpio_setup_in (lc->pin_irq);
        gpio_setup_in (lc->pin_busy);
        gpio_setup_out(lc->pin_tx_en, 0);
        gpio_setup_out(lc->pin_rx_en, 0);

        /* Open SPI */
        lc->spi_fd = spi_open(pa->lora_spi_bus[chan], pa->lora_spi_dev[chan],
                              pa->lora_spi_speed[chan] > 0 ?
                                  (uint32_t)pa->lora_spi_speed[chan] : 2000000U);
        if (lc->spi_fd < 0) {
            s_lora_count--;
            continue;
        }

        /* Initialise chip */
        if (!chip_init(lc)) {
            close(lc->spi_fd);
            s_lora_count--;
            continue;
        }

        text_color_set(DW_COLOR_INFO);
        dw_printf ("LoRa channel %d: %.3f MHz  SF%d  BW%d kHz  %s\n",
            chan, lc->freq_mhz, lc->sf, lc->bw_khz,
            lc->chip == LORA_CHIP_SX1276 ? "SX1276" : "SX1262");

        /* Start threads */
        txq_init(&lc->txq);
        pthread_mutex_init(&lc->spi_lock, NULL);
        lc->running = true;
        pthread_create(&lc->rx_thread, NULL, rx_thread, lc);
        pthread_create(&lc->tx_thread, NULL, tx_thread, lc);
    }
}

void loraspi_send_packet (int chan, packet_t pp) {
    /* Find the lora_chan_t for this channel */
    lora_chan_t *lc = NULL;
    for (int i = 0; i < s_lora_count; i++) {
        if (s_lora[i].chan == chan) { lc = &s_lora[i]; break; }
    }
    if (!lc) return;

    /* Convert AX.25 packet to TNC2 text */
    char tnc2[300];
    ax25_format_addrs(pp, tnc2);
    unsigned char *pinfo;
    int info_len = ax25_get_info(pp, &pinfo);
    if (info_len > 0 && info_len < (int)(sizeof(tnc2) - strlen(tnc2) - 1)) {
        strncat(tnc2, (char *)pinfo, info_len);
    }

    /* Build wire payload: preamble + TNC2 */
    uint8_t payload[256];
    int tnc2_len = (int)strlen(tnc2);
    int total = LORA_PREAMBLE_LEN + tnc2_len;
    if (total > (int)sizeof(payload)) {
        text_color_set(DW_COLOR_ERROR);
        dw_printf ("loraspi: TX packet too long (%d bytes) — dropped\n", total);
        return;
    }
    memcpy(payload, LORA_PREAMBLE, LORA_PREAMBLE_LEN);
    memcpy(payload + LORA_PREAMBLE_LEN, tnc2, tnc2_len);

    if (!txq_push(&lc->txq, payload, total)) {
        text_color_set(DW_COLOR_ERROR);
        dw_printf ("loraspi: TX queue full — packet dropped\n");
    }
}

/*
 * loraspi_apply_profile  —  copy pin/chip settings from a named hardware
 * profile into pa for the given channel.  Returns 0 on success, -1 if the
 * profile name is unknown.  Called from config.c when the LORAHW directive
 * is parsed.
 */
int loraspi_apply_profile (int chan, const char *name, struct audio_s *pa) {
    const hw_profile_t *p = find_profile(name);
    if (!p) return -1;
    pa->lora_chip[chan]         = p->chip;
    pa->lora_spi_bus[chan]      = p->spi_bus;
    pa->lora_spi_dev[chan]      = p->spi_dev;
    pa->lora_pin_cs[chan]       = p->pin_cs;
    pa->lora_pin_reset[chan]    = p->pin_reset;
    pa->lora_pin_irq[chan]      = p->pin_irq;
    pa->lora_pin_busy[chan]     = p->pin_busy;
    pa->lora_pin_tx_en[chan]    = p->pin_tx_en;
    pa->lora_pin_rx_en[chan]    = p->pin_rx_en;
    pa->lora_pa_boost[chan]     = p->pa_boost ? 1 : 0;
    pa->lora_tcxo[chan]         = p->tcxo     ? 1 : 0;
    pa->lora_tcxo_voltage[chan] = p->tcxo_voltage;
    return 0;
}

#endif /* __linux__ */
