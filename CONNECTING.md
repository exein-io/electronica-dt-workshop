# Connecting to the DK and opening its serial port

Everything that can go wrong between plugging in an nRF9151 DK and seeing its log, on Linux, macOS and Windows. Symptom first, then cause and fix. Where the README or the web flasher's README already cover a point, this page only links to it.

The DK's on-board J-Link is **one USB device with two roles**: a vendor interface for flashing (WebUSB in the browser, or nrfutil/J-Link tools) and two virtual COM ports (CDC ACM) for the board's UARTs. Most problems come from one of three things: the OS not letting your user open the device, another program already holding it, or a sandboxed browser that can't see it.

## 1. Connect the J-Link (flashing over WebUSB)

**Requirements** are in [webflash/README.md → Requirements](webflash/README.md#requirements): Chrome or Edge 137 or newer, on Linux, macOS, Windows or Android. Firefox and Safari have no WebUSB. The browser's device chooser lists the probe as *J-Link* with its serial number, which is also printed on the DK's sticker; with several boards connected, pick by serial number.

**Before anything fails halfway**, read [README → CRITICAL: nuSIM safety rules](README.md#critical-nusim-safety-rules). A flash that stops mid-way is harmless as long as you don't unplug, power off or reset: click **Flash** again.

### Only one program can hold the probe

Error contains `claimInterface` (any OS), or the chooser shows the J-Link but connecting fails. Close nRF Connect for Desktop, `nrfutil`, J-Link Commander/GDB server, the VS Code nRF Connect extension and any other flasher tab, then replug. The flasher prints this hint itself.

### Linux: `Failed to execute 'open' on 'USBDevice': Access denied.`

Your user can't open the USB device node. The DK's J-Link enumerates as USB id `1366:1068`; the J-Link udev rules many distributions ship (libjaylink's `60-libjaylink.rules`, used by OpenOCD) list product ids only up to `1061`, so they never match and the node stays `root:root 0664`. Add a vendor-wide rule, which also covers every other SEGGER probe:

```sh
echo 'SUBSYSTEM=="usb", ATTRS{idVendor}=="1366", TAG+="uaccess"' | sudo tee /etc/udev/rules.d/70-jlink-usb.rules
sudo udevadm control --reload
sudo udevadm trigger --action=add --subsystem-match=usb --attr-match=idVendor=1366
lsusb -d 1366:                                   # Bus 003 Device 008 → /dev/bus/usb/003/008
getfacl -p /dev/bus/usb/003/008                  # expect user:<you>:rw-
```

If the ACL line is missing, replug the DK (the bus/device number changes). No browser restart is needed.

Sandboxed browsers: the Flatpak Chrome from Flathub ships with `devices=all`, so the host rule above is enough for WebUSB. Snap Chromium is reported to need `sudo snap connect chromium:raw-usb` (not tested here).

### Windows: connecting fails although nothing else is running

WebUSB needs the probe's vendor interface bound to the **WinUSB** driver: Device Manager must list it as *J-Link Device (WinUSB)*. With SEGGER's own J-Link driver bound instead, the browser gets the same claim error as when another program holds the probe. The web flasher on Windows is tested on one machine ([webflash/README.md → Requirements](webflash/README.md#requirements)); the alternative is [README → Flash a release → B. With nrfutil](README.md#b-with-nrfutil-windows-macos-linux).

### macOS, Android

Nothing extra is known to be required. Android needs a USB OTG adapter.

### Codespaces / devcontainer

USB is not passed into the container. The forwarded port opens the web flasher in your local browser, which talks to the board directly; the serial terminal is local too. See [README → Codespaces / devcontainer](README.md#codespaces--devcontainer-nothing-to-install).

## 2. Open the serial port (log and AT commands)

### Which port, which settings

The J-Link exposes two CDC ACM ports. The **first** one is the application UART: the log and, with the workshop firmware, the AT host. The second is silent unless modem tracing is enabled.

| OS | First port |
|---|---|
| Linux | `/dev/serial/by-id/usb-SEGGER_J-Link_<S/N>-if00` (stable name) → usually `/dev/ttyACM0`; `-if02` is the second |
| macOS | `/dev/cu.usbmodem<S/N>1`, the lower-numbered of the two |
| Windows | the lower-numbered *JLink CDC UART Port (COMx)* of the two |

115200 baud, 8N1, no flow control.

### A terminal that opens fine but shows nothing

**DTR must be asserted.** The J-Link only forwards the target's UART while DTR is set; without it the port opens without error and stays silent, including for typed AT commands. Known good: the web flasher terminal (sets DTR itself), `python3 -m serial.tools.miniterm --dtr 1 <port> 115200`, PuTTY and Tera Term (assert DTR on open). Known to fail silently: `picocom --noreset`, minicom, raw `stty` ([README → Troubleshooting](README.md#troubleshooting), "No AT response").

Also: **the board only logs at boot and on events.** A quiet terminal after a successful open is normal. Press RESET on the DK for the boot log, open the port *before* resetting to catch it from the start. While the monitor is offline it logs a `dispatcher` POST attempt about every 7 s, which at least proves the link.

### Only one program can hold the port

If miniterm (or any terminal) has the port, the browser's **Open serial port…** fails with *Failed to open serial port* or shows nothing. Quit miniterm with Ctrl+] first. The same applies the other way round.

### Linux: `Failed to execute 'open' on 'SerialPort': Failed to open serial port.`

`/dev/ttyACM*` is `root:dialout 0660`. Fix with the udev rule in [README → Troubleshooting](README.md#troubleshooting) (it also tells ModemManager to ignore the port; ModemManager otherwise probes every new ACM port for a few seconds after plug-in), or join the `dialout` group and log in again.

### Linux, Flatpak Chrome: the chooser says *No compatible devices found*

Chrome enumerates serial ports through the udev database in `/run/udev`, which the Flatpak sandbox doesn't share, so the chooser is empty even though the ports exist and you have permission. Grant read access and restart Chrome **completely**: every window, and the background process if "Continue running background apps" is on (`flatpak ps` shows what is still alive). No sudo needed.

```sh
flatpak override --user --filesystem=/run/udev:ro com.google.Chrome
flatpak run --command=ls com.google.Chrome -ld /run/udev/data     # must list the directory
# undo later: flatpak override --user --nofilesystem=/run/udev com.google.Chrome
```

Google's .rpm/.deb Chrome has none of this. Snap Chromium has a similar confinement (`serial-port` interface, not tested here). The web flasher only offers ports with SEGGER's vendor id, so a J-Link that is visible on the host but absent from the chooser is always a sandbox problem.

### Colour codes and line endings in miniterm

The log uses ANSI colours; the web terminal strips them, miniterm's default filter renders the escape byte as `␛[0m`. Pass them through to a colour-capable terminal with `--filter direct`. Keep the default end-of-line (CR): the firmware's AT host terminates commands on CR.

### AT commands on the same UART

- Type the command and press Enter. Add `--echo` to miniterm to see what you type. Responses are interleaved with log lines.
- The first command right after opening the port sometimes gets `ERROR` from a stray byte; just repeat it.
- The modem answers **strictly in order**. `AT+COPS=?` (network scan) blocks the channel for several minutes; everything typed meanwhile is answered afterwards. A board that "stopped responding" while still logging is usually waiting for a scan.
- `AT%CSUS`, `AT+CGDCONT`, `AT+CGAUTH` are only accepted in `AT+CFUN=0` (or 4). `AT+CFUN?` answering `21` is normal: the firmware activates LTE without GNSS.
- `AT%CLEARFPLMN` returns `ERROR` while the UICC is off (CFUN=0); `AT+CGAUTH=0,0` returns `ERROR` when no credentials are stored. Both are harmless; `AT+CGAUTH?` shows what is stored.
- `AT+CNEC=24` then `AT+CEREG=5` make the modem print attach reject causes (`+CNEC_EMM`, `+CNEC_ESM`) on the terminal. The firmware logs ESM causes only at debug level, so this is the way to see *why* an attach fails.
- Health check commands and expected answers: [README → Run the demo](README.md#run-the-demo) (last paragraph).

## 3. One-time Linux setup, all at once

```sh
# USB device (WebUSB flashing) and tty (serial terminal), ModemManager off the tty
echo 'SUBSYSTEM=="usb", ATTRS{idVendor}=="1366", TAG+="uaccess"' | sudo tee /etc/udev/rules.d/70-jlink-usb.rules
echo 'SUBSYSTEM=="tty", ATTRS{idVendor}=="1366", TAG+="uaccess", ENV{ID_MM_DEVICE_IGNORE}="1"' | sudo tee /etc/udev/rules.d/70-jlink-tty.rules
sudo udevadm control --reload && sudo udevadm trigger --action=add --subsystem-match=usb --attr-match=idVendor=1366 && sudo udevadm trigger --action=add --subsystem-match=tty
# Flatpak Chrome only
flatpak override --user --filesystem=/run/udev:ro com.google.Chrome
```

Then replug the DK, restart Chrome if it is the Flatpak, and verify: `getfacl -p /dev/bus/usb/<bus>/<dev>` and `getfacl /dev/ttyACM0` both show `user:<you>:rw-`.

## References

- [README → CRITICAL: nuSIM safety rules](README.md#critical-nusim-safety-rules): never unplug or reset after a failed flash
- [README → Flash a release](README.md#flash-a-release): browser and nrfutil paths
- [README → Run the demo](README.md#run-the-demo): what the log looks like, AT health checks
- [README → Troubleshooting](README.md#troubleshooting): tty permissions, DTR, modem status codes
- [webflash/README.md → Requirements](webflash/README.md#requirements), [→ Serial terminal](webflash/README.md#serial-terminal): browser versions, Windows status, how the terminal opens the port
- probe-rs [probe setup](https://probe.rs/docs/getting-started/probe-setup/): the upstream udev guidance the vendor-wide rule follows
