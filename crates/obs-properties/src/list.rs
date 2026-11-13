//! S-015 §5.10 (LIST, R27–R33) and §5.12 (FRAME_RATE, R36–R40).

use crate::enums::*;
use crate::model::{
    FpsOption, FpsRange, ItemValue, Kind, ListItem, ObsProperties, ObsProperty, add_checked,
    add_unchecked, has_prop,
};

fn copy(s: Option<&str>) -> Option<String> {
    s.map(str::to_string)
}

// ---------------------------------------------------------------------------
// R27: list creation
// ---------------------------------------------------------------------------

/// S-015-R27: `obs_properties_add_list`. An EDITABLE combo of any format
/// other than STRING is refused with a log (the R09 uniqueness check has
/// priority); every other combination — including INVALID and out-of-range
/// values — is accepted and stored.
pub fn properties_add_list(
    props: &ObsProperties,
    name: &str,
    desc: Option<&str>,
    ty: ComboType,
    format: ComboFormat,
) -> Option<ObsProperty> {
    if has_prop(&props.inner, name) {
        // R09 warning — logger not wired yet.
        return None;
    }
    if ty == ComboType::EDITABLE && format != ComboFormat::STRING {
        // LOG_WARNING "List '<name>', error: Editable combo boxes must be
        // of the 'string' type" — logger not wired yet.
        return None;
    }
    Some(ObsProperty::new(add_unchecked(
        &props.inner,
        name,
        PropType::LIST,
        copy(desc),
        Kind::List {
            combo_type: ty.as_raw(),
            format: format.as_raw(),
            items: vec![],
        },
    )))
}

// ---------------------------------------------------------------------------
// R28: getters
// ---------------------------------------------------------------------------

/// S-015-R28: `obs_property_list_type` (INVALID on a non-LIST).
pub fn property_list_type(p: &ObsProperty) -> ComboType {
    match &p.inner.borrow().kind {
        Kind::List { combo_type, .. } => ComboType::from_raw(*combo_type),
        _ => ComboType::INVALID,
    }
}

/// S-015-R28: `obs_property_list_format` (INVALID on a non-LIST).
pub fn property_list_format(p: &ObsProperty) -> ComboFormat {
    match &p.inner.borrow().kind {
        Kind::List { format, .. } => ComboFormat::from_raw(*format),
        _ => ComboFormat::INVALID,
    }
}

/// S-015-R28: `obs_property_list_item_count` (0 on a non-LIST).
pub fn property_list_item_count(p: &ObsProperty) -> usize {
    match &p.inner.borrow().kind {
        Kind::List { items, .. } => items.len(),
        _ => 0,
    }
}

// ---------------------------------------------------------------------------
// R29/R30: add and insert (QUIRK Q-3, D-5)
// ---------------------------------------------------------------------------

/// S-015-R29: `obs_property_list_add_string`. Returns the index of the new
/// item; on a wrong format (or non-LIST) it is a silent no-op returning 0
/// (QUIRK Q-3: indistinguishable from "stored at index 0").
pub fn property_list_add_string(p: &ObsProperty, name: Option<&str>, value: Option<&str>) -> usize {
    let mut g = p.inner.borrow_mut();
    let items = match &mut g.kind {
        Kind::List { format, items, .. } if *format == ComboFormat::STRING.as_raw() => items,
        _ => return 0,
    };
    let idx = items.len();
    items.push(ListItem {
        name: copy(name),
        disabled: false,
        value: ItemValue::Str(copy(value)),
    });
    idx
}

/// S-015-R29: `obs_property_list_add_int`.
pub fn property_list_add_int(p: &ObsProperty, name: Option<&str>, value: i64) -> usize {
    let mut g = p.inner.borrow_mut();
    let items = match &mut g.kind {
        Kind::List { format, items, .. } if *format == ComboFormat::INT.as_raw() => items,
        _ => return 0,
    };
    let idx = items.len();
    items.push(ListItem {
        name: copy(name),
        disabled: false,
        value: ItemValue::Int(value),
    });
    idx
}

/// S-015-R29: `obs_property_list_add_float`.
pub fn property_list_add_float(p: &ObsProperty, name: Option<&str>, value: f64) -> usize {
    let mut g = p.inner.borrow_mut();
    let items = match &mut g.kind {
        Kind::List { format, items, .. } if *format == ComboFormat::FLOAT.as_raw() => items,
        _ => return 0,
    };
    let idx = items.len();
    items.push(ListItem {
        name: copy(name),
        disabled: false,
        value: ItemValue::Float(value),
    });
    idx
}

/// S-015-R29: `obs_property_list_add_bool` — any non-zero input stores
/// `true` (QUIRK Q-11: the reference reads the union member `double`; with
/// its zero-initialised storage the result equals the stored bool).
pub fn property_list_add_bool(p: &ObsProperty, name: Option<&str>, value: i32) -> usize {
    let mut g = p.inner.borrow_mut();
    let items = match &mut g.kind {
        Kind::List { format, items, .. } if *format == ComboFormat::BOOL.as_raw() => items,
        _ => return 0,
    };
    let idx = items.len();
    items.push(ListItem {
        name: copy(name),
        disabled: false,
        value: ItemValue::Bool(value != 0),
    });
    idx
}

/// S-015-R30: `obs_property_list_insert_string`. Inserts at `idx` (shifting
/// later items up; `idx == count` appends) and returns `idx`. `idx > count`
/// is ignored (D-5); a wrong format is a silent no-op (Q-3).
pub fn property_list_insert_string(
    p: &ObsProperty,
    idx: usize,
    name: Option<&str>,
    value: Option<&str>,
) -> usize {
    let mut g = p.inner.borrow_mut();
    let items = match &mut g.kind {
        Kind::List { format, items, .. } if *format == ComboFormat::STRING.as_raw() => items,
        _ => return 0,
    };
    if idx > items.len() {
        return 0;
    }
    items.insert(
        idx,
        ListItem {
            name: copy(name),
            disabled: false,
            value: ItemValue::Str(copy(value)),
        },
    );
    idx
}

/// S-015-R30: `obs_property_list_insert_int`.
pub fn property_list_insert_int(
    p: &ObsProperty,
    idx: usize,
    name: Option<&str>,
    value: i64,
) -> usize {
    let mut g = p.inner.borrow_mut();
    let items = match &mut g.kind {
        Kind::List { format, items, .. } if *format == ComboFormat::INT.as_raw() => items,
        _ => return 0,
    };
    if idx > items.len() {
        return 0;
    }
    items.insert(
        idx,
        ListItem {
            name: copy(name),
            disabled: false,
            value: ItemValue::Int(value),
        },
    );
    idx
}

/// S-015-R30: `obs_property_list_insert_float`.
pub fn property_list_insert_float(
    p: &ObsProperty,
    idx: usize,
    name: Option<&str>,
    value: f64,
) -> usize {
    let mut g = p.inner.borrow_mut();
    let items = match &mut g.kind {
        Kind::List { format, items, .. } if *format == ComboFormat::FLOAT.as_raw() => items,
        _ => return 0,
    };
    if idx > items.len() {
        return 0;
    }
    items.insert(
        idx,
        ListItem {
            name: copy(name),
            disabled: false,
            value: ItemValue::Float(value),
        },
    );
    idx
}

/// S-015-R30: `obs_property_list_insert_bool`.
pub fn property_list_insert_bool(
    p: &ObsProperty,
    idx: usize,
    name: Option<&str>,
    value: i32,
) -> usize {
    let mut g = p.inner.borrow_mut();
    let items = match &mut g.kind {
        Kind::List { format, items, .. } if *format == ComboFormat::BOOL.as_raw() => items,
        _ => return 0,
    };
    if idx > items.len() {
        return 0;
    }
    items.insert(
        idx,
        ListItem {
            name: copy(name),
            disabled: false,
            value: ItemValue::Bool(value != 0),
        },
    );
    idx
}

// ---------------------------------------------------------------------------
// R31: remove / clear
// ---------------------------------------------------------------------------

/// S-015-R31: `obs_property_list_item_remove` — removes the item at `idx`
/// (later items shift down); out of range or non-LIST is a no-op.
pub fn property_list_item_remove(p: &ObsProperty, idx: usize) {
    if let Kind::List { items, .. } = &mut p.inner.borrow_mut().kind
        && idx < items.len()
    {
        items.remove(idx);
    }
}

/// S-015-R31: `obs_property_list_clear` — removes all items; type and
/// format are kept.
pub fn property_list_clear(p: &ObsProperty) {
    if let Kind::List { items, .. } = &mut p.inner.borrow_mut().kind {
        items.clear();
    }
}

// ---------------------------------------------------------------------------
// R32: disable
// ---------------------------------------------------------------------------

/// S-015-R32: `obs_property_list_item_disable`.
pub fn property_list_item_disable(p: &ObsProperty, idx: usize, disabled: bool) {
    if let Kind::List { items, .. } = &mut p.inner.borrow_mut().kind
        && idx < items.len()
    {
        items[idx].disabled = disabled;
    }
}

/// S-015-R32: `obs_property_list_item_disabled` — `false` out of range or
/// on a non-LIST.
pub fn property_list_item_disabled(p: &ObsProperty, idx: usize) -> bool {
    match &p.inner.borrow().kind {
        Kind::List { items, .. } => items.get(idx).is_some_and(|i| i.disabled),
        _ => false,
    }
}

// ---------------------------------------------------------------------------
// R33: item access
// ---------------------------------------------------------------------------

/// S-015-R33: `obs_property_list_item_name` — the item's name (may be
/// `None`); `None` out of range or on a non-LIST.
pub fn property_list_item_name(p: &ObsProperty, idx: usize) -> Option<String> {
    match &p.inner.borrow().kind {
        Kind::List { items, .. } => items.get(idx).and_then(|i| i.name.clone()),
        _ => None,
    }
}

/// S-015-R33: `obs_property_list_item_string` — the value iff `p` is a
/// LIST of the matching format and `idx < count` (a NULL value reads back
/// as `None`).
pub fn property_list_item_string(p: &ObsProperty, idx: usize) -> Option<String> {
    match &p.inner.borrow().kind {
        Kind::List { format, items, .. } if *format == ComboFormat::STRING.as_raw() => {
            match items.get(idx) {
                Some(ListItem {
                    value: ItemValue::Str(v),
                    ..
                }) => v.clone(),
                _ => None,
            }
        }
        _ => None,
    }
}

/// S-015-R33: `obs_property_list_item_int` (0 on mismatch).
pub fn property_list_item_int(p: &ObsProperty, idx: usize) -> i64 {
    match &p.inner.borrow().kind {
        Kind::List { format, items, .. } if *format == ComboFormat::INT.as_raw() => items
            .get(idx)
            .and_then(|i| match &i.value {
                ItemValue::Int(v) => Some(*v),
                _ => None,
            })
            .unwrap_or(0),
        _ => 0,
    }
}

/// S-015-R33: `obs_property_list_item_float` (0.0 on mismatch).
pub fn property_list_item_float(p: &ObsProperty, idx: usize) -> f64 {
    match &p.inner.borrow().kind {
        Kind::List { format, items, .. } if *format == ComboFormat::FLOAT.as_raw() => items
            .get(idx)
            .and_then(|i| match &i.value {
                ItemValue::Float(v) => Some(*v),
                _ => None,
            })
            .unwrap_or(0.0),
        _ => 0.0,
    }
}

/// S-015-R33: `obs_property_list_item_bool` (false on mismatch).
pub fn property_list_item_bool(p: &ObsProperty, idx: usize) -> bool {
    match &p.inner.borrow().kind {
        Kind::List { format, items, .. } if *format == ComboFormat::BOOL.as_raw() => items
            .get(idx)
            .and_then(|i| match &i.value {
                ItemValue::Bool(v) => Some(*v),
                _ => None,
            })
            .unwrap_or(false),
        _ => false,
    }
}

// ---------------------------------------------------------------------------
// R36–R40: FRAME_RATE
// ---------------------------------------------------------------------------

/// S-015-R36: `obs_properties_add_frame_rate` — two empty arrays.
pub fn properties_add_frame_rate(
    props: &ObsProperties,
    name: &str,
    desc: Option<&str>,
) -> Option<ObsProperty> {
    add_checked(
        &props.inner,
        name,
        PropType::FRAME_RATE,
        copy(desc),
        Kind::FrameRate {
            options: vec![],
            ranges: vec![],
        },
    )
    .map(ObsProperty::new)
}

/// S-015-R37: `obs_property_frame_rate_option_add` — appends and returns
/// the new index; `(usize)-1` on a non-FRAME_RATE.
pub fn property_frame_rate_option_add(
    p: &ObsProperty,
    name: Option<&str>,
    description: Option<&str>,
) -> usize {
    let mut g = p.inner.borrow_mut();
    let options = match &mut g.kind {
        Kind::FrameRate { options, .. } => options,
        _ => return usize::MAX,
    };
    let idx = options.len();
    options.push(FpsOption {
        name: copy(name),
        description: copy(description),
    });
    idx
}

/// S-015-R37: `obs_property_frame_rate_fps_range_add` — values are not
/// validated (zero denominators are stored); `(usize)-1` on a
/// non-FRAME_RATE.
pub fn property_frame_rate_fps_range_add(p: &ObsProperty, min: MediaFps, max: MediaFps) -> usize {
    let mut g = p.inner.borrow_mut();
    let ranges = match &mut g.kind {
        Kind::FrameRate { ranges, .. } => ranges,
        _ => return usize::MAX,
    };
    let idx = ranges.len();
    ranges.push(FpsRange { min, max });
    idx
}

/// S-015-R38: `obs_property_frame_rate_option_insert` — inserts at `idx`
/// (`idx == count` appends; `idx > count` ignored, D-5) and returns `idx`;
/// a non-FRAME_RATE is a no-op.
pub fn property_frame_rate_option_insert(
    p: &ObsProperty,
    idx: usize,
    name: Option<&str>,
    description: Option<&str>,
) -> usize {
    let mut g = p.inner.borrow_mut();
    let options = match &mut g.kind {
        Kind::FrameRate { options, .. } => options,
        _ => return usize::MAX,
    };
    if idx > options.len() {
        return 0;
    }
    options.insert(
        idx,
        FpsOption {
            name: copy(name),
            description: copy(description),
        },
    );
    idx
}

/// S-015-R38: `obs_property_frame_rate_fps_range_insert`.
pub fn property_frame_rate_fps_range_insert(
    p: &ObsProperty,
    idx: usize,
    min: MediaFps,
    max: MediaFps,
) -> usize {
    let mut g = p.inner.borrow_mut();
    let ranges = match &mut g.kind {
        Kind::FrameRate { ranges, .. } => ranges,
        _ => return usize::MAX,
    };
    if idx > ranges.len() {
        return 0;
    }
    ranges.insert(idx, FpsRange { min, max });
    idx
}

// ---------------------------------------------------------------------------
// R39: counts and getters
// ---------------------------------------------------------------------------

/// S-015-R39: `obs_property_frame_rate_options_count` (0 on a
/// non-FRAME_RATE).
pub fn frame_rate_options_count(p: &ObsProperty) -> usize {
    match &p.inner.borrow().kind {
        Kind::FrameRate { options, .. } => options.len(),
        _ => 0,
    }
}

/// S-015-R39: `obs_property_frame_rate_fps_ranges_count` (0 on a
/// non-FRAME_RATE).
pub fn frame_rate_fps_ranges_count(p: &ObsProperty) -> usize {
    match &p.inner.borrow().kind {
        Kind::FrameRate { ranges, .. } => ranges.len(),
        _ => 0,
    }
}

/// S-015-R39: `obs_property_frame_rate_option_name` (maybe `None`);
/// `None` out of range or on a non-FRAME_RATE.
pub fn frame_rate_option_name(p: &ObsProperty, idx: usize) -> Option<String> {
    match &p.inner.borrow().kind {
        Kind::FrameRate { options, .. } => options.get(idx).and_then(|o| o.name.clone()),
        _ => None,
    }
}

/// S-015-R39: `obs_property_frame_rate_option_description`.
pub fn frame_rate_option_description(p: &ObsProperty, idx: usize) -> Option<String> {
    match &p.inner.borrow().kind {
        Kind::FrameRate { options, .. } => options.get(idx).and_then(|o| o.description.clone()),
        _ => None,
    }
}

/// S-015-R39: `obs_property_frame_rate_fps_range_min` — the stored value
/// or `{0, 0}` out of range / on a non-FRAME_RATE.
pub fn frame_rate_fps_range_min(p: &ObsProperty, idx: usize) -> MediaFps {
    match &p.inner.borrow().kind {
        Kind::FrameRate { ranges, .. } => ranges.get(idx).map(|r| r.min).unwrap_or_default(),
        _ => MediaFps::default(),
    }
}

/// S-015-R39: `obs_property_frame_rate_fps_range_max`.
pub fn frame_rate_fps_range_max(p: &ObsProperty, idx: usize) -> MediaFps {
    match &p.inner.borrow().kind {
        Kind::FrameRate { ranges, .. } => ranges.get(idx).map(|r| r.max).unwrap_or_default(),
        _ => MediaFps::default(),
    }
}

// ---------------------------------------------------------------------------
// R40: clears
// ---------------------------------------------------------------------------

/// S-015-R40: `obs_property_frame_rate_clear` — empties both arrays.
pub fn frame_rate_clear(p: &ObsProperty) {
    if let Kind::FrameRate {
        options, ranges, ..
    } = &mut p.inner.borrow_mut().kind
    {
        options.clear();
        ranges.clear();
    }
}

/// S-015-R40: `obs_property_frame_rate_options_clear` — options only.
pub fn frame_rate_options_clear(p: &ObsProperty) {
    if let Kind::FrameRate { options, .. } = &mut p.inner.borrow_mut().kind {
        options.clear();
    }
}

/// S-015-R40: `obs_property_frame_rate_fps_ranges_clear` — ranges only.
pub fn frame_rate_fps_ranges_clear(p: &ObsProperty) {
    if let Kind::FrameRate { ranges, .. } = &mut p.inner.borrow_mut().kind {
        ranges.clear();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::model::properties_create;
    use crate::{
        group_content, properties_first, properties_get, property_enabled, property_get_type,
        property_int_min, property_next, property_set_enabled, property_set_visible,
        property_visible,
    };

    fn fps(n: u32, d: u32) -> MediaFps {
        MediaFps {
            numerator: n,
            denominator: d,
        }
    }

    // vector 08 — list string
    #[test]
    fn v08_list_string() {
        let p = properties_create();
        let l = properties_add_list(&p, "l", Some("List"), ComboType::LIST, ComboFormat::STRING)
            .unwrap();
        assert_eq!(property_list_type(&l), ComboType::LIST);
        assert_eq!(property_list_format(&l), ComboFormat::STRING);
        assert_eq!(property_list_item_count(&l), 0);

        assert_eq!(property_list_add_string(&l, Some("One"), Some("1")), 0);
        assert_eq!(property_list_add_string(&l, Some("Two"), Some("2")), 1);
        assert_eq!(property_list_add_string(&l, Some("Three"), Some("3")), 2);
        assert_eq!(property_list_item_count(&l), 3);

        // insert shifts later items up; idx == count appends
        assert_eq!(
            property_list_insert_string(&l, 0, Some("Zero"), Some("0")),
            0
        );
        assert_eq!(
            property_list_insert_string(&l, 2, Some("Mid"), Some("m")),
            2
        );
        assert_eq!(
            property_list_insert_string(&l, 5, Some("End"), Some("e")),
            5
        );
        assert_eq!(property_list_item_count(&l), 6);
        let names: Vec<_> = (0..6)
            .map(|i| property_list_item_name(&l, i).unwrap())
            .collect();
        assert_eq!(names, vec!["Zero", "One", "Mid", "Two", "Three", "End"]);
        assert_eq!(property_list_item_string(&l, 2).as_deref(), Some("m"));

        // remove
        property_list_item_remove(&l, 0);
        property_list_item_remove(&l, 3);
        property_list_item_remove(&l, 99); // out of range: no-op
        assert_eq!(property_list_item_count(&l), 4);
        let names: Vec<_> = (0..4)
            .map(|i| property_list_item_name(&l, i).unwrap())
            .collect();
        assert_eq!(names, vec!["One", "Mid", "Two", "End"]);

        // disable
        property_list_item_disable(&l, 1, true);
        property_list_item_disable(&l, 99, true); // out of range: no-op
        assert!(property_list_item_disabled(&l, 1));
        assert!(!property_list_item_disabled(&l, 0));
        property_list_item_disable(&l, 1, false);
        assert!(!property_list_item_disabled(&l, 1));

        // clear keeps the list kind
        property_list_clear(&l);
        assert_eq!(property_list_item_count(&l), 0);
        assert_eq!(property_list_type(&l), ComboType::LIST);
        assert_eq!(property_list_format(&l), ComboFormat::STRING);
        assert_eq!(property_list_add_string(&l, Some("After"), Some("a")), 0);

        // item access out of range
        assert!(property_list_item_name(&l, 1).is_none());
        assert!(property_list_item_string(&l, 1).is_none());
        assert_eq!(property_list_item_int(&l, 1), 0);
        assert_eq!(property_list_item_float(&l, 1), 0.0);
        assert!(!property_list_item_bool(&l, 1));
        assert!(!property_list_item_disabled(&l, 1));

        // null name and null value
        property_list_clear(&l);
        property_list_add_string(&l, None, Some("v"));
        property_list_add_string(&l, Some("n"), None);
        assert!(property_list_item_name(&l, 0).is_none());
        assert_eq!(property_list_item_string(&l, 0).as_deref(), Some("v"));
        assert_eq!(property_list_item_name(&l, 1).as_deref(), Some("n"));
        assert!(property_list_item_string(&l, 1).is_none());
    }

    // vector 09 — int/float/bool lists (QUIRK Q-3)
    #[test]
    fn v09_list_int_float_bool() {
        let p = properties_create();
        let li =
            properties_add_list(&p, "li", Some("Int"), ComboType::RADIO, ComboFormat::INT).unwrap();
        let lf = properties_add_list(&p, "lf", Some("Float"), ComboType::LIST, ComboFormat::FLOAT)
            .unwrap();
        let lb = properties_add_list(&p, "lb", Some("Bool"), ComboType::LIST, ComboFormat::BOOL)
            .unwrap();

        assert_eq!(property_list_add_int(&li, Some("a"), 10), 0);
        assert_eq!(property_list_add_int(&li, Some("b"), i64::MIN), 1);
        assert_eq!(property_list_add_int(&li, Some("c"), i64::MAX), 2);
        assert_eq!(property_list_insert_int(&li, 1, Some("ins"), 5), 1);
        assert_eq!(property_list_item_count(&li), 4);
        assert_eq!(property_list_item_int(&li, 0), 10);
        assert_eq!(property_list_item_int(&li, 1), 5);
        assert_eq!(property_list_item_int(&li, 2), i64::MIN);
        assert_eq!(property_list_item_int(&li, 3), i64::MAX);

        assert_eq!(property_list_add_float(&lf, Some("x"), 1.5), 0);
        assert_eq!(property_list_add_float(&lf, Some("y"), -0.1), 1);
        assert_eq!(property_list_insert_float(&lf, 0, Some("z"), 1e300), 0);
        assert_eq!(property_list_item_float(&lf, 0), 1e300);
        assert_eq!(property_list_item_float(&lf, 1), 1.5);
        assert_eq!(property_list_item_float(&lf, 2), -0.1);

        // any non-zero input stores true
        assert_eq!(property_list_add_bool(&lb, Some("t"), 1), 0);
        assert_eq!(property_list_add_bool(&lb, Some("f"), 0), 1);
        assert_eq!(property_list_add_bool(&lb, Some("t2"), 7), 2);
        assert!(property_list_item_bool(&lb, 0));
        assert!(!property_list_item_bool(&lb, 1));
        assert!(property_list_item_bool(&lb, 2));

        // wrong-format adds are silent no-ops returning 0 (Q-3)
        assert_eq!(property_list_add_string(&li, Some("bad"), Some("s")), 0);
        assert_eq!(property_list_add_float(&li, Some("bad"), 1.0), 0);
        assert_eq!(property_list_add_bool(&li, Some("bad"), 1), 0);
        assert_eq!(property_list_add_int(&lf, Some("bad"), 1), 0);
        assert_eq!(property_list_add_int(&lb, Some("bad"), 1), 0);
        assert_eq!(property_list_add_string(&lb, Some("bad"), Some("x")), 0);
        assert_eq!(
            property_list_insert_string(&li, 0, Some("bad"), Some("s")),
            0
        );
        assert_eq!(property_list_insert_int(&lf, 0, Some("bad"), 1), 0);
        assert_eq!(property_list_insert_float(&lb, 0, Some("bad"), 1.0), 0);
        assert_eq!(property_list_item_count(&li), 4);
        assert_eq!(property_list_item_count(&lf), 3);
        assert_eq!(property_list_item_count(&lb), 3);

        // item access with every getter (cross-format reads give defaults)
        assert_eq!(property_list_item_string(&li, 0), None);
        assert_eq!(property_list_item_int(&lf, 0), 0);
        assert!(property_list_item_bool(&lb, 0));
        assert!(!property_list_item_bool(&lb, 1));

        // D-5: insert beyond the end is ignored
        assert_eq!(property_list_insert_int(&li, 99, Some("x"), 1), 0);
        assert_eq!(property_list_item_count(&li), 4);
    }

    // vector 10 — list creation rules
    #[test]
    fn v10_list_creation_rules() {
        let p = properties_create();
        assert!(
            properties_add_list(
                &p,
                "e1",
                Some("E1"),
                ComboType::EDITABLE,
                ComboFormat::STRING
            )
            .is_some()
        );
        // editable + non-string: refused (with a log)
        assert!(
            properties_add_list(&p, "e2", Some("E2"), ComboType::EDITABLE, ComboFormat::INT)
                .is_none()
        );
        assert!(
            properties_add_list(
                &p,
                "e3",
                Some("E3"),
                ComboType::EDITABLE,
                ComboFormat::FLOAT
            )
            .is_none()
        );
        assert!(
            properties_add_list(&p, "e4", Some("E4"), ComboType::EDITABLE, ComboFormat::BOOL)
                .is_none()
        );
        assert!(
            properties_add_list(
                &p,
                "e5",
                Some("E5"),
                ComboType::EDITABLE,
                ComboFormat::INVALID
            )
            .is_none()
        );
        // every other combination is accepted, incl. invalid/out-of-range
        assert!(
            properties_add_list(&p, "r1", Some("R1"), ComboType::RADIO, ComboFormat::STRING)
                .is_some()
        );
        let l1 = properties_add_list(&p, "l1", Some("L1"), ComboType::LIST, ComboFormat::INVALID)
            .unwrap();
        let l2 = properties_add_list(&p, "l2", Some("L2"), ComboType::INVALID, ComboFormat::INT)
            .unwrap();
        let l3 = properties_add_list(
            &p,
            "l3",
            Some("L3"),
            ComboType::Unknown(9),
            ComboFormat::Unknown(7),
        )
        .unwrap();
        assert_eq!(property_list_type(&l1), ComboType::LIST);
        assert_eq!(property_list_format(&l1), ComboFormat::INVALID);
        assert_eq!(property_list_type(&l2), ComboType::INVALID);
        assert_eq!(property_list_format(&l2), ComboFormat::INT);
        assert_eq!(property_list_type(&l3), ComboType::Unknown(9));
        assert_eq!(property_list_format(&l3), ComboFormat::Unknown(7));

        // item ops on a non-list (or wrong format) are no-ops
        assert_eq!(property_list_add_string(&l1, Some("x"), Some("y")), 0);
        assert_eq!(property_list_add_int(&l1, Some("x"), 1), 0);
        assert_eq!(property_list_add_int(&l2, Some("x"), 1), 0);
        assert_eq!(property_list_add_string(&l2, Some("x"), Some("y")), 0);
        assert_eq!(property_list_item_count(&l1), 0);
        // l2 is a list of INT format: the int add above succeeded
        assert_eq!(property_list_item_count(&l2), 1);
        assert_eq!(property_list_item_name(&l2, 0).as_deref(), Some("x"));
        assert_eq!(property_list_item_int(&l2, 0), 1);

        let b = crate::properties_add_bool(&p, "notlist", Some("x")).unwrap();
        assert_eq!(property_list_add_string(&b, Some("x"), Some("y")), 0);
        assert_eq!(property_list_insert_string(&b, 0, Some("x"), Some("y")), 0);
        property_list_item_remove(&b, 0);
        property_list_clear(&b);
        property_list_item_disable(&b, 0, true);
        assert!(property_list_item_name(&b, 0).is_none());
        assert!(!property_list_item_disabled(&b, 0));

        // R09 has priority over the editable refusal
        assert!(
            properties_add_list(&p, "l1", Some("dup"), ComboType::EDITABLE, ComboFormat::INT)
                .is_none()
        );
        assert!(
            properties_get(&p, "l1")
                .map(|x| x.is_same(&l1))
                .unwrap_or(false)
        );
    }

    // vector 13 — frame rate
    #[test]
    fn v13_frame_rate() {
        let p = properties_create();
        let fr = properties_add_frame_rate(&p, "fr", Some("FPS")).unwrap();
        assert_eq!(property_get_type(&fr), PropType::FRAME_RATE);
        assert_eq!(frame_rate_options_count(&fr), 0);
        assert_eq!(frame_rate_fps_ranges_count(&fr), 0);

        assert_eq!(
            property_frame_rate_option_add(&fr, Some("Auto"), Some("Automatic")),
            0
        );
        assert_eq!(property_frame_rate_option_add(&fr, Some("Pal"), None), 1);
        assert_eq!(property_frame_rate_option_add(&fr, None, Some("x")), 2);
        assert_eq!(
            property_frame_rate_option_insert(&fr, 0, Some("First"), Some("first desc")),
            0
        );
        assert_eq!(
            property_frame_rate_option_insert(&fr, 4, Some("Last"), Some("last desc")),
            4
        );
        assert_eq!(frame_rate_options_count(&fr), 5);
        assert_eq!(frame_rate_option_name(&fr, 0).as_deref(), Some("First"));
        assert_eq!(
            frame_rate_option_description(&fr, 0).as_deref(),
            Some("first desc")
        );
        assert_eq!(frame_rate_option_name(&fr, 1).as_deref(), Some("Auto"));
        assert_eq!(frame_rate_option_name(&fr, 2).as_deref(), Some("Pal"));
        assert_eq!(frame_rate_option_name(&fr, 3), None);
        assert_eq!(frame_rate_option_description(&fr, 3).as_deref(), Some("x"));
        assert_eq!(frame_rate_option_name(&fr, 4).as_deref(), Some("Last"));
        assert!(frame_rate_option_name(&fr, 5).is_none());

        assert_eq!(
            property_frame_rate_fps_range_add(&fr, fps(30, 1), fps(60, 1)),
            0
        );
        assert_eq!(
            property_frame_rate_fps_range_add(&fr, fps(24000, 1001), fps(30000, 1001)),
            1
        );
        assert_eq!(
            property_frame_rate_fps_range_insert(&fr, 0, fps(1, 1), fps(10, 1)),
            0
        );
        // zero denominators are stored (no validation)
        assert_eq!(
            property_frame_rate_fps_range_insert(&fr, 3, fps(0, 0), fps(5, 0)),
            3
        );
        assert_eq!(frame_rate_fps_ranges_count(&fr), 4);
        assert_eq!(frame_rate_fps_range_min(&fr, 0), fps(1, 1));
        assert_eq!(frame_rate_fps_range_max(&fr, 0), fps(10, 1));
        assert_eq!(frame_rate_fps_range_min(&fr, 1), fps(30, 1));
        assert_eq!(frame_rate_fps_range_max(&fr, 2), fps(30000, 1001));
        assert_eq!(frame_rate_fps_range_min(&fr, 3), fps(0, 0));
        assert_eq!(frame_rate_fps_range_max(&fr, 3), fps(5, 0));
        assert_eq!(frame_rate_fps_range_min(&fr, 4), MediaFps::default());

        // clear options only
        frame_rate_options_clear(&fr);
        assert_eq!(frame_rate_options_count(&fr), 0);
        assert_eq!(frame_rate_fps_ranges_count(&fr), 4);

        // clear ranges only
        property_frame_rate_option_add(&fr, Some("Again"), Some("again"));
        frame_rate_fps_ranges_clear(&fr);
        assert_eq!(frame_rate_options_count(&fr), 1);
        assert_eq!(frame_rate_fps_ranges_count(&fr), 0);

        // clear all (+ no-op clear)
        property_frame_rate_fps_range_add(&fr, fps(25, 1), fps(25, 1));
        frame_rate_clear(&fr);
        frame_rate_clear(&fr);
        assert_eq!(frame_rate_options_count(&fr), 0);
        assert_eq!(frame_rate_fps_ranges_count(&fr), 0);

        // reuse after clear
        assert_eq!(property_frame_rate_option_add(&fr, Some("n"), Some("d")), 0);
        assert_eq!(
            property_frame_rate_fps_range_add(&fr, fps(1, 1), fps(2, 1)),
            0
        );
        assert_eq!(frame_rate_options_count(&fr), 1);
        assert_eq!(frame_rate_fps_ranges_count(&fr), 1);

        // wrong kind
        let b = crate::properties_add_bool(&p, "b", Some("x")).unwrap();
        assert_eq!(
            property_frame_rate_option_add(&b, Some("n"), Some("d")),
            usize::MAX
        );
        assert_eq!(
            property_frame_rate_fps_range_add(&b, fps(1, 1), fps(2, 1)),
            usize::MAX
        );
        assert_eq!(
            property_frame_rate_option_insert(&b, 0, Some("n"), Some("d")),
            usize::MAX
        );
        assert_eq!(
            property_frame_rate_fps_range_insert(&b, 0, fps(1, 1), fps(2, 1)),
            usize::MAX
        );
        frame_rate_clear(&b);
        frame_rate_options_clear(&b);
        frame_rate_fps_ranges_clear(&b);
        assert_eq!(frame_rate_options_count(&b), 0);
        assert_eq!(frame_rate_fps_ranges_count(&b), 0);
        assert!(frame_rate_option_name(&b, 0).is_none());
        assert!(frame_rate_option_description(&b, 0).is_none());
        assert_eq!(frame_rate_fps_range_min(&b, 0), MediaFps::default());
        assert_eq!(frame_rate_fps_range_max(&b, 0), MediaFps::default());
    }

    // vector 22 — adding to group content after adoption; visible/enabled
    // toggles on children
    #[test]
    fn v22_group_content_adds() {
        let p = properties_create();
        let g = properties_create();
        let gp = crate::properties_add_group(&p, "g", Some("G"), GroupType::NORMAL, &g).unwrap();
        drop(g);

        let content = group_content(&gp).unwrap();
        let ch = crate::properties_add_int(&content, "child", Some("c"), 0, 10, 1).unwrap();
        // findable from the top
        assert!(
            properties_get(&p, "child")
                .map(|x| x.is_same(&ch))
                .unwrap_or(false)
        );
        // iteration stays in its own container
        let mut a = properties_first(&content);
        assert!(a.is_some());
        assert!(!property_next(&mut a));
        property_set_visible(&ch, false);
        property_set_enabled(&ch, false);
        assert!(!property_visible(&ch));
        assert!(!property_enabled(&ch));
        assert_eq!(property_int_min(&ch), 0);
    }
}
