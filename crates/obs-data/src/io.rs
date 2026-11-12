//! S-014 §6.2–§6.5 — the JSON entry points (`get_json*`,
//! `create_from_json`, file save/load) and the file helpers they use
//! (S-023-R57/R58/R60, with `os_safe_replace` from WP-11).
//!
//! The log lines the C emits on failure are not wired yet (this crate
//! has no logger; the FFI layer will).

use std::fs;
use std::io::Write;
use std::path::Path;

use obs_util::os_fs::{os_file_exists, os_safe_replace};

use crate::json::{self, Json};
use crate::model::{
    Layer, ObsData, ObsDataArray, Ty, Value, array_push_back, effective_value, set_layer_value,
};

// ---------------------------------------------------------------------------
// file helpers (S-023-R57/R58/R60; the platform crate takes them over)
// ---------------------------------------------------------------------------

/// S-023-R57: the whole file as UTF-8 (one leading BOM stripped); `None`
/// when the file cannot be opened, or is empty / BOM-only. Invalid UTF-8
/// is a documented deviation (a `String` cannot carry raw bytes; the FFI
/// layer handles bytes directly).
pub fn os_quick_read_utf8_file(path: &str) -> Option<String> {
    let b = fs::read(path).ok()?;
    let b = if b.len() >= 3 && b[..3] == [0xEF, 0xBB, 0xBF] {
        &b[3..]
    } else {
        &b[..]
    };
    if b.is_empty() {
        return None;
    }
    String::from_utf8(b.to_vec()).ok()
}

/// S-023-R58: binary write (create/truncate); `marker` prepends a UTF-8
/// BOM. The result of the final flush/close is not checked (S-023 Q-13).
pub fn os_quick_write_utf8_file(path: &str, data: &[u8], marker: bool) -> bool {
    let Ok(mut f) = fs::File::create(path) else {
        return false;
    };
    if marker && f.write_all(&[0xEF, 0xBB, 0xBF]).is_err() {
        return false;
    }
    if !data.is_empty() && f.write_all(data).is_err() {
        return false;
    }
    let _ = f.flush();
    true
}

fn ext_path(path: &str, ext: &str) -> String {
    if ext.starts_with('.') {
        format!("{path}{ext}")
    } else {
        format!("{path}.{ext}")
    }
}

/// S-023-R60: write `<path>.<temp_ext>` and replace the target, keeping
/// the target as `<path>.<backup_ext>` when a non-empty backup extension
/// is given. `false` for a NULL/empty temp extension (a LOG_ERROR in C)
/// or a failed write; the temp file stays when the replace fails.
pub fn os_quick_write_utf8_file_safe(
    path: &str,
    data: &[u8],
    marker: bool,
    temp_ext: Option<&str>,
    backup_ext: Option<&str>,
) -> bool {
    let Some(temp_ext) = temp_ext.filter(|e| !e.is_empty()) else {
        return false;
    };
    let temp = ext_path(path, temp_ext);
    if !os_quick_write_utf8_file(&temp, data, marker) {
        return false;
    }
    let backup = backup_ext
        .filter(|e| !e.is_empty())
        .map(|e| ext_path(path, e));
    os_safe_replace(
        Path::new(path),
        Path::new(&temp),
        backup.as_deref().map(Path::new),
    ) == 0
}

// ---------------------------------------------------------------------------
// JSON writer (S-014 §6.2, R80–R83)
// ---------------------------------------------------------------------------

fn shell_zero(ty: Ty) -> Value {
    match ty {
        Ty::String => Value::Str(Vec::new()),
        // a shell's number type is INVALID: written as a DOUBLE 0.0
        Ty::Number => Value::Dbl(0.0),
        Ty::Bool => Value::Bool(false),
        Ty::Obj => Value::Obj(None),
        Ty::Arr => Value::Arr(None),
    }
}

fn value_to_json(v: Value, with_defaults: bool) -> Json {
    match v {
        Value::Str(b) => Json::Str(b),
        Value::Int(i) => Json::Int(i),
        Value::Dbl(f) => Json::Real(f),
        Value::Bool(b) => Json::Bool(b),
        // a present NULL object -> `null`
        Value::Obj(None) => Json::Null,
        // a present NULL array -> `[]`
        Value::Arr(None) => Json::Arr(Vec::new()),
        Value::Obj(Some(o)) => obj_to_json(&ObsData { inner: o }, with_defaults),
        Value::Arr(Some(a)) => {
            let elems = a.borrow().elements.clone();
            Json::Arr(
                elems
                    .iter()
                    .map(|e| obj_to_json(&ObsData { inner: e.clone() }, with_defaults))
                    .collect(),
            )
        }
    }
}

fn obj_to_json(d: &ObsData, with_defaults: bool) -> Json {
    let items = d.inner.borrow().items.clone();
    let members: Vec<(Vec<u8>, Json)> = items
        .iter()
        .filter_map(|it| {
            let b = it.borrow();
            if !with_defaults && b.user.is_none() {
                return None;
            }
            // R82: the effective value; a shell reads as the type's zero
            let v = effective_value(&b)
                .cloned()
                .unwrap_or_else(|| shell_zero(b.ty));
            Some((b.name.clone(), value_to_json(v, with_defaults)))
        })
        .collect();
    Json::Obj(members)
}

fn get_json_impl(d: &ObsData, with_defaults: bool, pretty: bool) -> String {
    let text = json::render(&obj_to_json(d, with_defaults), pretty);
    // QUIRK Q4: the buffer is stored with the object for get_last_json
    d.inner.borrow_mut().last_json = Some(text.clone());
    text
}

/// R80: compact, user values only.
pub fn data_get_json(d: &ObsData) -> String {
    get_json_impl(d, false, false)
}

/// R80: pretty, user values only.
pub fn data_get_json_pretty(d: &ObsData) -> String {
    get_json_impl(d, false, true)
}

/// R80: compact, all items (defaults included, R82).
pub fn data_get_json_defaults(d: &ObsData) -> String {
    get_json_impl(d, true, false)
}

/// R80: pretty, all items.
pub fn data_get_json_pretty_defaults(d: &ObsData) -> String {
    get_json_impl(d, true, true)
}

/// R81: the buffer last produced by any `get_json*` on this object.
pub fn data_get_last_json(d: &ObsData) -> Option<String> {
    d.inner.borrow().last_json.clone()
}

// ---------------------------------------------------------------------------
// JSON reader (S-014 §6.3)
// ---------------------------------------------------------------------------

fn json_to_data(inner: &crate::model::DataRc, name: &[u8], v: &Json) {
    // QUIRK Q2: the conversion is the ordinary user setters (§6.1 applies)
    match v {
        Json::Obj(members) => {
            let o = ObsData::create();
            for (k, v2) in members {
                json_to_data(&o.inner, k, v2);
            }
            set_layer_value(inner, Layer::User, name, Value::Obj(Some(o.inner.clone())));
        }
        // QUIRK Q3: only object elements survive an array
        Json::Arr(elems) => {
            let arr = ObsDataArray::create();
            for e in elems {
                if let Json::Obj(members) = &e {
                    let o = ObsData::create();
                    for (k, v2) in members {
                        json_to_data(&o.inner, k, v2);
                    }
                    array_push_back(&arr, &o);
                }
            }
            set_layer_value(
                inner,
                Layer::User,
                name,
                Value::Arr(Some(arr.inner.clone())),
            );
        }
        Json::Str(b) => set_layer_value(inner, Layer::User, name, Value::Str(b.clone())),
        Json::Int(i) => set_layer_value(inner, Layer::User, name, Value::Int(*i)),
        Json::Real(f) => set_layer_value(inner, Layer::User, name, Value::Dbl(*f)),
        Json::Bool(b) => set_layer_value(inner, Layer::User, name, Value::Bool(*b)),
        // `null` -> a present NULL object (R26)
        Json::Null => set_layer_value(inner, Layer::User, name, Value::Obj(None)),
    }
}

/// S-014 §6.3: parse `text` and convert the members (document order) to
/// user values. A top-level **array** yields a valid **empty** object
/// (QUIRK Q2); any jansson-level parse failure is an error (the C logs
/// one LOG_ERROR line, which is not wired yet).
#[allow(clippy::result_unit_err)] // unit error is the contract (S-014 logs one line)
pub fn data_create_from_json(text: &str) -> Result<ObsData, ()> {
    let v = json::parse(text.as_bytes())?;
    let d = ObsData::create();
    if let Json::Obj(members) = v {
        for (k, v2) in members {
            json_to_data(&d.inner, &k, &v2);
        }
    }
    Ok(d)
}

// ---------------------------------------------------------------------------
// files (S-014 §6.4, R70–R72)
// ---------------------------------------------------------------------------

/// R70: read the file (R57) and `create_from_json` it. Unreadable or
/// empty: `None` with no log; invalid text: the parse error (logged in
/// the C).
pub fn data_create_from_json_file(path: &str) -> Option<ObsData> {
    let text = os_quick_read_utf8_file(path)?;
    data_create_from_json(&text).ok()
}

/// R71: like R70, but when the load fails and a non-empty `ext` is given,
/// the backup `path.ext` (a leading dot is not doubled) is **renamed over**
/// `path` (a LOG_WARNING in C) and the load retried; otherwise `None`.
pub fn data_create_from_json_file_safe(path: &str, ext: &str) -> Option<ObsData> {
    let r = data_create_from_json_file(path);
    if r.is_none() && !ext.is_empty() {
        let backup = ext_path(path, ext);
        if os_file_exists(Path::new(&backup)) {
            // the backup is consumed even if it is corrupt; the rename
            // works also when the target is missing
            if fs::rename(&backup, path).is_ok() {
                return data_create_from_json_file(path);
            }
        }
    }
    r
}

/// R72: write the compact JSON (no BOM); `false` when the write fails.
pub fn data_save_json(d: &ObsData, path: &str) -> bool {
    let text = get_json_impl(d, false, false);
    os_quick_write_utf8_file(path, text.as_bytes(), false)
}

/// R72: safe save of the compact JSON (S-023-R60); a NULL/empty temp
/// extension is a failure (the target is untouched).
pub fn data_save_json_safe(
    d: &ObsData,
    path: &str,
    temp_ext: Option<&str>,
    backup_ext: Option<&str>,
) -> bool {
    let text = get_json_impl(d, false, false);
    os_quick_write_utf8_file_safe(path, text.as_bytes(), false, temp_ext, backup_ext)
}

/// R72: safe save of the pretty JSON.
pub fn data_save_json_pretty_safe(
    d: &ObsData,
    path: &str,
    temp_ext: Option<&str>,
    backup_ext: Option<&str>,
) -> bool {
    let text = get_json_impl(d, true, true);
    os_quick_write_utf8_file_safe(path, text.as_bytes(), false, temp_ext, backup_ext)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::api::{
        data_get_array, data_get_bool, data_get_double, data_get_int, data_get_obj,
        data_get_string, data_has_user_value, data_set_bool, data_set_default_int,
        data_set_default_obj, data_set_default_string, data_set_double, data_set_int, data_set_obj,
        data_set_string, data_unset_user_value,
    };
    use crate::model::data_count;
    use std::path::PathBuf;

    fn d() -> ObsData {
        ObsData::create()
    }

    fn tmp(name: &str) -> PathBuf {
        let p = std::env::temp_dir().join(format!("obs-data-io-{}-{}", std::process::id(), name));
        let _ = fs::remove_dir_all(&p);
        fs::create_dir_all(&p).unwrap();
        p
    }

    #[test]
    fn r80_get_json_user_only() {
        let o = d();
        data_set_string(&o, "s", "text");
        data_set_int(&o, "i", 42);
        data_set_double(&o, "d", 2.5);
        data_set_bool(&o, "b", true);
        let n = d();
        data_set_int(&n, "x", 1);
        data_set_obj(&o, "n", Some(&n));
        // a default-only item is skipped
        data_set_default_string(&o, "onlydef", "d");
        assert_eq!(
            data_get_json(&o),
            r#"{"s":"text","i":42,"d":2.5,"b":true,"n":{"x":1}}"#
        );
        // empty object
        let e = d();
        assert_eq!(data_get_json(&e), "{}");
    }

    #[test]
    fn r80_pretty_layout() {
        let o = d();
        data_set_int(&o, "a", 1);
        let n = d();
        data_set_string(&n, "k", "v");
        data_set_obj(&o, "b", Some(&n));
        assert_eq!(
            data_get_json_pretty(&o),
            "{\n    \"a\": 1,\n    \"b\": {\n        \"k\": \"v\"\n    }\n}"
        );
        let e = d();
        assert_eq!(data_get_json_pretty(&e), "{}");
    }

    #[test]
    fn r82_defaults_and_shells() {
        let o = d();
        data_set_string(&o, "u", "user");
        data_set_default_string(&o, "u", "def");
        data_set_default_int(&o, "di", 7);
        data_set_int(&o, "shell", 1);
        data_unset_user_value(&o, "shell"); // a number shell
        data_set_obj(&o, "no", None); // a present NULL
        assert_eq!(data_get_json(&o), r#"{"u":"user","no":null}"#);
        assert_eq!(
            data_get_json_defaults(&o),
            r#"{"u":"user","di":7,"shell":0.0,"no":null}"#
        );
        // a default object is written recursively with defaults
        let p = d();
        let inner = d();
        data_set_default_int(&inner, "x", 3);
        data_set_obj(&p, "inner", Some(&inner));
        data_set_default_obj(&p, "definner", Some(&inner));
        assert_eq!(
            data_get_json_defaults(&p),
            r#"{"inner":{"x":3},"definner":{"x":3}}"#
        );
    }

    #[test]
    fn q5_non_finite_real_omitted() {
        let o = d();
        data_set_double(&o, "a", 1.0);
        data_set_double(&o, "inf", f64::INFINITY);
        data_set_double(&o, "nan", f64::NAN);
        data_set_double(&o, "b", 2.0);
        assert_eq!(data_get_json(&o), r#"{"a":1.0,"b":2.0}"#);
    }

    #[test]
    fn r81_last_json() {
        let o = d();
        assert!(data_get_last_json(&o).is_none(), "none yet");
        data_set_int(&o, "a", 1);
        assert_eq!(data_get_json(&o), r#"{"a":1}"#);
        assert_eq!(data_get_last_json(&o).as_deref(), Some(r#"{"a":1}"#));
        assert_eq!(data_get_json_pretty(&o), "{\n    \"a\": 1\n}");
        assert_eq!(
            data_get_last_json(&o).as_deref(),
            Some("{\n    \"a\": 1\n}"),
            "the pretty form replaced the stored buffer"
        );
    }

    #[test]
    fn q2_q3_reader_quirks() {
        // a top-level array: a valid empty object
        let o = data_create_from_json("[1, \"x\", true]").expect("Q2");
        assert_eq!(data_count(&o), 0);
        // arrays of non-objects are dropped
        let o = data_create_from_json(r#"{"a":[1,"x",true,null,[2],{"k":1}]}"#).expect("Q3");
        let arr = data_get_array(&o, "a").expect("the array stays");
        assert_eq!(crate::model::array_count(&arr), 1);
        let e = crate::model::array_item(&arr, 0).expect("the object element");
        assert_eq!(data_get_int(&e, "k"), 1);
        // null member: a present NULL object
        assert!(data_has_user_value(&o, "a"));
        let o = data_create_from_json(r#"{"n":null}"#).expect("null member");
        assert!(data_has_user_value(&o, "n"));
        assert!(data_get_obj(&o, "n").is_none());
    }

    #[test]
    fn r83_roundtrip_keeps_typing() {
        let o = d();
        data_set_int(&o, "i", 7);
        data_set_double(&o, "d", 2.0);
        data_set_string(&o, "s", "héllo");
        data_set_bool(&o, "b", false);
        let text = data_get_json(&o);
        assert_eq!(text, r#"{"i":7,"d":2.0,"s":"héllo","b":false}"#);
        let r = data_create_from_json(&text).expect("re-read");
        assert_eq!(data_get_int(&r, "i"), 7);
        let it = crate::model::data_item_byname(&r, "i").unwrap();
        assert_eq!(
            crate::model::data_item_numtype(&it),
            crate::model::NumType::Int
        );
        let it = crate::model::data_item_byname(&r, "d").unwrap();
        assert_eq!(
            crate::model::data_item_numtype(&it),
            crate::model::NumType::Double,
            "2.0 stays a double"
        );
        assert_eq!(data_get_double(&r, "d"), 2.0);
        assert_eq!(data_get_string(&r, "s"), "héllo");
        assert!(!data_get_bool(&r, "b"));
    }

    #[test]
    fn reader_errors() {
        // top-level scalars / empty / trailing garbage
        assert!(data_create_from_json("5").is_err());
        assert!(data_create_from_json("\"x\"").is_err());
        assert!(data_create_from_json("true").is_err());
        assert!(data_create_from_json("null").is_err());
        assert!(data_create_from_json("").is_err());
        assert!(data_create_from_json("{}").is_ok());
        assert!(data_create_from_json("{} 5").is_err());
        // jansson specifics
        assert!(
            data_create_from_json(r#"{"a":1,"a":2}"#).is_err(),
            "duplicate keys"
        );
        assert!(
            data_create_from_json(r#"{"a":9223372036854775808}"#).is_err(),
            "i64 overflow"
        );
        assert!(
            data_create_from_json(r#"{"a":1e400}"#).is_err(),
            "real overflow"
        );
        assert!(
            data_create_from_json(r#"{"a":01}"#).is_err(),
            "leading zero"
        );
        assert!(data_create_from_json(r#"{"a":.5}"#).is_err());
        assert!(data_create_from_json(r#"{"a":1.}"#).is_err());
        assert!(data_create_from_json(r#"{"a":+5}"#).is_err());
        assert!(
            data_create_from_json(r#"{"a":"\u0000"}"#).is_err(),
            "NUL escape"
        );
        assert!(data_create_from_json(r#"{"a":"\u00e9"}"#).is_ok());
        assert!(
            data_create_from_json(r#"{"a":"\uD800"}"#).is_err(),
            "lone surrogate"
        );
        // underflow keeps the sign
        let o = data_create_from_json(r#"{"a":-1e-400}"#).expect("underflow");
        assert_eq!(data_get_double(&o, "a"), -0.0);
        // a BOM is an error for the reader
        assert!(data_create_from_json("\u{feff}{}").is_err());
    }

    #[cfg_attr(miri, ignore)]
    #[test]
    fn r70_r72_files() {
        let dir = tmp("files");
        let path = dir.join("conf.json").to_string_lossy().into_owned();
        let o = d();
        data_set_string(&o, "s", "text");
        data_set_int(&o, "i", 1);
        assert!(data_save_json(&o, &path));
        assert_eq!(
            os_quick_read_utf8_file(&path).as_deref(),
            Some(r#"{"s":"text","i":1}"#)
        );
        let r = data_create_from_json_file(&path).expect("re-read");
        assert_eq!(data_get_json(&r), r#"{"s":"text","i":1}"#);
        // empty data is written as {}
        let e = d();
        let epath = dir.join("empty.json").to_string_lossy().into_owned();
        assert!(data_save_json(&e, &epath));
        assert_eq!(os_quick_read_utf8_file(&epath).as_deref(), Some("{}"));
        // missing / empty files
        assert!(data_create_from_json_file(&dir.join("missing.json").to_string_lossy()).is_none());
        let blank = dir.join("blank.json");
        fs::write(&blank, b"").unwrap();
        assert!(data_create_from_json_file(&blank.to_string_lossy()).is_none());
        // invalid text
        let bad = dir.join("bad.json");
        fs::write(&bad, b"{not json").unwrap();
        assert!(data_create_from_json_file(&bad.to_string_lossy()).is_none());
        // a BOM is stripped on read
        let bom = dir.join("bom.json");
        fs::write(&bom, b"\xef\xbb\xbf{\"k\":1}").unwrap();
        let r = data_create_from_json_file(&bom.to_string_lossy()).expect("BOM stripped");
        assert_eq!(data_get_int(&r, "k"), 1);
        // an unwritable path fails cleanly
        assert!(!data_save_json(
            &o,
            &dir.join("nodir/x/conf.json").to_string_lossy()
        ));
        let _ = fs::remove_dir_all(&dir);
    }

    #[cfg_attr(miri, ignore)]
    #[test]
    fn r72_safe_save_and_r71_safe_load() {
        let dir = tmp("safe");
        let path = dir.join("p.json").to_string_lossy().into_owned();
        let o = d();
        data_set_int(&o, "i", 1);
        // first safe save: no target yet
        assert!(data_save_json_safe(&o, &path, Some("tmp"), Some("bak")));
        assert!(!dir.join("p.json.tmp").exists(), "the temp is gone");
        data_set_int(&o, "i", 2);
        assert!(data_save_json_safe(&o, &path, Some("tmp"), Some("bak")));
        // the previous target is the backup; a leading dot is not doubled
        let bak = dir.join("p.json.bak").to_string_lossy().into_owned();
        assert_eq!(os_quick_read_utf8_file(&bak).as_deref(), Some(r#"{"i":1}"#));
        assert_eq!(
            os_quick_read_utf8_file(&path).as_deref(),
            Some(r#"{"i":2}"#)
        );
        data_set_int(&o, "i", 3);
        assert!(data_save_json_safe(&o, &path, Some(".tmp"), Some(".bak")));
        assert!(!dir.join("p.json..tmp").exists(), "no doubled dot");
        assert_eq!(
            os_quick_read_utf8_file(&path).as_deref(),
            Some(r#"{"i":3}"#)
        );
        // an invalid temp extension: the target is untouched
        data_set_int(&o, "i", 4);
        assert!(!data_save_json_safe(&o, &path, None, Some("bak")));
        assert!(!data_save_json_safe(&o, &path, Some(""), Some("bak")));
        assert_eq!(
            os_quick_read_utf8_file(&path).as_deref(),
            Some(r#"{"i":3}"#)
        );
        // the safe loader: a good primary ignores the backup
        let p2 = dir.join("q.json").to_string_lossy().into_owned();
        fs::write(&p2, br#"{"from":"primary"}"#).unwrap();
        let p2b = dir.join("q.json.bak");
        fs::write(&p2b, br#"{"from":"backup"}"#).unwrap();
        let r = data_create_from_json_file_safe(&p2, "bak").expect("primary wins");
        assert_eq!(data_get_string(&r, "from"), "primary");
        assert!(p2b.exists(), "the backup stays when the primary is good");
        // a corrupt primary consumes the backup
        fs::write(&p2, b"{corrupt").unwrap();
        let r = data_create_from_json_file_safe(&p2, "bak")
            .expect("the backup is renamed over the primary");
        assert_eq!(data_get_string(&r, "from"), "backup");
        assert!(!p2b.exists(), "the backup is consumed");
        // a corrupt primary without a backup: still None
        fs::write(&p2, b"{corrupt").unwrap();
        assert!(data_create_from_json_file_safe(&p2, "bak").is_none());
        let _ = fs::remove_dir_all(&dir);
    }

    #[cfg_attr(miri, ignore)]
    #[test]
    fn r60_marker_and_len_zero() {
        let dir = tmp("marker");
        let path = dir.join("m.json").to_string_lossy().into_owned();
        assert!(os_quick_write_utf8_file(&path, b"hello", true));
        let b = fs::read(&path).unwrap();
        assert_eq!(b, b"\xef\xbb\xbfhello");
        // a zero-length write creates an empty file
        let path2 = dir.join("z.json").to_string_lossy().into_owned();
        assert!(os_quick_write_utf8_file(&path2, &[], false));
        assert_eq!(fs::read(&path2).unwrap(), b"");
        assert!(
            os_quick_read_utf8_file(&path2).is_none(),
            "empty reads as None (R57)"
        );
        let _ = fs::remove_dir_all(&dir);
    }
}
