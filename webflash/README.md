# webflash: browser flasher for the nRF9151 DK

An alternative to flashing with Nordic's tools. [probe-rs](https://probe.rs) compiled to WebAssembly drives the DK's onboard J-Link over WebUSB, straight from Chrome/Edge.

**Live:** https://exein-io.github.io/electronica-dt-workshop/ — CI publishes it on GitHub Pages for each `v*` tag, with that release's `workshop.hex` bundled (or pick your own `.hex`). The same `index.html` and `webflash.wasm` are attached to the release.

To serve it yourself (needs `gh`):
```bash
webflash/serve.sh                                     # latest release → http://localhost:8765
HEX=app/build/merged.hex webflash/serve.sh    # your own build
```
In the devcontainer on GitHub Codespaces, port 8765 is forwarded privately (behind your GitHub login): the page loads from the Codespace while WebUSB talks to the J-Link on your own machine.

## Requirements

- Chrome or Edge **137+** (WebUSB + WebAssembly JSPI) on **Linux, macOS or Android** (phone with USB OTG). On Windows the SEGGER J-Link driver owns the device, so WebUSB can't open it.
- Close nRF Connect, `nrfutil`, J-Link tools and anything else using the probe: only one program can hold it.
- Linux: your user needs access to the J-Link USB device (same udev setup as [probe-rs](https://probe.rs/docs/getting-started/probe-setup/)).

## Usage

1. Open the page, click **Connect J-Link…** and pick the J-Link.
2. Choose the image: **Release** (the latest CI build bundled with the page, preselected) or **Own .hex file** (an Intel hex built for `nrf9151dk/nrf9151/ns`, e.g. `app/build/merged.hex`). Optionally fill in the token, custom id and platform host (see below): with a token, the board registers itself on its next boot.
3. **Flash**: it erases the sectors the image covers, programs, verifies and resets the board.

## nuSIM safety

- `src/lib.rs` parses the image and **refuses any byte at or above `0xF8000`**. That covers the nuSIM (TF-M ITS at `0xF8000–0x100000`) and the UICR (`0xFF8000`), which holds the keys the ITS is encrypted with. Unit test: `cargo test`.
- probe-rs is attached without the erase-all permission, so it refuses ERASEALL (the only way to clear UICR on nRF91) instead of running it. It erases only the 4 KB sectors the image covers, then verifies.
- Tested on the workshop DK: a release image flashed from the browser; afterwards ICCID, `%XSIM: 1` and LTE registration were unchanged.

Images for boards without nuSIM that use the last 32 KB of flash are refused too. That's deliberate.

## How it works

```
index.html ── WebUSB (claim J-Link vendor interface) ──┐
   │ WASI shim (clock, sleep, stderr → log)             │ bulk_in / bulk_out
   ▼                                                    ▼  (WebAssembly.Suspending)
webflash.wasm (wasm32-wasip1): parse + guard hex → probe-rs JLink::from_transport → attach nRF9151_xxAA → erase/program/verify → reset
```

probe-rs is synchronous and WebUSB is async. JSPI (`WebAssembly.Suspending` / `promising`) suspends the wasm stack while a USB transfer or a sleep is pending, so probe-rs runs unmodified apart from its USB transport. Targeting `wasm32-wasip1` instead of `wasm32-unknown-unknown` keeps `std::time::Instant` and `thread::sleep` working through a few WASI calls in the page.

## Vendored probe-rs

`vendor/probe-rs` is probe-rs **0.32.0** from crates.io. The git history has two commits: the unmodified copy, then our changes. `git show` of the second one is the patch to propose upstream:

- `JLink::from_transport()`: build a J-Link probe from an already opened USB interface (any `InterfaceExt` implementation). `JLinkFactory::open()` uses it with nusb; here the page provides WebUSB.
- `cfg(target_family = "wasm")` gates: only the J-Link driver is compiled (the other probe drivers need serial ports, hidapi or blocking nusb), no USB enumeration, and no WCH-Link chip detection. `async-io` and `serialport` are native-only dependencies, and nusb goes from 0.2.1 to 0.2.7 (which has a WebUSB backend).

Once upstream probe-rs has this, drop `vendor/` and the `[patch.crates-io]` entry.

## Build and run from source

```bash
rustup target add wasm32-wasip1
cargo test --release                                   # nuSIM guard
cargo build --release --target wasm32-wasip1           # .cargo/config.toml sets web_sys_unstable_apis
mkdir -p /tmp/site && cp www/index.html target/wasm32-wasip1/release/webflash.wasm /tmp/site/
cp workshop.hex /tmp/site/ && echo '{"tag":"local","file":"workshop.hex"}' > /tmp/site/release.json
python3 -m http.server -d /tmp/site 8765                # WebUSB needs a secure context: localhost is one
```

## Settings: token, custom id, platform host

The page has three optional fields: **Exein Runtime token**, **Custom id** and **Platform host**. If any is set, it also writes a **settings page** at `0x80000` (start of the unused MCUboot secondary slot): `"EXEINCFG"`, then token, custom id and host as NUL-terminated strings (empty = keep the firmware's value). The signed app image can't carry them, because MCUboot checks its signature on every boot.

The workshop firmware reads that page on every boot:
- **Custom id:** sent in SystemInfo instead of the default `nRF9151-DK_<device id>`.
- **Platform host:** used for registration (`:8004`) and telemetry (`:8003`) instead of the built-in `platform.exein.io`. A hostname or IPv4 address, no scheme or port.
- **Token, no identity in the modem yet:** registers with it, stores the identity in the modem, then **clears the token** from the page after this one attempt (custom id and host stay). The platform consumes a token even when the device fails to read the reply, so retrying would only leave orphaned agents.
- **Token, identity already there:** ignored and cleared.

Flashing again without any field set leaves the page as it is; flashing with settings replaces it.

**Tested end to end on prod (2026-10-13):** a release hex + token from this page; the board registered with platform.exein.io, and Button 2 produced a HEAP_LOW record accepted 5.7 s after the press. Before runtime-platform#1539 reached prod, `/register` failed on-device with `-122` (EMSGSIZE).

## Serial terminal

**Open serial port…** connects to the board's UART through the J-Link's virtual COM port (Web Serial, 115200, DTR asserted) while the board runs normally, also during and after flashing. You get the live log and, with the workshop firmware, AT commands (`AT%XICCID`, `AT+CEREG?`). Pick the J-Link's first port.
