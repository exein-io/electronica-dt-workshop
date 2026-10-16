# Electronica DT workshop: heap exhaustion on nRF9151 + nuSIM

A joint workshop by [Exein](https://www.exein.io) and [Deutsche Telekom IoT](https://iot.telekom.com) at Electronica. The Exein Runtime Security Monitor runs on a Nordic nRF9151 DK with Deutsche Telekom's integrated [nuSIM](https://iot.telekom.com/global/iot-connectivity/isim). Press **Button 2**, the device burst-allocates past 60% of its heap, the monitor reports `heap_low` over LTE, and a **Heap Exhaustion** incident shows up on [runtime.exein.io](https://runtime.exein.io).

```
Button 2 ─▶ heap burst ─▶ HEAP_LOW policy ─▶ POST /zephyr/threats (modem TLS, LTE-M) ─▶ incident
```

**Try it from your browser:** [**exein-io.github.io/electronica-dt-workshop**](https://exein-io.github.io/electronica-dt-workshop/) flashes the latest release onto the DK over USB (Chrome/Edge on Linux, macOS or Android, nothing to install, nuSIM-safe), with a serial terminal and the registration token field. On Windows, or without the browser: [Flash a release with nrfutil](#b-with-nrfutil-windows-macos-linux).

---

## Hardware

| Item | Value |
|------|-------|
| Board | Nordic nRF9151 DK (PCA10171) |
| IMEI / ICCID | per board: `AT+CGSN` / `AT%XICCID` in the serial terminal |
| Modem FW | `mfw_nrf91x1_2.0.1` |
| J-Link S/N | per board: on the DK's sticker, or `nrfutil device list` |
| Operator | Telekom Deutschland (MCC 262, MNC 01), LTE-M |

---

## CRITICAL: nuSIM safety rules

The nuSIM credentials live in TF-M ITS flash at `0xF8000–0xFDFFF`, encrypted with keys in UICR (`0xFF8000`). **Erasing or overwriting that region destroys the SIM permanently.**

These destroy the nuSIM:
- `west flash --erase`, full chip / mass erase, `nrfjprog --eraseall`
- nRF Connect for Desktop → Programmer or Quick Start (they erase by default)
- **A different TF-M**: standard NCS TF-M finds the Redtea ITS format incompatible and silently reformats it on first boot. `pm_static.yml` does not prevent that. Only use the Redtea TF-M from `ncs-nusim-app` `ncs-v3.1.0` (proven compatible with the factory ITS).
- Any hex with data at or above `0xF8000` (TF-M storage `0xF8000–0xFFFFF`, UICR)

Before every flash:
```bash
scripts/check-nusim-hex.py app/build/merged.hex   # exits 1 on DANGER
```
A safe `west flash` logs `Erasing address ranges touched by firmware` (sector erase of the hex regions only).

`app/pm_static.yml` pins `tfm_its` at `0xf8000` (not `0xff8000`: that is UICR, outside the 1 MB flash).

---

## Setup

| Tool | Version |
|------|---------|
| NCS | **v3.1.0** (pinned by `ncs-nusim-app`; newer TF-M would wipe the nuSIM) |
| Zephyr SDK | 0.17.4 |
| west | ≥ 1.4 |
| nrfutil | ≥ 8 with the `device` command (`nrfutil install device`); `west flash` uses it |
| SEGGER J-Link | ≥ V9.52 (device `NRF9151_XXCA`) |
| pyserial | any |

### Codespaces / devcontainer (nothing to install)

**Code → Codespaces → Create codespace on main** (or *Reopen in Container* in VS Code). `.devcontainer/` uses Nordic's `ghcr.io/nrfconnect/sdk-nrf-toolchain:v3.1.0` image and sets up the workspace in `/workspaces` from the vendored snapshot (a few minutes on first start). Then, in the terminal:
```bash
scripts/build.sh                                   # → app/build/merged.hex
HEX=app/build/merged.hex webflash/serve.sh         # flash it from your browser (WebUSB)
```
USB isn't passed into the container: the forwarded port 8765 opens the web flasher in your local Chrome/Edge, which talks to the board. Or download `merged.hex` (right click → Download) and flash it with [the web flasher](https://exein-io.github.io/electronica-dt-workshop/) (**Own .hex file**) or with nrfutil as in [Flash a release](#b-with-nrfutil-windows-macos-linux).

### Workspace

The west workspace is built from `vendor/ncs-nusim-app-ncs-v3.1.0-52f7cd3.zip`, a snapshot of `redteamobile/ncs-nusim-app` `ncs-v3.1.0` (NCS v3.1.0 + the Redtea nuSIM TF-M); `scripts/build.sh` adds `runtime-zephyr/` (the binary monitor module). Locally, the same as the devcontainer and CI:
```bash
mkdir -p ~/git/dt-ws && cd ~/git/dt-ws
git clone https://github.com/exein-io/electronica-dt-workshop
electronica-dt-workshop/scripts/setup-workspace.sh
```

On a flaky connection: `git config --global http.version HTTP/1.1`.

---

## Flash a release

Every release has `workshop.hex` (the firmware) on the [Releases page](https://github.com/exein-io/electronica-dt-workshop/releases/latest). A **new board also needs its Exein Runtime token** (the bootstrap token from runtime.exein.io) once, as a small settings page next to the firmware; the board registers itself on its next boot. Reflashing a registered board needs no token.

### A. From the browser (Linux, macOS, Android)

Open **https://exein-io.github.io/electronica-dt-workshop/** in Chrome or Edge, connect the J-Link, enter the token (first time), click **Flash**. Nothing to install. Details: [`webflash/`](webflash/README.md).

### B. With nrfutil (Windows, macOS, Linux)

Use this on Windows (the SEGGER driver keeps WebUSB from opening the J-Link) or whenever you prefer Nordic's tools. **Never** use nRF Connect for Desktop's Programmer or Quick Start: they erase the whole chip and destroy the nuSIM.

1. Install the [SEGGER J-Link Software](https://www.segger.com/downloads/jlink/) (≥ V9.52), [nRF Util](https://www.nordicsemi.com/Products/Development-tools/nRF-Util) and Python 3, then run `nrfutil install device`.
2. Get this repo (`git clone https://github.com/exein-io/electronica-dt-workshop`, or **Code → Download ZIP**) and `workshop.hex` from the [latest release](https://github.com/exein-io/electronica-dt-workshop/releases/latest) into the repo folder.
3. In a terminal in the repo folder (PowerShell, Terminal, bash: the commands are the same; use `python` where `python3` doesn't exist):
   ```bash
   python3 scripts/check-nusim-hex.py workshop.hex                       # must print SAFE
   python3 scripts/settings-hex.py --token <bootstrap token> -o settings.hex   # first time only; optional --custom-id, --host
   nrfutil device program --firmware workshop.hex --options chip_erase_mode=ERASE_RANGES_TOUCHED_BY_FIRMWARE
   nrfutil device program --firmware settings.hex --options chip_erase_mode=ERASE_RANGES_TOUCHED_BY_FIRMWARE
   nrfutil device reset
   ```
   `ERASE_RANGES_TOUCHED_BY_FIRMWARE` is mandatory: never a full erase. With several boards connected, add `--serial-number <J-Link S/N>` (`nrfutil device list`).
4. Watch the log in a serial terminal that asserts DTR (115200 baud, the J-Link's first COM port / tty; e.g. PuTTY, or `python3 -m serial.tools.miniterm --dtr 1 <port> 115200`): a new board logs `registered with the platform; token cleared`, then press **Button 2**.

`settings.hex` is the same settings page the web flasher writes (`0x80000`). The board clears the token after one registration attempt; a board that already has an identity ignores it, so reflashing never creates a new agent.

## Build and flash from source

```bash
cd ~/git/dt-ws
export ZEPHYR_SDK_INSTALL_DIR=~/zephyr-sdk-0.17.4
electronica-dt-workshop/scripts/build.sh   # west build + nuSIM check, → app/build/merged.hex
# log shows: runtime_zephyr: linking precompiled blob .../libruntime_zephyr.a

west flash -d electronica-dt-workshop/app/build   # NEVER --erase

# once per board: the token as a settings page (see Flash a release), or built in for a local build only:
#   electronica-dt-workshop/scripts/build.sh '-DCONFIG_RUNTIME_MONITOR_BOOTSTRAP_TOKEN="<bootstrap token>"'
```

`overlay-nusim.conf` is mandatory: it enables `CONFIG_LIB_NUSIM` and the Redtea TF-M. MCUboot is required by the nuSIM module (`sysbuild.conf`, `sysbuild/mcuboot/`; the signing key is Redtea's public sample key, fine for a demo, not for products).

**Registration:** the board registers itself on first boot when it has a token (from the settings page, or built in with `CONFIG_RUNTIME_MONITOR_BOOTSTRAP_TOKEN`) and stores cert and key in the modem key store (sec_tag 4242). The key store survives reflashing, so you register a board once; each registration creates a new agent. To register again, delete the identity in the serial terminal (`AT+CFUN=4`, `AT%CMNG=3,4242,1`, `AT%CMNG=3,4242,2`), then flash a new settings page. Never commit a token.

## Run the demo

UART log (Linux: `/dev/serial/by-id/usb-SEGGER_J-Link_<S/N>-if00`, 115200, DTR asserted):

```
net: L4 connected                                   ← LTE up (LED1 solid; blinking = no LTE)
time_sync: wall clock set from pool.ntp.org
registration: client identity present in the credential store
heartbeat: sending system info … POST status 204
workload: Press Button 2 for a heap exhaustion (HEAP_LOW) incident
```

Press **Button 2** (nothing else running in between):
```
workload: BTN2: burst-allocated 24 x 512 B (expect HEAP_LOW)
dispatcher: POST 1 record(s) … POST status 204
```
→ runtime.exein.io: **Heap Exhaustion**, severity high, "heap used 12 kB, threshold 10 kB" (60% of the 16 KB heap = 9831 B).

**Platform support (2026-10):** `/zephyr/threats` accepts only `heap_low`. Every other finding (`heap_drop`, `stack_low`, `log`, …) gets the **whole batch** rejected with `400 unsupported finding`, so the blob is built with `POLICY_HEAP_LOW` only.

nuSIM health check over AT (the app's AT host shares the log UART; any terminal with DTR asserted, e.g. `python3 -m serial.tools.miniterm --dtr 1 <port> 115200`): `AT%XICCID` → your nuSIM's ICCID, `AT+CEREG?` → `0,1`/`0,5`, `AT+COPS?` → `26201`.

---

## How it fits together

- **`app/src/main.c`**: `nrf_modem_lib_init()`, platform CA into the modem key store (LTE must be off), LTE via the modem's Zephyr net_if (`conn_mgr`), `runtime_zephyr_start()` (waits for IPv4, SNTP, SystemInfo), `workload_start()`. A small credential backend keeps CA + client identity in the modem; nothing goes to TF-M ITS. LED1 tracks L4 connectivity.
- **`app/src/app_workload.c`**: Button 2 → 24 × 512 B burst on the system heap → freed again.
- **`app/prj.conf`**: monitor feature set (frozen into the blob), modem TLS, platform endpoint, AT host.
- **Monitor as a binary Zephyr module:** see [How the Exein runtime library is configured](#how-the-exein-runtime-library-is-configured).
- **Rebuilding the blob** (internal; needed after changing any monitor option in `prj.conf`): build `app/` against the `exein-runtime-zephyr` sources (with a `platform-protobuf` checkout at `../protobuf`, branch `zephyr-agent-schema`, plus the issue #3 patch applied), then package it with upstream's script and commit `dist/runtime-zephyr/*` as `runtime-zephyr/`:
  ```bash
  west build -b nrf9151dk/nrf9151/ns --sysbuild -p -d dist/build electronica-dt-workshop/app \
    -- -DEXTRA_CONF_FILE=$PWD/modules/lib/redteamobile-nusim/overlay-nusim.conf \
       -DZEPHYR_EXTRA_MODULES=~/git/exein-runtime-zephyr      # sources win over the blob module
  ~/git/exein-runtime-zephyr/scripts/package_blob.sh dist/build/app dist
  ```
  `app/compat/zephyr/sys/clock.h` maps upstream's Zephyr 4.2 clock API onto POSIX for NCS v3.1 (issue #4).
- **CI** (`.github/workflows/build.yml`): same toolchain image, workspace from the vendored zip, builds `app/`, refuses images that touch the nuSIM region, uploads `workshop.hex` / `workshop-dfu.zip`. Builds and tests `webflash`. A `v*` tag publishes a GitHub Release with the firmware and the web flasher, and deploys the flasher with that firmware to GitHub Pages.

## How the Exein runtime library is configured

The library is configured in four layers. Some settings are fixed when the precompiled library (the blob) is built, others when you build the app, and others at runtime in `main.c`.

### 1. The library is a binary Zephyr module

`runtime-zephyr/` holds no sources; `scripts/build.sh` adds it with `ZEPHYR_EXTRA_MODULES`:

| File | Role |
|---|---|
| `zephyr/module.yml` | Registers the Zephyr module `runtime-zephyr` (depends on `nanopb`, `mbedtls`) and lists the blob with its sha256 |
| `zephyr/blobs/cortex-m33-tfm/libruntime_zephyr.a` | The precompiled monitor (Cortex-M33 + TF-M) |
| `CMakeLists.txt` | Builds from sources if present, otherwise links the blob and **checks its sha256** against `module.yml` |
| `Kconfig`, `lib/Kconfig` | All `CONFIG_RUNTIME_*` / `POLICY_*` / `THREAT_*` options |
| `include/runtime_zephyr.h` | The API the app calls |

The build log confirms it: `runtime_zephyr: linking precompiled blob …`. The blob version `ec8d57d-dirty` is upstream `main` ec8d57d plus the `network.c` fix from issue #3.

### 2. Fixed in the blob: the monitor block in `prj.conf`

Every option the library code itself reads is compiled into the `.a`. Changing these in `app/prj.conf` has **no effect until the blob is rebuilt**:

```conf
CONFIG_RUNTIME_MONITOR=y
CONFIG_POLICY_HEAP_LOW=y        # the only finding prod accepts
CONFIG_POLICY_P2=n              # memory-leak trend   (rejected by the platform today)
CONFIG_POLICY_STACK_LOW=n       # stack high-water    (rejected)
CONFIG_POLICY_LOG=n             # error logs → alerts (rejected)
CONFIG_THREAD_COLLECT_PERIOD_MS=2000
CONFIG_ANALYZER_PERIOD_MS=1000      # analyzer drains the event buffer every second
CONFIG_MONITOR_EVENT_BUF_SIZE=16384
CONFIG_HEAP_MSGQ_DEPTH=512          # alloc/free hook queue; the default 16 drops Button 2's burst
CONFIG_HEAP_LOW_PCT=60              # HEAP_LOW at 60 % of the 16 KB heap = 9831 B
CONFIG_THREAD_RUNTIME_STATS=y       # changes struct k_thread: must match the blob
```

How the pieces work together:
- **Collector:** samples the threads every `THREAD_COLLECT_PERIOD_MS`. A heap listener sees every `k_malloc` / `k_free` through `heap_msgq`.
- **Analyzer:** runs every `ANALYZER_PERIOD_MS` and evaluates the enabled policies.
- **Dispatcher:** batches findings and POSTs them as protobuf to `/zephyr/threats` (batch 16, flush 5 s, heartbeat 5 min).

The monitor's log level (`CONFIG_RUNTIME_MONITOR_LOG_LEVEL_DBG`) is compiled in too, and so is the **platform CA certificate** (`runtime_zephyr_ca_pem()`).

### 3. Read when the app is built: Zephyr support and the platform connection

These options are also in `prj.conf`, but they are read when the **app** is built, so changing them needs no new blob:
- **What the monitor needs from Zephyr:** zbus with static buffers (alert transport); networking through the modem's Zephyr net_if + `conn_mgr` (the monitor waits for the `L4 connected` event); modem TLS (`RUNTIME_MONITOR_TLS=y`, `NET_SOCKETS_SOCKOPT_TLS=n`, `MODEM_KEY_MGMT`); `POSIX_TIMERS` for the wall clock (Zephyr 4.1 shim, issue #4).
- **The platform connection**, read by the `RUNTIME_ZEPHYR_CONNECTION_INIT` macro:
  ```conf
  CONFIG_THREAT_HTTP_DEST_ADDR="platform.exein.io"        # incidents + heartbeats
  CONFIG_THREAT_HTTP_PORT=8003
  CONFIG_RUNTIME_MONITOR_REGISTRATION_ADDR="platform.exein.io"
  CONFIG_RUNTIME_MONITOR_REGISTRATION_PORT=8004
  CONFIG_RUNTIME_MONITOR_NET_CONNECT_TIMEOUT=180          # nuSIM LTE-M attach is slow
  # CONFIG_RUNTIME_MONITOR_BOOTSTRAP_TOKEN="…"            # token built in (local builds only)
  ```

### 4. Runtime: `runtime_zephyr_start()` in `src/main.c`

```c
static struct runtime_zephyr_sysinfo info = RUNTIME_ZEPHYR_SYSINFO_INIT;  // board, SoC, libc, features
info.custom_id = custom_id;            // settings page, else "nRF9151-DK_<FICR device id>" from hwinfo
struct runtime_zephyr_connection conn = RUNTIME_ZEPHYR_CONNECTION_INIT;         // from layer 3

conn.host = conn.registration_host = cfg.host;  // only if the settings page sets a host
conn.bootstrap_token = cfg.token;      // only with a settings-page token and no identity yet
conn.creds   = &creds;                 // our credential backend
conn.sec_tag = SEC_TAG;                // 4242

rc = runtime_zephyr_start(&info, &conn, NULL);  // NULL = thresholds from Kconfig (layer 2)
```

- **`info`:** goes into the SystemInfo message, which the platform shows as device details. `custom_id` is `nRF9151-DK_` plus the chip's 8-byte FICR device id (`hwinfo_get_device_id()`, logged at boot), so every workshop board shows up with a unique id without a per-board build. The web flasher's *Custom id* field overrides it.
- **`conn`:** where to connect and how to authenticate. Upstream makes this a runtime parameter on purpose, so a precompiled library contains no endpoint or token.
- **`conn.creds`:** our nRF91 backend, three functions that keep everything in the **modem key store**, so nothing is written to TF-M storage next to the nuSIM:
  - `install_ca`: writes the embedded platform CA, before LTE comes up.
  - `has_identity`: is a client key stored under sec_tag 4242?
  - `install_identity`: after a successful registration, takes LTE offline, writes cert + key, brings LTE back.
- **Third argument, thresholds:** `struct runtime_zephyr_thresholds` could override `heap_low_pct`, `stack_warn_pct` and the leak windows at runtime. `NULL` keeps the Kconfig values.

`runtime_zephyr_start()` blocks while it runs these steps:
1. Wait for IPv4 (up to 180 s).
2. Set the clock via SNTP.
3. If `has_identity()`, skip registration. Otherwise POST `/register` with the token. The token comes from the settings page or the build.
4. Send SystemInfo.
5. Start the collector, analyzer and dispatcher threads.

Then `workload_start()` arms Button 2.

### What to change where

| You want to change | Edit | Needs |
|---|---|---|
| Endpoint, ports, connect timeout | `prj.conf` connection block | App rebuild |
| Platform host, token, custom id | settings page (web flasher or `scripts/settings-hex.py`) | Nothing: flash the page |
| Policies, periods, buffer sizes | `prj.conf` monitor block | **Blob rebuild** (see "Rebuilding the blob" above), then app |
| HEAP_LOW threshold only | pass `runtime_zephyr_thresholds` to `runtime_zephyr_start()` | App rebuild |
| Where credentials are stored | `creds` backend in `main.c` | App rebuild |

`runtime-zephyr/` also contains upstream's `runtime_monitor.conf`, which is the TLS-on-app-core (mbedTLS) variant. This app doesn't use it: `prj.conf` follows upstream's `runtime_monitor_ncs.conf` (TLS in the modem).

## Troubleshooting

- **`NW reg status: 90` (UICC_FAIL):** built without the Redtea TF-M (`overlay-nusim.conf` missing), or the nuSIM ITS is erased (check `0xF8000`; restoring needs a full backup taken before, otherwise DT has to re-provision).
- **`no connectivity before registration (-116)`:** LTE attach slower than `CONFIG_RUNTIME_MONITOR_NET_CONNECT_TIMEOUT` (180 s).
- **Incidents POST `400`:** an unsupported finding in the batch; see *Platform support*.
- **No AT response:** DTR must be asserted (picocom `--noreset`, minicom and raw `stty` fail silently).

## Contacts

| Purpose | Contact |
|---------|---------|
| Exein Runtime / this workshop | thomas@exein.io |
| DT IoT support / nuSIM re-provisioning | iot-embedded-connect@telekom.de |
| nuSIM library (Redtea Mobile) | nusim@redteamobile.com |

## Links

- [nuSIM](https://iot.telekom.com/global/iot-connectivity/isim): Deutsche Telekom's integrated SIM (iSIM) for IoT
- [nuSIM explained](https://iot.telekom.com/en/blog/nusim-the-integrated-sim-for-the-internet-of-things) (Telekom IoT blog)
- [IoT Embedded Connect](https://iot.telekom.com/en/iot-connectivity/connected-iot-module): Telekom's modules with nuSIM and connectivity, including the nRF9151
- [nuSIM for the nRF91 Series](https://www.nordicsemi.com/Nordic-news/2025/10/Nordic-simplifies-cellular-IoT-connectivity-with-nuSIM-solution-for-nRF91-Series) (Nordic Semiconductor)
- [Web flasher](https://exein-io.github.io/electronica-dt-workshop/): flash the latest release from the browser
- [Exein Runtime](https://runtime.exein.io): the platform the incidents show up on

## Repo layout

```
├── app/                     # the demo app (prj.conf, pm_static.yml, sysbuild/, src/, compat/)
├── runtime-zephyr/          # the binary monitor module (blob, Kconfig, module.yml)
├── scripts/
│   ├── setup-workspace.sh   # west workspace from vendor/
│   ├── build.sh             # build app/ + nuSIM check
│   ├── settings-hex.py      # settings page (token, custom id, host) as a hex for nrfutil
│   └── check-nusim-hex.py   # refuse images that write the nuSIM region
├── vendor/                  # ncs-nusim-app ncs-v3.1.0 snapshot (the workspace manifest)
├── webflash/                # browser flasher: probe-rs → WebAssembly, WebUSB (vendored probe-rs)
├── .devcontainer/, .github/workflows/
```
