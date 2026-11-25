//! P1-33.2 (WP-34): the C ABI for `shared/file-updater` (S-024 M10).
//!
//! The full update algorithm of `file-updater.c` ported on top of the C
//! services still linked in Phase 1: `obs_data_*` (obs-data.c), libcurl
//! (the conformance runner links fakes), the `os_*` file API (the runner
//! links an in-memory fake; production links libobs), `blog` and `bmem`.
//! The pure helpers (`get_path`, the ETag header cleaning) are
//! [`obs_file_updater::file_updater`].
//!
//! `get_path` is file-local (static) in the C reference — like
//! `build_addr_list` it has no header declaration and is excluded from the
//! cbindgen header diff (the conformance runner declares it in swap mode).
//! The package and single-file update flows run on one thread each in the
//! reference and are bridged as-is (`update_thread` / `single_file_thread`).

use std::ffi::CStr;
use std::mem::size_of;
use std::os::raw::{c_char, c_int, c_long, c_void};
use std::ptr;

use obs_file_updater::file_updater;

use crate::abi_common::{bfree, bmalloc, bstrdup_bytes};
use crate::{LOG_INFO, LOG_WARNING};

// ---------------------------------------------------------------------------
// C constants (curl.h) — the libcurl X-macro scheme `CURLOPT(na, t, nu) na =
// t + nu`: the value is the argument TYPE (LONG 0 / OBJECTPOINT 10000 /
// FUNCTIONPOINT 20000) plus the option number, as defined by the system
// curl.h the conformance runner compiles against.

const CURLE_OK: c_int = 0;
const CURLOPT_URL: c_long = 10000 + 2; // STRINGPOINT
const CURLOPT_HTTPHEADER: c_long = 10000 + 23; // SLISTPOINT
const CURLOPT_ERRORBUFFER: c_long = 10000 + 10; // OBJECTPOINT
const CURLOPT_WRITEFUNCTION: c_long = 20000 + 11; // FUNCTIONPOINT
const CURLOPT_WRITEDATA: c_long = 10000 + 1; // CBPOINT
const CURLOPT_FAILONERROR: c_long = 45; // LONG
const CURLOPT_HEADERFUNCTION: c_long = 20000 + 79; // FUNCTIONPOINT
const CURLOPT_HEADERDATA: c_long = 10000 + 29; // CBPOINT
const CURLOPT_ACCEPT_ENCODING: c_long = 10000 + 102; // STRINGPOINT
const CURLOPT_NOSIGNAL: c_long = 99; // LONG
const CURLOPT_SSL_OPTIONS: c_long = 216; // VALUES (LONG)
const CURLINFO_RESPONSE_CODE: c_long = 0x200000 + 2; // CURLINFO_LONG + 2
// curl_obs_set_revoke_setting: the reference picks
// CURLSSLOPT_REVOKE_BEST_EFFORT when the local curl defines it, else
// CURLSSLOPT_NO_REVOKE. The conformance fake ignores the option; matching
// the local curl's revoke behaviour is a TLS detail outside S-024.
// shortcut: hard-coded NO_REVOKE, <track the local curl's best-effort
// constant if revoke behaviour must match it>
const CURLSSLOPT_NO_REVOKE: c_long = 2;

// ---------------------------------------------------------------------------
// Mirrors of the file-local reference types

/// `struct darray` of the reference (util/darray.h): 3 words.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct darray_c {
    array: *mut u8,
    num: usize,
    capacity: usize,
}

/// `struct file_download_data` (file-updater.h).
#[repr(C)]
pub struct file_download_data {
    pub name: *const c_char,
    pub version: c_int,
    pub buffer: darray_c,
}

type ConfirmCallback = Option<unsafe extern "C" fn(*mut c_void, *mut file_download_data) -> bool>;

/// The file-local `struct update_info` of file-updater.c — exact glibc
/// layout (the swap-mode runner carries the same C struct; the `fu-*`
/// vectors pin the round trip).
#[repr(C)]
pub struct update_info {
    error: [u8; 256],
    header: *mut c_void,
    file_data: darray_c,
    user_agent: *mut c_char,
    curl: *mut c_void,
    url: *mut c_char,
    local: *mut c_char,
    cache: *mut c_char,
    temp: *mut c_char,
    remote_url: *const c_char,
    local_package: *mut c_void,
    cache_package: *mut c_void,
    remote_package: *mut c_void,
    etag_local: *mut c_char,
    etag_remote: *mut c_char,
    callback: ConfirmCallback,
    param: *mut c_void,
    thread: u64,
    thread_created: bool,
    log_prefix: *mut c_char,
}

unsafe extern "C" {
    // obs-data.c (still linked in Phase 1; S-014 semantics).
    fn obs_data_create() -> *mut c_void;
    fn obs_data_create_from_json(json: *const c_char) -> *mut c_void;
    fn obs_data_create_from_json_file(path: *const c_char) -> *mut c_void;
    fn obs_data_release(data: *mut c_void);
    fn obs_data_addref(data: *mut c_void);
    fn obs_data_get_string(data: *mut c_void, name: *const c_char) -> *const c_char;
    fn obs_data_get_int(data: *mut c_void, name: *const c_char) -> i64;
    fn obs_data_get_array(data: *mut c_void, name: *const c_char) -> *mut c_void;
    fn obs_data_array_count(array: *mut c_void) -> usize;
    fn obs_data_array_item(array: *mut c_void, index: usize) -> *mut c_void;
    fn obs_data_set_string(data: *mut c_void, name: *const c_char, value: *const c_char);
    fn obs_data_save_json(data: *mut c_void, file: *const c_char) -> bool;

    // libcurl (the conformance runner links fakes; production links libcurl).
    fn curl_easy_init() -> *mut c_void;
    fn curl_easy_cleanup(handle: *mut c_void);
    fn curl_easy_setopt(handle: *mut c_void, option: c_long, ...) -> c_int;
    fn curl_easy_perform(handle: *mut c_void) -> c_int;
    fn curl_easy_getinfo(handle: *mut c_void, info: c_long, ...) -> c_int;
    fn curl_slist_append(list: *mut c_void, s: *const c_char) -> *mut c_void;
    fn curl_slist_free_all(list: *mut c_void);

    // The platform file API (the conformance runner links in-memory fakes;
    // production links libobs).
    fn os_mkdir(path: *const c_char) -> c_int;
    fn os_rmdir(path: *const c_char) -> c_int;
    fn os_copyfile(input: *const c_char, output: *const c_char) -> c_int;
    fn os_unlink(path: *const c_char) -> c_int;
    fn os_rename(from: *const c_char, to: *const c_char) -> c_int;
    fn os_quick_write_utf8_file(
        path: *const c_char,
        data: *const c_char,
        len: usize,
        marker: bool,
    ) -> bool;

    // base-logging.c.
    fn blog(level: c_int, format: *const c_char, ...) -> c_int;

    // pthreads.
    fn pthread_create(
        thread: *mut u64,
        attr: *const c_void,
        start: *mut c_void,
        arg: *mut c_void,
    ) -> c_int;
    fn pthread_join(thread: u64, value: *mut *mut c_void) -> c_int;
}

// ---------------------------------------------------------------------------
// Small pointer helpers (each a single, justified unsafe op)

/// The live C string at `p` as a lossy Rust `String` ("" for NULL).
fn c_str(p: *const c_char) -> String {
    if p.is_null() {
        return String::new();
    }
    // SAFETY: p is a live NUL-terminated C string (the callers guarantee it).
    unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned()
}

/// A bmem-owned copy of `s` (the reference's bstrdup allocation form).
fn bmem_str(s: &str) -> *mut c_char {
    // SAFETY: a fresh bmem allocation; the caller owns and frees it.
    unsafe { bstrdup_bytes(s.as_bytes()) }
}

/// `blog(LOG_WARNING/LOG_INFO, "%s%s", log_prefix, msg)` of the reference
/// (the warn/info macros).
fn blog_msg(level: c_int, info: *mut update_info, msg: &str) {
    let msg_c = bmem_str(msg);
    // SAFETY: log_prefix is a live bmem string and msg_c a live C string
    // for the duration of the call (freed after).
    unsafe { blog(level, c"%s%s".as_ptr(), (*info).log_prefix, msg_c) };
    // SAFETY: msg_c came from the C bmem allocator.
    unsafe { bfree(msg_c) };
}

// ---------------------------------------------------------------------------
// darray helpers (C bmem allocator — the runner's C callbacks and the port
// share the same darray, so both must use the C allocator)

/// `da_resize(v, 0)`: drop the contents, keep the buffer.
fn da_reset(da: &mut darray_c) {
    da.num = 0;
}

/// `da_push_back_array`: grow (C bmalloc/bfree) and copy the bytes.
fn da_push_bytes(da: &mut darray_c, bytes: &[u8]) {
    // SAFETY: the array, if non-NULL, was allocated by the C bmem
    // allocator (or a previous call here) and holds `num` valid bytes.
    unsafe {
        if da.num + bytes.len() > da.capacity {
            let mut new_cap = da.capacity;
            if new_cap == 0 {
                new_cap = 8;
            }
            while new_cap < da.num + bytes.len() {
                new_cap *= 2;
            }
            let new_array = bmalloc(new_cap);
            if !da.array.is_null() {
                ptr::copy_nonoverlapping(da.array, new_array, da.num);
                bfree(da.array);
            }
            da.array = new_array;
            da.capacity = new_cap;
        }
        ptr::copy_nonoverlapping(bytes.as_ptr(), da.array.add(da.num), bytes.len());
        da.num += bytes.len();
    }
}

/// `da_free`: release the array (if any).
fn da_free(da: &mut darray_c) {
    // SAFETY: the array was allocated by the C bmem allocator (or is NULL).
    unsafe {
        if !da.array.is_null() {
            bfree(da.array);
            da.array = ptr::null_mut();
        }
    }
}

/// The NUL-terminated error string in `info->error` (a fixed 256-byte
/// buffer — `CStr::from_bytes_with_nul` cannot be used on the whole array,
/// it requires the NUL to be the final byte of the slice).
fn error_string(info: *mut update_info) -> String {
    // SAFETY: error is a valid 256-byte buffer on the live info.
    let sl = unsafe { std::slice::from_raw_parts((*info).error.as_ptr(), 256) };
    let end = sl.iter().position(|&b| b == 0).unwrap_or(256);
    String::from_utf8_lossy(&sl[..end]).into_owned()
}

// ---------------------------------------------------------------------------
// HTTP plumbing (the reference's http_write / http_header / do_http_request)

/// `http_write`: append the body chunk to `file_data`.
unsafe extern "C" fn http_write(
    ptr_: *mut c_char,
    size: usize,
    nmemb: usize,
    param: *mut c_void,
) -> usize {
    let total = size * nmemb;
    if total > 0 {
        let info = param as *mut update_info;
        // SAFETY: the curl callback passes `total` valid bytes and a live
        // update_info; the darray is grown via the C bmem allocator.
        unsafe {
            da_push_bytes(
                &mut (*info).file_data,
                std::slice::from_raw_parts(ptr_ as *const u8, total),
            );
        }
    }
    total
}

/// `http_header`: an exact `"ETag: "` prefix with a non-empty remainder is
/// cleaned (truncate at `\r`, then `\n`) and stored as `etag_remote`.
unsafe extern "C" fn http_header(
    buffer: *mut c_char,
    size: usize,
    nitems: usize,
    param: *mut c_void,
) -> usize {
    // SAFETY: the curl callback passes `size * nitems` valid bytes.
    let line = unsafe {
        std::str::from_utf8(std::slice::from_raw_parts(
            buffer as *const u8,
            size * nitems,
        ))
    };
    if let Some(cleaned) = line.ok().and_then(file_updater::clean_etag_header) {
        let info = param as *mut update_info;
        // SAFETY: info is live; etag_remote, if set, was bmem-allocated by a
        // previous call (the reference leaks it; the port frees the previous
        // copy — the conformance fake sends at most one ETag line per
        // request, so the vectors are unaffected).
        unsafe {
            if !(*info).etag_remote.is_null() {
                bfree((*info).etag_remote);
            }
            (*info).etag_remote = bstrdup_bytes(cleaned.as_bytes());
        }
    }
    nitems * size
}

/// `do_http_request`: reset the body buffer, issue the request (the ETag
/// header callback only while `remote_url` is unset), and append the NUL
/// terminator on success.
fn do_http_request(info: *mut update_info, url: *const c_char, response_code: *mut c_long) -> bool {
    // SAFETY: info is live with a valid file_data darray; the curl handle,
    // header list and error buffer are its live fields; the option values
    // are the CURL constants above (the conformance fake stores the URL,
    // headers, error buffer and callbacks).
    unsafe {
        da_reset(&mut (*info).file_data);
        curl_easy_setopt((*info).curl, CURLOPT_URL, url);
        curl_easy_setopt((*info).curl, CURLOPT_HTTPHEADER, (*info).header);
        curl_easy_setopt(
            (*info).curl,
            CURLOPT_ERRORBUFFER,
            (*info).error.as_mut_ptr(),
        );
        curl_easy_setopt(
            (*info).curl,
            CURLOPT_WRITEFUNCTION,
            http_write as unsafe extern "C" fn(*mut c_char, usize, usize, *mut c_void) -> usize
                as *mut c_void,
        );
        curl_easy_setopt((*info).curl, CURLOPT_WRITEDATA, info);
        curl_easy_setopt((*info).curl, CURLOPT_FAILONERROR, 1);
        curl_easy_setopt((*info).curl, CURLOPT_NOSIGNAL, 1);
        curl_easy_setopt((*info).curl, CURLOPT_ACCEPT_ENCODING, c"".as_ptr());
        // curl_obs_set_revoke_setting(handle).
        curl_easy_setopt((*info).curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NO_REVOKE);

        if (*info).remote_url.is_null() {
            // We only care about headers from the main package file.
            curl_easy_setopt(
                (*info).curl,
                CURLOPT_HEADERFUNCTION,
                http_header as unsafe extern "C" fn(*mut c_char, usize, usize, *mut c_void) -> usize
                    as *mut c_void,
            );
            curl_easy_setopt((*info).curl, CURLOPT_HEADERDATA, info);
        }

        let code = curl_easy_perform((*info).curl);
        if code != CURLE_OK {
            // warn("Remote update of URL \"%s\" failed: %s", url, info->error)
            let err = error_string(info);
            blog_msg(
                LOG_WARNING,
                info,
                &format!("Remote update of URL \"{}\" failed: {}", c_str(url), err),
            );
            return false;
        }

        if curl_easy_getinfo((*info).curl, CURLINFO_RESPONSE_CODE, response_code) != CURLE_OK {
            return false;
        }

        if *response_code >= 400 {
            // warn("Remote update of URL \"%s\" failed: HTTP/%ld", url, *response_code)
            blog_msg(
                LOG_WARNING,
                info,
                &format!(
                    "Remote update of URL \"{}\" failed: HTTP/{}",
                    c_str(url),
                    *response_code
                ),
            );
            return false;
        }

        da_push_bytes(&mut (*info).file_data, &[0]);
    }
    true
}

// ---------------------------------------------------------------------------
// The update algorithm (the reference's static functions)

/// `get_path` with a bmem-owned result (the reference's allocation form).
fn get_path_owned(dir: *const c_char, file: *const c_char) -> *mut c_char {
    bmem_str(&file_updater::get_path(&c_str(dir), &c_str(file)))
}

fn get_package(base_path: *const c_char, file: *const c_char) -> *mut c_void {
    let full = get_path_owned(base_path, file);
    // SAFETY: full is a live NUL-terminated path; obs-data.c reads it
    // through the (fake) platform file API.
    let package = unsafe { obs_data_create_from_json_file(full) };
    // SAFETY: full came from the C bmem allocator.
    unsafe { bfree(full) };
    package
}

fn copy_local_to_cache(info: *mut update_info, file: *const c_char) {
    // SAFETY: local/cache/temp are live bmem strings.
    let (local_path, cache_path, temp_path) = unsafe {
        (
            get_path_owned((*info).local, file),
            get_path_owned((*info).cache, file),
            get_path_owned((*info).temp, file),
        )
    };
    // SAFETY: the three paths are live C strings; the (fake) file API
    // traces and performs the copy (the result codes are ignored, as in
    // the reference).
    unsafe {
        os_copyfile(local_path, temp_path);
        os_unlink(cache_path);
        os_rename(temp_path, cache_path);
    }
    // SAFETY: all three paths came from the C bmem allocator.
    unsafe {
        bfree(local_path);
        bfree(cache_path);
        bfree(temp_path);
    }
}

struct FileUpdateData {
    name: *const c_char,
    version: i32,
    newer: bool,
    found: bool,
}

/// `newer_than_cache`: the first cache file with the same name sets
/// found/newer and stops the enumeration.
fn newer_than_cache(cache_file: *mut c_void, data: &mut FileUpdateData) -> bool {
    // SAFETY: cache_file is a live obs_data item; S-014: a missing string
    // reads as "" (never NULL).
    unsafe {
        let name = obs_data_get_string(cache_file, c"name".as_ptr());
        let version = obs_data_get_int(cache_file, c"version".as_ptr()) as i32;
        if !name.is_null() && !data.name.is_null() && c_str(name) == c_str(data.name) {
            data.found = true;
            data.newer = data.version > version;
            false
        } else {
            true
        }
    }
}

/// `enum_files`: call `f` for each entry of `package.files` in order;
/// `f` returning false stops the enumeration (the reference's bool
/// continue contract).
fn enum_files(package: *mut c_void, mut f: impl FnMut(*mut c_void) -> bool) {
    // SAFETY: package is a live obs_data object (possibly NULL — a NULL
    // package reads a NULL array and no-ops, as in the reference); the
    // array and each item are released as the reference does.
    unsafe {
        let array = obs_data_get_array(package, c"files".as_ptr());
        if array.is_null() {
            return;
        }
        let num = obs_data_array_count(array);
        for i in 0..num {
            let file = obs_data_array_item(array, i);
            let cont = f(file);
            obs_data_release(file);
            if !cont {
                break;
            }
        }
        obs_data_release(array);
    }
}

/// `update_files_to_local`: copy one local file to the cache when it is
/// missing there or newer.
fn update_files_to_local(info: *mut update_info, local_file: *mut c_void) -> bool {
    // SAFETY: local_file is a live obs_data item (S-014 defaults for the
    // missing fields).
    let (name, version) = unsafe {
        (
            obs_data_get_string(local_file, c"name".as_ptr()),
            obs_data_get_int(local_file, c"version".as_ptr()) as i32,
        )
    };
    let mut data = FileUpdateData {
        name,
        version,
        newer: false,
        found: false,
    };
    // SAFETY: cache_package is live (possibly NULL); data outlives the call.
    enum_files(unsafe { (*info).cache_package }, |cf| {
        newer_than_cache(cf, &mut data)
    });
    if data.newer || !data.found {
        // SAFETY: info is live; name is a live obs_data string.
        copy_local_to_cache(info, name)
    }
    true
}

/// `update_local_version` (S-024 §4.10 step 2).
fn update_local_version(info: *mut update_info) -> i32 {
    // SAFETY: the package objects are live (a missing file reads as a NULL
    // package; obs_data_get_int on NULL reads 0, matching the reference).
    let (local_version, cache_version) = unsafe {
        (
            obs_data_get_int((*info).local_package, c"version".as_ptr()) as i32,
            obs_data_get_int((*info).cache_package, c"version".as_ptr()) as i32,
        )
    };

    if cache_version < local_version {
        // SAFETY: local_package is live; the closure releases each item.
        enum_files(unsafe { (*info).local_package }, |lf| {
            update_files_to_local(info, lf)
        });
        // SAFETY: info is live; package.json is a literal.
        copy_local_to_cache(info, c"package.json".as_ptr());

        // cache_package := local_package (release/addref, as in C).
        // SAFETY: both packages are live obs_data objects.
        unsafe {
            obs_data_release((*info).cache_package);
            obs_data_addref((*info).local_package);
            (*info).cache_package = (*info).local_package;
        }
        return local_version;
    }
    cache_version
}

/// `write_file_data`: write `file_data[0..num-1]` (the NUL terminator is
/// not part of the file).
fn write_file_data(info: *mut update_info, base_path: *const c_char, file: *const c_char) {
    let full_path = get_path_owned(base_path, file);
    // SAFETY: file_data holds at least the NUL terminator appended by
    // do_http_request (num >= 1); the (fake) file API traces the write.
    unsafe {
        os_quick_write_utf8_file(
            full_path,
            (*info).file_data.array as *const c_char,
            (*info).file_data.num - 1,
            false,
        );
    }
    // SAFETY: full_path came from the C bmem allocator.
    unsafe { bfree(full_path) };
}

/// `replace_file`: unlink the destination, rename the source over it.
fn replace_file(src_base_path: *const c_char, dst_base_path: *const c_char, file: *const c_char) {
    let src_path = get_path_owned(src_base_path, file);
    let dst_path = get_path_owned(dst_base_path, file);
    // SAFETY: both paths are live C strings; the (fake) file API traces
    // the unlink/rename (the reference ignores the result codes).
    unsafe {
        os_unlink(dst_path);
        os_rename(src_path, dst_path);
    }
    // SAFETY: both paths came from the C bmem allocator.
    unsafe {
        bfree(dst_path);
        bfree(src_path);
    }
}

/// `update_save_metadata` (S-024 §4.10): `cache + "meta.json"` with NO
/// separator (Q-12) when an ETag was received.
fn update_save_metadata(info: *mut update_info) {
    // SAFETY: info is live.
    if unsafe { (*info).etag_remote.is_null() } {
        return;
    }
    // The reference builds `dstr_copy(cache); dstr_cat("meta.json")` —
    // no separator.
    // SAFETY: cache is a live bmem string.
    let path_c = bmem_str(&format!("{}meta.json", unsafe { c_str((*info).cache) }));
    // SAFETY: the fresh obs_data object is released below; save_json
    // writes through the (fake) file API (the `fs write` trace).
    unsafe {
        let data = obs_data_create();
        obs_data_set_string(data, c"etag".as_ptr(), (*info).etag_remote);
        obs_data_save_json(data, path_c);
        obs_data_release(data);
    }
    // SAFETY: path_c came from the C bmem allocator.
    unsafe { bfree(path_c) };
}

/// `update_remote_files`: fetch one remote file when it is newer than (or
/// missing from) the cache, offer it to the callback, write it to temp
/// and move it into the cache.
fn update_remote_files(info: *mut update_info, remote_file: *mut c_void) -> bool {
    // SAFETY: remote_file is a live obs_data item (S-014 defaults).
    let (name, version) = unsafe {
        (
            obs_data_get_string(remote_file, c"name".as_ptr()),
            obs_data_get_int(remote_file, c"version".as_ptr()) as i32,
        )
    };
    let mut data = FileUpdateData {
        name,
        version,
        newer: false,
        found: false,
    };
    // SAFETY: cache_package is live (possibly NULL); data outlives the call.
    enum_files(unsafe { (*info).cache_package }, |cf| {
        newer_than_cache(cf, &mut data)
    });
    if !data.newer && data.found {
        return true;
    }

    // do_relative_http_request: GET remote_url/name; success and 200.
    // SAFETY: info is live; remote_url is a live obs_data string (set in
    // update_remote_version).
    let file_url = unsafe { get_path_owned((*info).remote_url, name) };
    let mut response_code: c_long = 0;
    let fetched = do_http_request(info, file_url, &mut response_code) && response_code == 200;
    // SAFETY: file_url came from the C bmem allocator.
    unsafe { bfree(file_url) };
    if !fetched {
        return true;
    }

    // SAFETY: the callback (if any) was registered by the reference
    // caller and takes (param, file_download_data*).
    if let Some(callback) = unsafe { (*info).callback } {
        let mut download_data = file_download_data {
            name: data.name,
            version: data.version,
            // SAFETY: info is live; file_data is a valid darray.
            buffer: unsafe { (*info).file_data },
        };
        // SAFETY: download_data is live for the call; the callback may
        // replace buffer (adopted below, as in the reference).
        let confirm = unsafe { callback((*info).param, &mut download_data) };
        // SAFETY: info is live; the buffer was adopted from download_data.
        unsafe {
            (*info).file_data = download_data.buffer;
        }
        if !confirm {
            // info("Update file '%s' (version %d) rejected", name, version)
            blog_msg(
                LOG_INFO,
                info,
                &format!(
                    "Update file '{}' (version {}) rejected",
                    c_str(data.name),
                    data.version
                ),
            );
            return true;
        }
    }

    // SAFETY: info is live; name is a live obs_data string; temp is a
    // live bmem string.
    unsafe { write_file_data(info, (*info).temp, name) };
    // SAFETY: temp/cache are live bmem strings.
    unsafe { replace_file((*info).temp, (*info).cache, name) };

    // info("Successfully updated file '%s' (version %d)", name, version)
    blog_msg(
        LOG_INFO,
        info,
        &format!(
            "Successfully updated file '{}' (version {})",
            c_str(name),
            version
        ),
    );
    true
}

/// `update_remote_version` (S-024 §4.10 step 3).
fn update_remote_version(info: *mut update_info, cur_version: i32) {
    let mut response_code: c_long = 0;
    // SAFETY: url is a live bmem string.
    if !unsafe { do_http_request(info, (*info).url, &mut response_code) } {
        return;
    }
    if response_code == 304 {
        return;
    }

    // The body must start with '{' (a missing body is an absent array).
    // SAFETY: file_data is a valid darray.
    let not_json = unsafe { (*info).file_data.array.is_null() || *(*info).file_data.array != b'{' };
    if not_json {
        // warn("Remote package does not exist or is not valid json")
        blog_msg(
            LOG_WARNING,
            info,
            "Remote package does not exist or is not valid json",
        );
        return;
    }

    // SAFETY: etag_remote/file_data were set by do_http_request.
    update_save_metadata(info);

    // Parse the body as JSON.
    // SAFETY: the body bytes are a NUL-terminated C string (do_http_request
    // appended the terminator).
    let remote_package =
        unsafe { obs_data_create_from_json((*info).file_data.array as *const c_char) };
    if remote_package.is_null() {
        // warn("Failed to initialize remote package json")
        blog_msg(
            LOG_WARNING,
            info,
            "Failed to initialize remote package json",
        );
        return;
    }
    // SAFETY: remote_package is a live obs_data object (replaces the
    // previous one — the reference leaks it, the port releases it; at most
    // one remote package lives per run, so vectors are unaffected).
    unsafe {
        if !(*info).remote_package.is_null() {
            obs_data_release((*info).remote_package);
        }
        (*info).remote_package = remote_package;
    }

    // SAFETY: remote_package is a live obs_data object.
    let remote_version = unsafe { obs_data_get_int(remote_package, c"version".as_ptr()) } as i32;
    if remote_version <= cur_version {
        return;
    }

    // SAFETY: info is live; the body was validated above; temp is a live
    // bmem string; package.json is a literal.
    unsafe { write_file_data(info, (*info).temp, c"package.json".as_ptr()) };

    // SAFETY: S-014: a missing "url" reads as "" (never NULL) — the
    // reference's NULL check is for a non-string field; an empty url still
    // flows into the (failing) file GETs, as in the reference.
    let remote_url = unsafe { obs_data_get_string(remote_package, c"url".as_ptr()) };
    // SAFETY: info is live.
    unsafe {
        (*info).remote_url = remote_url;
    }
    if remote_url.is_null() {
        // warn("No remote url in package file")
        blog_msg(LOG_WARNING, info, "No remote url in package file");
        return;
    }

    // Download the new files.
    // SAFETY: remote_package is live; the closure releases each item.
    enum_files(remote_package, |rf| update_remote_files(info, rf));

    // SAFETY: temp/cache are live bmem strings.
    unsafe { replace_file((*info).temp, (*info).cache, c"package.json".as_ptr()) };

    // info("Successfully updated package (version %d)", remote_version)
    blog_msg(
        LOG_INFO,
        info,
        &format!("Successfully updated package (version {remote_version})"),
    );
}

/// `init_update` (S-024 §4.10 step 1): the client, the local/cache
/// packages, the meta.json ETag (Q-16) and the header list
/// (If-None-Match first, then User-Agent).
fn init_update(info: *mut update_info) -> bool {
    // SAFETY: curl_easy_init returns a live handle (or NULL — the
    // conformance fake never fails).
    let curl = unsafe { curl_easy_init() };
    if curl.is_null() {
        // warn("Could not initialize Curl")
        blog_msg(LOG_WARNING, info, "Could not initialize Curl");
        return false;
    }
    // SAFETY: the handle is live for the run (destroyed by update_info_destroy).
    unsafe {
        (*info).curl = curl;
    }

    // SAFETY: local is a live bmem string; obs-data.c reads through the
    // (fake) file API (the `fs read` traces).
    let local_package = unsafe { get_package((*info).local, c"package.json".as_ptr()) };
    // SAFETY: cache is a live bmem string.
    let cache_package = unsafe { get_package((*info).cache, c"package.json".as_ptr()) };

    // SAFETY: cache is a live bmem string.
    let metadata = unsafe { get_package((*info).cache, c"meta.json".as_ptr()) };
    // SAFETY: metadata, if live, is released below; a missing etag key
    // reads as "" (Q-16 — the header is still sent, with an empty value).
    if !metadata.is_null() {
        // SAFETY: metadata is live (checked above); S-014: a missing etag
        // key reads as "" (never NULL).
        let etag = unsafe { obs_data_get_string(metadata, c"etag".as_ptr()) };
        let inm = bmem_str(&format!("If-None-Match: {}", c_str(etag)));
        // SAFETY: the header list is built with the C curl slist API.
        unsafe {
            (*info).etag_local = bstrdup_bytes(c_str(etag).as_bytes());
            (*info).header = curl_slist_append((*info).header, inm);
        }
        // SAFETY: inm came from the C bmem allocator; metadata is a live
        // obs_data object.
        unsafe {
            bfree(inm);
            obs_data_release(metadata);
        }
    }

    // SAFETY: user_agent is a live bmem string.
    let ua = bmem_str(&format!("User-Agent: {}", unsafe {
        c_str((*info).user_agent)
    }));
    // SAFETY: the header list continues to be owned by info.
    unsafe {
        (*info).header = curl_slist_append((*info).header, ua);
    }
    // SAFETY: ua came from the C bmem allocator.
    unsafe { bfree(ua) };

    // SAFETY: the packages are stored on info (released by destroy).
    unsafe {
        (*info).local_package = local_package;
        (*info).cache_package = cache_package;
    }
    true
}

// ---------------------------------------------------------------------------
// The C ABI surface (the file-updater.h declarations + the file-local
// thread entry points the conformance runner drives directly)

/// `void *update_thread(void *data)` — the reference's thread body,
/// driven synchronously by the conformance runner.
///
/// # Safety
/// `data` must be a live `update_info` (bzalloc'ed by the runner or
/// allocated by `update_info_create`); it is not freed here.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn update_thread(data: *mut c_void) -> *mut c_void {
    let info = data as *mut update_info;
    // SAFETY: data is a live update_info (bzalloc'ed by the runner /
    // allocated by update_info_create); init_update returns true only when
    // curl succeeded, so the fields it sets are live below.
    unsafe {
        if init_update(info) {
            let cur_version = update_local_version(info);
            update_remote_version(info, cur_version);
            // SAFETY: temp is a live bmem string.
            os_rmdir((*info).temp);
            if !(*info).etag_local.is_null() {
                bfree((*info).etag_local);
            }
            if !(*info).etag_remote.is_null() {
                bfree((*info).etag_remote);
            }
        }
    }
    ptr::null_mut()
}

/// `void *single_file_thread(void *data)` (S-024 §4.10 single-file mode).
///
/// # Safety
/// `data` must be a live `update_info` allocated by
/// `update_info_create_single`; it is not freed here.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn single_file_thread(data: *mut c_void) -> *mut c_void {
    let info = data as *mut update_info;
    // SAFETY: data is a live update_info; curl_easy_init never fails in
    // the conformance fake.
    let curl = unsafe { curl_easy_init() };
    if curl.is_null() {
        // warn("Could not initialize Curl")
        blog_msg(LOG_WARNING, info, "Could not initialize Curl");
        return ptr::null_mut();
    }
    // SAFETY: the handle is live until update_info_destroy.
    unsafe {
        (*info).curl = curl;
    }

    let mut response_code: c_long = 0;
    // SAFETY: url is a live bmem string.
    let ok = unsafe { do_http_request(info, (*info).url, &mut response_code) };
    // An empty body (first byte NUL) skips the callback, as in the
    // reference (`!info->file_data.array[0]`).
    // SAFETY: file_data is a valid darray.
    let has_body = unsafe { !(*info).file_data.array.is_null() && *(*info).file_data.array != 0 };
    if ok && has_body {
        // SAFETY: info is live; url is a live bmem string; file_data is a
        // valid darray.
        let mut download_data = unsafe {
            file_download_data {
                name: (*info).url as *const c_char,
                version: 0,
                buffer: (*info).file_data,
            }
        };
        // SAFETY: the callback is the runner's (non-NULL in single-file
        // mode); download_data is live for the call.
        unsafe {
            if let Some(callback) = (*info).callback {
                let _ = callback((*info).param, &mut download_data);
                (*info).file_data = download_data.buffer;
            }
        }
    }
    ptr::null_mut()
}

/// `void update_info_destroy(update_info_t *info)` (file-updater.h).
///
/// # Safety
/// `info` must be a live update_info (or NULL).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn update_info_destroy(info: *mut update_info) {
    if info.is_null() {
        return;
    }
    // SAFETY: info is live; every field it owns was allocated by the C
    // bmem allocator / the C curl / obs_data APIs (or is NULL).
    unsafe {
        if (*info).thread_created {
            pthread_join((*info).thread, ptr::null_mut());
        }
        da_free(&mut (*info).file_data);
        bfree((*info).log_prefix);
        bfree((*info).user_agent);
        bfree((*info).temp);
        bfree((*info).cache);
        bfree((*info).local);
        bfree((*info).url);
        if !(*info).header.is_null() {
            curl_slist_free_all((*info).header);
        }
        if !(*info).curl.is_null() {
            curl_easy_cleanup((*info).curl);
        }
        if !(*info).local_package.is_null() {
            obs_data_release((*info).local_package);
        }
        if !(*info).cache_package.is_null() {
            obs_data_release((*info).cache_package);
        }
        if !(*info).remote_package.is_null() {
            obs_data_release((*info).remote_package);
        }
        bfree(info);
    }
}

/// `char *get_path(const char *dir, const char *file)` — file-local in
/// the C reference; bmem-allocated result (the reference's allocation
/// form, the runner bfree's it).
///
/// # Safety
/// `dir` and `file` must be live NUL-terminated C strings.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn get_path(dir: *const c_char, file: *const c_char) -> *mut c_char {
    get_path_owned(dir, file)
}

/// `update_info_t *update_info_create(...)` (file-updater.h).
///
/// # Safety
/// All string parameters must be live NUL-terminated C strings (or NULL
/// for `log_prefix`).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn update_info_create(
    log_prefix: *const c_char,
    user_agent: *const c_char,
    update_url: *const c_char,
    local_dir: *const c_char,
    cache_dir: *const c_char,
    confirm_callback: ConfirmCallback,
    param: *mut c_void,
) -> *mut update_info {
    let log_prefix = if log_prefix.is_null() {
        c"".as_ptr()
    } else {
        log_prefix
    };
    // SAFETY: cache_dir is a live C string.
    if unsafe { os_mkdir(cache_dir) } < 0 {
        // blog(LOG_WARNING, "%sCould not create cache directory %s", ...)
        blog_msg_c(
            LOG_WARNING,
            log_prefix,
            &format!("Could not create cache directory {}", c_str(cache_dir)),
        );
        return ptr::null_mut();
    }

    // dir = cache_dir + (separator unless it ends with / or \) + ".temp"
    let dir = bmem_str(&file_updater::get_path(&c_str(cache_dir), ".temp"));
    // SAFETY: dir is a live C string.
    if unsafe { os_mkdir(dir) } < 0 {
        // blog(LOG_WARNING, "%sCould not create temp directory %s", ..., cache_dir)
        blog_msg_c(
            LOG_WARNING,
            log_prefix,
            &format!("Could not create temp directory {}", c_str(cache_dir)),
        );
        // SAFETY: dir came from the C bmem allocator.
        unsafe { bfree(dir) };
        return ptr::null_mut();
    }

    // SAFETY: a zeroed update_info of the exact C size (all fields are
    // integers, pointers or POD — zero is a valid pattern); the bstrdup /
    // get_path allocations below are live bmem strings owned by info.
    unsafe {
        let raw = bmalloc(size_of::<update_info>()) as *mut update_info;
        ptr::write_bytes(raw as *mut u8, 0, size_of::<update_info>());
        (*raw).log_prefix = bmem_str(&c_str(log_prefix));
        (*raw).user_agent = bmem_str(&c_str(user_agent));
        (*raw).temp = dir;
        (*raw).local = bmem_str(&c_str(local_dir));
        (*raw).cache = bmem_str(&c_str(cache_dir));
        (*raw).url = get_path_owned(update_url, c"package.json".as_ptr());
        (*raw).callback = confirm_callback;
        (*raw).param = param;
        let result = pthread_create(
            &mut (*raw).thread,
            ptr::null(),
            update_thread as unsafe extern "C" fn(*mut c_void) -> *mut c_void as *mut c_void,
            raw as *mut c_void,
        );
        if result == 0 {
            (*raw).thread_created = true;
        }
        raw
    }
}

/// `update_info_t *update_info_create_single(...)` (file-updater.h).
///
/// # Safety
/// All string parameters must be live NUL-terminated C strings (or NULL
/// for `log_prefix`).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn update_info_create_single(
    log_prefix: *const c_char,
    user_agent: *const c_char,
    file_url: *const c_char,
    confirm_callback: ConfirmCallback,
    param: *mut c_void,
) -> *mut update_info {
    let log_prefix = if log_prefix.is_null() {
        c"".as_ptr()
    } else {
        log_prefix
    };
    // SAFETY: a zeroed update_info of the exact C size; the allocations
    // below are live bmem strings owned by info.
    unsafe {
        let raw = bmalloc(size_of::<update_info>()) as *mut update_info;
        ptr::write_bytes(raw as *mut u8, 0, size_of::<update_info>());
        (*raw).log_prefix = bmem_str(&c_str(log_prefix));
        (*raw).user_agent = bmem_str(&c_str(user_agent));
        (*raw).url = bmem_str(&c_str(file_url));
        (*raw).callback = confirm_callback;
        (*raw).param = param;
        let result = pthread_create(
            &mut (*raw).thread,
            ptr::null(),
            single_file_thread as unsafe extern "C" fn(*mut c_void) -> *mut c_void as *mut c_void,
            raw as *mut c_void,
        );
        if result == 0 {
            (*raw).thread_created = true;
        }
        raw
    }
}

/// `blog` with an explicit `log_prefix` C string (the create() path logs
/// before an update_info exists).
fn blog_msg_c(level: c_int, log_prefix: *const c_char, msg: &str) {
    let msg_c = bmem_str(msg);
    // SAFETY: log_prefix and msg_c are live C strings for the call.
    unsafe { blog(level, c"%s%s".as_ptr(), log_prefix, msg_c) };
    // SAFETY: msg_c came from the C bmem allocator.
    unsafe { bfree(msg_c) };
}

// Test stand-ins for the C externs the MSVC backend keeps in the test
// binary (it does not dead-strip `#[no_mangle]` functions the way the ELF
// backends do, so on Windows every C service the bridge references must
// resolve here; on ELF the references are stripped and the conformance
// runner's fakes / the C shim provide them instead). The obs_data/curl/os_
// names are OBS- or libcurl-specific — no system-symbol collisions; the
// pthread stand-ins are Windows-only (on ELF the system pthreads provide
// them and a local definition would shadow them for the whole test
// binary, including the threading tests).
#[cfg(test)]
mod test_standin {
    use super::*;

    #[unsafe(no_mangle)]
    pub extern "C" fn obs_data_create() -> *mut c_void {
        ptr::null_mut()
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn obs_data_create_from_json(_json: *const c_char) -> *mut c_void {
        ptr::null_mut()
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn obs_data_create_from_json_file(_path: *const c_char) -> *mut c_void {
        ptr::null_mut()
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn obs_data_release(_data: *mut c_void) {}

    #[unsafe(no_mangle)]
    pub extern "C" fn obs_data_addref(_data: *mut c_void) {}

    #[unsafe(no_mangle)]
    pub extern "C" fn obs_data_get_string(
        _data: *mut c_void,
        _name: *const c_char,
    ) -> *const c_char {
        ptr::null()
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn obs_data_get_int(_data: *mut c_void, _name: *const c_char) -> i64 {
        0
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn obs_data_get_array(_data: *mut c_void, _name: *const c_char) -> *mut c_void {
        ptr::null_mut()
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn obs_data_array_count(_array: *mut c_void) -> usize {
        0
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn obs_data_array_item(_array: *mut c_void, _index: usize) -> *mut c_void {
        ptr::null_mut()
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn obs_data_set_string(
        _data: *mut c_void,
        _name: *const c_char,
        _value: *const c_char,
    ) {
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn obs_data_save_json(_data: *mut c_void, _file: *const c_char) -> bool {
        false
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn curl_easy_init() -> *mut c_void {
        ptr::null_mut()
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn curl_easy_cleanup(_handle: *mut c_void) {}

    #[unsafe(no_mangle)]
    pub unsafe extern "C" fn curl_easy_setopt(
        _handle: *mut c_void,
        _option: c_long,
        _: ... // variadic stand-in: the value is never read.
    ) -> c_int {
        -1
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn curl_easy_perform(_handle: *mut c_void) -> c_int {
        6 // CURLE_COULDNT_CONNECT
    }

    #[unsafe(no_mangle)]
    pub unsafe extern "C" fn curl_easy_getinfo(
        _handle: *mut c_void,
        _info: c_long,
        _: ... // variadic stand-in: the value is never read.
    ) -> c_int {
        -1
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn curl_slist_append(_list: *mut c_void, _s: *const c_char) -> *mut c_void {
        ptr::null_mut()
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn curl_slist_free_all(_list: *mut c_void) {}

    #[unsafe(no_mangle)]
    pub extern "C" fn os_mkdir(_path: *const c_char) -> c_int {
        -1
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn os_rmdir(_path: *const c_char) -> c_int {
        -1
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn os_copyfile(_input: *const c_char, _output: *const c_char) -> c_int {
        -1
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn os_unlink(_path: *const c_char) -> c_int {
        -1
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn os_rename(_from: *const c_char, _to: *const c_char) -> c_int {
        -1
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn os_quick_write_utf8_file(
        _path: *const c_char,
        _data: *const c_char,
        _len: usize,
        _marker: bool,
    ) -> bool {
        false
    }

    #[cfg(windows)]
    #[unsafe(no_mangle)]
    pub extern "C" fn pthread_create(
        _thread: *mut u64,
        _attr: *const c_void,
        _start: *mut c_void,
        _arg: *mut c_void,
    ) -> c_int {
        1 // failure: no threads in the test build
    }

    #[cfg(windows)]
    #[unsafe(no_mangle)]
    pub extern "C" fn pthread_join(_thread: u64, _value: *mut *mut c_void) -> c_int {
        1
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The offsets the swap-mode runner fills/reads directly (it carries
    /// the same C struct; the fu_* vectors pin the round trip) — pin the
    /// critical ones against layout drift.
    #[test]
    fn layout_pins() {
        use std::mem::offset_of;
        assert_eq!(offset_of!(update_info, error), 0);
        assert_eq!(offset_of!(update_info, header), 256);
        assert_eq!(offset_of!(update_info, file_data), 264);
        assert_eq!(offset_of!(update_info, user_agent), 288);
        assert_eq!(offset_of!(update_info, url), 304);
        assert_eq!(offset_of!(update_info, remote_url), 336);
        assert_eq!(offset_of!(update_info, callback), 384);
        assert_eq!(offset_of!(update_info, log_prefix), 416);
        assert_eq!(size_of::<update_info>(), 424);
        assert_eq!(size_of::<file_download_data>(), 40);
        assert_eq!(offset_of!(file_download_data, buffer), 16);
    }

    /// darray growth/reset through the C bmem allocator (the test
    /// stand-in in abi_cf_lexer).
    #[test]
    fn darray_push_and_reset() {
        let mut da = darray_c::default();
        // SAFETY: fresh zeroed darray; the push/reset/free helpers are the
        // only users of its pointer.
        unsafe {
            da_push_bytes(&mut da, b"hello");
            assert_eq!(std::slice::from_raw_parts(da.array, da.num), b"hello");
            da_push_bytes(&mut da, b" world!");
            assert_eq!(
                std::slice::from_raw_parts(da.array, da.num),
                b"hello world!"
            );
            da_reset(&mut da);
            assert_eq!(da.num, 0);
            da_free(&mut da);
            assert!(da.array.is_null());
        }
    }
}
