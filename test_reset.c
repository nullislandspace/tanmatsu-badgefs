/*
 * test_reset.c - Minimal ESP32-P4 bootloader + stub upload test
 *
 * Does USBJTAGSerialReset, syncs with bootloader, uploads flasher stub,
 * and reads chip ID register through the stub.
 *
 * Build: gcc -Wall -o test_reset test_reset.c
 * Usage: ./test_reset /dev/ttyACM0
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <time.h>

#include "stub_esp32p4.h"

/* SLIP framing */
#define SLIP_END     0xC0
#define SLIP_ESC     0xDB
#define SLIP_ESC_END 0xDC
#define SLIP_ESC_ESC 0xDD

/* ESP bootloader commands */
#define CMD_MEM_BEGIN         0x05
#define CMD_MEM_END           0x06
#define CMD_MEM_DATA          0x07
#define CMD_SYNC              0x08
#define CMD_WRITE_REG         0x09
#define CMD_READ_REG          0x0A
#define CMD_GET_SECURITY_INFO 0x14

#define ESP_RAM_BLOCK 0x1800

/* ESP32-P4 watchdog registers */
#define LP_WDT_BASE           0x50116000
#define WDT_CONFIG0_REG       (LP_WDT_BASE + 0x0000)
#define WDT_WPROTECT_REG      (LP_WDT_BASE + 0x0018)
#define SWD_CONF_REG          (LP_WDT_BASE + 0x001C)
#define SWD_WPROTECT_REG      (LP_WDT_BASE + 0x0020)
#define WDT_WKEY              0x50D83AA1
#define SWD_AUTO_FEED_EN      (1 << 18)

static long ts_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (ts.tv_sec % 10000) * 1000 + ts.tv_nsec / 1000000;
}

static int serial_open(const char *dev)
{
	int fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (fd < 0)
		return -1;

	struct termios tio;
	tcgetattr(fd, &tio);
	cfmakeraw(&tio);
	tio.c_cflag |= CLOCAL | CREAD;
	tio.c_cflag &= ~(CSIZE | PARENB | CSTOPB | CRTSCTS);
	tio.c_cflag |= CS8;
	tio.c_iflag &= ~(IXON | IXOFF | IXANY);
	tio.c_cc[VMIN] = 0;
	tio.c_cc[VTIME] = 0;
	cfsetispeed(&tio, B115200);
	cfsetospeed(&tio, B115200);
	tcsetattr(fd, TCSANOW, &tio);
	tcflush(fd, TCIOFLUSH);
	return fd;
}

static void set_dtr(int fd, int state)
{
	int bits = TIOCM_DTR;
	ioctl(fd, state ? TIOCMBIS : TIOCMBIC, &bits);
}

static void set_rts(int fd, int state)
{
	int bits = TIOCM_RTS;
	ioctl(fd, state ? TIOCMBIS : TIOCMBIC, &bits);
}

static void usb_jtag_reset(int fd)
{
	printf("[%ld] Reset: idle\n", ts_ms());
	set_rts(fd, 0);
	set_dtr(fd, 0);
	usleep(100000);

	printf("[%ld] Reset: set IO0\n", ts_ms());
	set_dtr(fd, 1);
	set_rts(fd, 0);
	usleep(100000);

	printf("[%ld] Reset: assert reset\n", ts_ms());
	set_rts(fd, 1);
	set_dtr(fd, 0);
	set_rts(fd, 1);
	usleep(100000);

	printf("[%ld] Reset: release\n", ts_ms());
	set_dtr(fd, 0);
	set_rts(fd, 0);
	printf("[%ld] Reset: done\n", ts_ms());
}

/* Read with timeout. Returns bytes read */
static int serial_read(int fd, uint8_t *buf, int maxlen, int timeout_ms)
{
	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	int total = 0;

	while (total < maxlen) {
		int ret = poll(&pfd, 1, total == 0 ? timeout_ms : 50);
		if (ret <= 0)
			break;
		if (pfd.revents & POLLERR)
			return -1;
		int n = read(fd, buf + total, maxlen - total);
		if (n <= 0)
			break;
		total += n;
	}
	return total;
}

/* Send a SLIP frame */
static int slip_send(int fd, const uint8_t *data, int len)
{
	uint8_t frame[8192];
	int pos = 0;

	frame[pos++] = SLIP_END;
	for (int i = 0; i < len; i++) {
		if (data[i] == SLIP_END) {
			frame[pos++] = SLIP_ESC;
			frame[pos++] = SLIP_ESC_END;
		} else if (data[i] == SLIP_ESC) {
			frame[pos++] = SLIP_ESC;
			frame[pos++] = SLIP_ESC_ESC;
		} else {
			frame[pos++] = data[i];
		}
	}
	frame[pos++] = SLIP_END;

	int sent = 0;
	while (sent < pos) {
		int n = write(fd, frame + sent, pos - sent);
		if (n < 0) {
			if (errno == EAGAIN) {
				usleep(1000);
				continue;
			}
			return -1;
		}
		sent += n;
	}
	return sent;
}

/* Read a SLIP frame. Returns payload length, -1 on timeout */
static int slip_recv(int fd, uint8_t *buf, int maxlen, int timeout_ms)
{
	uint8_t raw[8192];
	int n = serial_read(fd, raw, sizeof(raw), timeout_ms);
	if (n <= 0)
		return -1;

	int start = -1;
	for (int i = 0; i < n; i++) {
		if (raw[i] == SLIP_END) {
			if (start < 0) {
				start = i + 1;
			} else if (i > start) {
				int out = 0;
				for (int j = start; j < i && out < maxlen; j++) {
					if (raw[j] == SLIP_ESC && j + 1 < i) {
						j++;
						if (raw[j] == SLIP_ESC_END)
							buf[out++] = SLIP_END;
						else if (raw[j] == SLIP_ESC_ESC)
							buf[out++] = SLIP_ESC;
					} else {
						buf[out++] = raw[j];
					}
				}
				return out;
			}
		}
	}

	printf("  [no SLIP frame in %d bytes: ", n);
	for (int i = 0; i < n && i < 40; i++)
		printf("%02x ", raw[i]);
	if (n > 40)
		printf("...");
	printf("]\n");
	return -1;
}

static uint8_t checksum(const uint8_t *data, int len)
{
	uint8_t ck = 0xEF;
	for (int i = 0; i < len; i++)
		ck ^= data[i];
	return ck;
}

/*
 * Send ESP command, get response. Returns response length, -1 on fail.
 * The chk parameter is the checksum — for most commands it's 0,
 * for MEM_DATA it's the checksum of the data block only.
 */
static int esp_command_chk(int fd, uint8_t cmd, const uint8_t *payload,
                           int plen, uint32_t chk, uint8_t *resp,
                           int resp_max, int timeout_ms)
{
	uint8_t pkt[8192];

	pkt[0] = 0x00; /* request */
	pkt[1] = cmd;
	pkt[2] = plen & 0xFF;
	pkt[3] = (plen >> 8) & 0xFF;
	pkt[4] = chk & 0xFF;
	pkt[5] = (chk >> 8) & 0xFF;
	pkt[6] = (chk >> 16) & 0xFF;
	pkt[7] = (chk >> 24) & 0xFF;

	if (payload && plen > 0)
		memcpy(pkt + 8, payload, plen);

	slip_send(fd, pkt, 8 + plen);
	return slip_recv(fd, resp, resp_max, timeout_ms);
}

static int esp_command(int fd, uint8_t cmd, const uint8_t *payload,
                       int plen, uint8_t *resp, int resp_max,
                       int timeout_ms)
{
	return esp_command_chk(fd, cmd, payload, plen, 0,
	                       resp, resp_max, timeout_ms);
}

static void hexdump(const char *label, const uint8_t *data, int len)
{
	printf("  %s (%d bytes):", label, len);
	for (int i = 0; i < len && i < 64; i++)
		printf(" %02x", data[i]);
	if (len > 64)
		printf(" ...");
	printf("\n");
}

static int write_reg(int fd, uint32_t addr, uint32_t val)
{
	/* WRITE_REG payload: addr(4) + value(4) + mask(4) + delay_us(4) */
	uint8_t pkt[16], resp[64];
	pkt[0]  = addr & 0xFF;
	pkt[1]  = (addr >> 8) & 0xFF;
	pkt[2]  = (addr >> 16) & 0xFF;
	pkt[3]  = (addr >> 24) & 0xFF;
	pkt[4]  = val & 0xFF;
	pkt[5]  = (val >> 8) & 0xFF;
	pkt[6]  = (val >> 16) & 0xFF;
	pkt[7]  = (val >> 24) & 0xFF;
	pkt[8]  = 0xFF; pkt[9] = 0xFF;
	pkt[10] = 0xFF; pkt[11] = 0xFF; /* mask = 0xFFFFFFFF */
	pkt[12] = 0; pkt[13] = 0;
	pkt[14] = 0; pkt[15] = 0; /* delay = 0 */

	return esp_command(fd, CMD_WRITE_REG, pkt, 16, resp, sizeof(resp), 1000);
}

static uint32_t read_reg(int fd, uint32_t addr)
{
	uint8_t pkt[4], resp[64];
	pkt[0] = addr & 0xFF;
	pkt[1] = (addr >> 8) & 0xFF;
	pkt[2] = (addr >> 16) & 0xFF;
	pkt[3] = (addr >> 24) & 0xFF;

	int n = esp_command(fd, CMD_READ_REG, pkt, 4, resp, sizeof(resp), 1000);
	if (n >= 8)
		return resp[4] | (resp[5] << 8) | (resp[6] << 16) | (resp[7] << 24);
	return 0;
}

static void disable_watchdogs(int fd)
{
	printf("[%ld] Disabling watchdogs...\n", ts_ms());

	/* Disable RTC WDT */
	write_reg(fd, WDT_WPROTECT_REG, WDT_WKEY);
	write_reg(fd, WDT_CONFIG0_REG, 0);
	write_reg(fd, WDT_WPROTECT_REG, 0);

	/* Auto-feed SWD */
	write_reg(fd, SWD_WPROTECT_REG, WDT_WKEY);
	uint32_t swd_conf = read_reg(fd, SWD_CONF_REG);
	write_reg(fd, SWD_CONF_REG, swd_conf | SWD_AUTO_FEED_EN);
	write_reg(fd, SWD_WPROTECT_REG, 0);

	printf("[%ld] Watchdogs disabled\n", ts_ms());
}

/* Upload a memory segment: MEM_BEGIN + MEM_DATA blocks */
static int mem_upload(int fd, uint32_t addr, const uint8_t *data, int len)
{
	int blocks = (len + ESP_RAM_BLOCK - 1) / ESP_RAM_BLOCK;
	uint8_t resp[256];

	/* MEM_BEGIN: size, blocks, blocksize, offset */
	uint8_t begin_pkt[16];
	begin_pkt[0]  = len & 0xFF;
	begin_pkt[1]  = (len >> 8) & 0xFF;
	begin_pkt[2]  = (len >> 16) & 0xFF;
	begin_pkt[3]  = (len >> 24) & 0xFF;
	begin_pkt[4]  = blocks & 0xFF;
	begin_pkt[5]  = (blocks >> 8) & 0xFF;
	begin_pkt[6]  = (blocks >> 16) & 0xFF;
	begin_pkt[7]  = (blocks >> 24) & 0xFF;
	begin_pkt[8]  = ESP_RAM_BLOCK & 0xFF;
	begin_pkt[9]  = (ESP_RAM_BLOCK >> 8) & 0xFF;
	begin_pkt[10] = (ESP_RAM_BLOCK >> 16) & 0xFF;
	begin_pkt[11] = (ESP_RAM_BLOCK >> 24) & 0xFF;
	begin_pkt[12] = addr & 0xFF;
	begin_pkt[13] = (addr >> 8) & 0xFF;
	begin_pkt[14] = (addr >> 16) & 0xFF;
	begin_pkt[15] = (addr >> 24) & 0xFF;

	int n = esp_command(fd, CMD_MEM_BEGIN, begin_pkt, 16,
	                    resp, sizeof(resp), 3000);
	if (n < 0) {
		printf("  MEM_BEGIN failed\n");
		return -1;
	}

	/* MEM_DATA: for each block */
	for (int seq = 0; seq < blocks; seq++) {
		int offset = seq * ESP_RAM_BLOCK;
		int chunk = len - offset;
		if (chunk > ESP_RAM_BLOCK)
			chunk = ESP_RAM_BLOCK;

		/* Header: data_len(4) + seq(4) + 0(4) + 0(4) + data */
		uint8_t data_pkt[16 + ESP_RAM_BLOCK];
		data_pkt[0]  = chunk & 0xFF;
		data_pkt[1]  = (chunk >> 8) & 0xFF;
		data_pkt[2]  = (chunk >> 16) & 0xFF;
		data_pkt[3]  = (chunk >> 24) & 0xFF;
		data_pkt[4]  = seq & 0xFF;
		data_pkt[5]  = (seq >> 8) & 0xFF;
		data_pkt[6]  = (seq >> 16) & 0xFF;
		data_pkt[7]  = (seq >> 24) & 0xFF;
		memset(data_pkt + 8, 0, 8);
		memcpy(data_pkt + 16, data + offset, chunk);

		/* Checksum is over the DATA only, not the 16-byte header */
		uint32_t ck = checksum(data + offset, chunk);
		n = esp_command_chk(fd, CMD_MEM_DATA, data_pkt, 16 + chunk,
		                    ck, resp, sizeof(resp), 3000);
		if (n < 0) {
			printf("  MEM_DATA block %d/%d failed\n",
			       seq + 1, blocks);
			return -1;
		}
	}

	return 0;
}

/* MEM_END: finish upload, optionally run at entrypoint */
static int mem_finish(int fd, uint32_t entry)
{
	uint8_t pkt[8];

	/* no_entry(4) + entry(4) */
	int no_entry = (entry == 0) ? 1 : 0;
	pkt[0] = no_entry & 0xFF;
	pkt[1] = 0;
	pkt[2] = 0;
	pkt[3] = 0;
	pkt[4] = entry & 0xFF;
	pkt[5] = (entry >> 8) & 0xFF;
	pkt[6] = (entry >> 16) & 0xFF;
	pkt[7] = (entry >> 24) & 0xFF;

	/*
	 * Send MEM_END command, then read RAW bytes to catch both
	 * the ROM bootloader response and the stub's "OHAI".
	 */
	uint8_t full[16];
	full[0] = 0x00; /* direction: request */
	full[1] = CMD_MEM_END;
	full[2] = 8; full[3] = 0; /* size = 8 */
	full[4] = 0; full[5] = 0; full[6] = 0; full[7] = 0; /* checksum */
	memcpy(full + 8, pkt, 8); /* payload */
	slip_send(fd, full, 16);

	/* Read all raw data that comes back */
	uint8_t raw[512];
	int total = 0;
	for (int i = 0; i < 30; i++) { /* poll up to 3 seconds */
		int n = serial_read(fd, raw + total,
		                    (int)sizeof(raw) - total, 100);
		if (n > 0) {
			total += n;
			/* Check if we got OHAI */
			for (int j = 0; j + 3 < total; j++) {
				if (raw[j] == 'O' && raw[j+1] == 'H' &&
				    raw[j+2] == 'A' && raw[j+3] == 'I') {
					printf("  raw (%d bytes):", total);
					for (int k = 0; k < total && k < 40; k++)
						printf(" %02x", raw[k]);
					if (total > 40) printf(" ...");
					printf("\n");
					printf("  -> OHAI at offset %d\n", j);
					return 0;
				}
			}
		}
	}

	printf("  raw (%d bytes):", total);
	for (int i = 0; i < total && i < 60; i++)
		printf(" %02x", raw[i]);
	if (total > 60) printf(" ...");
	printf("\n  -> OHAI not found\n");
	return 0;
}

int main(int argc, char *argv[])
{
	if (argc < 2) {
		fprintf(stderr, "Usage: %s /dev/ttyACM0\n", argv[0]);
		return 1;
	}
	const char *dev = argv[1];
	uint8_t resp[256];
	int n;

	/* Step 1: Open */
	printf("[%ld] Opening %s\n", ts_ms(), dev);
	int fd = serial_open(dev);
	if (fd < 0) { perror("open"); return 1; }

	/* Step 2: Reset */
	printf("[%ld] Performing USBJTAGSerialReset\n", ts_ms());
	usb_jtag_reset(fd);

	/* Step 3: Read boot log */
	printf("[%ld] Reading boot log...\n", ts_ms());
	uint8_t bootlog[1024];
	n = serial_read(fd, bootlog, sizeof(bootlog), 500);
	if (n > 0) {
		printf("[%ld] Boot log (%d bytes):\n", ts_ms(), n);
		fwrite(bootlog, 1, n, stdout);
		printf("\n");
	} else {
		printf("[%ld] No boot log (trying reopen)...\n", ts_ms());
		close(fd);
		for (int i = 0; i < 2000; i++) {
			usleep(5000);
			fd = serial_open(dev);
			if (fd >= 0) {
				printf("[%ld] Reopened after %dms\n",
				       ts_ms(), (i + 1) * 5);
				n = serial_read(fd, bootlog,
				                sizeof(bootlog), 2000);
				if (n > 0) {
					printf("[%ld] Boot log (%d bytes):\n",
					       ts_ms(), n);
					fwrite(bootlog, 1, n, stdout);
					printf("\n");
				}
				break;
			}
		}
		if (fd < 0) {
			fprintf(stderr, "Device did not reappear\n");
			return 1;
		}
	}

	/* Step 4: SYNC */
	printf("[%ld] Sending SYNC...\n", ts_ms());
	uint8_t sync_payload[36];
	sync_payload[0] = 0x07;
	sync_payload[1] = 0x07;
	sync_payload[2] = 0x12;
	sync_payload[3] = 0x20;
	memset(sync_payload + 4, 0x55, 32);

	for (int attempt = 0; attempt < 10; attempt++) {
		n = esp_command(fd, CMD_SYNC, sync_payload, 36,
		                resp, sizeof(resp), 500);
		if (n > 0) {
			printf("[%ld] SYNC OK (attempt %d)\n",
			       ts_ms(), attempt + 1);
			hexdump("resp", resp, n);
			break;
		}
		printf("[%ld] SYNC attempt %d failed\n",
		       ts_ms(), attempt + 1);
		usleep(50000);
	}
	if (n <= 0) {
		fprintf(stderr, "Could not sync\n");
		close(fd);
		return 1;
	}

	/* Drain extra sync responses */
	for (int i = 0; i < 10; i++) {
		int extra = slip_recv(fd, resp, sizeof(resp), 100);
		if (extra <= 0) break;
	}

	/* Step 5: GET_SECURITY_INFO */
	printf("[%ld] GET_SECURITY_INFO...\n", ts_ms());
	n = esp_command(fd, CMD_GET_SECURITY_INFO, NULL, 0,
	                resp, sizeof(resp), 1000);
	if (n > 0) {
		hexdump("resp", resp, n);
	} else {
		printf("  failed\n");
	}

	/* Step 6: Disable watchdogs (required for USB JTAG/Serial) */
	disable_watchdogs(fd);

	/* Step 7: Upload stub */
	printf("[%ld] Uploading stub text (%d bytes -> 0x%08x)...\n",
	       ts_ms(), STUB_TEXT_LEN, STUB_TEXT_START);
	if (mem_upload(fd, STUB_TEXT_START, stub_text, STUB_TEXT_LEN) < 0) {
		fprintf(stderr, "Stub text upload failed\n");
		close(fd);
		return 1;
	}

	printf("[%ld] Uploading stub data (%d bytes -> 0x%08x)...\n",
	       ts_ms(), STUB_DATA_LEN, STUB_DATA_START);
	if (mem_upload(fd, STUB_DATA_START, stub_data, STUB_DATA_LEN) < 0) {
		fprintf(stderr, "Stub data upload failed\n");
		close(fd);
		return 1;
	}

	/* Step 7: Run stub */
	printf("[%ld] Running stub (entry 0x%08x)...\n",
	       ts_ms(), STUB_ENTRY);
	mem_finish(fd, STUB_ENTRY);

	/* Stub sends "OHAI" as a SLIP frame on startup */
	/* OHAI is already detected by mem_finish above */

	/* Step 9: READ_REG through stub */
	printf("[%ld] Reading chip magic (0x40001000)...\n", ts_ms());
	uint32_t magic = read_reg(fd, 0x40001000);
	printf("  Chip magic: 0x%08x\n", magic);

	close(fd);
	printf("[%ld] Done\n", ts_ms());
	return 0;
}
