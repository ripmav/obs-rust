//! P1-33.4 (WP-34) — the portable core of `shared/obs-inject-library`.
//!
//! The reference is Windows-only (`HANDLE`/`LoadLibraryW`/`CreateRemoteThread`
//! / `SetWindowsHookEx`) and stays **linked** until `plugins/win-capture`
//! (P5-31.6/P5-31.7) is ported. What is portable — and what this module
//! unit-tests — is the **symbol resolution** (the half-byte XOR
//! de-obfuscation of the API names) and the **injection control flow**
//! (the exact error-code mapping and cleanup ordering of the reference).
//! The Windows calls are taken as injected callables, so every failure
//! branch is testable cross-platform; the real WinAPI bindings are
//! supplied by the caller at the P5-31.6/7 integration point.

use std::collections::HashMap;

/// `INJECT_ERROR_INJECT_FAILED` (inject-library.h): the remote thread ran
/// but `LoadLibraryW` returned NULL (exit code 0).
pub const INJECT_ERROR_INJECT_FAILED: i32 = -1;
/// `INJECT_ERROR_INVALID_PARAMS` (inject-library.h) — used by the
/// win-capture callers, not by the two core functions.
pub const INJECT_ERROR_INVALID_PARAMS: i32 = -2;
/// `INJECT_ERROR_OPEN_PROCESS_FAIL` (inject-library.h) — used by the
/// win-capture callers, not by the two core functions.
pub const INJECT_ERROR_OPEN_PROCESS_FAIL: i32 = -3;
/// `INJECT_ERROR_UNLIKELY_FAIL` (inject-library.h): any failure before the
/// remote thread's exit code is read.
pub const INJECT_ERROR_UNLIKELY_FAIL: i32 = -4;

/// `RETRY_INTERVAL_MS` / `TOTAL_RETRY_TIME_MS` (inject-library.c).
pub const RETRY_INTERVAL_MS: u32 = 500;
pub const TOTAL_RETRY_TIME_MS: u32 = 4000;
/// `RETRY_COUNT` (inject-library.c): `TOTAL_RETRY_TIME_MS / RETRY_INTERVAL_MS`.
pub const RETRY_COUNT: u32 = TOTAL_RETRY_TIME_MS / RETRY_INTERVAL_MS;

/// A resolved module: the API name -> function pointer the reference would
/// get from `GetModuleHandleW` + `GetProcAddress` (or
/// `ms_get_obfuscated_func`). In unit tests this is a hand-built map; the
/// P5 WinAPI binding supplies the real one.
pub type Module = HashMap<String, usize>;

/// `deobfuscate_str` (obfuscate.c): XOR each byte of `name` with the low
/// half-byte of `key`'s little-endian byte `i/2` when `i` is even and the
/// high half-byte when odd, wrapping every 16 bytes. The reference stops
/// at the first NUL; a Rust slice carries no NUL, so the whole slice is
/// de-obfuscated.
#[must_use]
pub fn deobfuscate(name: &[u8], key: u64) -> Vec<u8> {
    let key_bytes = key.to_le_bytes();
    let mut out = Vec::with_capacity(name.len());
    let mut i: usize = 0;
    for &c in name {
        let pos = i / 2;
        let half = if i.is_multiple_of(2) {
            key_bytes[pos] & 0xf
        } else {
            (key_bytes[pos] >> 4) & 0xf
        };
        out.push(c ^ half);
        i += 1;
        if i == 16 {
            i = 0;
        }
    }
    out
}

/// `ms_get_obfuscated_func` (obfuscate.c) reduced to its portable core:
/// de-obfuscate `obfuscated` with `key`, then look the real API name up in
/// `module`. Returns `None` when the resolved name is absent (the
/// reference would then receive NULL from `GetProcAddress`).
#[must_use]
pub fn resolve_symbol(module: &Module, obfuscated: &str, key: u64) -> Option<usize> {
    let name = deobfuscate(obfuscated.as_bytes(), key);
    let name = String::from_utf8(name).ok()?;
    module.get(&name).copied()
}

/// The WinAPI shapes the [`ObfApis`]/[`SafeApis`] fields model.
type WriteProcessMemory = dyn Fn(usize, usize, &[u16], &mut usize) -> bool;
type CreateRemoteThread = dyn Fn(usize, usize, usize, &mut u32) -> Option<usize>;
type GetExitCodeThread = dyn Fn(usize, &mut u32);
type LoadLibraryW = dyn Fn(&[u16]) -> Option<usize>;
type GetProcAddress = dyn Fn(usize, &str) -> Option<usize>;

/// The Windows calls `inject_library_obf` drives — the exact WinAPI at
/// each step (same argument order); the P5 binding fills these from real
/// `GetProcAddress` handles.
pub struct ObfApis {
    /// `VirtualAllocEx(process, NULL, size, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE)`.
    pub virtual_alloc_ex: Box<dyn Fn(usize, usize) -> Option<usize>>,
    /// `WriteProcessMemory(process, mem, dll, size, &written)` -> success.
    pub write_process_memory: Box<WriteProcessMemory>,
    /// `CreateRemoteThread(process, NULL, 0, load_library_w, mem, 0, &thread_id)` -> handle.
    pub create_remote_thread: Box<CreateRemoteThread>,
    /// `WaitForSingleObject(thread, 4000) == WAIT_OBJECT_0`.
    pub wait_for_single_object: Box<dyn Fn(usize, u32) -> bool>,
    /// `GetExitCodeThread(thread, &code)`.
    pub get_exit_code_thread: Box<GetExitCodeThread>,
    /// `GetLastError()`.
    pub get_last_error: Box<dyn Fn() -> u32>,
    /// `SetLastError(v)`.
    pub set_last_error: Box<dyn Fn(u32)>,
    /// `CloseHandle(thread)`.
    pub close_handle: Box<dyn Fn(usize)>,
    /// `VirtualFreeEx(process, mem, 0, MEM_RELEASE)`.
    pub virtual_free_ex: Box<dyn Fn(usize, usize)>,
}

/// The reference's `fail:` label: capture `last_error` only when the
/// result is [`INJECT_ERROR_UNLIKELY_FAIL`], close the thread, free the
/// remote memory, restore `last_error`.
fn finish(
    last_error: &mut u32,
    apis: &ObfApis,
    thread: &mut Option<usize>,
    mem: &mut Option<usize>,
    process: usize,
    ret: i32,
) -> i32 {
    if ret == INJECT_ERROR_UNLIKELY_FAIL {
        *last_error = (apis.get_last_error)();
    }
    if let Some(t) = thread.take() {
        (apis.close_handle)(t);
    }
    if let Some(m) = mem.take() {
        (apis.virtual_free_ex)(process, m);
    }
    if *last_error != 0 {
        (apis.set_last_error)(*last_error);
    }
    ret
}

/// `inject_library_obf` (inject-library.c): the control flow over the
/// injected [`ObfApis`]. `kernel32` is the resolved KERNEL32 module; the
/// five `(name, key)` pairs are the obfuscated API names exactly as the
/// win-capture callers pass them. Returns the reference's `int` (0 on
/// success, a negative `INJECT_ERROR_*` otherwise).
///
/// Deviation (documented): the reference calls the five resolved pointers
/// without NULL checks (a missing symbol faults). The port returns
/// [`INJECT_ERROR_UNLIKELY_FAIL`] when a symbol does not resolve — a clean
/// error instead of a crash, before any WinAPI call is made.
#[must_use]
#[allow(clippy::too_many_arguments)] // mirrors the reference's flat 11-parameter C signature
pub fn inject_library_obf(
    kernel32: &Module,
    process: usize,
    dll: &[u16],
    create_remote_thread: (&str, u64),
    write_process_memory: (&str, u64),
    virtual_alloc_ex: (&str, u64),
    virtual_free_ex: (&str, u64),
    load_library_w: (&str, u64),
    apis: &ObfApis,
) -> i32 {
    let ret = INJECT_ERROR_UNLIKELY_FAIL;
    let mut last_error = 0u32;
    let mut thread: Option<usize> = None;
    let mut mem: Option<usize> = None;

    let (crt, wpm, vac, vfe, llw) = (
        resolve_symbol(kernel32, create_remote_thread.0, create_remote_thread.1),
        resolve_symbol(kernel32, write_process_memory.0, write_process_memory.1),
        resolve_symbol(kernel32, virtual_alloc_ex.0, virtual_alloc_ex.1),
        resolve_symbol(kernel32, virtual_free_ex.0, virtual_free_ex.1),
        resolve_symbol(kernel32, load_library_w.0, load_library_w.1),
    );
    let (Some(_), Some(_), Some(_), Some(_), Some(llw)) = (crt, wpm, vac, vfe, llw) else {
        // Unresolved symbol: the reference would fault; the port fails
        // cleanly with nothing allocated, so no cleanup runs.
        return ret;
    };

    // size = (wcslen(dll) + 1) * sizeof(wchar_t)
    let size = (dll.len() + 1) * std::mem::size_of::<u16>();

    if let Some(m) = (apis.virtual_alloc_ex)(process, size) {
        mem = Some(m);
        let mut written = 0usize;
        if (apis.write_process_memory)(process, m, dll, &mut written) {
            let mut thread_id = 0u32;
            if let Some(t) = (apis.create_remote_thread)(process, llw, m, &mut thread_id) {
                thread = Some(t);
                if (apis.wait_for_single_object)(t, 4000) {
                    let mut code = 0u32;
                    (apis.get_exit_code_thread)(t, &mut code);
                    let result = if code != 0 {
                        0
                    } else {
                        INJECT_ERROR_INJECT_FAILED
                    };
                    (apis.set_last_error)(0);
                    return finish(
                        &mut last_error,
                        apis,
                        &mut thread,
                        &mut mem,
                        process,
                        result,
                    );
                }
            }
        }
    }

    finish(&mut last_error, apis, &mut thread, &mut mem, process, ret)
}

/// The Windows calls `inject_library_safe_obf` drives.
pub struct SafeApis {
    /// `GetModuleHandleW(L"USER32")` -> the resolved USER32 module (empty
    /// models a NULL handle).
    pub user32: Box<dyn Fn() -> Module>,
    /// `LoadLibraryW(dll)` -> handle.
    pub load_library_w: Box<LoadLibraryW>,
    /// `GetProcAddress(lib, proc_name)` -> hook proc.
    pub get_proc_address: Box<GetProcAddress>,
    /// `SetWindowsHookEx(WH_GETMESSAGE, proc, lib, thread_id)` -> hook.
    pub set_windows_hook_ex: Box<dyn Fn(i32, usize, usize, u32) -> Option<usize>>,
    /// `GetLastError()`.
    pub get_last_error: Box<dyn Fn() -> u32>,
    /// `Sleep(ms)`.
    pub sleep: Box<dyn Fn(u32)>,
    /// `PostThreadMessage(thread_id, WM_USER+432, 0, hook)` -> posted.
    pub post_thread_message: Box<dyn Fn(u32, u32, u32, usize) -> bool>,
}

/// `WH_GETMESSAGE` (windows.h).
const WH_GETMESSAGE: i32 = -1;
/// `WM_USER + 432` (inject-library.c): the message posted to start the hook.
const WM_USER_432: u32 = 0x8000 + 432;
/// The hook library's export, 64-bit (the reference's `#ifdef _WIN64`).
const PROC_NAME_64: &str = "dummy_debug_proc";
/// The hook library's export, 32-bit `__cdecl` decorated.
const PROC_NAME_32: &str = "_dummy_debug_proc@12";

/// `inject_library_safe_obf` (inject-library.c): the control flow over the
/// injected [`SafeApis`]. Returns the reference's `int`.
///
/// Deviation (documented): the reference calls the resolved
/// `set_windows_hook_ex` without a NULL check; the port returns
/// [`INJECT_ERROR_UNLIKELY_FAIL`] when the symbol does not resolve.
#[must_use]
pub fn inject_library_safe_obf(
    thread_id: u32,
    dll: &[u16],
    set_windows_hook_ex: (&str, u64),
    apis: &SafeApis,
) -> i32 {
    let ret = INJECT_ERROR_UNLIKELY_FAIL;

    // The reference resolves USER32 first, then LoadLibraryW(dll), then
    // checks `!lib || !user32` (lib first).
    let user32_module = (apis.user32)();
    let lib = (apis.load_library_w)(dll);
    let Some(lib) = lib else {
        return ret;
    };
    if user32_module.is_empty() {
        return ret;
    }

    let proc_name = if cfg!(target_pointer_width = "64") {
        PROC_NAME_64
    } else {
        PROC_NAME_32
    };
    let Some(proc) = (apis.get_proc_address)(lib, proc_name) else {
        return ret;
    };

    // The reference calls the resolved pointer directly; an unresolved
    // symbol faults there — the port fails cleanly instead.
    if resolve_symbol(&user32_module, set_windows_hook_ex.0, set_windows_hook_ex.1).is_none() {
        return ret;
    }

    let Some(hook) = (apis.set_windows_hook_ex)(WH_GETMESSAGE, proc, lib, thread_id) else {
        // The reference returns the DWORD GetLastError() from an int fn.
        return (apis.get_last_error)() as i32;
    };

    // The reference posts the hook message RETRY_COUNT times at
    // RETRY_INTERVAL_MS to nudge SetWindowsHookEx into injecting the DLL
    // ("mostly just a precaution").
    for _ in 0..RETRY_COUNT {
        (apis.sleep)(RETRY_INTERVAL_MS);
        (apis.post_thread_message)(thread_id, WM_USER_432, 0, hook);
    }
    0
}

#[cfg(test)]
mod tests {
    #![allow(clippy::arc_with_non_send_sync)] // single-threaded RefCell traces

    use super::*;
    use std::sync::Arc;

    /// A fake kernel32 with the five symbols `inject_library_obf` resolves
    /// (the win-capture obfuscated names de-obfuscate to these real names).
    fn kernel32() -> Module {
        let mut m = Module::new();
        m.insert("CreateRemoteThread".into(), 0xc1);
        m.insert("WriteProcessMemory".into(), 0xc2);
        m.insert("VirtualAllocEx".into(), 0xc3);
        m.insert("VirtualFreeEx".into(), 0xc4);
        m.insert("LoadLibraryW".into(), 0xc5);
        m
    }

    const NCR: (&str, u64) = ("E}mo|d[cefubWk~bgk", 0x7c3371986918e8f6);
    const NWP: (&str, u64) = ("Rqbr`T{cnor{Bnlgwz", 0x81bf81adc9456b35);
    const NVA: (&str, u64) = ("]`~wrl`KeghiCt", 0xadc6a7b9acd73c9b);
    const NVF: (&str, u64) = ("Zh}{}agHzfd@{", 0x57135138eb08ff1c);
    const NLL: (&str, u64) = ("DnafGhj}l~sX", 0x350bfacdf81b2018);
    const NSH: (&str, u64) = ("[bs^fbkmwuKfmfOvI", 0xEAD293602FCF9778);

    /// The win-capture obfuscated names/keys (both call sites) resolve to
    /// the real API names — the symbol-resolution test.
    #[test]
    fn obfuscated_names_resolve() {
        let k = kernel32();
        // inject-helper.c
        assert_eq!(resolve_symbol(&k, NCR.0, NCR.1), Some(0xc1));
        assert_eq!(resolve_symbol(&k, NWP.0, NWP.1), Some(0xc2));
        assert_eq!(resolve_symbol(&k, NVA.0, NVA.1), Some(0xc3));
        assert_eq!(resolve_symbol(&k, NVF.0, NVF.1), Some(0xc4));
        assert_eq!(resolve_symbol(&k, NLL.0, NLL.1), Some(0xc5));
        // game-capture.c (different obfuscation, same targets)
        assert_eq!(
            resolve_symbol(&k, "D|hkqkW`kl{k\\osofj", 0xa178ef3655e5ade7),
            Some(0xc1)
        );
        assert_eq!(
            resolve_symbol(&k, "[uawaRzbhh{tIdkj~~", 0x561478dbd824387c),
            Some(0xc2)
        );
        assert_eq!(
            resolve_symbol(&k, "[fr}pboIe`dlN}", 0x395bfbc9833590fd),
            Some(0xc3)
        );
        assert_eq!(
            resolve_symbol(&k, "\\`zs}gmOzhhBq", 0x12897dd89168789a),
            Some(0xc4)
        );
        assert_eq!(
            resolve_symbol(&k, "GbfkDaezbp~X", 0x76aff7238788f7db),
            Some(0xc5)
        );
    }

    /// A wrong key de-obfuscates to a different name -> no resolution.
    #[test]
    fn wrong_key_does_not_resolve() {
        assert_eq!(resolve_symbol(&kernel32(), NCR.0, 1), None);
    }

    /// The de-obfuscation is an involution (XOR with the same half-bytes).
    #[test]
    fn deobfuscate_roundtrip() {
        let name = b"CreateRemoteThread";
        let once = deobfuscate(name, NCR.1);
        assert_ne!(&once, name);
        assert_eq!(deobfuscate(&once, NCR.1), name.to_vec());
    }

    /// The outcome script of the injected calls.
    #[derive(Clone, Copy)]
    struct Script {
        alloc: bool,
        write: bool,
        thread: bool,
        wait: bool,
        exit_code: u32,
        last_error: u32,
    }

    /// `inject_library_obf` call trace for the unit tests (single-threaded).
    #[derive(Default)]
    struct Trace {
        allocs: std::cell::RefCell<Vec<(usize, usize)>>,
        writes: std::cell::RefCell<Vec<(usize, usize)>>,
        threads: std::cell::RefCell<Vec<(usize, usize)>>,
        closes: std::cell::RefCell<Vec<usize>>,
        frees: std::cell::RefCell<Vec<(usize, usize)>>,
        last_error_sets: std::cell::RefCell<Vec<u32>>,
    }

    fn make_apis(s: Script) -> (ObfApis, Arc<Trace>) {
        let tr = Arc::new(Trace::default());
        let apis = ObfApis {
            virtual_alloc_ex: {
                let tr = tr.clone();
                Box::new(move |p, sz| {
                    tr.allocs.borrow_mut().push((p, sz));
                    if s.alloc { Some(0x4000) } else { None }
                })
            },
            write_process_memory: {
                let tr = tr.clone();
                Box::new(move |p, m, _, w| {
                    tr.writes.borrow_mut().push((p, m));
                    *w = 4;
                    s.write
                })
            },
            create_remote_thread: {
                let tr = tr.clone();
                Box::new(move |p, start, _, tid| {
                    tr.threads.borrow_mut().push((p, start));
                    *tid = 7;
                    if s.thread { Some(0x9000) } else { None }
                })
            },
            wait_for_single_object: Box::new(move |_, _| s.wait),
            get_exit_code_thread: Box::new(move |_, c| *c = s.exit_code),
            get_last_error: Box::new(move || s.last_error),
            set_last_error: {
                let tr = tr.clone();
                Box::new(move |v| tr.last_error_sets.borrow_mut().push(v))
            },
            close_handle: {
                let tr = tr.clone();
                Box::new(move |h| tr.closes.borrow_mut().push(h))
            },
            virtual_free_ex: {
                let tr = tr.clone();
                Box::new(move |p, m| tr.frees.borrow_mut().push((p, m)))
            },
        };
        (apis, tr)
    }

    fn run(s: Script) -> (i32, Arc<Trace>) {
        let (apis, tr) = make_apis(s);
        let r = inject_library_obf(
            &kernel32(),
            0x1000,
            &[0x61, 0x62],
            NCR,
            NWP,
            NVA,
            NVF,
            NLL,
            &apis,
        );
        (r, tr)
    }

    #[test]
    fn success_path() {
        let (r, tr) = run(Script {
            alloc: true,
            write: true,
            thread: true,
            wait: true,
            exit_code: 1,
            last_error: 42,
        });
        assert_eq!(r, 0);
        assert_eq!(tr.allocs.borrow().len(), 1);
        assert_eq!(tr.writes.borrow().len(), 1);
        assert_eq!(tr.threads.borrow().len(), 1);
        // exit code != 0 -> SetLastError(0); no last_error capture (ret != UNLIKELY).
        assert_eq!(*tr.last_error_sets.borrow(), vec![0]);
        assert_eq!(*tr.closes.borrow(), vec![0x9000]);
        assert_eq!(*tr.frees.borrow(), vec![(0x1000, 0x4000)]);
    }

    #[test]
    fn alloc_failure() {
        let (r, tr) = run(Script {
            alloc: false,
            write: true,
            thread: true,
            wait: true,
            exit_code: 1,
            last_error: 42,
        });
        assert_eq!(r, INJECT_ERROR_UNLIKELY_FAIL);
        assert_eq!(tr.writes.borrow().len(), 0);
        assert_eq!(tr.threads.borrow().len(), 0);
        assert_eq!(tr.closes.borrow().len(), 0);
        assert_eq!(tr.frees.borrow().len(), 0);
        // last_error captured and restored.
        assert_eq!(*tr.last_error_sets.borrow(), vec![42]);
    }

    #[test]
    fn write_failure_frees_memory() {
        let (r, tr) = run(Script {
            alloc: true,
            write: false,
            thread: true,
            wait: true,
            exit_code: 1,
            last_error: 42,
        });
        assert_eq!(r, INJECT_ERROR_UNLIKELY_FAIL);
        assert_eq!(tr.threads.borrow().len(), 0);
        assert_eq!(tr.closes.borrow().len(), 0);
        assert_eq!(*tr.frees.borrow(), vec![(0x1000, 0x4000)]);
        assert_eq!(*tr.last_error_sets.borrow(), vec![42]);
    }

    #[test]
    fn thread_creation_failure_frees_memory() {
        let (r, tr) = run(Script {
            alloc: true,
            write: true,
            thread: false,
            wait: true,
            exit_code: 1,
            last_error: 42,
        });
        assert_eq!(r, INJECT_ERROR_UNLIKELY_FAIL);
        assert_eq!(tr.closes.borrow().len(), 0);
        assert_eq!(*tr.frees.borrow(), vec![(0x1000, 0x4000)]);
        assert_eq!(*tr.last_error_sets.borrow(), vec![42]);
    }

    #[test]
    fn wait_timeout_closes_and_frees() {
        let (r, tr) = run(Script {
            alloc: true,
            write: true,
            thread: true,
            wait: false,
            exit_code: 1,
            last_error: 42,
        });
        assert_eq!(r, INJECT_ERROR_UNLIKELY_FAIL);
        assert_eq!(*tr.closes.borrow(), vec![0x9000]);
        assert_eq!(*tr.frees.borrow(), vec![(0x1000, 0x4000)]);
        assert_eq!(*tr.last_error_sets.borrow(), vec![42]);
    }

    #[test]
    fn exit_code_zero_is_inject_failed() {
        let (r, tr) = run(Script {
            alloc: true,
            write: true,
            thread: true,
            wait: true,
            exit_code: 0,
            last_error: 42,
        });
        assert_eq!(r, INJECT_ERROR_INJECT_FAILED);
        // SetLastError(0) in the wait branch; no capture (ret != UNLIKELY).
        assert_eq!(*tr.last_error_sets.borrow(), vec![0]);
        assert_eq!(*tr.closes.borrow(), vec![0x9000]);
        assert_eq!(*tr.frees.borrow(), vec![(0x1000, 0x4000)]);
    }

    /// A missing symbol fails cleanly before any WinAPI call (documented
    /// deviation: the reference would fault on the NULL call).
    #[test]
    fn missing_symbol_fails_cleanly() {
        let mut k = kernel32();
        k.remove("LoadLibraryW");
        let (apis, tr) = make_apis(Script {
            alloc: true,
            write: true,
            thread: true,
            wait: true,
            exit_code: 1,
            last_error: 42,
        });
        let r = inject_library_obf(&k, 0x1000, &[0x61], NCR, NWP, NVA, NVF, NLL, &apis);
        assert_eq!(r, INJECT_ERROR_UNLIKELY_FAIL);
        assert_eq!(tr.allocs.borrow().len(), 0);
        assert_eq!(tr.writes.borrow().len(), 0);
        assert_eq!(tr.closes.borrow().len(), 0);
        assert_eq!(tr.frees.borrow().len(), 0);
        assert_eq!(tr.last_error_sets.borrow().len(), 0);
    }

    /// A fake USER32 with the hook function (NSH de-obfuscates to
    /// SetWindowsHookExA).
    fn user32() -> Module {
        let mut m = Module::new();
        m.insert("SetWindowsHookExA".into(), 0xd1);
        m
    }

    #[derive(Default)]
    struct SafeTrace {
        sleeps: std::cell::RefCell<Vec<u32>>,
        posts: std::cell::RefCell<Vec<(u32, u32, u32, usize)>>,
    }

    struct SafeCfg {
        lib: Option<usize>,
        user32: bool,
        proc: bool,
        hook_ok: bool,
        last_error: u32,
    }

    fn make_safe_apis(c: SafeCfg) -> (SafeApis, Arc<SafeTrace>) {
        let tr = Arc::new(SafeTrace::default());
        let apis = SafeApis {
            user32: Box::new(move || if c.user32 { user32() } else { Module::new() }),
            load_library_w: Box::new(move |_| c.lib),
            get_proc_address: Box::new(move |_, _| if c.proc { Some(0xe1) } else { None }),
            set_windows_hook_ex: Box::new(
                move |_, _, _, _| if c.hook_ok { Some(0xf1) } else { None },
            ),
            get_last_error: Box::new(move || c.last_error),
            sleep: {
                let tr = tr.clone();
                Box::new(move |ms| tr.sleeps.borrow_mut().push(ms))
            },
            post_thread_message: {
                let tr = tr.clone();
                Box::new(move |t, m, w, h| {
                    tr.posts.borrow_mut().push((t, m, w, h));
                    true
                })
            },
        };
        (apis, tr)
    }

    fn run_safe(c: SafeCfg) -> (i32, Arc<SafeTrace>) {
        let (apis, tr) = make_safe_apis(c);
        let r = inject_library_safe_obf(0x30, &[0x61], NSH, &apis);
        (r, tr)
    }

    #[test]
    fn safe_success_posts_retries() {
        let (r, tr) = run_safe(SafeCfg {
            lib: Some(0x100),
            user32: true,
            proc: true,
            hook_ok: true,
            last_error: 9,
        });
        assert_eq!(r, 0);
        assert_eq!(
            *tr.sleeps.borrow(),
            vec![RETRY_INTERVAL_MS; RETRY_COUNT as usize]
        );
        let posts = tr.posts.borrow();
        assert_eq!(posts.len(), RETRY_COUNT as usize);
        assert!(
            posts
                .iter()
                .all(|&(t, m, w, h)| (t, m, w, h) == (0x30, WM_USER_432, 0, 0xf1))
        );
    }

    #[test]
    fn safe_load_library_failure() {
        let (r, tr) = run_safe(SafeCfg {
            lib: None,
            user32: true,
            proc: true,
            hook_ok: true,
            last_error: 9,
        });
        assert_eq!(r, INJECT_ERROR_UNLIKELY_FAIL);
        assert_eq!(tr.sleeps.borrow().len(), 0);
        assert_eq!(tr.posts.borrow().len(), 0);
    }

    #[test]
    fn safe_user32_missing() {
        let (r, _tr) = run_safe(SafeCfg {
            lib: Some(0x100),
            user32: false,
            proc: true,
            hook_ok: true,
            last_error: 9,
        });
        assert_eq!(r, INJECT_ERROR_UNLIKELY_FAIL);
    }

    #[test]
    fn safe_proc_missing() {
        let (r, _tr) = run_safe(SafeCfg {
            lib: Some(0x100),
            user32: true,
            proc: false,
            hook_ok: true,
            last_error: 9,
        });
        assert_eq!(r, INJECT_ERROR_UNLIKELY_FAIL);
    }

    /// A USER32 without the hook symbol fails cleanly (documented
    /// deviation: the reference would fault on the NULL call).
    #[test]
    fn safe_hook_symbol_missing() {
        let tr = Arc::new(SafeTrace::default());
        let other = Module::from([("SomeOtherFunction".to_string(), 0x1)]);
        let apis = SafeApis {
            // non-empty USER32 that lacks SetWindowsHookExA
            user32: Box::new(move || other.clone()),
            load_library_w: Box::new(|_| Some(0x100)),
            get_proc_address: Box::new(|_, _| Some(0xe1)),
            set_windows_hook_ex: Box::new(|_, _, _, _| Some(0xf1)),
            get_last_error: Box::new(|| 9),
            sleep: {
                let tr = tr.clone();
                Box::new(move |ms| tr.sleeps.borrow_mut().push(ms))
            },
            post_thread_message: {
                let tr = tr.clone();
                Box::new(move |t, m, w, h| {
                    tr.posts.borrow_mut().push((t, m, w, h));
                    true
                })
            },
        };
        let r = inject_library_safe_obf(0x30, &[0x61], NSH, &apis);
        assert_eq!(r, INJECT_ERROR_UNLIKELY_FAIL);
        assert_eq!(tr.sleeps.borrow().len(), 0);
        assert_eq!(tr.posts.borrow().len(), 0);
    }

    /// A failing SetWindowsHookEx returns GetLastError().
    #[test]
    fn safe_hook_setup_failure_returns_last_error() {
        let (r, tr) = run_safe(SafeCfg {
            lib: Some(0x100),
            user32: true,
            proc: true,
            hook_ok: false,
            last_error: 7,
        });
        assert_eq!(r, 7);
        assert_eq!(tr.sleeps.borrow().len(), 0);
        assert_eq!(tr.posts.borrow().len(), 0);
    }
}
