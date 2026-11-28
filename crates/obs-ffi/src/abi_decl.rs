//! P1-21.3/21.4 recipe step 2: the C ABI for the declaration parser
//! (`libobs/callback/decl.c`, S-003).
//!
//! One export: `parse_decl_string`. `struct decl_info` / `decl_param`
//! are the Rust `DeclInfo` / `DeclParam` (repr-C mirrors of the C
//! structs, `DArray` included — the same arrangement as the C header's
//! `static inline decl_info_free`, which keeps operating on the layout).

use std::os::raw::c_char;

use crate::abi_common::ensure_c_log_sink;
use obs_signal::decl::{DeclInfo, parse_decl_string as cd_parse_decl_string};

/// `EXPORT bool parse_decl_string(struct decl_info *decl, const char
/// *decl_string)`
///
/// # Safety
/// `decl` must be a live `struct decl_info` (zeroed or not — the
/// function resets it; C dereferences it unconditionally, so NULL is
/// UB in C and rejected here); `decl_string` must be a NUL-terminated
/// string (it is **borrowed** into `decl->decl_string`, not copied —
/// R01) or, for the empty-input case, any readable string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn parse_decl_string(
    decl: *mut DeclInfo,
    decl_string: *const c_char,
) -> bool {
    ensure_c_log_sink();
    let Some(decl) = (if decl.is_null() { None } else { Some(decl) }) else {
        return false; // C: UB (unconditional dereference); rejected here.
    };
    let Some(decl_string) = crate::abi_common::c_bytes(decl_string) else {
        return false; // C: cf_parser on NULL is UB; rejected here.
    };
    // SAFETY: caller contract — `decl` is a live struct for the call; the parsed names are C-allocated (S-001 allocator, here the Rust bmem) and owned by the decl afterwards.
    unsafe { cd_parse_decl_string(&mut *decl, decl_string) }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::CStr;

    /// A valid parse fills the C-visible struct; a bad parse resets it
    /// (R10) and reports false.
    #[test]
    fn parse_decl_bridge() {
        // SAFETY: stack struct, live for the call.
        unsafe {
            let mut decl: DeclInfo = std::mem::zeroed();
            let src = c"int add(int a, int b)".as_ptr();
            assert!(parse_decl_string(&mut decl, src));
            let name = std::ffi::CStr::from_ptr(decl.name.cast::<c_char>());
            assert_eq!(name.to_bytes(), b"add");
            assert_eq!(decl.params.num, 3);
            // SAFETY: params was just filled by the successful parse above.
            let params = std::slice::from_raw_parts(decl.params.array, 3);
            assert_eq!(
                CStr::from_ptr(params[0].name.cast::<c_char>()).to_bytes(),
                b"a"
            );
            assert_eq!(params[0].ty, obs_signal::decl::CallParamType::Int);
            assert_eq!(params[0].flags, obs_signal::decl::CALL_PARAM_IN);
            assert_eq!(
                CStr::from_ptr(params[1].name.cast::<c_char>()).to_bytes(),
                b"b"
            );
            assert_eq!(
                CStr::from_ptr(params[2].name.cast::<c_char>()).to_bytes(),
                b"return"
            );
            assert_eq!(params[2].flags, obs_signal::decl::CALL_PARAM_OUT);
            obs_signal::decl::decl_info_free(&mut decl);

            let mut decl: DeclInfo = std::mem::zeroed();
            assert!(!parse_decl_string(&mut decl, c"bad decl".as_ptr()));
            assert!(decl.name.is_null());
            assert_eq!(decl.params.num, 0);
            assert!(!parse_decl_string(
                std::ptr::null_mut(),
                c"int f()".as_ptr()
            ));
        }
    }
}
