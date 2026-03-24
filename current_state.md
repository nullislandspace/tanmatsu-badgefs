# RFC2217 Proxy - Current State (2026-03-24)

## What works

- RFC2217 telnet negotiation (BINARY, SGA, COM-PORT-OPTION) - fully functional
- PURGE acknowledgment - the root cause of the original ser2net failure
- Baud rate, data size, parity, stop bits configuration
- Bidirectional data forwarding with IAC escaping
- pyserial and ESP-IDF pyserial both connect and negotiate successfully
- Direct flash via `/dev/ttyACM0` works perfectly

## What doesn't work yet

ESP32-P4 bootloader entry via RFC2217 reset sequence.

## Root cause analysis

### 1. esptool uses the wrong reset strategy for RFC2217

In `esptool/loader.py:697` (`_construct_reset_strategy_sequence`):

```python
# USB-JTAG/Serial mode - detected by PID
if mode == "usb_reset" or self._get_pid() == self.USB_JTAG_SERIAL_PID:
    return (USBJTAGSerialReset(self._port),)

# USB-to-Serial bridge (Unix only, NOT for rfc2217)
if os.name != "nt" and not self._port.name.startswith("rfc2217:"):
    return (UnixTightReset(...), ClassicReset(...))

# Fallback (used for rfc2217)
return (ClassicReset(...),)
```

When connecting via `rfc2217://`, esptool:
- Cannot detect the USB PID (no `/dev/` path)
- Skips `UnixTightReset` (explicitly excluded for rfc2217)
- Falls back to `ClassicReset`, which is **wrong** for ESP32-P4 USB JTAG/Serial

The direct flash works because esptool detects `USB_JTAG_SERIAL_PID` and uses `USBJTAGSerialReset`.

### 2. USB JTAG/Serial disconnects during reset

When the ESP32-P4 resets, the USB JTAG/serial debug unit disconnects and re-enumerates. `/dev/ttyACM0` disappears and reappears (potentially as a different number). The proxy must:
1. Close the stale serial fd
2. Wait for the device to reappear
3. Reopen the serial port

### 3. Timing gap after reopen

The reopen takes 1-2 seconds. During this time:
- esptool sends sync packets that are buffered in TCP but have no serial port to forward to
- By the time the port reopens, the bootloader's sync window may have passed
- esptool retries (sends another DTR ON), triggering another reset cycle

This creates an infinite loop: reset -> USB disconnect -> reopen (slow) -> sync window missed -> retry -> reset again.

## Current proxy approach

The proxy intercepts DTR ON (first occurrence) and runs `USBJTAGSerialReset` locally with proper timing, then reopens the serial port. Subsequent DTR/RTS changes are absorbed while in "download mode" until DTR is released.

The USBJTAGSerialReset sequence (from esptool source):
```
RTS=0, DTR=0        # Idle
sleep 100ms
DTR=1, RTS=0        # Set IO0
sleep 100ms
RTS=1, DTR=0        # Reset (through 1,1 not 0,0)
RTS=1                # Windows compat
sleep 100ms
DTR=0, RTS=0        # Out of reset
```

## The `ign_set_control` parameter

The PORT URL was originally `rfc2217://localhost:4000?ign_set_control`. We removed the `?ign_set_control` parameter during testing.

**What `ign_set_control` does:** Tells pyserial to NOT wait for the server's SET_CONTROL response. Instead it sleeps 100ms after each DTR/RTS change and moves on.

**Without `ign_set_control`:** pyserial waits for our proxy's response to each SET_CONTROL before sending the next command. Since we intercept DTR ON and run the full reset sequence locally (~400ms), pyserial is blocked waiting for our response during that entire time. After we respond, pyserial sends the remaining DTR/RTS commands which we absorb — but this creates back-and-forth round trips that add delay.

**With `ign_set_control`:** pyserial fires all DTR/RTS commands rapidly without waiting. Our proxy intercepts the first DTR ON and runs the reset locally while absorbing the rest. The 100ms sleeps between commands from pyserial don't matter since we're absorbing them anyway.

**Impact on PURGE:** The `ign_set_control` parameter only affects SET_CONTROL, not PURGE. PURGE acknowledgment works correctly regardless.

**Impact on open():** Without `ign_set_control`, pyserial also waits for SET_CONTROL responses during the initial `_reconfigure_port()` and `_update_dtr_state()`/`_update_rts_state()` calls in `open()`. This adds latency before the reset sequence even starts.

**Bottom line:** Restoring `?ign_set_control` may reduce latency in the proxy's reset interception flow, but won't solve the USB disconnect/reopen timing gap.

## Possible solutions to explore

1. **Restore `?ign_set_control`** - Quick test. Reduces latency in SET_CONTROL round trips. Proxy still intercepts DTR ON for local reset. Test with `PORT='rfc2217://localhost:4000?ign_set_control'`.

2. **Don't reopen after reset** - The USB JTAG/serial debug unit on ESP32-P4 may be a separate chip that doesn't fully disconnect during SoC reset. The "Serial data stream stopped" error without reopen may have been caused by esptool failing to sync (timing), not a dead fd. Worth testing: remove the close/reopen logic and just let the poll loop handle any POLLERR naturally.

3. **Use `--before usb_reset`** - esptool supports `--before usb_reset` which forces `USBJTAGSerialReset` regardless of port type. If set in the Makefile or as an esptool config option, the proxy wouldn't need to intercept at all — just forward DTR/RTS directly. This would be the cleanest solution IF the ~50ms per SET_CONTROL round-trip through RFC2217 doesn't destroy USBJTAGSerialReset timing (the strategy has ~6 DTR/RTS changes). Can test locally by modifying the tanmatsu-launcher Makefile's flash target.

4. **Faster reopen with stable path** - Use udev symlinks (`/dev/tanmatsu_p4`) instead of `/dev/ttyACMx` for reliable reopening. Reduce the reopen polling interval from 100ms to 10ms. The current reopen takes 1-2 seconds which misses the bootloader sync window.

5. **Pre-buffer sync packets** - After reopen, immediately forward any TCP data that accumulated during the reopen wait. Currently, sync packets from esptool pile up in the TCP receive buffer while the proxy is blocked in the reopen loop.

6. **Custom esptool reset config** - esptool supports `custom_reset_sequence` in its config file. Could define the USBJTAGSerialReset sequence there, which esptool would use instead of ClassicReset for rfc2217 ports.

7. **Combine interception + no reopen + ign_set_control** - Try all three together: intercept DTR ON to run USBJTAGSerialReset locally, don't close/reopen the serial port, and use `?ign_set_control` in the URL. If the USB doesn't actually disconnect, this should work.

## How to run a full test cycle

### 1. Build the proxy

```bash
cd ~/src/badgefs
make rfc2217proxy
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

### 4. Start the proxy manually

```bash
~/src/badgefs/rfc2217proxy -d /dev/ttyACM0 -p 4000 -b 115200 &
```

### 5. Quick RFC2217 negotiation test (should print "RFC2217 OK")

```bash
timeout 15 python3 -c "
import serial.rfc2217
s = serial.rfc2217.Serial('rfc2217://localhost:4000', 115200, timeout=5)
print('RFC2217 OK')
s.close()
"
```

### 6. Full flash test via esptool

```bash
cd ~/src/tanmatsu/tanmatsu-launcher
export IDF_TOOLS_PATH="$(pwd)/esp-idf-tools"
source esp-idf/export.sh
export PORT='rfc2217://localhost:4000'
make install
```

### 7. Cleanup

```bash
killall rfc2217proxy 2>/dev/null
```

## Files

- `/home/cavac/src/badgefs/rfc2217proxy.c` - The RFC2217 proxy (~700 lines C)
- `/home/cavac/src/badgefs/Makefile` - Updated to build rfc2217proxy
- `/home/cavac/src/ser2net/install.sh` - Updated to use rfc2217proxy instead of ser2net
- `/home/cavac/src/ser2net/ser2net-tanmatsu-p4.service` - Updated for rfc2217proxy
- `/home/cavac/src/ser2net/ser2net-tanmatsu-p6.service` - Updated for rfc2217proxy
