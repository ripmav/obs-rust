//! S-015 §3.2/§5: the `add_*` family and the property API (getters,
//! setters, kind-specific accessors).
//!
//! C-`NULL` handle cases (R14/§5.14 "NULL handle → default") are
//! unrepresentable in the safe API and noted per function.

use std::os::raw::c_void;

use crate::enums::*;
use crate::model::{Kind, ObsProperties, ObsProperty, add_checked};

/// Store a string argument as the C does (S-015 §3.3): copied, NULL stays
/// NULL.
fn copy(s: Option<&str>) -> Option<String> {
    s.map(str::to_string)
}

// ---------------------------------------------------------------------------
// R13/R14: common property operations
// ---------------------------------------------------------------------------

/// S-015-R14: `obs_property_name` (a copy; the C pointer points into the
/// property).
pub fn property_name(p: &ObsProperty) -> String {
    p.inner.borrow().name.clone()
}

/// S-015-R14: `obs_property_description` — the stored string or `None`.
pub fn property_description(p: &ObsProperty) -> Option<String> {
    p.inner.borrow().desc.clone()
}

/// S-015-R14: `obs_property_long_description` — the stored string or
/// `None`.
pub fn property_long_description(p: &ObsProperty) -> Option<String> {
    p.inner.borrow().long_desc.clone()
}

/// S-015-R14: `obs_property_get_type`.
pub fn property_get_type(p: &ObsProperty) -> PropType {
    p.inner.borrow().ty
}

/// S-015-R14: `obs_property_enabled`.
pub fn property_enabled(p: &ObsProperty) -> bool {
    p.inner.borrow().enabled
}

/// S-015-R14: `obs_property_visible`.
pub fn property_visible(p: &ObsProperty) -> bool {
    p.inner.borrow().visible
}

/// S-015-R14: `obs_property_set_visible`.
pub fn property_set_visible(p: &ObsProperty, visible: bool) {
    p.inner.borrow_mut().visible = visible;
}

/// S-015-R14: `obs_property_set_enabled`.
pub fn property_set_enabled(p: &ObsProperty, enabled: bool) {
    p.inner.borrow_mut().enabled = enabled;
}

/// S-015-R15: `obs_property_set_description`. `None` or an empty string
/// stores `None` (QUIRK Q-9); `add_*` keeps an empty description as `""`.
pub fn property_set_description(p: &ObsProperty, desc: Option<&str>) {
    p.inner.borrow_mut().desc = copy(desc.filter(|s| !s.is_empty()));
}

/// S-015-R15: `obs_property_set_long_description` (same empty → NULL rule).
pub fn property_set_long_description(p: &ObsProperty, desc: Option<&str>) {
    p.inner.borrow_mut().long_desc = copy(desc.filter(|s| !s.is_empty()));
}

// ---------------------------------------------------------------------------
// Adding (R09 uniqueness check inside every add)
// ---------------------------------------------------------------------------

/// S-015-R20: `obs_properties_add_bool`.
pub fn properties_add_bool(
    props: &ObsProperties,
    name: &str,
    desc: Option<&str>,
) -> Option<ObsProperty> {
    add_checked(&props.inner, name, PropType::BOOL, copy(desc), Kind::Bool).map(ObsProperty::new)
}

/// S-015-R21: `obs_properties_add_int` (SCROLLER, no validation).
pub fn properties_add_int(
    props: &ObsProperties,
    name: &str,
    desc: Option<&str>,
    min: i32,
    max: i32,
    step: i32,
) -> Option<ObsProperty> {
    add_checked(
        &props.inner,
        name,
        PropType::INT,
        copy(desc),
        Kind::Int {
            min,
            max,
            step,
            numtype: NumberType::SCROLLER,
            suffix: None,
        },
    )
    .map(ObsProperty::new)
}

/// S-015-R21: `obs_properties_add_int_slider` (SLIDER).
pub fn properties_add_int_slider(
    props: &ObsProperties,
    name: &str,
    desc: Option<&str>,
    min: i32,
    max: i32,
    step: i32,
) -> Option<ObsProperty> {
    add_checked(
        &props.inner,
        name,
        PropType::INT,
        copy(desc),
        Kind::Int {
            min,
            max,
            step,
            numtype: NumberType::SLIDER,
            suffix: None,
        },
    )
    .map(ObsProperty::new)
}

/// S-015-R21: `obs_properties_add_float` (SCROLLER, no validation).
pub fn properties_add_float(
    props: &ObsProperties,
    name: &str,
    desc: Option<&str>,
    min: f64,
    max: f64,
    step: f64,
) -> Option<ObsProperty> {
    add_checked(
        &props.inner,
        name,
        PropType::FLOAT,
        copy(desc),
        Kind::Float {
            min,
            max,
            step,
            numtype: NumberType::SCROLLER,
            suffix: None,
        },
    )
    .map(ObsProperty::new)
}

/// S-015-R21: `obs_properties_add_float_slider` (SLIDER).
pub fn properties_add_float_slider(
    props: &ObsProperties,
    name: &str,
    desc: Option<&str>,
    min: f64,
    max: f64,
    step: f64,
) -> Option<ObsProperty> {
    add_checked(
        &props.inner,
        name,
        PropType::FLOAT,
        copy(desc),
        Kind::Float {
            min,
            max,
            step,
            numtype: NumberType::SLIDER,
            suffix: None,
        },
    )
    .map(ObsProperty::new)
}

/// S-015-R24: `obs_properties_add_text` — monospace false, info type
/// NORMAL, word wrap true.
pub fn properties_add_text(
    props: &ObsProperties,
    name: &str,
    desc: Option<&str>,
    ty: TextType,
) -> Option<ObsProperty> {
    add_checked(
        &props.inner,
        name,
        PropType::TEXT,
        copy(desc),
        Kind::Text {
            ty: ty.as_raw(),
            monospace: false,
            info_type: TextInfoType::NORMAL.as_raw(),
            word_wrap: true,
        },
    )
    .map(ObsProperty::new)
}

/// S-015-R25: `obs_properties_add_path`. The default path is stored for
/// every type; the filter is stored **only** for FILE and FILE_SAVE
/// (QUIRK Q-10: DIRECTORY and out-of-range types drop it).
pub fn properties_add_path(
    props: &ObsProperties,
    name: &str,
    desc: Option<&str>,
    ty: PathType,
    filter: Option<&str>,
    default_path: Option<&str>,
) -> Option<ObsProperty> {
    let keep_filter = matches!(ty, PathType::FILE | PathType::FILE_SAVE) && filter.is_some();
    add_checked(
        &props.inner,
        name,
        PropType::PATH,
        copy(desc),
        Kind::Path {
            ty: ty.as_raw(),
            filter: if keep_filter { copy(filter) } else { None },
            default_path: copy(default_path),
        },
    )
    .map(ObsProperty::new)
}

/// S-015-R26: `obs_properties_add_editable_list` — all three stored
/// verbatim (the filter is never dropped).
pub fn properties_add_editable_list(
    props: &ObsProperties,
    name: &str,
    desc: Option<&str>,
    ty: EditableListType,
    filter: Option<&str>,
    default_path: Option<&str>,
) -> Option<ObsProperty> {
    add_checked(
        &props.inner,
        name,
        PropType::EDITABLE_LIST,
        copy(desc),
        Kind::EditableList {
            ty: ty.as_raw(),
            filter: copy(filter),
            default_path: copy(default_path),
        },
    )
    .map(ObsProperty::new)
}

/// S-015-R20: `obs_properties_add_color`.
pub fn properties_add_color(
    props: &ObsProperties,
    name: &str,
    desc: Option<&str>,
) -> Option<ObsProperty> {
    add_checked(&props.inner, name, PropType::COLOR, copy(desc), Kind::Color).map(ObsProperty::new)
}

/// S-015-R20: `obs_properties_add_color_alpha`.
pub fn properties_add_color_alpha(
    props: &ObsProperties,
    name: &str,
    desc: Option<&str>,
) -> Option<ObsProperty> {
    add_checked(
        &props.inner,
        name,
        PropType::COLOR_ALPHA,
        copy(desc),
        Kind::ColorAlpha,
    )
    .map(ObsProperty::new)
}

/// S-015-R20: `obs_properties_add_font`.
pub fn properties_add_font(
    props: &ObsProperties,
    name: &str,
    desc: Option<&str>,
) -> Option<ObsProperty> {
    add_checked(&props.inner, name, PropType::FONT, copy(desc), Kind::Font).map(ObsProperty::new)
}

/// S-015-R34: `obs_properties_add_button` (deprecated in the header) —
/// the text becomes the description, type DEFAULT, no priv.
pub fn properties_add_button(
    props: &ObsProperties,
    name: &str,
    desc: Option<&str>,
    callback: Option<crate::model::ClickedCb>,
) -> Option<ObsProperty> {
    properties_add_button2(props, name, desc, callback, std::ptr::null_mut())
}

/// S-015-R34: `obs_properties_add_button2` — stores `priv` in the property's
/// single priv slot (QUIRK Q-4).
pub fn properties_add_button2(
    props: &ObsProperties,
    name: &str,
    desc: Option<&str>,
    callback: Option<crate::model::ClickedCb>,
    priv_: *mut c_void,
) -> Option<ObsProperty> {
    let r = add_checked(
        &props.inner,
        name,
        PropType::BUTTON,
        copy(desc),
        Kind::Button {
            callback,
            ty: ButtonType::DEFAULT.as_raw(),
            url: None,
        },
    )?;
    if !priv_.is_null() {
        r.borrow_mut().priv_slot = priv_;
    }
    Some(ObsProperty::new(r))
}

// ---------------------------------------------------------------------------
// R22/R23: INT and FLOAT
// ---------------------------------------------------------------------------

fn int_fields(p: &ObsProperty) -> Option<(i32, i32, i32, NumberType, Option<String>)> {
    match &p.inner.borrow().kind {
        Kind::Int {
            min,
            max,
            step,
            numtype,
            suffix,
        } => Some((*min, *max, *step, *numtype, suffix.clone())),
        _ => None,
    }
}

/// S-015-R22: `obs_property_int_min` (0 on a non-INT).
pub fn property_int_min(p: &ObsProperty) -> i32 {
    int_fields(p).map(|f| f.0).unwrap_or(0)
}

/// S-015-R22: `obs_property_int_max` (0 on a non-INT).
pub fn property_int_max(p: &ObsProperty) -> i32 {
    int_fields(p).map(|f| f.1).unwrap_or(0)
}

/// S-015-R22: `obs_property_int_step` (0 on a non-INT).
pub fn property_int_step(p: &ObsProperty) -> i32 {
    int_fields(p).map(|f| f.2).unwrap_or(0)
}

/// S-015-R22: `obs_property_int_type` (SCROLLER on a non-INT).
pub fn property_int_type(p: &ObsProperty) -> NumberType {
    int_fields(p).map(|f| f.3).unwrap_or(NumberType::SCROLLER)
}

/// S-015-R22: `obs_property_int_suffix` (`None` on a non-INT or no suffix).
pub fn property_int_suffix(p: &ObsProperty) -> Option<String> {
    int_fields(p).and_then(|f| f.4)
}

/// S-015-R23: `obs_property_int_set_limits` — INT only, other kinds
/// no-op.
pub fn property_int_set_limits(p: &ObsProperty, min: i32, max: i32, step: i32) {
    if let Kind::Int {
        min: m,
        max: x,
        step: s,
        ..
    } = &mut p.inner.borrow_mut().kind
    {
        *m = min;
        *x = max;
        *s = step;
    }
}

/// S-015-R23: `obs_property_int_set_suffix` — INT only; `None` clears.
pub fn property_int_set_suffix(p: &ObsProperty, suffix: Option<&str>) {
    if let Kind::Int { suffix: s, .. } = &mut p.inner.borrow_mut().kind {
        *s = copy(suffix);
    }
}

fn float_fields(p: &ObsProperty) -> Option<(f64, f64, f64, NumberType, Option<String>)> {
    match &p.inner.borrow().kind {
        Kind::Float {
            min,
            max,
            step,
            numtype,
            suffix,
        } => Some((*min, *max, *step, *numtype, suffix.clone())),
        _ => None,
    }
}

/// S-015-R22: `obs_property_float_min` (0.0 on a non-FLOAT).
pub fn property_float_min(p: &ObsProperty) -> f64 {
    float_fields(p).map(|f| f.0).unwrap_or(0.0)
}

/// S-015-R22: `obs_property_float_max` (0.0 on a non-FLOAT).
pub fn property_float_max(p: &ObsProperty) -> f64 {
    float_fields(p).map(|f| f.1).unwrap_or(0.0)
}

/// S-015-R22: `obs_property_float_step` (0.0 on a non-FLOAT).
pub fn property_float_step(p: &ObsProperty) -> f64 {
    float_fields(p).map(|f| f.2).unwrap_or(0.0)
}

/// S-015-R22: `obs_property_float_type` (SCROLLER on a non-FLOAT).
pub fn property_float_type(p: &ObsProperty) -> NumberType {
    float_fields(p).map(|f| f.3).unwrap_or(NumberType::SCROLLER)
}

/// S-015-R22: `obs_property_float_suffix` (`None` on a non-FLOAT or no
/// suffix).
pub fn property_float_suffix(p: &ObsProperty) -> Option<String> {
    float_fields(p).and_then(|f| f.4)
}

/// S-015-R23: `obs_property_float_set_limits` — FLOAT only.
pub fn property_float_set_limits(p: &ObsProperty, min: f64, max: f64, step: f64) {
    if let Kind::Float {
        min: m,
        max: x,
        step: s,
        ..
    } = &mut p.inner.borrow_mut().kind
    {
        *m = min;
        *x = max;
        *s = step;
    }
}

/// S-015-R23: `obs_property_float_set_suffix` — FLOAT only; `None`
/// clears.
pub fn property_float_set_suffix(p: &ObsProperty, suffix: Option<&str>) {
    if let Kind::Float { suffix: s, .. } = &mut p.inner.borrow_mut().kind {
        *s = copy(suffix);
    }
}

// ---------------------------------------------------------------------------
// R24: TEXT
// ---------------------------------------------------------------------------

/// S-015-R24: `obs_property_text_type` (DEFAULT on a non-TEXT).
pub fn property_text_type(p: &ObsProperty) -> TextType {
    match &p.inner.borrow().kind {
        Kind::Text { ty, .. } => TextType::from_raw(*ty),
        _ => TextType::DEFAULT,
    }
}

/// S-015-R24: `obs_property_text_monospace` (false on a non-TEXT).
pub fn property_text_monospace(p: &ObsProperty) -> bool {
    matches!(
        &p.inner.borrow().kind,
        Kind::Text {
            monospace: true,
            ..
        }
    )
}

/// S-015-R24: `obs_property_text_info_type` (NORMAL on a non-TEXT).
pub fn property_text_info_type(p: &ObsProperty) -> TextInfoType {
    match &p.inner.borrow().kind {
        Kind::Text { info_type, .. } => TextInfoType::from_raw(*info_type),
        _ => TextInfoType::NORMAL,
    }
}

/// S-015-R24: `obs_property_text_info_word_wrap` (**true** on a
/// non-TEXT).
pub fn property_text_info_word_wrap(p: &ObsProperty) -> bool {
    match &p.inner.borrow().kind {
        Kind::Text { word_wrap, .. } => *word_wrap,
        _ => true,
    }
}

/// S-015-R24: `obs_property_text_set_monospace` — TEXT only.
pub fn property_text_set_monospace(p: &ObsProperty, monospace: bool) {
    if let Kind::Text { monospace: m, .. } = &mut p.inner.borrow_mut().kind {
        *m = monospace;
    }
}

/// S-015-R24: `obs_property_text_set_info_type` — TEXT only (any text
/// type).
pub fn property_text_set_info_type(p: &ObsProperty, ty: TextInfoType) {
    if let Kind::Text { info_type: i, .. } = &mut p.inner.borrow_mut().kind {
        *i = ty.as_raw();
    }
}

/// S-015-R24: `obs_property_text_set_info_word_wrap` — TEXT only.
pub fn property_text_set_info_word_wrap(p: &ObsProperty, wrap: bool) {
    if let Kind::Text { word_wrap: w, .. } = &mut p.inner.borrow_mut().kind {
        *w = wrap;
    }
}

// ---------------------------------------------------------------------------
// R25/R26: PATH and EDITABLE_LIST
// ---------------------------------------------------------------------------

/// S-015-R25: `obs_property_path_type` (DIRECTORY on a non-PATH).
pub fn property_path_type(p: &ObsProperty) -> PathType {
    match &p.inner.borrow().kind {
        Kind::Path { ty, .. } => PathType::from_raw(*ty),
        _ => PathType::DIRECTORY,
    }
}

/// S-015-R25: `obs_property_path_filter` (`None` on a non-PATH or a
/// dropped filter).
pub fn property_path_filter(p: &ObsProperty) -> Option<String> {
    match &p.inner.borrow().kind {
        Kind::Path { filter, .. } => filter.clone(),
        _ => None,
    }
}

/// S-015-R25: `obs_property_path_default_path` (`None` on a non-PATH or
/// no default).
pub fn property_path_default_path(p: &ObsProperty) -> Option<String> {
    match &p.inner.borrow().kind {
        Kind::Path { default_path, .. } => default_path.clone(),
        _ => None,
    }
}

/// S-015-R26: `obs_property_editable_list_type` (STRINGS on a
/// non-EDITABLE_LIST).
pub fn property_editable_list_type(p: &ObsProperty) -> EditableListType {
    match &p.inner.borrow().kind {
        Kind::EditableList { ty, .. } => EditableListType::from_raw(*ty),
        _ => EditableListType::STRINGS,
    }
}

/// S-015-R26: `obs_property_editable_list_filter`.
pub fn property_editable_list_filter(p: &ObsProperty) -> Option<String> {
    match &p.inner.borrow().kind {
        Kind::EditableList { filter, .. } => filter.clone(),
        _ => None,
    }
}

/// S-015-R26: `obs_property_editable_list_default_path`.
pub fn property_editable_list_default_path(p: &ObsProperty) -> Option<String> {
    match &p.inner.borrow().kind {
        Kind::EditableList { default_path, .. } => default_path.clone(),
        _ => None,
    }
}

// ---------------------------------------------------------------------------
// R34/R35: BUTTON
// ---------------------------------------------------------------------------

/// S-015-R35: `obs_property_button_type` (DEFAULT on a non-BUTTON).
pub fn property_button_type(p: &ObsProperty) -> ButtonType {
    match &p.inner.borrow().kind {
        Kind::Button { ty, .. } => ButtonType::from_raw(*ty),
        _ => ButtonType::DEFAULT,
    }
}

/// S-015-R35: `obs_property_button_url` — the URL or `None` on a BUTTON,
/// but `Some("")` (not `None`) on any other kind (QUIRK Q-6).
pub fn property_button_url(p: &ObsProperty) -> Option<String> {
    match &p.inner.borrow().kind {
        Kind::Button { url, .. } => url.clone(),
        _ => Some(String::new()),
    }
}

/// S-015-R35: `obs_property_button_set_type` — BUTTON only.
pub fn property_button_set_type(p: &ObsProperty, ty: ButtonType) {
    if let Kind::Button { ty: t, .. } = &mut p.inner.borrow_mut().kind {
        *t = ty.as_raw();
    }
}

/// S-015-R35: `obs_property_button_set_url` — BUTTON only; `None`
/// clears. (DEFECT D-2: the reference leaks the previous URL; the Rust
/// `Option::replace` frees it.)
pub fn property_button_set_url(p: &ObsProperty, url: Option<&str>) {
    if let Kind::Button { url: u, .. } = &mut p.inner.borrow_mut().kind {
        *u = copy(url);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::model::properties_create;
    use crate::{
        frame_rate_fps_range_min, frame_rate_fps_ranges_count, frame_rate_option_name,
        frame_rate_options_count, group_content, group_type, properties_add_group,
        properties_add_list, properties_get, property_list_format, property_list_item_count,
        property_list_type,
    };

    // vector 04 — simple kinds
    #[test]
    fn v04_simple_kinds() {
        let p = properties_create();
        let b = properties_add_bool(&p, "b", Some("Bool desc")).unwrap();
        let c = properties_add_color(&p, "c", Some("Color")).unwrap();
        let ca = properties_add_color_alpha(&p, "ca", Some("Color alpha")).unwrap();
        let f = properties_add_font(&p, "f", Some("Font")).unwrap();

        assert_eq!(property_get_type(&b), PropType::BOOL);
        assert_eq!(property_get_type(&c), PropType::COLOR);
        assert_eq!(property_get_type(&ca), PropType::COLOR_ALPHA);
        assert_eq!(property_get_type(&f), PropType::FONT);
        for x in [&b, &c, &ca, &f] {
            assert!(property_enabled(x));
            assert!(property_visible(x));
            assert!(property_long_description(x).is_none());
        }
        assert_eq!(property_name(&b), "b");
        assert_eq!(property_description(&b).as_deref(), Some("Bool desc"));

        // description NULL and empty at add time
        let n = properties_add_bool(&p, "n", None).unwrap();
        let e = properties_add_bool(&p, "e", Some("")).unwrap();
        assert!(property_description(&n).is_none());
        assert_eq!(property_description(&e).as_deref(), Some(""));

        // visible/enabled setters
        property_set_visible(&b, false);
        property_set_enabled(&b, false);
        assert!(!property_visible(&b));
        assert!(!property_enabled(&b));
        property_set_visible(&b, true);
        property_set_enabled(&b, true);
        assert!(property_visible(&b));
        assert!(property_enabled(&b));
    }

    // vector 05 — int/float
    #[test]
    fn v05_int_float() {
        let p = properties_create();
        let i = properties_add_int(&p, "i", Some("Int"), 1, 100, 5).unwrap();
        let is = properties_add_int_slider(&p, "is", Some("Int slider"), -10, 10, 2).unwrap();
        let f = properties_add_float(&p, "f", Some("Float"), 0.5, 99.25, 0.25).unwrap();
        let fs =
            properties_add_float_slider(&p, "fs", Some("Float slider"), 0.0, 1.0, 0.001).unwrap();

        assert_eq!(
            (
                property_int_min(&i),
                property_int_max(&i),
                property_int_step(&i)
            ),
            (1, 100, 5)
        );
        assert_eq!(property_int_type(&i), NumberType::SCROLLER);
        assert_eq!(property_int_type(&is), NumberType::SLIDER);
        assert_eq!(
            (
                property_int_min(&is),
                property_int_max(&is),
                property_int_step(&is)
            ),
            (-10, 10, 2)
        );
        assert_eq!(
            (
                property_float_min(&f),
                property_float_max(&f),
                property_float_step(&f)
            ),
            (0.5, 99.25, 0.25)
        );
        assert_eq!(property_float_type(&f), NumberType::SCROLLER);
        assert_eq!(property_float_type(&fs), NumberType::SLIDER);
        assert!(property_int_suffix(&i).is_none());

        // no validation: min > max kept, step 0 kept
        let w = properties_add_int(&p, "weird", Some("w"), 10, -10, 0).unwrap();
        let wf = properties_add_float(&p, "weirdf", Some("w"), 1e300, -1e300, 0.0).unwrap();
        assert_eq!(
            (
                property_int_min(&w),
                property_int_max(&w),
                property_int_step(&w)
            ),
            (10, -10, 0)
        );
        assert_eq!(
            (property_float_min(&wf), property_float_max(&wf)),
            (1e300, -1e300)
        );

        // set_limits
        property_int_set_limits(&i, 7, 8, 9);
        assert_eq!(
            (
                property_int_min(&i),
                property_int_max(&i),
                property_int_step(&i)
            ),
            (7, 8, 9)
        );
        property_float_set_limits(&f, -1.5, 2.5, 0.125);
        assert_eq!(
            (
                property_float_min(&f),
                property_float_max(&f),
                property_float_step(&f)
            ),
            (-1.5, 2.5, 0.125)
        );

        // wrong-kind limits are ignored
        property_float_set_limits(&i, 100.0, 200.0, 300.0);
        property_int_set_limits(&f, 100, 200, 300);
        property_float_set_limits(&w, 100.0, 200.0, 300.0);
        assert_eq!(property_int_min(&i), 7);
        assert_eq!(property_float_min(&f), -1.5);
        assert_eq!(property_float_min(&wf), 1e300);

        // suffix
        property_int_set_suffix(&i, Some(" px"));
        property_float_set_suffix(&f, Some("%"));
        assert_eq!(property_int_suffix(&i).as_deref(), Some(" px"));
        assert_eq!(property_float_suffix(&f).as_deref(), Some("%"));
        property_int_set_suffix(&i, Some("other"));
        assert_eq!(property_int_suffix(&i).as_deref(), Some("other"));
        property_int_set_suffix(&i, None);
        assert!(property_int_suffix(&i).is_none());
        property_float_set_suffix(&f, Some(""));
        assert_eq!(property_float_suffix(&f).as_deref(), Some(""));
        // wrong-kind suffix is ignored
        property_float_set_suffix(&i, Some("x"));
        property_int_set_suffix(&f, Some("x"));
        assert!(property_int_suffix(&i).is_none());
        assert_eq!(property_float_suffix(&f).as_deref(), Some(""));
    }

    // vector 06 — text
    #[test]
    fn v06_text() {
        let p = properties_create();
        let d = properties_add_text(&p, "d", Some("Default"), TextType::DEFAULT).unwrap();
        let pw = properties_add_text(&p, "pw", Some("Password"), TextType::PASSWORD).unwrap();
        let ml = properties_add_text(&p, "ml", Some("Multi"), TextType::MULTILINE).unwrap();
        let inf = properties_add_text(&p, "inf", Some("Info"), TextType::INFO).unwrap();

        assert_eq!(property_text_type(&d), TextType::DEFAULT);
        assert_eq!(property_text_type(&pw), TextType::PASSWORD);
        assert_eq!(property_text_type(&ml), TextType::MULTILINE);
        assert_eq!(property_text_type(&inf), TextType::INFO);
        assert!(!property_text_monospace(&d));
        assert_eq!(property_text_info_type(&d), TextInfoType::NORMAL);
        assert!(property_text_info_word_wrap(&d));

        // setters work for every text type
        property_text_set_info_type(&inf, TextInfoType::WARNING);
        property_text_set_info_word_wrap(&inf, false);
        property_text_set_monospace(&d, true);
        property_text_set_info_type(&d, TextInfoType::ERROR);
        assert_eq!(property_text_info_type(&inf), TextInfoType::WARNING);
        assert!(!property_text_info_word_wrap(&inf));
        assert!(property_text_monospace(&d));
        assert_eq!(property_text_info_type(&d), TextInfoType::ERROR);

        // out-of-range numeric type is stored and returned unchanged
        let w = properties_add_text(&p, "weird", Some("w"), TextType::Unknown(9)).unwrap();
        assert_eq!(property_text_type(&w), TextType::Unknown(9));

        // setters on other kinds are ignored
        let b = properties_add_bool(&p, "b", Some("B")).unwrap();
        property_text_set_monospace(&b, true);
        property_text_set_info_word_wrap(&b, false);
        property_text_set_info_type(&b, TextInfoType::ERROR);
        assert!(!property_text_monospace(&b));
        // §5.14: wrong-kind defaults
        assert_eq!(property_text_type(&b), TextType::DEFAULT);
        assert_eq!(property_text_info_type(&b), TextInfoType::NORMAL);
        assert!(property_text_info_word_wrap(&b));
    }

    // vector 07 — path and editable list
    #[test]
    fn v07_path_editable_list() {
        let p = properties_create();
        let pf = properties_add_path(
            &p,
            "pf",
            Some("File"),
            PathType::FILE,
            Some("Text (*.txt);;All (*)"),
            Some("/dflt"),
        )
        .unwrap();
        let ps = properties_add_path(
            &p,
            "ps",
            Some("Save"),
            PathType::FILE_SAVE,
            Some("Img (*.png)"),
            None,
        )
        .unwrap();
        let pd = properties_add_path(
            &p,
            "pd",
            Some("Dir"),
            PathType::DIRECTORY,
            Some("ignored filter"),
            Some("/dir"),
        )
        .unwrap();
        let pn =
            properties_add_path(&p, "pn", Some("NoFilter"), PathType::FILE, None, None).unwrap();
        let pe = properties_add_path(&p, "pe", Some("Empty"), PathType::FILE, Some(""), Some(""))
            .unwrap();

        assert_eq!(property_path_type(&pf), PathType::FILE);
        assert_eq!(
            property_path_filter(&pf).as_deref(),
            Some("Text (*.txt);;All (*)")
        );
        assert_eq!(property_path_default_path(&pf).as_deref(), Some("/dflt"));
        assert_eq!(property_path_type(&ps), PathType::FILE_SAVE);
        assert_eq!(property_path_filter(&ps).as_deref(), Some("Img (*.png)"));
        assert!(property_path_default_path(&ps).is_none());
        // Q-10: DIRECTORY drops the filter, keeps the default path
        assert_eq!(property_path_type(&pd), PathType::DIRECTORY);
        assert!(property_path_filter(&pd).is_none());
        assert_eq!(property_path_default_path(&pd).as_deref(), Some("/dir"));
        assert!(property_path_filter(&pn).is_none());
        // empty strings are stored (copied, only NULL stays NULL)
        assert_eq!(property_path_filter(&pe).as_deref(), Some(""));
        assert_eq!(property_path_default_path(&pe).as_deref(), Some(""));

        // out-of-range path type: filter dropped (Q-10)
        let px = properties_add_path(
            &p,
            "px",
            Some("X"),
            PathType::Unknown(9),
            Some("f"),
            Some("d"),
        )
        .unwrap();
        assert_eq!(property_path_type(&px), PathType::Unknown(9));
        assert!(property_path_filter(&px).is_none());
        assert_eq!(property_path_default_path(&px).as_deref(), Some("d"));

        // editable list: stored verbatim for every type
        let e1 = properties_add_editable_list(
            &p,
            "e1",
            Some("E1"),
            EditableListType::STRINGS,
            Some("ignored?"),
            Some("/p"),
        )
        .unwrap();
        let e2 = properties_add_editable_list(
            &p,
            "e2",
            Some("E2"),
            EditableListType::FILES,
            Some("Img (*.png)"),
            None,
        )
        .unwrap();
        let e3 = properties_add_editable_list(
            &p,
            "e3",
            Some("E3"),
            EditableListType::FILES_AND_URLS,
            None,
            Some("x"),
        )
        .unwrap();
        assert_eq!(property_editable_list_type(&e1), EditableListType::STRINGS);
        assert_eq!(
            property_editable_list_filter(&e1).as_deref(),
            Some("ignored?")
        );
        assert_eq!(
            property_editable_list_default_path(&e1).as_deref(),
            Some("/p")
        );
        assert_eq!(property_editable_list_type(&e2), EditableListType::FILES);
        assert!(property_editable_list_default_path(&e2).is_none());
        assert_eq!(
            property_editable_list_type(&e3),
            EditableListType::FILES_AND_URLS
        );
        assert!(property_editable_list_filter(&e3).is_none());
        assert_eq!(
            property_editable_list_default_path(&e3).as_deref(),
            Some("x")
        );
        let ex = properties_add_editable_list(
            &p,
            "ex",
            Some("X"),
            EditableListType::Unknown(9),
            Some("f"),
            Some("d"),
        )
        .unwrap();
        assert_eq!(
            property_editable_list_type(&ex),
            EditableListType::Unknown(9)
        );
        assert_eq!(property_editable_list_filter(&ex).as_deref(), Some("f"));

        // wrong-kind defaults (§5.14)
        let b = properties_add_bool(&p, "b", Some("B")).unwrap();
        assert_eq!(property_path_type(&b), PathType::DIRECTORY);
        assert!(property_path_filter(&b).is_none());
        assert!(property_path_default_path(&b).is_none());
        assert_eq!(property_editable_list_type(&b), EditableListType::STRINGS);
        assert!(property_editable_list_filter(&b).is_none());
    }

    // vector 12 — button url/type (QUIRK Q-6)
    #[test]
    fn v12_button_url_type() {
        let p = properties_create();
        let b = properties_add_button(&p, "b", Some("B"), None).unwrap();
        let x = properties_add_bool(&p, "x", Some("X")).unwrap();

        assert_eq!(property_get_type(&b), PropType::BUTTON);
        assert_eq!(property_button_type(&b), ButtonType::DEFAULT);
        // NULL on a button without URL
        assert!(property_button_url(&b).is_none());

        property_button_set_type(&b, ButtonType::URL);
        assert_eq!(property_button_type(&b), ButtonType::URL);
        property_button_set_url(&b, Some("https://example.com/a"));
        assert_eq!(
            property_button_url(&b).as_deref(),
            Some("https://example.com/a")
        );
        property_button_set_url(&b, Some("https://example.com/b"));
        assert_eq!(
            property_button_url(&b).as_deref(),
            Some("https://example.com/b")
        );
        property_button_set_url(&b, None);
        assert!(property_button_url(&b).is_none());

        // setters on a non-button are no-ops; the url default is "" (Q-6)
        property_button_set_type(&x, ButtonType::URL);
        property_button_set_url(&x, Some("ignored"));
        assert_eq!(property_button_type(&x), ButtonType::DEFAULT);
        assert_eq!(property_button_url(&x).as_deref(), Some(""));
    }

    // vector 19 — string ownership, description setters (QUIRK Q-9)
    #[test]
    fn v19_string_ownership() {
        let p = properties_create();
        let a = properties_add_bool(&p, "a", Some("Initial")).unwrap();
        assert_eq!(property_description(&a).as_deref(), Some("Initial"));

        property_set_description(&a, Some("New"));
        assert_eq!(property_description(&a).as_deref(), Some("New"));
        property_set_description(&a, Some(""));
        assert!(property_description(&a).is_none(), "empty -> NULL (Q-9)");
        property_set_description(&a, None);
        assert!(property_description(&a).is_none());
        property_set_description(&a, Some("Back"));
        assert_eq!(property_description(&a).as_deref(), Some("Back"));

        assert!(property_long_description(&a).is_none());
        property_set_long_description(&a, Some("Long help"));
        assert_eq!(property_long_description(&a).as_deref(), Some("Long help"));
        property_set_long_description(&a, Some(""));
        assert!(property_long_description(&a).is_none());
        property_set_long_description(&a, Some("Again"));
        assert_eq!(property_long_description(&a).as_deref(), Some("Again"));
        property_set_long_description(&a, None);
        assert!(property_long_description(&a).is_none());

        // the name never changes
        assert_eq!(property_name(&a), "a");
    }

    // vector 20 — wrong-kind and NULL defaults (§5.14)
    #[test]
    fn v20_wrong_kind_defaults() {
        let p = properties_create();
        let b = properties_add_bool(&p, "b", Some("B")).unwrap();
        let i = properties_add_int(&p, "i", Some("I"), 1, 2, 3).unwrap();
        let l =
            properties_add_list(&p, "l", Some("L"), ComboType::LIST, ComboFormat::STRING).unwrap();
        let g = properties_create();
        let gprop = properties_add_group(&p, "g", Some("G"), GroupType::NORMAL, &g).unwrap();
        drop(g);

        // ints on a bool
        assert_eq!(property_int_min(&b), 0);
        assert_eq!(property_int_max(&b), 0);
        assert_eq!(property_int_step(&b), 0);
        assert_eq!(property_int_type(&b), NumberType::SCROLLER);
        assert!(property_int_suffix(&b).is_none());
        // floats on a bool
        assert_eq!(property_float_min(&b), 0.0);
        assert_eq!(property_float_max(&b), 0.0);
        assert_eq!(property_float_step(&b), 0.0);
        assert_eq!(property_float_type(&b), NumberType::SCROLLER);
        assert!(property_float_suffix(&b).is_none());
        // text on a bool
        assert_eq!(property_text_type(&b), TextType::DEFAULT);
        assert!(!property_text_monospace(&b));
        assert_eq!(property_text_info_type(&b), TextInfoType::NORMAL);
        assert!(property_text_info_word_wrap(&b));
        // path / editable list on a bool
        assert_eq!(property_path_type(&b), PathType::DIRECTORY);
        assert!(property_path_filter(&b).is_none());
        assert!(property_path_default_path(&b).is_none());
        assert_eq!(property_editable_list_type(&b), EditableListType::STRINGS);
        // list on a bool
        assert_eq!(property_list_type(&b), ComboType::INVALID);
        assert_eq!(property_list_format(&b), ComboFormat::INVALID);
        assert_eq!(property_list_item_count(&b), 0);
        // group on the others
        assert_eq!(group_type(&b), GroupType::INVALID);
        assert!(group_content(&b).is_none());
        assert_eq!(group_type(&i), GroupType::INVALID);
        assert_eq!(group_type(&l), GroupType::INVALID);
        assert_eq!(group_type(&gprop), GroupType::NORMAL);
        assert!(group_content(&gprop).is_some());
        // button on a bool
        assert_eq!(property_button_type(&b), ButtonType::DEFAULT);
        assert_eq!(property_button_url(&b).as_deref(), Some(""));
        // frame rate on a bool
        assert_eq!(frame_rate_options_count(&b), 0);
        assert_eq!(frame_rate_fps_ranges_count(&b), 0);
        assert!(frame_rate_option_name(&b, 0).is_none());
        assert_eq!(frame_rate_fps_range_min(&b, 0), MediaFps::default());
    }

    // vector 25 (misc edges) — UTF-8 and empty names are legal
    #[test]
    fn v25_names_utf8_and_empty() {
        let p = properties_create();
        let en = properties_add_bool(&p, "", Some("empty name")).unwrap();
        let u = properties_add_bool(&p, "héllo wörld", Some("Ünïcode")).unwrap();
        let ws = properties_add_bool(&p, "with space", Some("x")).unwrap();
        assert_eq!(property_name(&en), "");
        assert_eq!(property_name(&u), "héllo wörld");
        assert_eq!(property_name(&ws), "with space");
        assert!(
            properties_get(&p, "")
                .map(|x| x.is_same(&en))
                .unwrap_or(false)
        );
        assert!(
            properties_get(&p, "héllo wörld")
                .map(|x| x.is_same(&u))
                .unwrap_or(false)
        );
    }
}
