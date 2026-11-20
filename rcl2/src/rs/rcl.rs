//! rcl2 - Linux init, Rust core (`#![no_std]`, zero dependencies).
//!
//! This crate is responsible for *parsing*: service config, kernel cmdline
//! flags and /proc/self/statm. It never allocates, never unwraps, never
//! panics on malformed input - every result writes bounded, fixed-size data
//! into the pinned `#[repr(C)]` ABI declared in `src/rcl.h`. Process control
//! (fork/exec/signal/syscalls) stays in the C core; the two halves share one
//! binary and one static layout asserted on both sides.
//!
//! Build: `rustc --edition 2021 --crate-type staticlib` (see Makefile).

#![no_std]

use core::slice;

/* ---------------------------------------------------------------- ABI ---- */

pub const RCL_MAX_SVCS: usize = 16;
pub const RCL_MAX_ARGS: usize = 6;
pub const RCL_MAX_ENV: usize = 8;
pub const RCL_NAME_LEN: usize = 24;
pub const RCL_PATH_LEN: usize = 96;
pub const RCL_ARG_LEN: usize = 64;
pub const RCL_ENV_LEN: usize = 64;
pub const RCL_HOST_LEN: usize = 64;

pub const RCL_MODE_ONCE: u8 = 0;
pub const RCL_MODE_RESPAWN: u8 = 1;
pub const RCL_MODE_WAIT: u8 = 2;

pub const RCL_CAD_REBOOT: u8 = 0;
pub const RCL_CAD_POWEROFF: u8 = 1;

pub const BOOTFLAG_RESCUE: u32 = 1;

#[repr(C)]
pub struct Svc {
    pub name: [u8; RCL_NAME_LEN],
    pub path: [u8; RCL_PATH_LEN],
    pub args: [[u8; RCL_ARG_LEN]; RCL_MAX_ARGS],
    pub nargs: u8,
    pub mode: u8,
    pub flags: u8,
}
#[repr(C)]
pub struct Conf {
    pub nsvc: u8,
    pub nenv: u8,
    pub klog: u8,
    pub cad: u8,
    pub hostname: [u8; RCL_HOST_LEN],
    pub env: [[u8; RCL_ENV_LEN]; RCL_MAX_ENV],
    pub svcs: [Svc; RCL_MAX_SVCS],
}

/* layout must stay identical to C (src/rcl.h static asserts) */
const _: () = assert!(core::mem::size_of::<Svc>() == 507);
const _: () = assert!(core::mem::size_of::<Conf>() == 8692);
const _: () = assert!(core::mem::offset_of!(Svc, nargs) == 504);
const _: () = assert!(core::mem::offset_of!(Svc, mode) == 505);
const _: () = assert!(core::mem::offset_of!(Conf, hostname) == 4);
const _: () = assert!(core::mem::offset_of!(Conf, env) == 68);
const _: () = assert!(core::mem::offset_of!(Conf, svcs) == 580);

/* -------------------------------------------------------------- helpers -- */

fn is_ws(b: u8) -> bool {
    b == b' ' || b == b'\t' || b == b'\r' || b == 0x0B || b == 0x0C
}

/// Cut a line [start, len) ending at '\n'; the given cursor is advanced past it.
fn line_end(s: &[u8], i: usize) -> (usize, usize) {
    let mut j = i;
    while j < s.len() && s[j] != b'\n' {
        j += 1;
    }
    let next = if j < s.len() { j + 1 } else { s.len() };
    (j, next)
}

/// tokenize one word from `s`, starting at *i; None past comment/EOF
fn next_token<'a>(s: &'a [u8], i: &mut usize) -> Option<&'a [u8]> {
    while *i < s.len() && is_ws(s[*i]) {
        *i += 1;
    }
    if *i >= s.len() || s[*i] == b'#' {
        return None;
    }
    let start = *i;
    while *i < s.len() && !is_ws(s[*i]) {
        *i += 1;
    }
    s.get(start..*i)
}

struct Toks<'a> {
    s: &'a [u8],
    i: usize,
    done: bool,
}
impl<'a> Toks<'a> {
    fn new(s: &'a [u8]) -> Self {
        Toks { s, i: 0, done: false }
    }
    fn next(&mut self) -> Option<&'a [u8]> {
        if self.done {
            return None;
        }
        let t = next_token(self.s, &mut self.i);
        if t.is_none() {
            self.done = true;
        }
        t
    }
}

/// NUL-terminated copy with truncation, strlcpy without libc
fn puts(dst: &mut [u8], src: &[u8]) {
    if dst.is_empty() {
        return;
    }
    let cap = dst.len() - 1;
    let m = if src.len() < cap { src.len() } else { cap };
    let mut k = 0usize;
    while k < m {
        if let (Some(d), Some(&b)) = (dst.get_mut(k), src.get(k)) {
            *d = b;
        }
        k += 1;
    }
    if let Some(d) = dst.get_mut(m) {
        *d = 0;
    }
}

/* ---------------------------------------------------------- directive fns */

/// "service NAME respawn|once|wait PATH [ARG...]"; returns true when a slot
/// was filled. Unknown modes and too-long paths are skipped without error.
fn parse_service(conf: &mut Conf, idx: usize, s: &[u8]) -> bool {
    if idx >= RCL_MAX_SVCS {
        return false;
    }
    let mut t = Toks::new(s);
    let _directive = t.next();          /* "service" */
    let name = t.next();
    let mtok = t.next();
    let path = t.next();
    let (name, mtok, path) = match (name, mtok, path) {
        (Some(n), Some(m), Some(p)) if !n.is_empty() => (n, m, p),
        _ => return false,
    };

    let mode = if mtok == b"respawn" {
        RCL_MODE_RESPAWN
    } else if mtok == b"once" {
        RCL_MODE_ONCE
    } else if mtok == b"wait" {
        RCL_MODE_WAIT
    } else {
        return false;
    };

    if path.len() > RCL_PATH_LEN - 1 {
        return false;               /* a truncated path would exec something else */
    }

    let svc = match conf.svcs.get_mut(idx) {
        Some(s) => s,
        None => return false,
    };
    puts(&mut svc.name, name);
    puts(&mut svc.path, path);
    let mut nargs = 0usize;
    for k in 0..RCL_MAX_ARGS {
        match t.next() {
            Some(a) => {
                if let Some(dst_arg) = svc.args.get_mut(k) {
                    puts(dst_arg, a);
                    nargs += 1;
                }
            }
            None => break,
        }
    }
    svc.nargs = nargs as u8;
    svc.mode = mode;
    svc.flags = 0;
    conf.nsvc = (idx + 1) as u8;
    true
}

fn parse_hostname(conf: &mut Conf, s: &[u8]) {
    let mut t = Toks::new(s);
    let _directive = t.next();
    if let Some(h) = t.next() {
        puts(&mut conf.hostname, h);
    }
}

fn parse_klog(conf: &mut Conf, s: &[u8]) {
    let mut t = Toks::new(s);
    let _directive = t.next();
    if let Some(n) = t.next() {
        if let Some(v) = parse_u8_token(n) {
            /* console loglevel sanity: 0..7, 0 means "kernel default" */
            conf.klog = if v > 7 { 7 } else { v };
        }
    }
}

/// "setenv KEY=VALUE": full pair must fit one 64B cell
fn parse_setenv(conf: &mut Conf, s: &[u8]) {
    let mut t = Toks::new(s);
    let _directive = t.next();
    let kv = match t.next() {
        Some(k) => k,
        None => return,
    };
    let mut p: Option<usize> = None;
    for k in 0..kv.len() {
        if kv[k] == b'=' {
            p = Some(k);
            break;
        }
    }
    match p {
        Some(pos) if pos > 0 && pos + 1 < kv.len() => {}
        _ => return,
    }
    if kv.len() > RCL_ENV_LEN - 1 || conf.nenv as usize >= RCL_MAX_ENV {
        return;
    }
    let n = conf.nenv as usize;
    if let Some(slot) = conf.env.get_mut(n) {
        puts(slot, kv);
        conf.nenv = (n + 1) as u8;
    }
}

fn parse_cad(conf: &mut Conf, s: &[u8]) {
    let mut t = Toks::new(s);
    let _directive = t.next();
    if let Some(w) = t.next() {
        if w == b"poweroff" {
            conf.cad = RCL_CAD_POWEROFF;
        } else if w == b"reboot" {
            conf.cad = RCL_CAD_REBOOT;
        }
    }
}

fn parse_u8_token(tok: &[u8]) -> Option<u8> {
    if tok.is_empty() || tok.len() > 3 {
        return None;
    }
    let mut v: u32 = 0;
    for &b in tok {
        if !b.is_ascii_digit() {
            return None;
        }
        v = v * 10 + (b - b'0') as u32;
    }
    if v > 255 {
        return None;
    }
    Some(v as u8)
}

/* -------------------------------------------------------- rcl_rs_parse --- */

/// parse whole config text; returns number of service slots filled.
/// Never aborts: bad lines are skipped, later lines still load.
#[no_mangle]
pub extern "C" fn rcl_rs_parse(c: *mut Conf, buf: *const u8, len: usize) -> i32 {
    if c.is_null() {
        return -1;
    }
    if (buf.is_null() && len > 0) || len > (1 << 20) {
        return -1;
    }
    let conf = unsafe { &mut *c };
    let s: &[u8] = if len == 0 { &[] } else { unsafe { slice::from_raw_parts(buf, len) } };

    let mut idx = 0usize;
    let mut i = 0usize;
    while i < s.len() {
        let from = i;
        let (le, next) = line_end(s, i);
        i = next;
        let line = match s.get(from..le) {
            Some(l) => l,
            None => continue,
        };

        let mut t = Toks::new(line);
        let d = match t.next() {
            Some(d) => d,
            None => continue,        /* blank or comment line */
        };

        if d == b"service" {
            if parse_service(conf, idx, line) {
                idx += 1;
            }
        } else if d == b"hostname" {
            parse_hostname(conf, line);
        } else if d == b"klog" {
            parse_klog(conf, line);
        } else if d == b"setenv" {
            parse_setenv(conf, line);
        } else if d == b"ctrlaltdel" {
            parse_cad(conf, line);
        }
        /* unknown directives are skipped silently */
    }
    idx as i32
}

/* ----------------------------------------------------- rcl_rs_bootflags -- */

/// scan /proc/cmdline tokens for rescue/single-user markers
#[no_mangle]
pub extern "C" fn rcl_rs_bootflags(buf: *const u8, len: usize) -> u32 {
    if (buf.is_null() && len > 0) || len > (1 << 20) {
        return 0;
    }
    let s: &[u8] = if len == 0 { &[] } else { unsafe { slice::from_raw_parts(buf, len) } };
    let mut flags = 0u32;
    let mut t = Toks::new(s);
    while let Some(tok) = t.next() {
        if tok == b"single" || tok == b"s" || tok == b"-s"
            || tok == b"emergency" || tok == b"-b" || tok == b"l2.rescue=1"
        {
            flags |= BOOTFLAG_RESCUE;
        }
    }
    flags
}

/* -------------------------------------------------------- rcl_rs_statm --- */

/// /proc/self/statm: "total resident shared text lib data dt";
/// returns the resident field (pages); 0 when unparsable
#[no_mangle]
pub extern "C" fn rcl_rs_statm(buf: *const u8, len: usize) -> u64 {
    if (buf.is_null() && len > 0) || len == 0 || len > (1 << 16) {
        return 0;
    }
    let s: &[u8] = unsafe { slice::from_raw_parts(buf, len) };
    let mut t = Toks::new(s);
    let _total = t.next();
    let res = match t.next() {
        Some(r) => r,
        None => return 0,
    };
    if res.len() > 20 {
        return 0;
    }
    let mut v: u64 = 0;
    for &b in res {
        if !b.is_ascii_digit() {
            return 0;
        }
        v = v * 10 + (b - b'0') as u64;
    }
    v
}

/* ------------------------------------------------- banner / panic hooks -- */

const BANNER: &[u8] = b"rcl2 1.0 (pid1: c core + rust core, zero deps)";
const VERSION: &[u8; 9] = b"rcl2 1.0\0";

/// copy banner into a C buffer (truncation-safe); returns copied length
#[no_mangle]
pub extern "C" fn rcl_rs_banner(out: *mut u8, cap: usize) -> usize {
    if out.is_null() || cap == 0 {
        return 0;
    }
    let dst = unsafe { slice::from_raw_parts_mut(out, cap) };
    let max = cap - 1;
    let m = if BANNER.len() < max { BANNER.len() } else { max };
    puts(dst, BANNER);
    m
}

/// NUL-terminated version string for userland tools
#[no_mangle]
pub extern "C" fn rcl_rs_version() -> *const i8 {
    VERSION.as_ptr() as *const i8
}

/// last resort: rust panics must never kill pid 1; the C core logs the msg
/// to the console and idles forever
#[panic_handler]
fn h(_info: &core::panic::PanicInfo) -> ! {
    extern "C" {
        fn rcl_c_panic(msg: *const u8, len: usize);
    }
    const M: &[u8] = b"rust core panic";
    unsafe { rcl_c_panic(M.as_ptr(), M.len()) };
    loop {
        core::hint::spin_loop();
    }
}
