//! S-014 §5.2–§5.5, §5.10–§5.11 — the typed object API: name-based and
//! item-based setters/getters for the three layers, `has`/`unset`,
//! vec/quat and frames-per-second helpers.
//!
//! NULL `D` / missing names read as the type's zero value (R20); the safe
//! API cannot pass them. Strings are byte strings internally; the safe
//! API renders them as UTF-8 (lossy for invalid bytes, which only the
//! FFI can produce).

use obs_math::{Quat, Vec2, Vec3, Vec4};

use crate::model::{
    ObsData, ObsDataArray, ObsDataItem, Value, data_find, effective_value, item_layer_value,
    item_set_layer, set_layer_value, unset_layer,
};

/// The `option` out-parameter of [`data_get_frames_per_second`] (R66):
/// mirrors the C `const char **` — `NotRequested` is a NULL pointer,
/// `Unset` is a passed pointer that the call did not set.
#[derive(Clone, PartialEq, Eq, Debug)]
pub enum FpsOption {
    /// The C NULL option pointer: the call never touches it.
    NotRequested,
    /// A passed pointer the call did not set (printed `<unset>` by the
    /// conformance runner).
    Unset,
    /// The option member's text (its `get_string`, `""` for a non-string).
    Value(String),
}

// ---------------------------------------------------------------------------
// name-based setters (R10–R12)
// ---------------------------------------------------------------------------

/// R10: user-layer setters. `set_obj`/`set_array` with `None` store a
/// **present** NULL object/array (R26).
pub fn data_set_string(d: &ObsData, name: &str, v: &str) {
    set_layer_value(
        &d.inner,
        crate::model::Layer::User,
        name.as_bytes(),
        Value::Str(v.as_bytes().to_vec()),
    )
}
pub fn data_set_int(d: &ObsData, name: &str, v: i64) {
    set_layer_value(
        &d.inner,
        crate::model::Layer::User,
        name.as_bytes(),
        Value::Int(v),
    )
}
/// The 64 bits are stored as the number's bits (the C `uint64`/`int64`
/// union overlap); [`data_get_uint`] gives them back unchanged.
pub fn data_set_uint(d: &ObsData, name: &str, v: u64) {
    set_layer_value(
        &d.inner,
        crate::model::Layer::User,
        name.as_bytes(),
        Value::Int(v as i64),
    )
}
pub fn data_set_double(d: &ObsData, name: &str, v: f64) {
    set_layer_value(
        &d.inner,
        crate::model::Layer::User,
        name.as_bytes(),
        Value::Dbl(v),
    )
}
pub fn data_set_bool(d: &ObsData, name: &str, v: bool) {
    set_layer_value(
        &d.inner,
        crate::model::Layer::User,
        name.as_bytes(),
        Value::Bool(v),
    )
}
pub fn data_set_obj(d: &ObsData, name: &str, v: Option<&ObsData>) {
    set_layer_value(
        &d.inner,
        crate::model::Layer::User,
        name.as_bytes(),
        Value::Obj(v.map(|o| o.inner.clone())),
    )
}
pub fn data_set_array(d: &ObsData, name: &str, v: Option<&ObsDataArray>) {
    set_layer_value(
        &d.inner,
        crate::model::Layer::User,
        name.as_bytes(),
        Value::Arr(v.map(|a| a.inner.clone())),
    )
}

/// R11: default-layer setters — an existing item of a different type is
/// ignored (including creating nothing).
pub fn data_set_default_string(d: &ObsData, name: &str, v: &str) {
    set_layer_value(
        &d.inner,
        crate::model::Layer::Default,
        name.as_bytes(),
        Value::Str(v.as_bytes().to_vec()),
    )
}
pub fn data_set_default_int(d: &ObsData, name: &str, v: i64) {
    set_layer_value(
        &d.inner,
        crate::model::Layer::Default,
        name.as_bytes(),
        Value::Int(v),
    )
}
pub fn data_set_default_uint(d: &ObsData, name: &str, v: u64) {
    set_layer_value(
        &d.inner,
        crate::model::Layer::Default,
        name.as_bytes(),
        Value::Int(v as i64),
    )
}
pub fn data_set_default_double(d: &ObsData, name: &str, v: f64) {
    set_layer_value(
        &d.inner,
        crate::model::Layer::Default,
        name.as_bytes(),
        Value::Dbl(v),
    )
}
pub fn data_set_default_bool(d: &ObsData, name: &str, v: bool) {
    set_layer_value(
        &d.inner,
        crate::model::Layer::Default,
        name.as_bytes(),
        Value::Bool(v),
    )
}
pub fn data_set_default_obj(d: &ObsData, name: &str, v: Option<&ObsData>) {
    set_layer_value(
        &d.inner,
        crate::model::Layer::Default,
        name.as_bytes(),
        Value::Obj(v.map(|o| o.inner.clone())),
    )
}
pub fn data_set_default_array(d: &ObsData, name: &str, v: Option<&ObsDataArray>) {
    set_layer_value(
        &d.inner,
        crate::model::Layer::Default,
        name.as_bytes(),
        Value::Arr(v.map(|a| a.inner.clone())),
    )
}

/// R12: autoselect-layer setters (deprecated in the C header, still
/// part of the API).
pub fn data_set_autoselect_string(d: &ObsData, name: &str, v: &str) {
    set_layer_value(
        &d.inner,
        crate::model::Layer::Autoselect,
        name.as_bytes(),
        Value::Str(v.as_bytes().to_vec()),
    )
}
pub fn data_set_autoselect_int(d: &ObsData, name: &str, v: i64) {
    set_layer_value(
        &d.inner,
        crate::model::Layer::Autoselect,
        name.as_bytes(),
        Value::Int(v),
    )
}
pub fn data_set_autoselect_uint(d: &ObsData, name: &str, v: u64) {
    set_layer_value(
        &d.inner,
        crate::model::Layer::Autoselect,
        name.as_bytes(),
        Value::Int(v as i64),
    )
}
pub fn data_set_autoselect_double(d: &ObsData, name: &str, v: f64) {
    set_layer_value(
        &d.inner,
        crate::model::Layer::Autoselect,
        name.as_bytes(),
        Value::Dbl(v),
    )
}
pub fn data_set_autoselect_bool(d: &ObsData, name: &str, v: bool) {
    set_layer_value(
        &d.inner,
        crate::model::Layer::Autoselect,
        name.as_bytes(),
        Value::Bool(v),
    )
}
pub fn data_set_autoselect_obj(d: &ObsData, name: &str, v: Option<&ObsData>) {
    set_layer_value(
        &d.inner,
        crate::model::Layer::Autoselect,
        name.as_bytes(),
        Value::Obj(v.map(|o| o.inner.clone())),
    )
}
pub fn data_set_autoselect_array(d: &ObsData, name: &str, v: Option<&ObsDataArray>) {
    set_layer_value(
        &d.inner,
        crate::model::Layer::Autoselect,
        name.as_bytes(),
        Value::Arr(v.map(|a| a.inner.clone())),
    )
}

// ---------------------------------------------------------------------------
// name-based getters (R20–R24, R26)
// ---------------------------------------------------------------------------

fn eff_value(d: &ObsData, name: &str) -> Option<Value> {
    data_find(d, name).and_then(|it| {
        let b = it.borrow();
        effective_value(&b).cloned()
    })
}

fn layer_of(d: &ObsData, name: &str, layer: crate::model::Layer) -> Option<Value> {
    data_find(d, name)
        .and_then(|it| item_layer_value(&crate::model::ObsDataItem { item: it }, layer))
}

/// R20: the effective value as a string; a missing item, a type mismatch
/// or an absent effective value reads as `""`.
pub fn data_get_string(d: &ObsData, name: &str) -> String {
    match eff_value(d, name) {
        Some(Value::Str(b)) => String::from_utf8_lossy(&b).into_owned(),
        _ => String::new(),
    }
}

/// R20/R22: the effective number; a double truncates toward zero (out of
/// `i64` range or NaN saturates, D5), a non-number item reads as `0`.
pub fn data_get_int(d: &ObsData, name: &str) -> i64 {
    match eff_value(d, name) {
        Some(Value::Int(i)) => i,
        Some(Value::Dbl(f)) => f.trunc() as i64,
        _ => 0,
    }
}

pub fn data_get_uint(d: &ObsData, name: &str) -> u64 {
    match eff_value(d, name) {
        Some(Value::Int(i)) => i as u64,
        Some(Value::Dbl(f)) => f.trunc() as u64,
        _ => 0,
    }
}

/// R20/R22: an int member converts to a double (round to nearest).
pub fn data_get_double(d: &ObsData, name: &str) -> f64 {
    match eff_value(d, name) {
        Some(Value::Int(i)) => i as f64,
        Some(Value::Dbl(f)) => f,
        _ => 0.0,
    }
}

pub fn data_get_bool(d: &ObsData, name: &str) -> bool {
    matches!(eff_value(d, name), Some(Value::Bool(true)))
}

/// R20/R02/R26: a **new reference** to the effective object; a present
/// NULL object (or a non-object) reads as `None` without falling back to
/// a default.
pub fn data_get_obj(d: &ObsData, name: &str) -> Option<ObsData> {
    match eff_value(d, name) {
        Some(Value::Obj(Some(o))) => Some(ObsData { inner: o }),
        _ => None,
    }
}

pub fn data_get_array(d: &ObsData, name: &str) -> Option<ObsDataArray> {
    match eff_value(d, name) {
        Some(Value::Arr(Some(a))) => Some(ObsDataArray { inner: a }),
        _ => None,
    }
}

/// R21: default-layer getters (absent layer or type mismatch: zero value).
pub fn data_get_default_string(d: &ObsData, name: &str) -> String {
    match layer_of(d, name, crate::model::Layer::Default) {
        Some(Value::Str(b)) => String::from_utf8_lossy(&b).into_owned(),
        _ => String::new(),
    }
}
pub fn data_get_default_int(d: &ObsData, name: &str) -> i64 {
    match layer_of(d, name, crate::model::Layer::Default) {
        Some(Value::Int(i)) => i,
        Some(Value::Dbl(f)) => f.trunc() as i64,
        _ => 0,
    }
}
pub fn data_get_default_uint(d: &ObsData, name: &str) -> u64 {
    match layer_of(d, name, crate::model::Layer::Default) {
        Some(Value::Int(i)) => i as u64,
        Some(Value::Dbl(f)) => f.trunc() as u64,
        _ => 0,
    }
}
pub fn data_get_default_double(d: &ObsData, name: &str) -> f64 {
    match layer_of(d, name, crate::model::Layer::Default) {
        Some(Value::Int(i)) => i as f64,
        Some(Value::Dbl(f)) => f,
        _ => 0.0,
    }
}
pub fn data_get_default_bool(d: &ObsData, name: &str) -> bool {
    matches!(
        layer_of(d, name, crate::model::Layer::Default),
        Some(Value::Bool(true))
    )
}
pub fn data_get_default_obj(d: &ObsData, name: &str) -> Option<ObsData> {
    match layer_of(d, name, crate::model::Layer::Default) {
        Some(Value::Obj(Some(o))) => Some(ObsData { inner: o }),
        _ => None,
    }
}
pub fn data_get_default_array(d: &ObsData, name: &str) -> Option<ObsDataArray> {
    match layer_of(d, name, crate::model::Layer::Default) {
        Some(Value::Arr(Some(a))) => Some(ObsDataArray { inner: a }),
        _ => None,
    }
}

/// R21: autoselect-layer getters.
pub fn data_get_autoselect_string(d: &ObsData, name: &str) -> String {
    match layer_of(d, name, crate::model::Layer::Autoselect) {
        Some(Value::Str(b)) => String::from_utf8_lossy(&b).into_owned(),
        _ => String::new(),
    }
}
pub fn data_get_autoselect_int(d: &ObsData, name: &str) -> i64 {
    match layer_of(d, name, crate::model::Layer::Autoselect) {
        Some(Value::Int(i)) => i,
        Some(Value::Dbl(f)) => f.trunc() as i64,
        _ => 0,
    }
}
pub fn data_get_autoselect_uint(d: &ObsData, name: &str) -> u64 {
    match layer_of(d, name, crate::model::Layer::Autoselect) {
        Some(Value::Int(i)) => i as u64,
        Some(Value::Dbl(f)) => f.trunc() as u64,
        _ => 0,
    }
}
pub fn data_get_autoselect_double(d: &ObsData, name: &str) -> f64 {
    match layer_of(d, name, crate::model::Layer::Autoselect) {
        Some(Value::Int(i)) => i as f64,
        Some(Value::Dbl(f)) => f,
        _ => 0.0,
    }
}
pub fn data_get_autoselect_bool(d: &ObsData, name: &str) -> bool {
    matches!(
        layer_of(d, name, crate::model::Layer::Autoselect),
        Some(Value::Bool(true))
    )
}
pub fn data_get_autoselect_obj(d: &ObsData, name: &str) -> Option<ObsData> {
    match layer_of(d, name, crate::model::Layer::Autoselect) {
        Some(Value::Obj(Some(o))) => Some(ObsData { inner: o }),
        _ => None,
    }
}
pub fn data_get_autoselect_array(d: &ObsData, name: &str) -> Option<ObsDataArray> {
    match layer_of(d, name, crate::model::Layer::Autoselect) {
        Some(Value::Arr(Some(a))) => Some(ObsDataArray { inner: a }),
        _ => None,
    }
}

// ---------------------------------------------------------------------------
// presence / unset (R24, R30)
// ---------------------------------------------------------------------------

/// R24: true iff the item exists and the user layer is present (a present
/// NULL object/array value counts).
pub fn data_has_user_value(d: &ObsData, name: &str) -> bool {
    data_find(d, name)
        .map(|it| it.borrow().user.is_some())
        .unwrap_or(false)
}
pub fn data_has_default_value(d: &ObsData, name: &str) -> bool {
    data_find(d, name)
        .map(|it| it.borrow().default.is_some())
        .unwrap_or(false)
}
pub fn data_has_autoselect_value(d: &ObsData, name: &str) -> bool {
    data_find(d, name)
        .map(|it| it.borrow().autoselect.is_some())
        .unwrap_or(false)
}

/// R30: remove just the user layer (the item stays, possibly as a shell).
pub fn data_unset_user_value(d: &ObsData, name: &str) {
    unset_layer(&d.inner, crate::model::Layer::User, name.as_bytes())
}
pub fn data_unset_default_value(d: &ObsData, name: &str) {
    unset_layer(&d.inner, crate::model::Layer::Default, name.as_bytes())
}
pub fn data_unset_autoselect_value(d: &ObsData, name: &str) {
    unset_layer(&d.inner, crate::model::Layer::Autoselect, name.as_bytes())
}

/// R31: remove the item (handles to it stay valid, R41).
pub fn data_erase(d: &ObsData, name: &str) {
    crate::model::erase_inner(&d.inner, name.as_bytes())
}

/// R31: remove the user layer of every item (the items stay, with their
/// default/autoselect layers, or as shells).
pub fn data_clear(d: &ObsData) {
    crate::model::clear_inner(&d.inner)
}

/// Pointer identity of two objects (the conformance runner's `same`).
pub fn data_same(a: &ObsData, b: &ObsData) -> bool {
    std::rc::Rc::ptr_eq(&a.inner, &b.inner)
}

// ---------------------------------------------------------------------------
// vec / quat helpers (R60–R61)
// ---------------------------------------------------------------------------

fn vec_obj(f: &[(f32, &str)]) -> ObsData {
    let o = ObsData::create();
    for (value, name) in f {
        set_layer_value(
            &o.inner,
            crate::model::Layer::User,
            name.as_bytes(),
            Value::Dbl(*value as f64),
        );
    }
    o
}

/// R60: stores a **new object** with double members `x`,`y` (in order) as
/// the user-layer value; floats widen exactly to doubles.
pub fn data_set_vec2(d: &ObsData, name: &str, v: &Vec2) {
    data_set_obj(d, name, Some(&vec_obj(&[(v.x, "x"), (v.y, "y")])))
}
pub fn data_set_vec3(d: &ObsData, name: &str, v: &Vec3) {
    data_set_obj(
        d,
        name,
        Some(&vec_obj(&[(v.x, "x"), (v.y, "y"), (v.z, "z")])),
    )
}
pub fn data_set_vec4(d: &ObsData, name: &str, v: &Vec4) {
    data_set_obj(
        d,
        name,
        Some(&vec_obj(&[(v.x, "x"), (v.y, "y"), (v.z, "z"), (v.w, "w")])),
    )
}
/// R60: a quat is stored like vec4 (member order `x,y,z,w`).
pub fn data_set_quat(d: &ObsData, name: &str, v: &Quat) {
    data_set_obj(
        d,
        name,
        Some(&vec_obj(&[(v.x, "x"), (v.y, "y"), (v.z, "z"), (v.w, "w")])),
    )
}

pub fn data_set_default_vec2(d: &ObsData, name: &str, v: &Vec2) {
    data_set_default_obj(d, name, Some(&vec_obj(&[(v.x, "x"), (v.y, "y")])))
}
pub fn data_set_default_vec3(d: &ObsData, name: &str, v: &Vec3) {
    data_set_default_obj(
        d,
        name,
        Some(&vec_obj(&[(v.x, "x"), (v.y, "y"), (v.z, "z")])),
    )
}
pub fn data_set_default_vec4(d: &ObsData, name: &str, v: &Vec4) {
    data_set_default_obj(
        d,
        name,
        Some(&vec_obj(&[(v.x, "x"), (v.y, "y"), (v.z, "z"), (v.w, "w")])),
    )
}
pub fn data_set_default_quat(d: &ObsData, name: &str, v: &Quat) {
    data_set_default_obj(
        d,
        name,
        Some(&vec_obj(&[(v.x, "x"), (v.y, "y"), (v.z, "z"), (v.w, "w")])),
    )
}

pub fn data_set_autoselect_vec2(d: &ObsData, name: &str, v: &Vec2) {
    data_set_autoselect_obj(d, name, Some(&vec_obj(&[(v.x, "x"), (v.y, "y")])))
}
pub fn data_set_autoselect_vec3(d: &ObsData, name: &str, v: &Vec3) {
    data_set_autoselect_obj(
        d,
        name,
        Some(&vec_obj(&[(v.x, "x"), (v.y, "y"), (v.z, "z")])),
    )
}
pub fn data_set_autoselect_vec4(d: &ObsData, name: &str, v: &Vec4) {
    data_set_autoselect_obj(
        d,
        name,
        Some(&vec_obj(&[(v.x, "x"), (v.y, "y"), (v.z, "z"), (v.w, "w")])),
    )
}
pub fn data_set_autoselect_quat(d: &ObsData, name: &str, v: &Quat) {
    data_set_autoselect_obj(
        d,
        name,
        Some(&vec_obj(&[(v.x, "x"), (v.y, "y"), (v.z, "z"), (v.w, "w")])),
    )
}

/// R61: the effective object's `x`,`y` as doubles narrowed to f32;
/// missing members read as 0. Returns false and leaves `out` **untouched**
/// when there is no object value (missing item, other type, NULL object).
pub fn data_get_vec2(d: &ObsData, name: &str, out: &mut Vec2) -> bool {
    let Some(o) = data_get_obj(d, name) else {
        return false;
    };
    out.x = data_get_double(&o, "x") as f32;
    out.y = data_get_double(&o, "y") as f32;
    true
}
pub fn data_get_vec3(d: &ObsData, name: &str, out: &mut Vec3) -> bool {
    let Some(o) = data_get_obj(d, name) else {
        return false;
    };
    out.x = data_get_double(&o, "x") as f32;
    out.y = data_get_double(&o, "y") as f32;
    out.z = data_get_double(&o, "z") as f32;
    // the C vec3 has no w component: out.w is untouched
    true
}
pub fn data_get_vec4(d: &ObsData, name: &str, out: &mut Vec4) -> bool {
    let Some(o) = data_get_obj(d, name) else {
        return false;
    };
    out.x = data_get_double(&o, "x") as f32;
    out.y = data_get_double(&o, "y") as f32;
    out.z = data_get_double(&o, "z") as f32;
    out.w = data_get_double(&o, "w") as f32;
    true
}
pub fn data_get_quat(d: &ObsData, name: &str, out: &mut Quat) -> bool {
    let Some(o) = data_get_obj(d, name) else {
        return false;
    };
    out.x = data_get_double(&o, "x") as f32;
    out.y = data_get_double(&o, "y") as f32;
    out.z = data_get_double(&o, "z") as f32;
    out.w = data_get_double(&o, "w") as f32;
    true
}

pub fn data_get_default_vec2(d: &ObsData, name: &str, out: &mut Vec2) -> bool {
    let Some(o) = data_get_default_obj(d, name) else {
        return false;
    };
    out.x = data_get_double(&o, "x") as f32;
    out.y = data_get_double(&o, "y") as f32;
    true
}
pub fn data_get_default_vec3(d: &ObsData, name: &str, out: &mut Vec3) -> bool {
    let Some(o) = data_get_default_obj(d, name) else {
        return false;
    };
    out.x = data_get_double(&o, "x") as f32;
    out.y = data_get_double(&o, "y") as f32;
    out.z = data_get_double(&o, "z") as f32;
    true
}
pub fn data_get_default_vec4(d: &ObsData, name: &str, out: &mut Vec4) -> bool {
    let Some(o) = data_get_default_obj(d, name) else {
        return false;
    };
    out.x = data_get_double(&o, "x") as f32;
    out.y = data_get_double(&o, "y") as f32;
    out.z = data_get_double(&o, "z") as f32;
    out.w = data_get_double(&o, "w") as f32;
    true
}
pub fn data_get_default_quat(d: &ObsData, name: &str, out: &mut Quat) -> bool {
    let Some(o) = data_get_default_obj(d, name) else {
        return false;
    };
    out.x = data_get_double(&o, "x") as f32;
    out.y = data_get_double(&o, "y") as f32;
    out.z = data_get_double(&o, "z") as f32;
    out.w = data_get_double(&o, "w") as f32;
    true
}

pub fn data_get_autoselect_vec2(d: &ObsData, name: &str, out: &mut Vec2) -> bool {
    let Some(o) = data_get_autoselect_obj(d, name) else {
        return false;
    };
    out.x = data_get_double(&o, "x") as f32;
    out.y = data_get_double(&o, "y") as f32;
    true
}
pub fn data_get_autoselect_vec3(d: &ObsData, name: &str, out: &mut Vec3) -> bool {
    let Some(o) = data_get_autoselect_obj(d, name) else {
        return false;
    };
    out.x = data_get_double(&o, "x") as f32;
    out.y = data_get_double(&o, "y") as f32;
    out.z = data_get_double(&o, "z") as f32;
    true
}
pub fn data_get_autoselect_vec4(d: &ObsData, name: &str, out: &mut Vec4) -> bool {
    let Some(o) = data_get_autoselect_obj(d, name) else {
        return false;
    };
    out.x = data_get_double(&o, "x") as f32;
    out.y = data_get_double(&o, "y") as f32;
    out.z = data_get_double(&o, "z") as f32;
    out.w = data_get_double(&o, "w") as f32;
    true
}
pub fn data_get_autoselect_quat(d: &ObsData, name: &str, out: &mut Quat) -> bool {
    let Some(o) = data_get_autoselect_obj(d, name) else {
        return false;
    };
    out.x = data_get_double(&o, "x") as f32;
    out.y = data_get_double(&o, "y") as f32;
    out.z = data_get_double(&o, "z") as f32;
    out.w = data_get_double(&o, "w") as f32;
    true
}

// ---------------------------------------------------------------------------
// frames-per-second helpers (R65–R66)
// ---------------------------------------------------------------------------

/// R65: stores a new object value. With `None` option: `{numerator,
/// denominator}` as **doubles** (in this order); with `Some(option)`:
/// `{option}` (the fps is ignored).
pub fn data_set_frames_per_second(d: &ObsData, name: &str, fps: (u32, u32), option: Option<&str>) {
    let o = ObsData::create();
    match option {
        None => {
            set_layer_value(
                &o.inner,
                crate::model::Layer::User,
                b"numerator",
                Value::Dbl(fps.0 as f64),
            );
            set_layer_value(
                &o.inner,
                crate::model::Layer::User,
                b"denominator",
                Value::Dbl(fps.1 as f64),
            );
        }
        Some(s) => {
            set_layer_value(
                &o.inner,
                crate::model::Layer::User,
                b"option",
                Value::Str(s.as_bytes().to_vec()),
            );
        }
    }
    data_set_obj(d, name, Some(&o));
}

pub fn data_set_default_frames_per_second(
    d: &ObsData,
    name: &str,
    fps: (u32, u32),
    option: Option<&str>,
) {
    let o = ObsData::create();
    match option {
        None => {
            set_layer_value(
                &o.inner,
                crate::model::Layer::User,
                b"numerator",
                Value::Dbl(fps.0 as f64),
            );
            set_layer_value(
                &o.inner,
                crate::model::Layer::User,
                b"denominator",
                Value::Dbl(fps.1 as f64),
            );
        }
        Some(s) => {
            set_layer_value(
                &o.inner,
                crate::model::Layer::User,
                b"option",
                Value::Str(s.as_bytes().to_vec()),
            );
        }
    }
    data_set_default_obj(d, name, Some(&o));
}

pub fn data_set_autoselect_frames_per_second(
    d: &ObsData,
    name: &str,
    fps: (u32, u32),
    option: Option<&str>,
) {
    let o = ObsData::create();
    match option {
        None => {
            set_layer_value(
                &o.inner,
                crate::model::Layer::User,
                b"numerator",
                Value::Dbl(fps.0 as f64),
            );
            set_layer_value(
                &o.inner,
                crate::model::Layer::User,
                b"denominator",
                Value::Dbl(fps.1 as f64),
            );
        }
        Some(s) => {
            set_layer_value(
                &o.inner,
                crate::model::Layer::User,
                b"option",
                Value::Str(s.as_bytes().to_vec()),
            );
        }
    }
    data_set_autoselect_obj(d, name, Some(&o));
}

/// R66: reads the effective object's fps.
///
/// - no object value: `false`, nothing touched.
/// - `option` requested (not `NotRequested`) and the object has a member
///   `option` (any type): it is stored (its `get_string`, `""` for a
///   non-string), the result is `true`, `fps` untouched.
/// - else `fps` not given: `false`.
/// - else a missing `numerator`/`denominator` member (type ignored):
///   `false`, untouched.
/// - else: both read with `get_int` (truncating), clamped to
///   `[0, 4294967295]`, stored even when the result is false; the result
///   is `numerator != 0 && denominator != 0`.
pub fn data_get_frames_per_second(
    d: &ObsData,
    name: &str,
    fps: Option<&mut (u32, u32)>,
    option: &mut FpsOption,
) -> bool {
    let Some(o) = data_get_obj(d, name) else {
        return false;
    };
    if *option == FpsOption::Unset && data_has_user_value(&o, "option") {
        *option = FpsOption::Value(data_get_string(&o, "option"));
        return true;
    }
    let Some(fps) = fps else {
        return false;
    };
    if !data_has_user_value(&o, "numerator") || !data_has_user_value(&o, "denominator") {
        return false;
    }
    let n = data_get_int(&o, "numerator").clamp(0, 4_294_967_295);
    let de = data_get_int(&o, "denominator").clamp(0, 4_294_967_295);
    *fps = (n as u32, de as u32);
    fps.0 != 0 && fps.1 != 0
}

// ---------------------------------------------------------------------------
// item-level typed API (R13 + item getters)
// ---------------------------------------------------------------------------

/// R13: the item-level user setters — name-based semantics applied to the
/// handle's item, never creating one; the handle follows a grown item.
pub fn data_item_set_string(it: &ObsDataItem, v: &str) {
    item_set_layer(
        &it.item,
        crate::model::Layer::User,
        Value::Str(v.as_bytes().to_vec()),
    )
}
pub fn data_item_set_int(it: &ObsDataItem, v: i64) {
    item_set_layer(&it.item, crate::model::Layer::User, Value::Int(v))
}
pub fn data_item_set_uint(it: &ObsDataItem, v: u64) {
    item_set_layer(&it.item, crate::model::Layer::User, Value::Int(v as i64))
}
pub fn data_item_set_double(it: &ObsDataItem, v: f64) {
    item_set_layer(&it.item, crate::model::Layer::User, Value::Dbl(v))
}
pub fn data_item_set_bool(it: &ObsDataItem, v: bool) {
    item_set_layer(&it.item, crate::model::Layer::User, Value::Bool(v))
}
pub fn data_item_set_obj(it: &ObsDataItem, v: Option<&ObsData>) {
    item_set_layer(
        &it.item,
        crate::model::Layer::User,
        Value::Obj(v.map(|o| o.inner.clone())),
    )
}
pub fn data_item_set_array(it: &ObsDataItem, v: Option<&ObsDataArray>) {
    item_set_layer(
        &it.item,
        crate::model::Layer::User,
        Value::Arr(v.map(|a| a.inner.clone())),
    )
}

pub fn data_item_set_default_string(it: &ObsDataItem, v: &str) {
    item_set_layer(
        &it.item,
        crate::model::Layer::Default,
        Value::Str(v.as_bytes().to_vec()),
    )
}
pub fn data_item_set_default_int(it: &ObsDataItem, v: i64) {
    item_set_layer(&it.item, crate::model::Layer::Default, Value::Int(v))
}
pub fn data_item_set_default_uint(it: &ObsDataItem, v: u64) {
    item_set_layer(&it.item, crate::model::Layer::Default, Value::Int(v as i64))
}
pub fn data_item_set_default_double(it: &ObsDataItem, v: f64) {
    item_set_layer(&it.item, crate::model::Layer::Default, Value::Dbl(v))
}
pub fn data_item_set_default_bool(it: &ObsDataItem, v: bool) {
    item_set_layer(&it.item, crate::model::Layer::Default, Value::Bool(v))
}
pub fn data_item_set_default_obj(it: &ObsDataItem, v: Option<&ObsData>) {
    item_set_layer(
        &it.item,
        crate::model::Layer::Default,
        Value::Obj(v.map(|o| o.inner.clone())),
    )
}
pub fn data_item_set_default_array(it: &ObsDataItem, v: Option<&ObsDataArray>) {
    item_set_layer(
        &it.item,
        crate::model::Layer::Default,
        Value::Arr(v.map(|a| a.inner.clone())),
    )
}

pub fn data_item_set_autoselect_string(it: &ObsDataItem, v: &str) {
    item_set_layer(
        &it.item,
        crate::model::Layer::Autoselect,
        Value::Str(v.as_bytes().to_vec()),
    )
}
pub fn data_item_set_autoselect_int(it: &ObsDataItem, v: i64) {
    item_set_layer(&it.item, crate::model::Layer::Autoselect, Value::Int(v))
}
pub fn data_item_set_autoselect_uint(it: &ObsDataItem, v: u64) {
    item_set_layer(
        &it.item,
        crate::model::Layer::Autoselect,
        Value::Int(v as i64),
    )
}
pub fn data_item_set_autoselect_double(it: &ObsDataItem, v: f64) {
    item_set_layer(&it.item, crate::model::Layer::Autoselect, Value::Dbl(v))
}
pub fn data_item_set_autoselect_bool(it: &ObsDataItem, v: bool) {
    item_set_layer(&it.item, crate::model::Layer::Autoselect, Value::Bool(v))
}
pub fn data_item_set_autoselect_obj(it: &ObsDataItem, v: Option<&ObsData>) {
    item_set_layer(
        &it.item,
        crate::model::Layer::Autoselect,
        Value::Obj(v.map(|o| o.inner.clone())),
    )
}
pub fn data_item_set_autoselect_array(it: &ObsDataItem, v: Option<&ObsDataArray>) {
    item_set_layer(
        &it.item,
        crate::model::Layer::Autoselect,
        Value::Arr(v.map(|a| a.inner.clone())),
    )
}

/// The item-level user getters (own layer, zero values like the name
/// forms).
pub fn data_item_get_string(it: &ObsDataItem) -> String {
    match item_layer_value(it, crate::model::Layer::User) {
        Some(Value::Str(b)) => String::from_utf8_lossy(&b).into_owned(),
        _ => String::new(),
    }
}
pub fn data_item_get_int(it: &ObsDataItem) -> i64 {
    match item_layer_value(it, crate::model::Layer::User) {
        Some(Value::Int(i)) => i,
        Some(Value::Dbl(f)) => f.trunc() as i64,
        _ => 0,
    }
}
pub fn data_item_get_uint(it: &ObsDataItem) -> u64 {
    match item_layer_value(it, crate::model::Layer::User) {
        Some(Value::Int(i)) => i as u64,
        Some(Value::Dbl(f)) => f.trunc() as u64,
        _ => 0,
    }
}
pub fn data_item_get_double(it: &ObsDataItem) -> f64 {
    match item_layer_value(it, crate::model::Layer::User) {
        Some(Value::Int(i)) => i as f64,
        Some(Value::Dbl(f)) => f,
        _ => 0.0,
    }
}
pub fn data_item_get_bool(it: &ObsDataItem) -> bool {
    matches!(
        item_layer_value(it, crate::model::Layer::User),
        Some(Value::Bool(true))
    )
}
pub fn data_item_get_obj(it: &ObsDataItem) -> Option<ObsData> {
    match item_layer_value(it, crate::model::Layer::User) {
        Some(Value::Obj(Some(o))) => Some(ObsData { inner: o }),
        _ => None,
    }
}
pub fn data_item_get_array(it: &ObsDataItem) -> Option<ObsDataArray> {
    match item_layer_value(it, crate::model::Layer::User) {
        Some(Value::Arr(Some(a))) => Some(ObsDataArray { inner: a }),
        _ => None,
    }
}

pub fn data_item_get_default_string(it: &ObsDataItem) -> String {
    match item_layer_value(it, crate::model::Layer::Default) {
        Some(Value::Str(b)) => String::from_utf8_lossy(&b).into_owned(),
        _ => String::new(),
    }
}
pub fn data_item_get_default_int(it: &ObsDataItem) -> i64 {
    match item_layer_value(it, crate::model::Layer::Default) {
        Some(Value::Int(i)) => i,
        Some(Value::Dbl(f)) => f.trunc() as i64,
        _ => 0,
    }
}
pub fn data_item_get_default_uint(it: &ObsDataItem) -> u64 {
    match item_layer_value(it, crate::model::Layer::Default) {
        Some(Value::Int(i)) => i as u64,
        Some(Value::Dbl(f)) => f.trunc() as u64,
        _ => 0,
    }
}
pub fn data_item_get_default_double(it: &ObsDataItem) -> f64 {
    match item_layer_value(it, crate::model::Layer::Default) {
        Some(Value::Int(i)) => i as f64,
        Some(Value::Dbl(f)) => f,
        _ => 0.0,
    }
}
pub fn data_item_get_default_bool(it: &ObsDataItem) -> bool {
    matches!(
        item_layer_value(it, crate::model::Layer::Default),
        Some(Value::Bool(true))
    )
}
pub fn data_item_get_default_obj(it: &ObsDataItem) -> Option<ObsData> {
    match item_layer_value(it, crate::model::Layer::Default) {
        Some(Value::Obj(Some(o))) => Some(ObsData { inner: o }),
        _ => None,
    }
}
pub fn data_item_get_default_array(it: &ObsDataItem) -> Option<ObsDataArray> {
    match item_layer_value(it, crate::model::Layer::Default) {
        Some(Value::Arr(Some(a))) => Some(ObsDataArray { inner: a }),
        _ => None,
    }
}

pub fn data_item_get_autoselect_string(it: &ObsDataItem) -> String {
    match item_layer_value(it, crate::model::Layer::Autoselect) {
        Some(Value::Str(b)) => String::from_utf8_lossy(&b).into_owned(),
        _ => String::new(),
    }
}
pub fn data_item_get_autoselect_int(it: &ObsDataItem) -> i64 {
    match item_layer_value(it, crate::model::Layer::Autoselect) {
        Some(Value::Int(i)) => i,
        Some(Value::Dbl(f)) => f.trunc() as i64,
        _ => 0,
    }
}
pub fn data_item_get_autoselect_uint(it: &ObsDataItem) -> u64 {
    match item_layer_value(it, crate::model::Layer::Autoselect) {
        Some(Value::Int(i)) => i as u64,
        Some(Value::Dbl(f)) => f.trunc() as u64,
        _ => 0,
    }
}
pub fn data_item_get_autoselect_double(it: &ObsDataItem) -> f64 {
    match item_layer_value(it, crate::model::Layer::Autoselect) {
        Some(Value::Int(i)) => i as f64,
        Some(Value::Dbl(f)) => f,
        _ => 0.0,
    }
}
pub fn data_item_get_autoselect_bool(it: &ObsDataItem) -> bool {
    matches!(
        item_layer_value(it, crate::model::Layer::Autoselect),
        Some(Value::Bool(true))
    )
}
pub fn data_item_get_autoselect_obj(it: &ObsDataItem) -> Option<ObsData> {
    match item_layer_value(it, crate::model::Layer::Autoselect) {
        Some(Value::Obj(Some(o))) => Some(ObsData { inner: o }),
        _ => None,
    }
}
pub fn data_item_get_autoselect_array(it: &ObsDataItem) -> Option<ObsDataArray> {
    match item_layer_value(it, crate::model::Layer::Autoselect) {
        Some(Value::Arr(Some(a))) => Some(ObsDataArray { inner: a }),
        _ => None,
    }
}

// ---------------------------------------------------------------------------
// item vec/quat/fps forms
// ---------------------------------------------------------------------------

pub fn data_item_set_vec2(it: &ObsDataItem, v: &Vec2) {
    data_item_set_obj(it, Some(&vec_obj(&[(v.x, "x"), (v.y, "y")])))
}
pub fn data_item_set_vec3(it: &ObsDataItem, v: &Vec3) {
    data_item_set_obj(it, Some(&vec_obj(&[(v.x, "x"), (v.y, "y"), (v.z, "z")])))
}
pub fn data_item_set_vec4(it: &ObsDataItem, v: &Vec4) {
    data_item_set_obj(
        it,
        Some(&vec_obj(&[(v.x, "x"), (v.y, "y"), (v.z, "z"), (v.w, "w")])),
    )
}
pub fn data_item_set_quat(it: &ObsDataItem, v: &Quat) {
    data_item_set_obj(
        it,
        Some(&vec_obj(&[(v.x, "x"), (v.y, "y"), (v.z, "z"), (v.w, "w")])),
    )
}

pub fn data_item_get_vec2(it: &ObsDataItem, out: &mut Vec2) -> bool {
    let Some(o) = data_item_get_obj(it) else {
        return false;
    };
    out.x = data_get_double(&o, "x") as f32;
    out.y = data_get_double(&o, "y") as f32;
    true
}
pub fn data_item_get_vec3(it: &ObsDataItem, out: &mut Vec3) -> bool {
    let Some(o) = data_item_get_obj(it) else {
        return false;
    };
    out.x = data_get_double(&o, "x") as f32;
    out.y = data_get_double(&o, "y") as f32;
    out.z = data_get_double(&o, "z") as f32;
    true
}
pub fn data_item_get_vec4(it: &ObsDataItem, out: &mut Vec4) -> bool {
    let Some(o) = data_item_get_obj(it) else {
        return false;
    };
    out.x = data_get_double(&o, "x") as f32;
    out.y = data_get_double(&o, "y") as f32;
    out.z = data_get_double(&o, "z") as f32;
    out.w = data_get_double(&o, "w") as f32;
    true
}
pub fn data_item_get_quat(it: &ObsDataItem, out: &mut Quat) -> bool {
    let Some(o) = data_item_get_obj(it) else {
        return false;
    };
    out.x = data_get_double(&o, "x") as f32;
    out.y = data_get_double(&o, "y") as f32;
    out.z = data_get_double(&o, "z") as f32;
    out.w = data_get_double(&o, "w") as f32;
    true
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::model::{
        Layer, Value, data_apply, data_first, data_get_defaults, data_item_next, data_newref,
    };

    fn d() -> ObsData {
        ObsData::create()
    }

    #[test]
    fn r20_zero_values_and_type_mismatch() {
        let o = d();
        // missing name: every getter reads the zero value
        assert_eq!(data_get_string(&o, "x"), "");
        assert_eq!(data_get_int(&o, "x"), 0);
        assert_eq!(data_get_uint(&o, "x"), 0);
        assert_eq!(data_get_double(&o, "x"), 0.0);
        assert!(!data_get_bool(&o, "x"));
        assert!(data_get_obj(&o, "x").is_none());
        assert!(data_get_array(&o, "x").is_none());
        // type mismatch: zero value of the requested kind
        data_set_int(&o, "n", 42);
        assert_eq!(data_get_string(&o, "n"), "");
        assert!(!data_get_bool(&o, "n"));
        assert!(data_get_obj(&o, "n").is_none());
        data_set_string(&o, "s", "text");
        assert_eq!(data_get_int(&o, "s"), 0);
        assert_eq!(data_get_double(&o, "s"), 0.0);
    }

    #[test]
    fn r22_number_conversion() {
        let o = d();
        data_set_double(&o, "d", -2.9);
        assert_eq!(data_get_int(&o, "d"), -2, "truncates toward zero");
        data_set_double(&o, "big", 3_000_000_000.75);
        assert_eq!(data_get_int(&o, "big"), 3_000_000_000);
        data_set_int(&o, "i", 9_007_199_254_740_993);
        assert_eq!(
            data_get_double(&o, "i"),
            9_007_199_254_740_992.0,
            "round to nearest"
        );
        // D5 saturation: out-of-range and NaN
        data_set_double(&o, "inf", f64::INFINITY);
        assert_eq!(data_get_int(&o, "inf"), i64::MAX);
        data_set_double(&o, "nan", f64::NAN);
        assert_eq!(data_get_int(&o, "nan"), 0, "Rust defines NaN as i64 -> 0");
        // uint: the stored bits come back unchanged
        data_set_uint(&o, "u", u64::MAX);
        assert_eq!(data_get_uint(&o, "u"), u64::MAX);
        assert_eq!(data_get_int(&o, "u"), -1, "the same bits as i64");
        data_set_int(&o, "neg", -5);
        assert_eq!(
            data_get_uint(&o, "neg"),
            u64::MAX - 4,
            "two's-complement bits"
        );
    }

    #[test]
    fn r10_type_change_replaces() {
        let o = d();
        data_set_string(&o, "a", "s");
        data_set_int(&o, "a", 7);
        assert_eq!(data_get_int(&o, "a"), 7);
        assert_eq!(data_get_string(&o, "a"), "");
        // only one item exists
        assert_eq!(crate::model::data_count(&o), 1);
    }

    #[test]
    fn r26_present_null_object() {
        let o = d();
        data_set_obj(&o, "n", None);
        assert!(data_has_user_value(&o, "n"), "a present NULL counts");
        assert!(data_get_obj(&o, "n").is_none());
        // no fallback to a default
        let child = d();
        data_set_default_obj(&o, "n", Some(&child));
        assert!(
            data_get_obj(&o, "n").is_none(),
            "the user NULL wins over the default"
        );
        assert!(data_get_default_obj(&o, "n").is_some());
        // arrays likewise
        data_set_array(&o, "a", None);
        assert!(data_has_user_value(&o, "a"));
        assert!(data_get_array(&o, "a").is_none());
    }

    #[test]
    fn r02_get_obj_returns_new_reference() {
        let o = d();
        let child = d();
        data_set_obj(&o, "c", Some(&child));
        assert_eq!(child.refcount(), 2); // caller + stored
        let got = data_get_obj(&o, "c").expect("new reference");
        assert_eq!(child.refcount(), 3); // caller + stored + the getter's result
        drop(got);
        assert_eq!(child.refcount(), 2);
    }

    #[test]
    fn r11_default_type_check() {
        let o = d();
        data_set_string(&o, "a", "s");
        data_set_default_int(&o, "a", 7); // ignored: type differs
        assert_eq!(data_get_default_int(&o, "a"), 0);
        assert!(!data_has_default_value(&o, "a"));
        data_set_default_string(&o, "a", "d");
        assert_eq!(data_get_default_string(&o, "a"), "d");
        // a fresh item gets the default
        data_set_default_int(&o, "b", 3);
        assert_eq!(data_get_default_int(&o, "b"), 3);
        assert!(!data_has_user_value(&o, "b"));
        assert_eq!(data_get_int(&o, "b"), 3, "the plain getter falls back");
    }

    #[test]
    fn r12_autoselect_layer() {
        let o = d();
        data_set_autoselect_string(&o, "a", "as");
        assert_eq!(data_get_autoselect_string(&o, "a"), "as");
        assert_eq!(
            data_get_string(&o, "a"),
            "as",
            "the effective value falls through"
        );
        data_set_autoselect_int(&o, "a", 5); // overwrites the type (R12)
        assert_eq!(data_get_int(&o, "a"), 5);
        assert_eq!(data_get_autoselect_string(&o, "a"), "");
    }

    #[test]
    fn r30_unset_layers_independently() {
        let o = d();
        data_set_string(&o, "a", "u");
        data_set_default_string(&o, "a", "d");
        data_set_autoselect_string(&o, "a", "x");
        data_unset_user_value(&o, "a");
        assert_eq!(data_get_string(&o, "a"), "d");
        assert!(!data_has_user_value(&o, "a"));
        data_unset_default_value(&o, "a");
        assert_eq!(data_get_string(&o, "a"), "x");
        data_unset_autoselect_value(&o, "a");
        assert_eq!(
            data_get_string(&o, "a"),
            "",
            "a shell reads as the zero value"
        );
        assert!(crate::model::data_count(&o) == 1, "the shell stays");
    }

    #[test]
    fn r60_r61_vec_quat() {
        let o = d();
        let v2 = Vec2 { x: 1.5, y: -2.25 };
        data_set_vec2(&o, "v", &v2);
        let mut out = Vec2 { x: 7.0, y: 7.0 };
        assert!(data_get_vec2(&o, "v", &mut out));
        assert_eq!(out, v2, "f32 values survive the double round trip");
        // stored as doubles in member order x, y
        let obj = data_get_obj(&o, "v").unwrap();
        assert_eq!(data_get_double(&obj, "x"), 1.5);
        assert_eq!(data_get_double(&obj, "y"), -2.25);
        assert_eq!(crate::model::data_count(&obj), 2);
        // no object value: untouched
        let mut untouched = Vec2 { x: 7.0, y: 7.0 };
        data_set_int(&o, "i", 1);
        assert!(!data_get_vec2(&o, "i", &mut untouched));
        assert_eq!(untouched, Vec2 { x: 7.0, y: 7.0 });
        data_set_obj(&o, "nul", None);
        assert!(
            !data_get_vec2(&o, "nul", &mut untouched),
            "a NULL object leaves it untouched"
        );
        // missing members read as 0; int members convert
        let m = d();
        data_set_int(&m, "x", 3);
        data_set_obj(&o, "m", Some(&m));
        let mut mout = Vec2 { x: 7.0, y: 7.0 };
        assert!(data_get_vec2(&o, "m", &mut mout));
        assert_eq!(mout, Vec2 { x: 3.0, y: 0.0 });
        // vec3/vec4/quat (the C vec3 has no w: it stays 7)
        let v3 = Vec3 {
            x: 1.0,
            y: 2.0,
            z: 3.0,
            w: 0.0,
        };
        data_set_vec3(&o, "v3", &v3);
        let mut v3o = Vec3 {
            x: 7.0,
            y: 7.0,
            z: 7.0,
            w: 7.0,
        };
        assert!(data_get_vec3(&o, "v3", &mut v3o));
        assert_eq!((v3o.x, v3o.y, v3o.z), (1.0, 2.0, 3.0));
        assert_eq!(v3o.w, 7.0, "w is not a C vec3 component");
        let q = Quat {
            x: 0.0,
            y: 0.0,
            z: 0.3,
            w: 0.4,
        };
        data_set_quat(&o, "q", &q);
        let mut qo = Quat {
            x: 7.0,
            y: 7.0,
            z: 7.0,
            w: 7.0,
        };
        assert!(data_get_quat(&o, "q", &mut qo));
        assert_eq!(qo, q);
        // the default layer form
        let v4 = Vec4 {
            x: 1.0,
            y: 2.0,
            z: 3.0,
            w: 4.0,
        };
        data_set_default_vec4(&o, "v4", &v4);
        let mut v4o = Vec4 {
            x: 7.0,
            y: 7.0,
            z: 7.0,
            w: 7.0,
        };
        assert!(crate::api::data_get_default_vec4(&o, "v4", &mut v4o));
        assert_eq!(v4o, v4);
    }

    #[test]
    fn r65_r66_frames_per_second() {
        let o = d();
        data_set_frames_per_second(&o, "fps", (60, 1), None);
        let mut fps = (7, 7);
        let mut opt = FpsOption::NotRequested;
        assert!(data_get_frames_per_second(
            &o,
            "fps",
            Some(&mut fps),
            &mut opt
        ));
        assert_eq!(fps, (60, 1));
        assert_eq!(opt, FpsOption::NotRequested);
        // zero numerator: stored, result false
        data_set_frames_per_second(&o, "z", (0, 1), None);
        let mut f2 = (7, 7);
        let mut oz = FpsOption::Unset;
        assert!(!data_get_frames_per_second(&o, "z", Some(&mut f2), &mut oz));
        // the clamp to [0, 4294967295]
        let c = d();
        let sub = d();
        crate::model::set_layer_value(&sub.inner, Layer::User, b"numerator", Value::Dbl(-5.9));
        crate::model::set_layer_value(&sub.inner, Layer::User, b"denominator", Value::Dbl(5e12));
        data_set_obj(&c, "c", Some(&sub));
        let mut fc = (7, 7);
        let mut oc = FpsOption::Unset;
        assert!(
            !data_get_frames_per_second(&c, "c", Some(&mut fc), &mut oc),
            "a clamped-to-zero numerator is not a valid fps"
        );
        assert_eq!(
            fc,
            (0, 4_294_967_295),
            "the clamped values are stored even when the result is false"
        );
        // the option member wins (any type -> its get_string)
        let s = d();
        let sub = d();
        crate::model::set_layer_value(
            &sub.inner,
            Layer::User,
            b"option",
            Value::Str(b"fast".to_vec()),
        );
        data_set_obj(&s, "o", Some(&sub));
        let mut fo = (7, 7);
        let mut opt = FpsOption::Unset;
        assert!(data_get_frames_per_second(&s, "o", Some(&mut fo), &mut opt));
        assert!(matches!(opt, FpsOption::Value(ref s) if s == "fast"));
        assert_eq!(fo, (7, 7), "fps untouched when the option is stored");
        // no object at all: false, untouched
        let mut fnu = (7, 7);
        let mut opt2 = FpsOption::Unset;
        assert!(!data_get_frames_per_second(
            &o,
            "none",
            Some(&mut fnu),
            &mut opt2
        ));
        assert_eq!(fnu, (7, 7));
        assert_eq!(opt2, FpsOption::Unset);
        // missing members: false
        let m = d();
        let sub = d();
        crate::model::set_layer_value(&sub.inner, Layer::User, b"numerator", Value::Dbl(30.0));
        data_set_obj(&m, "m", Some(&sub));
        let mut fm = (7, 7);
        let mut om = FpsOption::Unset;
        assert!(!data_get_frames_per_second(&m, "m", Some(&mut fm), &mut om));
        assert_eq!(fm, (7, 7));
        // fps = NULL (None): false unless the option answered
        let mut on = FpsOption::Unset;
        assert!(!data_get_frames_per_second(&o, "fps", None, &mut on));
    }

    #[test]
    fn item_typed_api() {
        let o = d();
        data_set_string(&o, "a", "s");
        let it = crate::model::data_item_byname(&o, "a").unwrap();
        assert_eq!(data_item_get_string(&it), "s");
        data_item_set_int(&it, 7); // R13: the type changes, the item stays
        assert_eq!(data_item_get_int(&it), 7);
        assert_eq!(data_get_int(&o, "a"), 7);
        assert!(crate::model::data_count(&o) == 1);
        // default layer through the item (R11 type check applies)
        data_item_set_default_string(&it, "d"); // ignored: the item is a number now
        assert_eq!(data_item_get_default_int(&it), 0);
        data_item_set_default_int(&it, 9);
        assert_eq!(data_item_get_default_int(&it), 9);
        // vec through the item
        let v = Vec3 {
            x: 1.0,
            y: 2.0,
            z: 3.0,
            w: 0.0,
        };
        data_item_set_vec3(&it, &v);
        let mut vo = Vec3 {
            x: 7.0,
            y: 7.0,
            z: 7.0,
            w: 7.0,
        };
        assert!(data_item_get_vec3(&it, &mut vo));
        assert_eq!((vo.x, vo.y, vo.z), (1.0, 2.0, 3.0));
        // a grow keeps the handle on the (now last) item
        let b = d();
        data_set_string(&b, "one", "x");
        data_set_string(&b, "two", "y");
        let h = crate::model::data_item_byname(&b, "one").unwrap();
        data_item_set_string(&h, &"z".repeat(40)); // grows: item moves to the end
        let first = data_first(&b).unwrap();
        assert_eq!(crate::model::data_item_get_name(&first), "two");
        assert_eq!(data_item_get_string(&h), "z".repeat(40));
        let _ = data_item_next(&mut Some(first));
    }

    #[test]
    fn apply_and_defaults_roundtrip() {
        let src = d();
        data_set_string(&src, "a", "b");
        data_set_default_int(&src, "a", 2);
        let dst = d();
        data_apply(&dst, &src);
        assert_eq!(data_get_string(&dst, "a"), "b");
        assert!(
            !data_has_default_value(&dst, "a"),
            "defaults are not applied"
        );
        let defs = data_get_defaults(&src);
        // R33 reads the default layers: the int default was ignored by
        // R11 (the item is a string), so the string default is the zero value
        assert_eq!(data_get_string(&defs, "a"), "");
        assert!(
            !data_has_default_value(&defs, "a"),
            "the result carries user values only"
        );
        // a number item with a default reads that default
        let src2 = d();
        data_set_int(&src2, "i", 1);
        data_set_default_int(&src2, "i", 2);
        let defs2 = data_get_defaults(&src2);
        assert_eq!(data_get_int(&defs2, "i"), 2);
        let _ = data_newref(Some(&dst));
    }
}
