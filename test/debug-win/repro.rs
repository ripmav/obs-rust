//! WP-38 CI debug (temporary): minimal replication of the windows-latest
//! 0xc0000005, isolated from obs-util. Stages:
//! A: SetThreadDescription / GetThreadDescription (raw externs + lookup)
//! B: std::process::Command control (does plain spawn work on the runner?)
//! C: raw CreateProcessW, no stdio pipes (si = NULL)
//! D: raw CreateProcessW with the exact obs-util pipe sequence
use std::os::raw::c_void;

#[repr(C)]
#[derive(Clone, Copy)]
struct SecurityAttributes {
    n_length: u32,
    lp_security_descriptor: *mut c_void,
    b_inherit_handle: i32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct StartUpInfoW {
    cb: u32,
    lp_reserved: *mut c_void,
    lp_title: *mut c_void,
    dw_flags: u32,
    w_show_window: u16,
    cb_reserved2: u16,
    lp_reserved2: *mut u16,
    h_std_input: *mut c_void,
    h_std_output: *mut c_void,
    h_std_error: *mut c_void,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct ProcessInformation {
    h_process: *mut c_void,
    h_thread: *mut c_void,
    dw_process_id: u32,
    dw_thread_id: u32,
}

unsafe extern "system" {
    fn CreatePipe(
        a: *mut *mut c_void,
        b: *mut *mut c_void,
        c: *const SecurityAttributes,
        d: u32,
    ) -> i32;
    fn SetHandleInformation(h: *mut c_void, m: u32, i: u32) -> i32;
    fn CreateProcessW(
        a: *const u16,
        b: *mut u16,
        c: *const SecurityAttributes,
        d: *const SecurityAttributes,
        e: i32,
        f: u32,
        g: *mut c_void,
        h: *const u16,
        i: *const StartUpInfoW,
        j: *mut ProcessInformation,
    ) -> i32;
    fn ReadFile(h: *mut c_void, b: *mut c_void, n: u32, r: *mut u32, o: *const c_void) -> i32;
    fn CloseHandle(h: *mut c_void) -> i32;
    fn WaitForSingleObject(h: *mut c_void, ms: u32) -> u32;
    fn GetExitCodeProcess(h: *mut c_void, c: *mut u32) -> i32;
    fn GetLastError() -> u32;
    fn LoadLibraryA(n: *const u8) -> *mut c_void;
    fn GetProcAddress(m: *mut c_void, n: *const u8) -> *mut c_void;
    fn FreeLibrary(m: *mut c_void) -> i32;
    fn GetCurrentThread() -> *mut c_void;
    fn GetThreadDescription(h: *mut c_void, p: *mut *mut u16) -> i32;
    fn LocalFree(h: *mut c_void) -> *mut c_void;
}

type SetDesc = unsafe extern "system" fn(*mut c_void, *const u16) -> i32;

fn main() {
    println!("=== A: SetThreadDescription / GetThreadDescription ===");
    let wide: Vec<u16> = "hello"
        .encode_utf16()
        .chain(std::iter::once(0))
        .collect();
    let module = unsafe { LoadLibraryA(c"KernelBase.dll".as_ptr().cast::<u8>()) };
    println!("module={module:p}");
    let proc = unsafe {
        GetProcAddress(module, c"SetThreadDescription".as_ptr().cast::<u8>())
    };
    println!("proc={proc:p}");
    let set_desc: SetDesc = unsafe { std::mem::transmute(proc) };
    let hr = unsafe { set_desc(GetCurrentThread(), wide.as_ptr()) };
    println!("SetThreadDescription hr={hr}");
    let mut desc: *mut u16 = std::ptr::null_mut();
    let hr2 = unsafe { GetThreadDescription(GetCurrentThread(), &mut desc) };
    println!("GetThreadDescription hr={hr2} desc={desc:p}");
    if !desc.is_null() {
        let end = (0..).find(|i| unsafe { *desc.add(*i) } == 0).unwrap();
        println!(
            "name={:?}",
            unsafe { String::from_utf16_lossy(std::slice::from_raw_parts(desc, end)) }
        );
        unsafe { LocalFree(desc as *mut c_void) };
    }
    unsafe { FreeLibrary(module) };

    println!("=== B: std::process::Command control ===");
    let out = std::process::Command::new("cmd")
        .args(["/c", "echo", "from-cmd"])
        .output();
    println!(
        "std Command: {:?}",
        out.map(|o| String::from_utf8_lossy(&o.stdout).into_owned())
    );

    println!("=== C: raw CreateProcessW, no stdio pipes (si=NULL) ===");
    {
        let mut cmd: Vec<u16> = "cmd /c echo raw-no-pipes".encode_utf16().collect();
        cmd.push(0);
        let mut pi: ProcessInformation = unsafe { std::mem::zeroed() };
        let ret = unsafe {
            CreateProcessW(
                std::ptr::null(),
                cmd.as_mut_ptr(),
                std::ptr::null(),
                std::ptr::null(),
                1,
                0x0800_0000,
                std::ptr::null_mut(),
                std::ptr::null(),
                std::ptr::null(),
                &mut pi,
            )
        };
        println!(
            "CreateProcessW(no-si) ret={} pid={} err={}",
            ret,
            pi.dw_process_id,
            unsafe { GetLastError() }
        );
        if ret != 0 {
            unsafe { WaitForSingleObject(pi.h_process, 10000) };
            unsafe { CloseHandle(pi.h_thread) };
            unsafe { CloseHandle(pi.h_process) };
            println!("no-si child reaped");
        }
    }

    println!("=== D: raw CreateProcessW with pipes (the obs-util sequence) ===");
    let sa = SecurityAttributes {
        n_length: std::mem::size_of::<SecurityAttributes>() as u32,
        lp_security_descriptor: std::ptr::null_mut(),
        b_inherit_handle: 1,
    };
    let mut input = std::ptr::null_mut();
    let mut output = std::ptr::null_mut();
    let r1 = unsafe { CreatePipe(&mut input, &mut output, &sa, 0) };
    println!("CreatePipe1 ret={r1} r={input:p} w={output:p}");
    let mut err_input = std::ptr::null_mut();
    let mut err_output = std::ptr::null_mut();
    let r2 = unsafe { CreatePipe(&mut err_input, &mut err_output, &sa, 0) };
    println!("CreatePipe2 ret={r2} r={err_input:p} w={err_output:p}");
    let s1 = unsafe { SetHandleInformation(input, 1, 0) };
    let s2 = unsafe { SetHandleInformation(err_input, 1, 0) };
    println!("SetHandleInformation {s1} {s2}");
    let mut cmd: Vec<u16> = "cmd /c echo hello".encode_utf16().collect();
    cmd.push(0);
    let mut si: StartUpInfoW = unsafe { std::mem::zeroed() };
    si.cb = std::mem::size_of::<StartUpInfoW>() as u32;
    si.dw_flags = 0x180; // STARTF_USESTDHANDLES | STARTF_FORCEOFFFEEDBACK
    si.h_std_input = std::ptr::null_mut();
    si.h_std_output = output;
    si.h_std_error = err_output;
    let mut pi: ProcessInformation = unsafe { std::mem::zeroed() };
    println!("about to call CreateProcessW");
    let ret = unsafe {
        CreateProcessW(
            std::ptr::null(),
            cmd.as_mut_ptr(),
            std::ptr::null(),
            std::ptr::null(),
            1,
            0x0800_0000,
            std::ptr::null_mut(),
            std::ptr::null(),
            &si,
            &mut pi,
        )
    };
    println!(
        "CreateProcessW ret={} pid={} tid={:?}",
        ret, pi.dw_process_id, pi.h_thread
    );
    if ret != 0 {
        unsafe { CloseHandle(pi.h_thread) };
        unsafe { CloseHandle(output) };
        unsafe { CloseHandle(err_output) };
        let mut buf = [0u8; 100];
        let mut bytes = 0u32;
        let rr = unsafe {
            ReadFile(
                input,
                buf.as_mut_ptr() as *mut c_void,
                buf.len() as u32,
                &mut bytes,
                std::ptr::null(),
            )
        };
        println!(
            "ReadFile ret={} bytes={}: {:?}",
            rr,
            bytes,
            String::from_utf8_lossy(&buf[..bytes as usize])
        );
        unsafe { CloseHandle(input) };
        unsafe { CloseHandle(err_input) };
        unsafe { WaitForSingleObject(pi.h_process, 10000) };
        let mut code = 0u32;
        let ok = unsafe { GetExitCodeProcess(pi.h_process, &mut code) } != 0;
        println!("exit={code} ok={ok}");
        unsafe { CloseHandle(pi.h_process) };
    }
    println!("=== repro done ===");
}
