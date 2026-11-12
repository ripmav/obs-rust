//! S-023-R82: `os_get_executable_path_ptr` (Linux: `/proc/self/exe`).

use std::ffi::CString;
use std::path::PathBuf;

/// The directory of the running executable, or that directory + `"/"` +
/// `name` when `name` is Some and non-empty (S-023-R82). `None` if the
/// executable path cannot be determined.
pub fn os_get_executable_path_ptr(name: Option<&str>) -> Option<String> {
    // SAFETY: a valid NUL-terminated path, buffer large enough.
    let c = CString::new("/proc/self/exe").ok()?;
    let mut buf = [0u8; 4096];
    // readlink(2) takes a c_char buffer (*mut i8 on x86_64, *mut u8 on
    // aarch64); .cast() infers the platform type.
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    #[cfg_attr(miri, ignore)]
    fn executable_path() {
        // R82: the directory of the running executable
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
}
