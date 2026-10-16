//! Browser flasher for the nRF9151 DK: probe-rs drives the onboard J-Link
//! over WebUSB.
//!
//! Built for `wasm32-wasip1`; `www/index.html` provides the WASI basics and
//! the WebUSB transport. probe-rs is synchronous while WebUSB is async, so
//! the two USB imports are wrapped in `WebAssembly.Suspending` (JSPI): the
//! wasm stack pauses until the transfer's promise settles.
//!
//! nuSIM safety: the image is parsed here and refused unless every byte lies
//! in 0x0..0xF8000. The nuSIM lives in TF-M ITS at 0xF8000..0x100000 and its
//! keys in UICR (0xFF8000), which only an ERASEALL can clear; probe-rs is
//! attached without the erase-all permission and erases 4 KB sectors only.

use std::io;
use std::time::Duration;

use probe_rs::flashing::{DownloadOptions, FlashProgress, ProgressEvent};
use probe_rs::probe::jlink::JLink;
use probe_rs::probe::usb_util::InterfaceExt;
use probe_rs::probe::{DebugProbeSelector, Probe};
use probe_rs::Permissions;

const CHIP: &str = "nRF9151_xxAA";
/// First byte of the nuSIM / TF-M ITS region; nothing may be written from here on.
const NUSIM_START: u32 = 0xF8000;
/// Settings page for runtime-lte (start of the unused MCUboot secondary slot):
/// "EXEINCFG" + token, custom id, host as NUL-terminated strings (empty =
/// keep the firmware's value). The firmware clears the token after one use.
const CFG_PAGE: u32 = 0x80000;
const CFG_MAGIC: &[u8] = b"EXEINCFG";
const TOKEN_MAX: usize = 128;
const CUSTOM_ID_MAX: usize = 64;
const HOST_MAX: usize = 64;
/// SWD clock; probe-rs' default for J-Link is far slower.
const SWD_KHZ: u32 = 4000;

#[link(wasm_import_module = "webusb")]
extern "C" {
    /// Bulk OUT on the claimed J-Link interface. Returns bytes written or < 0.
    fn bulk_out(ep: u32, ptr: *const u8, len: u32, timeout_ms: u32) -> i32;
    /// Bulk IN; returns bytes read or < 0.
    fn bulk_in(ep: u32, ptr: *mut u8, len: u32, timeout_ms: u32) -> i32;
    /// Progress: kind 0 = erase, 1 = program, 2 = verify; done/total in bytes.
    fn progress(kind: u32, done: u32, total: u32);
}

/// The J-Link's vendor interface, opened and claimed by the page.
struct WebUsb;

impl InterfaceExt for WebUsb {
    fn write_bulk(&self, endpoint: u8, buf: &[u8], timeout: Duration) -> io::Result<usize> {
        let n = unsafe { bulk_out(endpoint as u32, buf.as_ptr(), buf.len() as u32, timeout.as_millis() as u32) };
        usb_result(n)
    }

    fn read_bulk(&self, endpoint: u8, buf: &mut [u8], timeout: Duration) -> io::Result<usize> {
        let n = unsafe { bulk_in(endpoint as u32, buf.as_mut_ptr(), buf.len() as u32, timeout.as_millis() as u32) };
        usb_result(n)
    }
}

fn usb_result(n: i32) -> io::Result<usize> {
    match n {
        n if n >= 0 => Ok(n as usize),
        -2 => Err(io::Error::from(io::ErrorKind::TimedOut)),
        _ => Err(io::Error::other("WebUSB transfer failed")),
    }
}

/// Parses an Intel hex into (address, bytes) segments and enforces the
/// nuSIM rule. Errors name the first offending address.
fn parse_hex(text: &str) -> Result<Vec<(u32, Vec<u8>)>, String> {
    let mut base = 0u32;
    let mut segs: Vec<(u32, Vec<u8>)> = Vec::new();
    for rec in ihex::Reader::new(text) {
        match rec.map_err(|e| format!("invalid hex: {e}"))? {
            ihex::Record::ExtendedLinearAddress(hi) => base = (hi as u32) << 16,
            ihex::Record::ExtendedSegmentAddress(seg) => base = (seg as u32) << 4,
            ihex::Record::Data { offset, value } => {
                let addr = base + offset as u32;
                let end = addr as u64 + value.len() as u64;
                if end > NUSIM_START as u64 {
                    return Err(format!(
                        "image writes 0x{addr:08X}: only 0x0-0x{NUSIM_START:X} is allowed (nuSIM region / UICR)"
                    ));
                }
                match segs.last_mut() {
                    Some((a, d)) if *a + d.len() as u32 == addr => d.extend_from_slice(&value),
                    _ => segs.push((addr, value)),
                }
            }
            _ => {}
        }
    }
    if segs.is_empty() {
        return Err("image contains no data".into());
    }
    Ok(segs)
}

/// One settings field: at most `max` printable ASCII characters, no spaces.
fn cfg_field(name: &str, v: &str, max: usize) -> Result<Vec<u8>, String> {
    let v = v.trim();
    if v.len() > max || !v.bytes().all(|b| (0x21..=0x7e).contains(&b)) {
        return Err(format!("{name} must be up to {max} printable characters without spaces"));
    }
    let mut f = v.as_bytes().to_vec();
    f.push(0);
    Ok(f)
}

/// The settings page, or None when every field is empty.
fn cfg_page(token: &str, custom_id: &str, host: &str) -> Result<Option<Vec<u8>>, String> {
    if token.trim().is_empty() && custom_id.trim().is_empty() && host.trim().is_empty() {
        return Ok(None);
    }
    if !host.trim().bytes().all(|b| b.is_ascii_alphanumeric() || b == b'.' || b == b'-') {
        return Err("host must be a plain hostname or IPv4 address (no scheme or port)".into());
    }
    let mut page = CFG_MAGIC.to_vec();
    page.extend(cfg_field("token", token, TOKEN_MAX)?);
    page.extend(cfg_field("custom id", custom_id, CUSTOM_ID_MAX)?);
    page.extend(cfg_field("host", host, HOST_MAX)?);
    Ok(Some(page))
}

fn flash_image(hex: &str, cfg: Option<Vec<u8>>, vid: u16, pid: u16, serial: Option<String>, read_ep: u8, write_ep: u8, max_packet: usize) -> Result<(), String> {
    let mut segs = parse_hex(hex)?;
    if let Some(page) = cfg {
        if segs.iter().any(|(a, d)| *a < CFG_PAGE + 0x1000 && *a + d.len() as u32 > CFG_PAGE) {
            return Err(format!("image already uses the settings page at 0x{CFG_PAGE:X}"));
        }
        segs.push((CFG_PAGE, page));
        tracing::info!("settings page at 0x{CFG_PAGE:X} (token / custom id / host)");
    }
    let total: usize = segs.iter().map(|(_, d)| d.len()).sum();
    tracing::info!("image: {} segment(s), {} bytes, all below 0x{NUSIM_START:X}", segs.len(), total);

    let selector = DebugProbeSelector { vendor_id: vid, product_id: pid, interface: None, serial_number: serial };
    let jlink = JLink::from_transport(Box::new(WebUsb), read_ep, write_ep, max_packet, &selector)
        .map_err(|e| format!("J-Link: {e}"))?;
    let mut probe = Probe::from_specific_probe(jlink);
    if let Err(e) = probe.set_speed(SWD_KHZ) {
        tracing::warn!("could not set SWD speed to {SWD_KHZ} kHz: {e}");
    }
    // Permissions::new() = no erase-all: probe-rs refuses ERASEALL instead of wiping UICR.
    let mut session = probe.attach(CHIP, Permissions::new()).map_err(|e| format!("attach: {e}"))?;
    tracing::info!("attached to {CHIP}");

    let mut loader = session.target().flash_loader();
    for (addr, data) in &segs {
        loader.add_data(*addr as u64, data).map_err(|e| format!("loader: {e}"))?;
    }

    let mut opts = DownloadOptions::default();
    opts.verify = true;
    let (mut erased, mut programmed, mut verified) = (0u64, 0u64, 0u64);
    let (mut erase_total, mut program_total, mut verify_total) = (0u64, 0u64, 0u64);
    opts.progress = FlashProgress::new(move |ev| unsafe {
        match ev {
            ProgressEvent::AddProgressBar { operation, total } => match format!("{operation:?}").as_str() {
                "Erase" => erase_total += total.unwrap_or(0),
                "Program" => program_total += total.unwrap_or(0),
                "Verify" => verify_total += total.unwrap_or(0),
                _ => {}
            },
            ProgressEvent::Progress { operation, size, .. } => match format!("{operation:?}").as_str() {
                "Erase" => { erased += size; progress(0, erased as u32, erase_total as u32) }
                "Program" => { programmed += size; progress(1, programmed as u32, program_total as u32) }
                "Verify" => { verified += size; progress(2, verified as u32, verify_total as u32) }
                _ => {}
            },
            _ => {}
        }
    });
    loader.commit(&mut session, opts).map_err(|e| format!("flash: {e}"))?;
    tracing::info!("flashed and verified; resetting");

    session.core(0).and_then(|mut c| c.reset()).map_err(|e| format!("reset: {e}"))?;
    Ok(())
}

static LAST_ERROR: std::sync::Mutex<String> = std::sync::Mutex::new(String::new());

/// Buffer for the page to copy the hex into.
#[no_mangle]
pub extern "C" fn alloc(len: u32) -> *mut u8 {
    let mut v = Vec::<u8>::with_capacity(len as usize);
    let p = v.as_mut_ptr();
    std::mem::forget(v);
    p
}

/// Flashes the Intel hex at `ptr..ptr+len` through the opened J-Link.
/// Returns 0 on success; otherwise `last_error_ptr/len` describe the error.
#[no_mangle]
pub extern "C" fn flash(
    ptr: *mut u8, len: u32, vid: u32, pid: u32,
    serial_ptr: *const u8, serial_len: u32,
    token_ptr: *const u8, token_len: u32,
    custom_id_ptr: *const u8, custom_id_len: u32,
    host_ptr: *const u8, host_len: u32,
    read_ep: u32, write_ep: u32, max_packet: u32, verbose: u32,
) -> i32 {
    let _ = tracing_subscriber::fmt()
        .with_max_level(if verbose != 0 { tracing::Level::DEBUG } else { tracing::Level::INFO })
        .with_writer(io::stderr)
        .without_time()
        .with_ansi(false)
        .try_init();

    let hex = unsafe { Vec::from_raw_parts(ptr, len as usize, len as usize) };
    let serial = (serial_len > 0).then(|| unsafe {
        String::from_utf8_lossy(std::slice::from_raw_parts(serial_ptr, serial_len as usize)).into_owned()
    });
    let text = |p: *const u8, n: u32| if n == 0 { String::new() } else {
        unsafe { String::from_utf8_lossy(std::slice::from_raw_parts(p, n as usize)).into_owned() }
    };
    let cfg = cfg_page(&text(token_ptr, token_len), &text(custom_id_ptr, custom_id_len), &text(host_ptr, host_len));
    let result = match (std::str::from_utf8(&hex), cfg) {
        (_, Err(e)) => Err(e),
        (Ok(text), Ok(cfg)) => flash_image(text, cfg, vid as u16, pid as u16, serial, read_ep as u8, write_ep as u8, max_packet as usize),
        (Err(_), _) => Err("image is not a text Intel hex".into()),
    };
    match result {
        Ok(()) => 0,
        Err(e) => {
            tracing::error!("{e}");
            *LAST_ERROR.lock().unwrap() = e;
            -1
        }
    }
}

#[no_mangle]
pub extern "C" fn last_error_ptr() -> *const u8 {
    LAST_ERROR.lock().unwrap().as_ptr()
}

#[no_mangle]
pub extern "C" fn last_error_len() -> u32 {
    LAST_ERROR.lock().unwrap().len() as u32
}

#[cfg(test)]
mod tests {
    use super::parse_hex;

    #[test]
    fn nusim_guard() {
        // 4 bytes at 0x10000: fine.
        assert!(parse_hex(":020000040001F9\n:0400000001020304F2\n:00000001FF\n").is_ok());
        // 4 bytes at 0xF8000 (ext. linear 0x000F, offset 0x8000): refused.
        assert!(parse_hex(":02000004000FEB\n:048000000102030472\n:00000001FF\n").is_err());
        // Last byte crossing into 0xF8000: refused.
        assert!(parse_hex(":02000004000FEB\n:047FFE000102030475\n:00000001FF\n").is_err());
        // UICR (0xFF8000): refused.
        assert!(parse_hex(":0200000400FFFB\n:048000000102030472\n:00000001FF\n").is_err());
    }

    #[test]
    fn settings() {
        use super::cfg_page;
        assert_eq!(cfg_page("", " ", "").unwrap(), None);
        assert_eq!(cfg_page(" abc.DEF-123 ", "", "").unwrap().unwrap(), b"EXEINCFGabc.DEF-123\0\0\0");
        assert_eq!(cfg_page("", "dk_1", "platform.exein.live").unwrap().unwrap(),
                   b"EXEINCFG\0dk_1\0platform.exein.live\0");
        assert!(cfg_page("has space", "", "").is_err());
        assert!(cfg_page(&"x".repeat(129), "", "").is_err());
        assert!(cfg_page("", &"x".repeat(65), "").is_err());
        assert!(cfg_page("", "", "https://platform.exein.live").is_err());
        assert!(cfg_page("", "", "host:8004").is_err());
    }
}
