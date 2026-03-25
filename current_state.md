# RFC2217 Proxy - Current State (2026-03-25)

## Summary

Building an RFC2217-to-serial proxy (`rfc2217proxy.c`) to replace ser2net for remote ESP32-P4 flashing via esptool. The proxy's RFC2217/telnet implementation is proven correct. The remaining task is making the proxy handle the ESP32-P4's USB JTAG/Serial reset correctly without closing/reopening the serial port fd.

## What works

- RFC2217 telnet negotiation (BINARY, SGA, COM-PORT-OPTION)
- PURGE acknowledgment (ser2net 4.6.0 didn't support this — the original reason we wrote the proxy)
- Baud rate, data size, parity, stop bits configuration
- Bidirectional data forwarding with IAC escaping
- pyserial and ESP-IDF pyserial both connect and negotiate successfully
- Fake bootloader injection test proves the entire RFC2217 data path works end-to-end
- Direct C test (`test_reset.c`) proves the full bootloader chain works without RFC2217

## Key findings

### 1. The USB does NOT disconnect when the fd is kept open

The `test_reset.c` program proves this conclusively. When the serial port fd is kept open through the USBJTAGSerialReset sequence:
- Boot log arrives within 50ms
- SYNC succeeds on the first or second attempt
- Stub upload, OHAI, and READ_REG all work
- No close/reopen needed at any point

**The proxy's close+reopen after reset was the root cause of all previous failures.** Closing the fd causes the kernel CDC ACM driver to release the USB device, which then requires re-enumeration. Keeping the fd open avoids this entirely.

### 2. esptool uses the wrong reset strategy for RFC2217

In `esptool/loader.py:697`, when the port URL starts with `rfc2217://`, esptool cannot detect the USB PID and falls back to `ClassicReset`. The ESP32-P4 USB JTAG/Serial requires `USBJTAGSerialReset` which has a different DTR/RTS sequence. The proxy must intercept the reset and run the correct sequence locally.

### 3. Espressif's own esp_rfc2217_server also fails

Tested with the official `esp_rfc2217_server.py`. It also cannot handle the ESP32-P4 USB JTAG/Serial reset — the `Redirector.reader()` thread dies on OSError and kills the TCP connection. Upgrading esptool won't help; this is unfixed on current master.

### 4. The proxy's RFC2217 implementation is correct

Proven by injecting fake bootloader responses directly into the TCP stream. esptool synced, detected the chip, read registers, and started uploading the flasher stub — all through our proxy.

### 5. Watchdog disable is required

The ESP32-P4's RTC WDT and SWD watchdogs are not reset when using USB JTAG/Serial. Without disabling them via WRITE_REG before stub upload, the watchdog fires and resets the chip before the stub can start. esptool handles this in `_post_connect() -> disable_watchdogs()`.

### 6. Chip revision matters for stub selection

The Tanmatsu has ESP32-P4 revision 1.3 (eco2). `get_chip_revision()` returns 103. Since `103 < 300`, the correct stub is `esp32p4rc1.json` (text at `0x4ff10000`), not `esp32p4.json` (text at `0x4ff50000`). Using the wrong stub causes it to silently fail to start (no OHAI).

### 7. MEM_DATA checksum must cover data only

esptool's `mem_block()` passes `self.checksum(data)` — the checksum of the raw data block, NOT the 16-byte header (`len, seq, 0, 0`). Computing the checksum over the full payload (header + data) produces wrong checksums and the stub silently fails.

## What needs to be done next

Fix the proxy to:
1. **Intercept DTR ON** and run `USBJTAGSerialReset` locally (already implemented)
2. **Keep the fd open** through the reset — do NOT close/reopen
3. **Absorb subsequent DTR/RTS commands** from esptool's ClassicReset (already implemented via `in_download_mode` flag)
4. **Clear `in_download_mode`** after first successful serial read (bootloader is responding)
5. Handle POLLERR/read errors during download mode gracefully (the fd may temporarily return errors right after reset but recovers)

## test_reset.c — Reference implementation

A standalone C program that demonstrates the full working chain:

```
Open /dev/ttyACM0
  → USBJTAGSerialReset (300ms)
  → Read boot log (50ms)
  → SYNC (succeeds on attempt 1 or 2)
  → GET_SECURITY_INFO
  → Disable watchdogs (WRITE_REG)
  → Upload stub text (MEM_BEGIN + MEM_DATA, correct checksum)
  → Upload stub data
  → MEM_END → OHAI received
  → READ_REG 0x40001000 → Chip magic 0x26030007
Total: ~1.25 seconds, no fd close/reopen
```

Build: `make test_reset`
Run: `./test_reset /dev/ttyACM0`

## How to run a full test cycle

### 1. Build everything

```bash
cd ~/src/badgefs
make
```

### 2. Kill any existing proxies and services

```bash
sudo killall -9 rfc2217proxy 2>/dev/null
# If udev rules are installed, disable them to prevent interference:
sudo rm -f /etc/udev/rules.d/99-tanmatsu.rules
sudo udevadm control --reload-rules
```

### 3. Identify the serial device

```bash
ls /dev/ttyACM*
# Check which is P4 (serial 30:ED:A0:E2:F4:A8):
udevadm info /dev/ttyACM0 | grep ID_USB_SERIAL_SHORT
```

### 4. Run the direct test (no proxy, proves hardware works)

```bash
./test_reset /dev/ttyACM0
```

### 5. Start the proxy manually

```bash
~/src/badgefs/rfc2217proxy -d /dev/ttyACM0 -p 4000 -b 115200 &
```

### 6. Quick RFC2217 negotiation test (should print "RFC2217 OK")

```bash
timeout 15 python3 -c "
import serial.rfc2217
s = serial.rfc2217.Serial('rfc2217://localhost:4000', 115200, timeout=5)
print('RFC2217 OK')
s.close()
"
```

### 7. Full flash test via esptool

```bash
cd ~/src/tanmatsu/tanmatsu-launcher
export IDF_TOOLS_PATH="$(pwd)/esp-idf-tools"
source esp-idf/export.sh
export PORT='rfc2217://localhost:4000'
make install
```

### 8. Cleanup

```bash
killall rfc2217proxy 2>/dev/null
```

## Files

| File | Purpose |
|------|---------|
| `rfc2217proxy.c` | RFC2217 proxy (telnet COM port to serial) |
| `test_reset.c` | Standalone ESP32-P4 reset + stub upload test |
| `stub_esp32p4.h` | ESP32-P4 RC1 flasher stub (auto-generated from esptool JSON) |
| `Makefile` | Builds all targets including rfc2217proxy and test_reset |

### In `/home/cavac/src/ser2net/`:

| File | Purpose |
|------|---------|
| `install.sh` | Updated to use rfc2217proxy instead of ser2net |
| `ser2net-tanmatsu-p4.service` | Systemd service for P4 rfc2217proxy |
| `ser2net-tanmatsu-p6.service` | Systemd service for P6 rfc2217proxy |
| `tanmatsu-env.sh` | PORT environment variable for remote flashing |
