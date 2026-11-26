//! S-023-R82: `os_get_executable_path_ptr` (Linux: `/proc/self/exe`,
//! Windows: `GetModuleFileNameW` — WP-38 / finding 5).
//!
//! The macOS sysctl / FreeBSD paths are not ported (the nix reference in
//! `platform-nix.c` has them; the Linux `/proc/self/exe` branch is the
//! one the conformance vectors pin).

#[cfg(unix)]
use std::ffi::CString;
#[cfg(unix)]
use std::path::PathBuf;

/// The directory of the running executable, or that directory + `"/"` +
/// `name` when `name` is Some and non-empty (S-023-R82). `None` if the
/// executable path cannot be determined.
pub fn os_get_executable_path_ptr(name: Option<&str>) -> Option<String> {
    #[cfg(windows)]
    {
        // C reference (platform-windows.c): `GetModuleFileNameW(NULL,
        // MAX_PATH)` — the main module — then backslashes to forward
        // slashes, the directory up to and including the last slash, and
        // `name` appended when non-empty.
        use crate::winapi;
        let mut buf = [0u16; winapi::MAX_PATH as usize];
        // SAFETY: `buf` holds MAX_PATH wchars; NULL is the main module;
        // the return is the character count incl. NUL (0 on failure).
        let n = unsafe {
            winapi::GetModuleFileNameW(std::ptr::null_mut(), buf.as_mut_ptr(), winapi::MAX_PATH)
        };
        if n == 0 {
            return None;
        }
        // SAFETY: `buf[..n]` holds a valid UTF-16 string (NUL-terminated).
        let path = String::from_utf16_lossy(&buf[..n as usize]).replace('\\', "/");
        let dir = std::path::Path::new(&path).parent()?.to_path_buf();
        match name {
            Some(n) if !n.is_empty() => Some(format!("{}/{}", dir.display(), n)),
            _ => Some(dir.to_string_lossy().into_owned()),
        }
    }
    #[cfg(unix)]
    {
        // SAFETY: a valid NUL-terminated path, buffer large enough.
        let c = CString::new("/proc/self/exe").ok()?;
        let mut buf = [0u8; 4096];
        // readlink(2) takes a c_char buffer (*mut i8 on x86_64, *mut u8 on
        // aarch64); .cast() infers the platform type.
        // SAFETY: `buf` is valid for len bytes (allocated above); `c` is a NUL-terminated path.
        let n = unsafe { libc::readlink(c.as_ptr(), buf.as_mut_ptr().cast(), buf.len()) };
        if n <= 0 {
            return None;
        }
        let path = PathBuf::from(String::from_utf8_lossy(&buf[..n as usize]).into_owned());
        let dir = path.parent()?.to_path_buf();
        match name {
            Some(n) if !n.is_empty() => Some(format!("{}/{}", dir.display(), n)),
            _ => Some(dir.to_string_lossy().into_owned()),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    #[cfg(all(target_os = "linux", not(miri)))]
    fn executable_path() {
        // R82 Linux variant (/proc/self/exe): not present on macOS/Windows
        // — the per-OS variants arrive with the S-023 platform crate.
        // The directory of the running executable
        let dir = os_get_executable_path_ptr(None).expect("exe dir");
        let cur = std::env::current_exe().expect("current exe");
        assert_eq!(
            Some(dir.clone()),
            cur.parent().map(|p| p.to_string_lossy().into_owned())
        );

        // + "/" + name when non-empty
        let with_name = os_get_executable_path_ptr(Some("obs")).expect("exe dir + name");
        assert_eq!(with_name, format!("{dir}/obs"));

        // an empty name behaves like None
        let empty = os_get_executable_path_ptr(Some("")).expect("exe dir");
        assert_eq!(empty, dir);
    }

    #[test]
    #[cfg(windows)]
    #[cfg_attr(miri, ignore)]
    fn executable_path_windows() {
        // R82 Windows variant (GetModuleFileNameW): the directory of the
        // running executable (forward slashes, as in the C reference's
        // dstr_replace("\\", "/")).
        let dir = os_get_executable_path_ptr(None).expect("exe dir");
        let cur = std::env::current_exe().expect("current exe");
        let want = cur
            .parent()
            .map(|p| p.to_string_lossy().replace('\\', "/"))
            .unwrap_or_default();
        assert_eq!(dir, want, "exe dir");

        // + "/" + name when non-empty
        let with_name = os_get_executable_path_ptr(Some("obs")).expect("exe dir + name");
        assert_eq!(with_name, format!("{dir}/obs"));

        // an empty name behaves like None
        let empty = os_get_executable_path_ptr(Some("")).expect("exe dir");
        assert_eq!(empty, dir);
    }
}
