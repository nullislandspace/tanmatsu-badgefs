/*
 * rfc2217proxy.c - RFC2217 (Telnet COM Port) to serial port proxy
 *
 * Bridges an RFC2217 TCP connection to a local serial port, allowing
 * remote tools like esptool.py to flash and monitor ESP32 devices
 * over the network.
 *
 * Implements the subset of RFC2217 needed by esptool/pyserial:
 *   - Telnet option negotiation (BINARY, SGA, COM-PORT-OPTION)
 *   - Baud rate, data size, parity, stop bits configuration
 *   - DTR/RTS control (for bootloader entry)
 *   - PURGE acknowledgment
 *   - Bidirectional data forwarding with IAC escaping
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

/* ---- Telnet protocol constants ---- */

#define IAC   255  /* Interpret As Command */
#define DONT  254
#define DO    253
#define WONT  252
#define WILL  251
#define SB    250  /* Subnegotiation Begin */
#define SE    240  /* Subnegotiation End */

/* Telnet options */
#define OPT_BINARY  0   /* Binary Transmission */
#define OPT_ECHO    1   /* Echo */
#define OPT_SGA     3   /* Suppress Go Ahead */
#define OPT_COM_PORT 44 /* COM Port Control (RFC 2217) */

/* ---- RFC 2217 subnegotiation opcodes ---- */

/* Client-to-server (requests) */
#define RFC2217_SET_BAUDRATE        1
#define RFC2217_SET_DATASIZE        2
#define RFC2217_SET_PARITY          3
#define RFC2217_SET_STOPSIZE        4
#define RFC2217_SET_CONTROL         5
#define RFC2217_NOTIFY_LINESTATE    6
#define RFC2217_NOTIFY_MODEMSTATE   7
#define RFC2217_FLOWCONTROL_SUSPEND 8
#define RFC2217_FLOWCONTROL_RESUME  9
#define RFC2217_SET_LINESTATE_MASK  10
#define RFC2217_SET_MODEMSTATE_MASK 11
#define RFC2217_PURGE_DATA          12

/* Server-to-client (responses): request opcode + 100 */
#define RFC2217_RESP_OFFSET         100

/* SET_CONTROL values */
#define CONTROL_REQ_FLOW       0
#define CONTROL_USE_NO_FLOW    1
#define CONTROL_USE_XONXOFF    2
#define CONTROL_USE_HARDWARE   3
#define CONTROL_REQ_BREAK      4
#define CONTROL_BREAK_ON       5
#define CONTROL_BREAK_OFF      6
#define CONTROL_REQ_DTR        7
#define CONTROL_DTR_ON         8
#define CONTROL_DTR_OFF        9
#define CONTROL_REQ_RTS        10
#define CONTROL_RTS_ON         11
#define CONTROL_RTS_OFF        12

/* PURGE_DATA values */
#define PURGE_RX  1
#define PURGE_TX  2
#define PURGE_BOTH 3

/* ---- Proxy settings ---- */

#define DEFAULT_PORT    4000
#define DEFAULT_BAUD    115200
#define SERIAL_BUF_SIZE 4096
#define TCP_BUF_SIZE    4096
#define LOG_DIR         "/tmp/rfc2217_logs"

/* ---- Data logging ---- */

#include <time.h>
#include <sys/stat.h>

static FILE *log_tcp_in;    /* raw TCP bytes received from client */
static FILE *log_tcp_out;   /* raw TCP bytes sent to client */
static FILE *log_serial_in; /* raw serial bytes read from device */
static FILE *log_serial_out;/* raw serial bytes written to device */
static FILE *log_events;    /* timestamped event log */

static long log_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (ts.tv_sec % 10000) * 1000 + ts.tv_nsec / 1000000;
}

static void log_event(const char *fmt, ...)
{
	if (!log_events)
		return;
	fprintf(log_events, "[%ld] ", log_ms());
	va_list ap;
	va_start(ap, fmt);
	vfprintf(log_events, fmt, ap);
	va_end(ap);
	fprintf(log_events, "\n");
	fflush(log_events);
}

static void log_data(FILE *f, const uint8_t *data, size_t len)
{
	if (!f || len == 0)
		return;
	fwrite(data, 1, len, f);
	fflush(f);
}

static void log_open(void)
{
	mkdir(LOG_DIR, 0755);
	log_tcp_in     = fopen(LOG_DIR "/tcp_in.bin", "wb");
	log_tcp_out    = fopen(LOG_DIR "/tcp_out.bin", "wb");
	log_serial_in  = fopen(LOG_DIR "/serial_in.bin", "wb");
	log_serial_out = fopen(LOG_DIR "/serial_out.bin", "wb");
	log_events     = fopen(LOG_DIR "/events.log", "w");
	log_event("logging started");
}

static void log_close(void)
{
	log_event("logging stopped");
	if (log_tcp_in)     fclose(log_tcp_in);
	if (log_tcp_out)    fclose(log_tcp_out);
	if (log_serial_in)  fclose(log_serial_in);
	if (log_serial_out) fclose(log_serial_out);
	if (log_events)     fclose(log_events);
}
#define SB_BUF_SIZE     64

static volatile sig_atomic_t running = 1;

static void signal_handler(int sig)
{
	(void)sig;
	running = 0;
}

/* ---- Telnet send helpers ---- */

static int tcp_send(int fd, const uint8_t *data, size_t len)
{
	size_t sent = 0;
	while (sent < len) {
		ssize_t n = send(fd, data + sent, len - sent, MSG_NOSIGNAL);
		if (n <= 0)
			return -1;
		sent += n;
	}
	return 0;
}

static int telnet_send_option(int fd, uint8_t action, uint8_t option)
{
	uint8_t buf[3] = { IAC, action, option };
	return tcp_send(fd, buf, 3);
}

static int telnet_send_subneg(int fd, uint8_t option, const uint8_t *data,
                              size_t len)
{
	/* IAC SB <option> <data...> IAC SE */
	uint8_t hdr[3] = { IAC, SB, option };
	uint8_t trl[2] = { IAC, SE };

	if (tcp_send(fd, hdr, 3) < 0)
		return -1;
	if (len > 0 && tcp_send(fd, data, len) < 0)
		return -1;
	return tcp_send(fd, trl, 2);
}

/* Send RFC2217 subnegotiation response */
static int rfc2217_respond(int fd, uint8_t opcode, const uint8_t *data,
                           size_t len)
{
	uint8_t buf[SB_BUF_SIZE];

	if (1 + len > sizeof(buf))
		return -1;

	buf[0] = opcode + RFC2217_RESP_OFFSET;
	if (len > 0)
		memcpy(buf + 1, data, len);

	return telnet_send_subneg(fd, OPT_COM_PORT, buf, 1 + len);
}

/* ---- Serial port helpers ---- */

static speed_t baud_to_speed(unsigned int baud)
{
	switch (baud) {
	case 0:       return B0;
	case 50:      return B50;
	case 75:      return B75;
	case 110:     return B110;
	case 134:     return B134;
	case 150:     return B150;
	case 200:     return B200;
	case 300:     return B300;
	case 600:     return B600;
	case 1200:    return B1200;
	case 1800:    return B1800;
	case 2400:    return B2400;
	case 4800:    return B4800;
	case 9600:    return B9600;
	case 19200:   return B19200;
	case 38400:   return B38400;
	case 57600:   return B57600;
	case 115200:  return B115200;
	case 230400:  return B230400;
	case 460800:  return B460800;
	case 500000:  return B500000;
	case 576000:  return B576000;
	case 921600:  return B921600;
	case 1000000: return B1000000;
	case 1152000: return B1152000;
	case 1500000: return B1500000;
	case 2000000: return B2000000;
	case 2500000: return B2500000;
	case 3000000: return B3000000;
	case 3500000: return B3500000;
	case 4000000: return B4000000;
	default:      return B115200;
	}
}

static unsigned int speed_to_baud(speed_t speed)
{
	switch (speed) {
	case B0:       return 0;
	case B50:      return 50;
	case B75:      return 75;
	case B110:     return 110;
	case B134:     return 134;
	case B150:     return 150;
	case B200:     return 200;
	case B300:     return 300;
	case B600:     return 600;
	case B1200:    return 1200;
	case B1800:    return 1800;
	case B2400:    return 2400;
	case B4800:    return 4800;
	case B9600:    return 9600;
	case B19200:   return 19200;
	case B38400:   return 38400;
	case B57600:   return 57600;
	case B115200:  return 115200;
	case B230400:  return 230400;
	case B460800:  return 460800;
	case B500000:  return 500000;
	case B576000:  return 576000;
	case B921600:  return 921600;
	case B1000000: return 1000000;
	case B1152000: return 1152000;
	case B1500000: return 1500000;
	case B2000000: return 2000000;
	case B2500000: return 2500000;
	case B3000000: return 3000000;
	case B3500000: return 3500000;
	case B4000000: return 4000000;
	default:       return 0;
	}
}

static int serial_open(const char *device, unsigned int baud)
{
	int fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (fd < 0) {
		fprintf(stderr, "rfc2217proxy: open %s: %s\n", device,
		        strerror(errno));
		return -1;
	}

	struct termios tio;
	if (tcgetattr(fd, &tio) < 0) {
		fprintf(stderr, "rfc2217proxy: tcgetattr: %s\n",
		        strerror(errno));
		close(fd);
		return -1;
	}

	/* Raw mode */
	cfmakeraw(&tio);

	/* No modem control */
	tio.c_cflag |= CLOCAL | CREAD;

	/* Default: 8N1 */
	tio.c_cflag &= ~CSIZE;
	tio.c_cflag |= CS8;
	tio.c_cflag &= ~PARENB;
	tio.c_cflag &= ~CSTOPB;

	/* No flow control */
	tio.c_cflag &= ~CRTSCTS;
	tio.c_iflag &= ~(IXON | IXOFF | IXANY);

	/* Non-blocking reads */
	tio.c_cc[VMIN] = 0;
	tio.c_cc[VTIME] = 0;

	speed_t speed = baud_to_speed(baud);
	cfsetispeed(&tio, speed);
	cfsetospeed(&tio, speed);

	if (tcsetattr(fd, TCSANOW, &tio) < 0) {
		fprintf(stderr, "rfc2217proxy: tcsetattr: %s\n",
		        strerror(errno));
		close(fd);
		return -1;
	}

	/* Flush any stale data */
	tcflush(fd, TCIOFLUSH);

	return fd;
}

static int serial_set_baudrate(int fd, unsigned int baud)
{
	struct termios tio;
	if (tcgetattr(fd, &tio) < 0)
		return -1;

	speed_t speed = baud_to_speed(baud);
	cfsetispeed(&tio, speed);
	cfsetospeed(&tio, speed);

	return tcsetattr(fd, TCSANOW, &tio);
}

static unsigned int serial_get_baudrate(int fd)
{
	struct termios tio;
	if (tcgetattr(fd, &tio) < 0)
		return 0;
	return speed_to_baud(cfgetospeed(&tio));
}

static int serial_set_datasize(int fd, uint8_t size)
{
	struct termios tio;
	if (tcgetattr(fd, &tio) < 0)
		return -1;

	tio.c_cflag &= ~CSIZE;
	switch (size) {
	case 5: tio.c_cflag |= CS5; break;
	case 6: tio.c_cflag |= CS6; break;
	case 7: tio.c_cflag |= CS7; break;
	case 8: tio.c_cflag |= CS8; break;
	default: return -1;
	}

	return tcsetattr(fd, TCSANOW, &tio);
}

static uint8_t serial_get_datasize(int fd)
{
	struct termios tio;
	if (tcgetattr(fd, &tio) < 0)
		return 0;

	switch (tio.c_cflag & CSIZE) {
	case CS5: return 5;
	case CS6: return 6;
	case CS7: return 7;
	case CS8: return 8;
	default:  return 0;
	}
}

static int serial_set_parity(int fd, uint8_t parity)
{
	struct termios tio;
	if (tcgetattr(fd, &tio) < 0)
		return -1;

	switch (parity) {
	case 1: /* None */
		tio.c_cflag &= ~PARENB;
		break;
	case 2: /* Odd */
		tio.c_cflag |= PARENB | PARODD;
		break;
	case 3: /* Even */
		tio.c_cflag |= PARENB;
		tio.c_cflag &= ~PARODD;
		break;
	case 4: /* Mark */
		tio.c_cflag |= PARENB | PARODD | CMSPAR;
		break;
	case 5: /* Space */
		tio.c_cflag |= PARENB | CMSPAR;
		tio.c_cflag &= ~PARODD;
		break;
	default:
		return -1;
	}

	return tcsetattr(fd, TCSANOW, &tio);
}

static uint8_t serial_get_parity(int fd)
{
	struct termios tio;
	if (tcgetattr(fd, &tio) < 0)
		return 0;

	if (!(tio.c_cflag & PARENB))
		return 1; /* None */
	if (tio.c_cflag & CMSPAR)
		return (tio.c_cflag & PARODD) ? 4 : 5; /* Mark / Space */
	return (tio.c_cflag & PARODD) ? 2 : 3; /* Odd / Even */
}

static int serial_set_stopsize(int fd, uint8_t stop)
{
	struct termios tio;
	if (tcgetattr(fd, &tio) < 0)
		return -1;

	switch (stop) {
	case 1: tio.c_cflag &= ~CSTOPB; break;
	case 2: tio.c_cflag |= CSTOPB; break;
	case 3: tio.c_cflag &= ~CSTOPB; break; /* 1.5 -> treat as 1 */
	default: return -1;
	}

	return tcsetattr(fd, TCSANOW, &tio);
}

static uint8_t serial_get_stopsize(int fd)
{
	struct termios tio;
	if (tcgetattr(fd, &tio) < 0)
		return 0;

	return (tio.c_cflag & CSTOPB) ? 2 : 1;
}

static int serial_set_dtr(int fd, int state)
{
	int bits = TIOCM_DTR;
	return ioctl(fd, state ? TIOCMBIS : TIOCMBIC, &bits);
}

static int serial_set_rts(int fd, int state)
{
	int bits = TIOCM_RTS;
	return ioctl(fd, state ? TIOCMBIS : TIOCMBIC, &bits);
}

static int serial_set_break(int fd, int state)
{
	return ioctl(fd, state ? TIOCSBRK : TIOCCBRK, 0);
}

/*
 * Execute the USB JTAG/Serial bootloader entry reset sequence.
 * This matches esptool's USBJTAGSerialReset strategy exactly.
 *
 * When esptool connects via rfc2217://, it can't detect the USB PID
 * and falls back to ClassicReset, which doesn't work for USB JTAG/Serial
 * devices. We intercept the reset and run the correct sequence locally.
 */
static void esp_reset_to_bootloader(int fd)
{
	fprintf(stderr, "rfc2217proxy: USBJTAGSerialReset sequence\n");
	log_event("RESET: USBJTAGSerialReset begin");

	/* Idle */
	serial_set_rts(fd, 0);
	serial_set_dtr(fd, 0);
	usleep(100000);  /* 100ms */

	/* Set IO0 */
	serial_set_dtr(fd, 1);
	serial_set_rts(fd, 0);
	usleep(100000);  /* 100ms */

	/*
	 * Reset. Calls inverted to go through (1,1) instead of (0,0).
	 * RTS set as Windows only propagates DTR on RTS setting.
	 */
	serial_set_rts(fd, 1);
	serial_set_dtr(fd, 0);
	serial_set_rts(fd, 1);
	usleep(100000);  /* 100ms */

	/* Chip out of reset */
	serial_set_dtr(fd, 0);
	serial_set_rts(fd, 0);
	log_event("RESET: USBJTAGSerialReset done");
}

static void esp_hard_reset(int fd)
{
	fprintf(stderr, "rfc2217proxy: hard reset (USB)\n");

	serial_set_rts(fd, 1);
	usleep(200000);  /* 200ms for USB */
	serial_set_rts(fd, 0);
	usleep(200000);  /* 200ms settle */
}

/*
 * Track download mode state. When we see the first DTR ON, we run
 * the full bootloader entry sequence locally and absorb subsequent
 * DTR/RTS changes until the client releases DTR.
 */
static int in_download_mode = 0;

enum telnet_state {
	TS_DATA,
	TS_IAC,
	TS_WILL,
	TS_WONT,
	TS_DO,
	TS_DONT,
	TS_SB,
	TS_SB_DATA,
	TS_SB_IAC,
};

struct proxy_state {
	int tcp_fd;
	int serial_fd;
	enum telnet_state tstate;
	uint8_t sb_option;
	uint8_t sb_buf[SB_BUF_SIZE];
	size_t sb_len;
	const char *device;
	unsigned int baud;
};

/*
 * Reopen the serial port after a device reset.
 *
 * The USB JTAG/serial debug unit disconnects when the ESP32-P4 resets
 * and re-enumerates after a short time. We poll rapidly (every 5ms)
 * to catch the device reappearing as early as possible, then
 * immediately return so buffered TCP data (esptool sync packets)
 * can be forwarded.
 *
 * Returns 0 on success, -1 if the device didn't reappear within timeout.
 */
static int serial_reopen(struct proxy_state *ps)
{
	if (ps->serial_fd >= 0)
		close(ps->serial_fd);
	ps->serial_fd = -1;

	/*
	 * The USB JTAG debug unit disconnects during reset but the
	 * device node may linger in /dev. We can't reliably detect
	 * when the device is truly gone vs still present.
	 *
	 * Strategy: close the stale fd, wait for the USB to fully
	 * cycle (disconnect + re-enumerate), then reopen.
	 * We try opening every 5ms; a successful open() that also
	 * passes tcgetattr() means the device is truly back.
	 */
	fprintf(stderr, "rfc2217proxy: reopening device...\n");
	log_event("REOPEN: closing fd, waiting 100ms");

	/*
	 * Wait for the USB to fully cycle. The device node may linger
	 * briefly after reset (stale), so we must wait for it to
	 * actually disappear and re-enumerate. Sleep 500ms minimum
	 * to avoid reopening a stale node.
	 */
	/* No initial delay — the device node comes back quickly
	 * even if the USB briefly disconnects. The kernel CDC ACM
	 * driver handles re-enumeration transparently. */

	for (int i = 0; i < 2000; i++) { /* then poll up to 10 seconds */
		usleep(5000); /* 5ms */

		int fd = serial_open(ps->device, ps->baud);
		if (fd >= 0) {
			ps->serial_fd = fd;
			log_event("REOPEN: device reopened after %dms",
			          (i + 1) * 5);
			ps->serial_fd = fd;
			fprintf(stderr,
			        "rfc2217proxy: device reopened after %dms\n",
			        (i + 1) * 5);
			return 0;
		}
	}

	fprintf(stderr, "rfc2217proxy: device did not reappear\n");
	return -1;
}

static int send_serial_to_tcp(int tcp_fd, const uint8_t *data, size_t len);

/* ---- RFC 2217 subnegotiation handler ---- */

static void handle_com_port_subneg(struct proxy_state *ps,
                                   const uint8_t *data, size_t len)
{
	int tcp_fd = ps->tcp_fd;
	int serial_fd = ps->serial_fd;

	if (len < 1)
		return;

	uint8_t opcode = data[0];
	const uint8_t *payload = data + 1;
	size_t plen = len - 1;

	switch (opcode) {

	case RFC2217_SET_BAUDRATE: {
		if (plen < 4)
			break;
		uint32_t baud = ((uint32_t)payload[0] << 24) |
		                ((uint32_t)payload[1] << 16) |
		                ((uint32_t)payload[2] << 8) |
		                 (uint32_t)payload[3];

		if (baud == 0) {
			/* Request current baud rate */
			baud = serial_get_baudrate(serial_fd);
		} else {
			serial_set_baudrate(serial_fd, baud);
			baud = serial_get_baudrate(serial_fd);
		}

		uint8_t resp[4] = {
			(baud >> 24) & 0xFF,
			(baud >> 16) & 0xFF,
			(baud >> 8) & 0xFF,
			baud & 0xFF,
		};
		rfc2217_respond(tcp_fd, RFC2217_SET_BAUDRATE, resp, 4);
		break;
	}

	case RFC2217_SET_DATASIZE: {
		if (plen < 1)
			break;
		uint8_t val = payload[0];

		if (val == 0) {
			val = serial_get_datasize(serial_fd);
		} else {
			serial_set_datasize(serial_fd, val);
			val = serial_get_datasize(serial_fd);
		}

		rfc2217_respond(tcp_fd, RFC2217_SET_DATASIZE, &val, 1);
		break;
	}

	case RFC2217_SET_PARITY: {
		if (plen < 1)
			break;
		uint8_t val = payload[0];

		if (val == 0) {
			val = serial_get_parity(serial_fd);
		} else {
			serial_set_parity(serial_fd, val);
			val = serial_get_parity(serial_fd);
		}

		rfc2217_respond(tcp_fd, RFC2217_SET_PARITY, &val, 1);
		break;
	}

	case RFC2217_SET_STOPSIZE: {
		if (plen < 1)
			break;
		uint8_t val = payload[0];

		if (val == 0) {
			val = serial_get_stopsize(serial_fd);
		} else {
			serial_set_stopsize(serial_fd, val);
			val = serial_get_stopsize(serial_fd);
		}

		rfc2217_respond(tcp_fd, RFC2217_SET_STOPSIZE, &val, 1);
		break;
	}

	case RFC2217_SET_CONTROL: {
		if (plen < 1)
			break;
		uint8_t val = payload[0];

		switch (val) {
		case CONTROL_REQ_FLOW:
			val = CONTROL_USE_NO_FLOW;
			break;
		case CONTROL_USE_NO_FLOW:
		case CONTROL_USE_XONXOFF:
		case CONTROL_USE_HARDWARE:
			/* Accept but don't change flow control */
			break;
		case CONTROL_REQ_BREAK:
			val = CONTROL_BREAK_OFF;
			break;
		case CONTROL_BREAK_ON:
			serial_set_break(serial_fd, 1);
			break;
		case CONTROL_BREAK_OFF:
			serial_set_break(serial_fd, 0);
			break;
		case CONTROL_REQ_DTR:
			val = CONTROL_DTR_ON;
			break;
		case CONTROL_DTR_ON:
			if (!in_download_mode) {
				in_download_mode = 1;
				log_event("FAKE: injecting boot log");
				/* Send fake boot log directly to TCP */
				static const uint8_t fake_boot[] =
					"ESP-ROM:esp32p4-eco2-20240710\r\n"
					"Build:Jul 10 2024\r\n"
					"rst:0x17 (CHIP_USB_UART_RESET),"
					"boot:0x214 (DOWNLOAD(USB/UART0/"
					"SPI))\r\n"
					"Core0 Saved PC:0x4fc012cc\r\n"
					"Core1 Saved PC:0x4fc058e0\r\n"
					"waiting for download\r\n";
				send_serial_to_tcp(tcp_fd, fake_boot,
				                   sizeof(fake_boot) - 1);
			}
			/* else: absorbed */
			break;
		case CONTROL_DTR_OFF:
			/*
			 * Don't clear in_download_mode here — the POLLERR
			 * from the reset hasn't been processed yet. We clear
			 * it after the first successful serial read/write.
			 */
			if (serial_fd >= 0 && !in_download_mode)
				serial_set_dtr(serial_fd, 0);
			break;
		case CONTROL_REQ_RTS:
			val = CONTROL_RTS_ON;
			break;
		case CONTROL_RTS_ON:
			if (!in_download_mode && serial_fd >= 0)
				serial_set_rts(serial_fd, 1);
			/* else: absorbed */
			break;
		case CONTROL_RTS_OFF:
			if (!in_download_mode) {
				esp_hard_reset(serial_fd);
			}
			/* else: absorbed */
			break;
		}

		rfc2217_respond(tcp_fd, RFC2217_SET_CONTROL, &val, 1);
		break;
	}

	case RFC2217_PURGE_DATA: {
		if (plen < 1)
			break;
		uint8_t val = payload[0];

		switch (val) {
		case PURGE_RX:
			tcflush(serial_fd, TCIFLUSH);
			break;
		case PURGE_TX:
			tcflush(serial_fd, TCOFLUSH);
			break;
		case PURGE_BOTH:
			tcflush(serial_fd, TCIOFLUSH);
			break;
		}

		rfc2217_respond(tcp_fd, RFC2217_PURGE_DATA, &val, 1);
		break;
	}

	case RFC2217_SET_LINESTATE_MASK:
	case RFC2217_SET_MODEMSTATE_MASK: {
		/* Acknowledge the mask but we don't send async notifications */
		uint8_t val = (plen >= 1) ? payload[0] : 0;
		rfc2217_respond(tcp_fd, opcode, &val, 1);
		break;
	}

	case RFC2217_FLOWCONTROL_SUSPEND:
	case RFC2217_FLOWCONTROL_RESUME:
		/* Acknowledge */
		rfc2217_respond(tcp_fd, opcode, NULL, 0);
		break;

	default:
		/* Unknown opcode — ignore */
		break;
	}
}

/* ---- Telnet state machine ---- */

/*
 * Send initial telnet negotiation as server.
 * We announce that we WILL do BINARY, SGA, and COM-PORT-OPTION,
 * and request the client to DO BINARY.
 */
static int telnet_send_initial(int fd)
{
	if (telnet_send_option(fd, WILL, OPT_BINARY) < 0)
		return -1;
	if (telnet_send_option(fd, DO, OPT_BINARY) < 0)
		return -1;
	if (telnet_send_option(fd, WILL, OPT_SGA) < 0)
		return -1;
	if (telnet_send_option(fd, DO, OPT_SGA) < 0)
		return -1;
	if (telnet_send_option(fd, WILL, OPT_COM_PORT) < 0)
		return -1;
	if (telnet_send_option(fd, DO, OPT_ECHO) < 0)
		return -1;
	return 0;
}

static void handle_telnet_option(struct proxy_state *ps, uint8_t action,
                                 uint8_t option)
{
	switch (action) {
	case WILL:
		/* Client says WILL — we respond DO for options we support */
		switch (option) {
		case OPT_BINARY:
		case OPT_SGA:
		case OPT_ECHO:
		case OPT_COM_PORT:
			telnet_send_option(ps->tcp_fd, DO, option);
			break;
		default:
			telnet_send_option(ps->tcp_fd, DONT, option);
			break;
		}
		break;

	case DO:
		/* Client says DO — we respond WILL for options we support */
		switch (option) {
		case OPT_BINARY:
		case OPT_SGA:
		case OPT_COM_PORT:
			telnet_send_option(ps->tcp_fd, WILL, option);
			break;
		default:
			telnet_send_option(ps->tcp_fd, WONT, option);
			break;
		}
		break;

	case WONT:
		/* Client says WONT — acknowledge with DONT */
		telnet_send_option(ps->tcp_fd, DONT, option);
		break;

	case DONT:
		/* Client says DONT — acknowledge with WONT */
		telnet_send_option(ps->tcp_fd, WONT, option);
		break;
	}
}

/*
 * Process a byte from the TCP stream through the telnet state machine.
 * Serial data bytes are written directly to the serial port.
 */
static int process_tcp_byte(struct proxy_state *ps, uint8_t byte)
{
	switch (ps->tstate) {

	case TS_DATA:
		if (byte == IAC) {
			ps->tstate = TS_IAC;
		} else {
			/*
			 * TEMPORARY HACK: During download mode, intercept
			 * SLIP frames and inject fake bootloader responses
			 * directly on TCP to test if esptool works.
			 */
			if (in_download_mode) {
				/* Buffer data bytes to detect SLIP frames */
				static uint8_t fakebuf[256];
				static size_t fakelen = 0;

				if (byte == 0xc0 && fakelen > 0) {
					/* End of SLIP frame — check command */
					static const uint8_t sync_resp[] = {
						0xc0, 0x01, 0x08, 0x04,
						0x00, 0x07, 0x07, 0x12,
						0x20, 0x00, 0x00, 0x00,
						0x00, 0xc0
					};
					static const uint8_t sec_resp[] = {
						0xc0, 0x01, 0x14, 0x18,
						0x00, 0x00, 0x00, 0x00,
						0x00, 0x00, 0x00, 0x00,
						0x00, 0x01, 0x00, 0x00,
						0x00, 0x00, 0x00, 0x0c,
						0x12, 0x00, 0x00, 0x00,
						0x02, 0x00, 0x00, 0x00,
						0x00, 0x00, 0x00, 0x00,
						0xc0
					};
					static const uint8_t generic_resp[] = {
						0xc0, 0x01, 0x00, 0x02,
						0x00, 0x00, 0x00, 0x00,
						0x00, 0x00, 0xc0
					};
					static const uint8_t read_reg_resp[] = {
						0xc0, 0x01, 0x0a, 0x04,
						0x00, 0x00, 0x00, 0x00,
						0x00, 0x00, 0x00, 0x00,
						0xc0
					};

					if (fakelen >= 2 && fakebuf[0] == 0x00) {
						uint8_t cmd = fakebuf[1];
						log_event("FAKE: cmd=0x%02x len=%zu", cmd, fakelen);
						if (cmd == 0x08) { /* SYNC */
							for (int r = 0; r < 8; r++)
								tcp_send(ps->tcp_fd, sync_resp, sizeof(sync_resp));
						} else if (cmd == 0x14) { /* GET_SECURITY_INFO */
							tcp_send(ps->tcp_fd, sec_resp, sizeof(sec_resp));
						} else if (cmd == 0x0a) { /* READ_REG */
							tcp_send(ps->tcp_fd, read_reg_resp, sizeof(read_reg_resp));
						} else {
							tcp_send(ps->tcp_fd, generic_resp, sizeof(generic_resp));
						}
					}
					fakelen = 0;
				} else if (byte == 0xc0 && fakelen == 0) {
					/* Start of SLIP frame — skip marker */
				} else if (fakelen < sizeof(fakebuf)) {
					fakebuf[fakelen++] = byte;
				}
				break;
			}
			log_data(log_serial_out, &byte, 1);
			ssize_t wr = write(ps->serial_fd, &byte, 1);
			if (wr < 0 && errno != EAGAIN) {
				log_event("serial_write error: %s",
				          strerror(errno));
				return -1;
			}
		}
		break;

	case TS_IAC:
		switch (byte) {
		case IAC:
			/* Escaped 0xFF — write literal to serial */
			ps->tstate = TS_DATA;
			if (!in_download_mode) {
				if (write(ps->serial_fd, &byte, 1) < 0 &&
				    errno != EAGAIN)
					return -1;
			}
			break;
		case WILL:  ps->tstate = TS_WILL; break;
		case WONT:  ps->tstate = TS_WONT; break;
		case DO:    ps->tstate = TS_DO;   break;
		case DONT:  ps->tstate = TS_DONT; break;
		case SB:    ps->tstate = TS_SB;   break;
		default:
			/* Unknown command — ignore */
			ps->tstate = TS_DATA;
			break;
		}
		break;

	case TS_WILL:
		handle_telnet_option(ps, WILL, byte);
		ps->tstate = TS_DATA;
		break;

	case TS_WONT:
		handle_telnet_option(ps, WONT, byte);
		ps->tstate = TS_DATA;
		break;

	case TS_DO:
		handle_telnet_option(ps, DO, byte);
		ps->tstate = TS_DATA;
		break;

	case TS_DONT:
		handle_telnet_option(ps, DONT, byte);
		ps->tstate = TS_DATA;
		break;

	case TS_SB:
		/* First byte of subnegotiation is the option */
		ps->sb_option = byte;
		ps->sb_len = 0;
		ps->tstate = TS_SB_DATA;
		break;

	case TS_SB_DATA:
		if (byte == IAC) {
			ps->tstate = TS_SB_IAC;
		} else if (ps->sb_len < sizeof(ps->sb_buf)) {
			ps->sb_buf[ps->sb_len++] = byte;
		}
		break;

	case TS_SB_IAC:
		if (byte == SE) {
			/* End of subnegotiation */
			if (ps->sb_option == OPT_COM_PORT) {
				handle_com_port_subneg(ps,
				                       ps->sb_buf,
				                       ps->sb_len);
			}
			ps->tstate = TS_DATA;
		} else if (byte == IAC) {
			/* Escaped 0xFF in subnegotiation data */
			if (ps->sb_len < sizeof(ps->sb_buf))
				ps->sb_buf[ps->sb_len++] = IAC;
			ps->tstate = TS_SB_DATA;
		} else {
			/* Unexpected — return to data state */
			ps->tstate = TS_DATA;
		}
		break;
	}

	return 0;
}

/*
 * Send serial data to the TCP client with IAC escaping.
 * Any 0xFF byte in the data must be doubled (0xFF 0xFF).
 */
static int send_serial_to_tcp(int tcp_fd, const uint8_t *data, size_t len)
{
	uint8_t buf[SERIAL_BUF_SIZE * 2]; /* Worst case: every byte is 0xFF */
	size_t out = 0;

	for (size_t i = 0; i < len; i++) {
		buf[out++] = data[i];
		if (data[i] == IAC)
			buf[out++] = IAC;
	}

	log_event("tcp_out %zu bytes (from %zu serial)", out, len);
	log_data(log_tcp_out, buf, out);
	return tcp_send(tcp_fd, buf, out);
}

/* ---- Client session handler ---- */

static void handle_client(int client_fd, const char *device,
                          unsigned int baud)
{
	int serial_fd = serial_open(device, baud);
	if (serial_fd < 0) {
		close(client_fd);
		return;
	}

	fprintf(stderr, "rfc2217proxy: client connected, serial %s @ %u\n",
	        device, baud);

	log_open();
	log_event("client connected, serial %s @ %u", device, baud);

	/* Send initial telnet negotiation */
	if (telnet_send_initial(client_fd) < 0) {
		fprintf(stderr, "rfc2217proxy: initial negotiation failed\n");
		close(serial_fd);
		close(client_fd);
		return;
	}

	struct proxy_state ps = {
		.tcp_fd = client_fd,
		.serial_fd = serial_fd,
		.tstate = TS_DATA,
		.device = device,
		.baud = baud,
	};

	while (running) {
		/*
		 * Rebuild pollfd each iteration since serial_fd may
		 * change after a device reset/reopen.
		 */
		struct pollfd pfds[2] = {
			{ .fd = client_fd,     .events = POLLIN },
			{ .fd = ps.serial_fd,  .events = POLLIN },
		};

		int ret = poll(pfds, 2, 500);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (ret == 0)
			continue;

		/* TCP -> serial (via telnet state machine) */
		if (pfds[0].revents & POLLIN) {
			uint8_t buf[TCP_BUF_SIZE];
			ssize_t n = recv(client_fd, buf, sizeof(buf), 0);
			if (n <= 0)
				break;

			log_event("tcp_in %zd bytes", n);
			log_data(log_tcp_in, buf, n);

			for (ssize_t i = 0; i < n; i++) {
				if (process_tcp_byte(&ps, buf[i]) < 0)
					goto done;
			}
		}

		if (pfds[0].revents & (POLLERR | POLLHUP))
			break;

		/* Serial -> TCP (with IAC escaping) */
		if (pfds[1].revents & POLLIN) {
			uint8_t buf[SERIAL_BUF_SIZE];
			ssize_t n = read(ps.serial_fd, buf, sizeof(buf));
			if (n > 0) {
				log_event("serial_in %zd bytes", n);
				log_data(log_serial_in, buf, n);
				in_download_mode = 0; /* serial is alive */
				if (send_serial_to_tcp(client_fd, buf, n) < 0)
					break;
			} else if (n < 0 && errno != EAGAIN) {
				if (in_download_mode) {
					usleep(10000); /* 10ms */
					continue;
				}
				fprintf(stderr,
				        "rfc2217proxy: serial read error\n");
				break;
			}
		}

		if (pfds[1].revents & POLLERR) {
			if (in_download_mode) {
				/*
				 * Expected during reset. Sleep briefly
				 * to avoid busy-looping on POLLERR.
				 */
				usleep(10000); /* 10ms */
				continue;
			}
			fprintf(stderr, "rfc2217proxy: serial POLLERR\n");
			break;
		}
	}

done:
	if (ps.serial_fd >= 0)
		close(ps.serial_fd);
	close(client_fd);
	log_event("session ended");
	log_close();
	fprintf(stderr, "rfc2217proxy: session ended\n");
}

/* ---- TCP listener ---- */

static int tcp_listen(int port)
{
	int sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0) {
		perror("rfc2217proxy: socket");
		return -1;
	}

	int opt = 1;
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(port),
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};

	if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("rfc2217proxy: bind");
		close(sock);
		return -1;
	}

	if (listen(sock, 1) < 0) {
		perror("rfc2217proxy: listen");
		close(sock);
		return -1;
	}

	return sock;
}

/* ---- Main ---- */

static void usage(const char *prog)
{
	fprintf(stderr,
	        "Usage: %s -d <device> [-p <port>] [-b <baud>] [-h]\n\n"
	        "  -d device  Serial port device (required)\n"
	        "  -p port    TCP port to listen on (default: %d)\n"
	        "  -b baud    Initial baud rate (default: %d)\n"
	        "  -h         Show this help\n",
	        prog, DEFAULT_PORT, DEFAULT_BAUD);
}

int main(int argc, char *argv[])
{
	int port = DEFAULT_PORT;
	unsigned int baud = DEFAULT_BAUD;
	const char *device = NULL;
	int opt;

	while ((opt = getopt(argc, argv, "d:p:b:h")) != -1) {
		switch (opt) {
		case 'd':
			device = optarg;
			break;
		case 'p':
			port = atoi(optarg);
			if (port <= 0 || port > 65535) {
				fprintf(stderr,
				        "rfc2217proxy: invalid port: %s\n",
				        optarg);
				return 1;
			}
			break;
		case 'b':
			baud = atoi(optarg);
			if (baud == 0) {
				fprintf(stderr,
				        "rfc2217proxy: invalid baud: %s\n",
				        optarg);
				return 1;
			}
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (!device) {
		fprintf(stderr, "rfc2217proxy: -d <device> is required\n");
		usage(argv[0]);
		return 1;
	}

	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);
	signal(SIGPIPE, SIG_IGN);

	int listen_fd = tcp_listen(port);
	if (listen_fd < 0)
		return 1;

	fprintf(stderr, "rfc2217proxy: listening on 127.0.0.1:%d for %s\n",
	        port, device);

	while (running) {
		struct pollfd pfd = {
			.fd = listen_fd,
			.events = POLLIN,
		};

		int ret = poll(&pfd, 1, 1000);
		if (ret <= 0)
			continue;

		int client_fd = accept(listen_fd, NULL, NULL);
		if (client_fd < 0) {
			if (errno == EINTR)
				continue;
			perror("rfc2217proxy: accept");
			break;
		}

		/* Enable TCP_NODELAY for low latency */
		int flag = 1;
		setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY,
		           &flag, sizeof(flag));

		handle_client(client_fd, device, baud);
	}

	close(listen_fd);
	fprintf(stderr, "rfc2217proxy: shutdown\n");
	return 0;
}
