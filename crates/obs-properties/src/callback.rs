//! S-015 §5.5: callbacks (modified, button clicked) and
//! `obs_properties_apply_settings` (§6.2).

use std::os::raw::c_void;
use std::rc::Rc;

use crate::model::{ContainerRc, Kind, ObsProperties, ObsProperty, PropRc, topmost};
use crate::{Modified2Cb, ModifiedCb, SettingsHandle};

/// S-015-R16: `obs_property_set_modified_callback` — stores the v1
/// callback; `None` clears it.
pub fn property_set_modified_callback(p: &ObsProperty, cb: Option<ModifiedCb>) {
    p.inner.borrow_mut().modified = cb;
}

/// S-015-R16: `obs_property_set_modified_callback2` — stores the v2
/// callback and **overwrites the property's single `priv` slot** (QUIRK
/// Q-4) — also when `cb` or the priv is NULL.
pub fn property_set_modified_callback2(
    p: &ObsProperty,
    cb: Option<Modified2Cb>,
    priv_: *mut c_void,
) {
    let mut g = p.inner.borrow_mut();
    g.modified2 = cb;
    g.priv_slot = priv_;
}

/// S-015-R17: `obs_property_modified`. The v1 callback wins when both are
/// set (QUIRK Q-5); the properties argument is the **top-most** container
/// of the property's owner. The callback is taken out of the property
/// before the call (S-015 §9) so it may mutate the property and container.
pub fn property_modified(p: &ObsProperty, settings: &SettingsHandle) -> bool {
    let (cb1, cb2, priv_slot, owner) = {
        let g = p.inner.borrow();
        (g.modified, g.modified2, g.priv_slot, g.owner.upgrade())
    };
    let Some(owner) = owner else {
        return false;
    };
    let Some(top) = topmost(&owner) else {
        return false;
    };
    let top = ObsProperties { inner: top };
    if let Some(cb) = cb1 {
        cb(&top, p, settings)
    } else if let Some(cb) = cb2 {
        cb(priv_slot, &top, p, settings)
    } else {
        false
    }
}

/// S-015-R18: `obs_property_button_clicked`. `obj_data` is the object's
/// private data pointer (the C `obj->data`, S-015 §3.2); it is used only
/// when the property's priv slot is NULL (QUIRK Q-4). Returns `false` for
/// a non-BUTTON or a button without callback.
pub fn property_button_clicked(p: &ObsProperty, obj_data: *mut c_void) -> bool {
    let (cb, priv_slot, owner) = {
        let g = p.inner.borrow();
        let cb = match &g.kind {
            Kind::Button { callback, .. } => *callback,
            _ => return false,
        };
        (cb, g.priv_slot, g.owner.upgrade())
    };
    let Some(cb) = cb else {
        return false;
    };
    let Some(owner) = owner else {
        return false;
    };
    let Some(top) = topmost(&owner) else {
        return false;
    };
    let top = ObsProperties { inner: top };
    let data = if priv_slot.is_null() {
        obj_data
    } else {
        priv_slot
    };
    cb(&top, p, data)
}

/// §6.2: `gather` — depth-first pre-gather: a group's nested properties
/// are gathered **before** the group property itself, in insertion order.
fn gather(c: &ContainerRc, list: &mut Vec<PropRc>) {
    let props = c.borrow().props.clone();
    for p in props {
        if let Kind::Group { content, .. } = &p.borrow().kind {
            gather(content, list);
        }
        let has_cb = p.borrow().modified.is_some() || p.borrow().modified2.is_some();
        if has_cb {
            list.push(p.clone());
        }
    }
}

/// S-015-R19: `obs_properties_apply_settings`. Runs every gathered
/// property's callback in **reverse** gathering order (QUIRK Q-8), passing
/// **this** container (not the property's owner, not the top-most) as the
/// properties argument. The callback to run is chosen at call time: v1 if
/// set, else v2 (with its priv). Return values are ignored. DEFECT D-6: a
/// property removed from the container during the run is skipped.
pub fn properties_apply_settings(c: &ObsProperties, settings: &SettingsHandle) {
    let mut list = vec![];
    gather(&c.inner, &mut list);
    while let Some(p) = list.pop() {
        // D-6: skip properties removed during the run.
        let still = p
            .borrow()
            .owner
            .upgrade()
            .is_some_and(|o| o.borrow().props.iter().any(|q| Rc::ptr_eq(q, &p)));
        if !still {
            continue;
        }
        let (cb1, cb2, priv_slot) = {
            let g = p.borrow();
            (g.modified, g.modified2, g.priv_slot)
        };
        let prop = ObsProperty::new(p.clone());
        if let Some(cb) = cb1 {
            cb(c, &prop, settings);
        } else if let Some(cb) = cb2 {
            cb(priv_slot, c, &prop, settings);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::enums::*;
    use crate::model::properties_create;
    use crate::{
        group_content, properties_add_bool, properties_add_button, properties_add_button2,
        properties_add_group, properties_add_list, properties_get, properties_remove_by_name,
        property_button_clicked, property_list_add_string, property_list_clear,
        property_list_item_count, property_list_item_name,
    };
    use std::sync::Mutex;

    /// (container, property, priv) — the priv is 0 for v1 callbacks.
    static SEEN: Mutex<Vec<(usize, usize, usize)>> = Mutex::new(Vec::new());

    /// Serializes the tests that share the recording statics.
    static TEST_LOCK: Mutex<()> = Mutex::new(());

    fn clear() {
        SEEN.lock().unwrap().clear();
    }
    fn seen() -> Vec<(usize, usize, usize)> {
        SEEN.lock().unwrap().clone()
    }

    fn record_pair(props: &ObsProperties, prop: &ObsProperty, _s: &SettingsHandle) -> bool {
        SEEN.lock().unwrap().push((
            Rc::as_ptr(&props.inner) as usize,
            Rc::as_ptr(&prop.inner) as usize,
            0,
        ));
        true
    }

    fn cb_true(_props: &ObsProperties, _prop: &ObsProperty, _s: &SettingsHandle) -> bool {
        true
    }
    fn cb_false(_props: &ObsProperties, _prop: &ObsProperty, _s: &SettingsHandle) -> bool {
        false
    }

    fn cb2(
        priv_: *mut c_void,
        props: &ObsProperties,
        prop: &ObsProperty,
        _s: &SettingsHandle,
    ) -> bool {
        SEEN.lock().unwrap().push((
            Rc::as_ptr(&props.inner) as usize,
            Rc::as_ptr(&prop.inner) as usize,
            priv_ as usize,
        ));
        true
    }

    fn token(t: &'static str) -> *mut c_void {
        Box::leak(Box::new(t)) as *mut &'static str as *mut c_void
    }

    fn prop_ptr(p: &ObsProperty) -> usize {
        Rc::as_ptr(&p.inner) as usize
    }
    fn container_ptr(c: &ObsProperties) -> usize {
        Rc::as_ptr(&c.inner) as usize
    }

    // ------------------------------------------------------------------
    // vector 17 — modified callbacks
    // ------------------------------------------------------------------
    #[test]
    fn v17_modified_callbacks() {
        let _lock = TEST_LOCK.lock().unwrap();
        clear();
        let top = properties_create();
        let g = properties_create();
        let h = properties_create();
        let deep = properties_add_bool(&h, "deep", Some("Deep")).unwrap();
        properties_add_group(&g, "inner", Some("Inner"), GroupType::NORMAL, &h).unwrap();
        drop(h);
        let mid = properties_add_bool(&g, "mid", Some("Mid")).unwrap();
        properties_add_group(&top, "grp", Some("Grp"), GroupType::NORMAL, &g).unwrap();
        drop(g);
        let flat = properties_add_bool(&top, "flat", Some("Flat")).unwrap();
        let s = SettingsHandle::NULL;

        // no callback: false
        assert!(!property_modified(&flat, &s));
        assert!(!property_modified(&deep, &s));

        // v1 callback: return value passed through
        property_set_modified_callback(&flat, Some(cb_true));
        assert!(property_modified(&flat, &s));
        property_set_modified_callback(&flat, Some(cb_false));
        assert!(!property_modified(&flat, &s));

        // callbacks on nested properties run; they see the TOP-most container
        property_set_modified_callback(&mid, Some(record_pair));
        property_set_modified_callback(&deep, Some(record_pair));
        assert!(property_modified(&mid, &s));
        assert!(property_modified(&deep, &s));
        let calls = seen();
        assert_eq!(calls.len(), 2);
        let top_ptr = container_ptr(&top);
        assert_eq!(calls[0], (top_ptr, prop_ptr(&mid), 0));
        assert_eq!(calls[1], (top_ptr, prop_ptr(&deep), 0));
        clear();

        // modified2 gets the priv
        property_set_modified_callback(&flat, None);
        let priv1 = token("tpriv");
        property_set_modified_callback2(&flat, Some(cb2), priv1);
        assert!(property_modified(&flat, &s));
        let (c, prop, priv_seen) = seen().pop().unwrap();
        assert_eq!(c, top_ptr);
        assert_eq!(prop, prop_ptr(&flat));
        assert_eq!(priv_seen, priv1 as usize);
        clear();
        property_set_modified_callback2(&flat, Some(cb2), std::ptr::null_mut());
        assert!(property_modified(&flat, &s));
        let (c, _, priv_seen) = seen().pop().unwrap();
        assert_eq!(c, top_ptr);
        assert_eq!(priv_seen, 0, "NULL priv is passed through");
        clear();

        // v1 wins over v2
        property_set_modified_callback(&flat, Some(cb_true));
        property_set_modified_callback2(&flat, Some(cb2), token("tpriv2"));
        assert!(property_modified(&flat, &s));
        assert!(seen().is_empty(), "v1 (non-recording) ran, v2 did not");
        clear();

        // clearing v1 lets v2 run; clearing v2 leaves nothing
        property_set_modified_callback(&flat, None);
        assert!(property_modified(&flat, &s));
        assert_eq!(seen().len(), 1);
        clear();
        property_set_modified_callback2(&flat, None, std::ptr::null_mut());
        assert!(!property_modified(&flat, &s));

        let _ = (deep, mid, flat);
    }

    // ------------------------------------------------------------------
    // vector 18 — apply_settings order (QUIRK Q-8)
    // ------------------------------------------------------------------
    #[test]
    fn v18_apply_settings_order() {
        let _lock = TEST_LOCK.lock().unwrap();
        clear();
        let top = properties_create();
        let g1 = properties_create();
        let g2 = properties_create();
        let first = properties_add_bool(&top, "first", Some("First")).unwrap();
        let g1a = properties_add_bool(&g1, "g1a", Some("G1a")).unwrap();
        let g1b = properties_add_bool(&g1, "g1b", Some("G1b")).unwrap();
        let g2a = properties_add_bool(&g2, "g2a", Some("G2a")).unwrap();
        let sub = properties_add_group(&g1, "sub", Some("Sub"), GroupType::NORMAL, &g2).unwrap();
        let grp = properties_add_group(&top, "grp", Some("Grp"), GroupType::NORMAL, &g1).unwrap();
        let last = properties_add_bool(&top, "last", Some("Last")).unwrap();
        let _nocb = properties_add_bool(&top, "nocb", Some("No callback")).unwrap();
        drop(g2);
        let s = SettingsHandle::NULL;

        // gather order = first, g1a, g1b, g2a, sub, grp, last
        // execution order = the reverse; the container argument is always
        // the container apply was called on
        property_set_modified_callback(&first, Some(record_pair));
        property_set_modified_callback(&g1a, Some(record_pair));
        property_set_modified_callback2(&g1b, Some(cb2), token("tk"));
        property_set_modified_callback(&g2a, Some(record_pair));
        property_set_modified_callback(&sub, Some(record_pair));
        property_set_modified_callback2(&grp, Some(cb2), token("tk"));
        property_set_modified_callback(&last, Some(record_pair));

        properties_apply_settings(&top, &s);

        let top_ptr = container_ptr(&top);
        let calls = seen();
        assert_eq!(calls.len(), 7, "every callback property runs once");
        let order: Vec<usize> = calls.iter().map(|(_, p, _)| *p).collect();
        assert_eq!(
            order,
            vec![
                prop_ptr(&last),
                prop_ptr(&grp),
                prop_ptr(&sub),
                prop_ptr(&g2a),
                prop_ptr(&g1b),
                prop_ptr(&g1a),
                prop_ptr(&first)
            ]
        );
        // every call got the applied container
        assert!(calls.iter().all(|(c, _, _)| *c == top_ptr));
        clear();

        // applying on a group content passes THAT container
        let content = group_content(&sub).unwrap();
        properties_apply_settings(&content, &s);
        let calls = seen();
        // g2a has the only callback in g2's content
        assert_eq!(calls.len(), 1);
        assert_eq!(calls[0], (container_ptr(&content), prop_ptr(&g2a), 0));
        let _ = (first, g1a, g1b, g2a, sub, grp, last);
    }

    // ------------------------------------------------------------------
    // vector 11 — button clicks (R18)
    // ------------------------------------------------------------------
    #[test]
    fn v11_button_clicks() {
        let _lock = TEST_LOCK.lock().unwrap();
        clear();
        let p = properties_create();
        let b1 = properties_add_button(&p, "b1", Some("Btn1"), Some(click_ctx)).unwrap();
        let privp = token("tk_priv");
        let b2 = properties_add_button2(&p, "b2", Some("Btn2"), Some(click_priv), privp).unwrap();
        let b3 = properties_add_button2(
            &p,
            "b3",
            Some("Btn3"),
            Some(click_ctx),
            std::ptr::null_mut(),
        )
        .unwrap();
        let b4 = properties_add_button(&p, "b4", Some("Btn4"), None).unwrap();
        let b5 = properties_add_button2(&p, "b5", Some("Btn5"), None, privp).unwrap();
        let ctx = token("tk_ctx");

        // the callback gets the top-most container
        assert!(property_button_clicked(&b1, ctx));
        let (c, prop, priv_seen) = seen().pop().unwrap();
        assert_eq!(c, container_ptr(&p));
        assert_eq!(prop, prop_ptr(&b1));
        assert_eq!(priv_seen, ctx as usize);
        assert_eq!(data_pop(), ctx as usize);
        // a NULL object: data NULL
        assert!(property_button_clicked(&b1, std::ptr::null_mut()));
        assert_eq!(data_pop(), 0);
        clear();
        tag_data_reset();

        // button2 gets its priv regardless of the context
        assert!(property_button_clicked(&b2, ctx));
        assert_eq!(data_pop(), privp as usize);
        assert!(property_button_clicked(&b2, std::ptr::null_mut()));
        assert_eq!(data_pop(), privp as usize);
        tag_data_reset();

        // button2 with NULL priv falls back to the context data
        assert!(property_button_clicked(&b3, ctx));
        assert_eq!(data_pop(), ctx as usize);
        assert!(property_button_clicked(&b3, std::ptr::null_mut()));
        assert_eq!(data_pop(), 0);
        tag_data_reset();

        // no callback: false
        assert!(!property_button_clicked(&b4, ctx));
        assert!(!property_button_clicked(&b5, ctx));

        // clicking a non-button: false
        let nb = properties_add_bool(&p, "nb", Some("x")).unwrap();
        assert!(!property_button_clicked(&nb, ctx));

        let _ = (b1, b2, b3, b4, b5, nb);
    }

    // ------------------------------------------------------------------
    // vector 24 — the shared priv slot (QUIRK Q-4)
    // ------------------------------------------------------------------
    #[test]
    fn v24_priv_sharing_quirk() {
        let _lock = TEST_LOCK.lock().unwrap();
        clear();
        let p = properties_create();
        let tbtn = token("tbtn");
        let b = properties_add_button2(&p, "b", Some("B"), Some(click_priv), tbtn).unwrap();
        let ctx = token("tctx");

        // the click receives the button2 priv
        assert!(property_button_clicked(&b, ctx));
        assert_eq!(data_pop(), tbtn as usize);
        tag_data_reset();

        // set_modified_callback2 replaces the priv the click receives
        let tmod = token("tmod");
        property_set_modified_callback2(&b, Some(cb2), tmod);
        assert!(property_button_clicked(&b, ctx));
        assert_eq!(data_pop(), tmod as usize);
        tag_data_reset();

        // a NULL priv on modified2 sends clicks back to the context data
        property_set_modified_callback2(&b, Some(cb2), std::ptr::null_mut());
        assert!(property_button_clicked(&b, ctx));
        assert_eq!(data_pop(), ctx as usize);
        tag_data_reset();

        // the modified callback sees the same slot
        property_set_modified_callback2(&b, Some(cb2), tmod);
        let s = SettingsHandle::NULL;
        assert!(property_modified(&b, &s));
        let (_, _, priv_seen) = seen().pop().unwrap();
        assert_eq!(priv_seen, tmod as usize);
        assert!(property_button_clicked(&b, std::ptr::null_mut()));
        assert_eq!(data_pop(), tmod as usize);
    }

    // the button-callback data recording (function pointers cannot
    // capture; a static per test process — the button tests run in
    // separate test functions and reset their own state)
    static DATA: Mutex<Vec<usize>> = Mutex::new(Vec::new());
    fn data_pop() -> usize {
        DATA.lock().unwrap().pop().unwrap()
    }
    fn tag_data_reset() {
        DATA.lock().unwrap().clear();
    }
    fn click_priv(props: &ObsProperties, prop: &ObsProperty, data: *mut c_void) -> bool {
        SEEN.lock().unwrap().push((
            Rc::as_ptr(&props.inner) as usize,
            Rc::as_ptr(&prop.inner) as usize,
            data as usize,
        ));
        DATA.lock().unwrap().push(data as usize);
        true
    }
    fn click_ctx(props: &ObsProperties, prop: &ObsProperty, data: *mut c_void) -> bool {
        SEEN.lock().unwrap().push((
            Rc::as_ptr(&props.inner) as usize,
            Rc::as_ptr(&prop.inner) as usize,
            data as usize,
        ));
        DATA.lock().unwrap().push(data as usize);
        true
    }

    // ------------------------------------------------------------------
    // vector 23 — callbacks edit the layout
    // ------------------------------------------------------------------
    #[test]
    fn v23_callbacks_edit_layout() {
        let top = properties_create();
        let g = properties_create();
        let trigger = properties_add_bool(&top, "trigger", Some("Trigger")).unwrap();
        let _victim = properties_add_bool(&top, "victim", Some("Victim")).unwrap();
        let _keep = properties_add_bool(&top, "keep", Some("Keep")).unwrap();
        let combo = properties_add_list(
            &top,
            "combo",
            Some("Combo"),
            ComboType::LIST,
            ComboFormat::STRING,
        )
        .unwrap();
        properties_add_group(&top, "grp", Some("Grp"), GroupType::NORMAL, &g).unwrap();
        let inner = properties_add_bool(&g, "inner", Some("Inner")).unwrap();
        let s = SettingsHandle::NULL;

        // a v1 callback may add, remove, hide, disable and re-describe
        // OTHER properties (it receives the top-most container)
        property_set_modified_callback(&trigger, Some(layout_cb));
        assert!(property_modified(&trigger, &s));

        // the changes are visible
        assert!(properties_get(&top, "extra").is_some());
        assert!(properties_get(&top, "victim").is_none());
        let keep2 = properties_get(&top, "keep").unwrap();
        assert!(!crate::property_visible(&keep2));
        assert!(!crate::property_enabled(&keep2));
        assert_eq!(
            crate::property_description(&keep2).as_deref(),
            Some("Edited")
        );

        // list refill: the callback fills items; running it twice clears
        // and refills each time
        assert_eq!(property_list_item_count(&combo), 0);
        let refill = properties_add_bool(&top, "refill", Some("Refill")).unwrap();
        property_set_modified_callback(&refill, Some(fill_cb));
        assert!(property_modified(&refill, &s));
        assert!(property_modified(&refill, &s));
        assert_eq!(property_list_item_count(&combo), 2);
        assert_eq!(property_list_item_name(&combo, 0).as_deref(), Some("First"));
        assert_eq!(
            property_list_item_name(&combo, 1).as_deref(),
            Some("Second")
        );

        // a callback of a property inside a group edits the TOP container
        property_set_modified_callback(&inner, Some(from_group_cb));
        assert!(property_modified(&inner, &s));
        assert!(properties_get(&top, "fromgroup").is_some());

        let _ = (trigger, combo, refill, inner);
    }

    fn layout_cb(props: &ObsProperties, _prop: &ObsProperty, _s: &SettingsHandle) -> bool {
        properties_add_bool(props, "extra", Some("Extra"));
        properties_remove_by_name(props, "victim");
        if let Some(keep) = properties_get(props, "keep") {
            crate::property_set_visible(&keep, false);
            crate::property_set_enabled(&keep, false);
            crate::property_set_description(&keep, Some("Edited"));
        }
        true
    }
    fn fill_cb(props: &ObsProperties, _prop: &ObsProperty, _s: &SettingsHandle) -> bool {
        if let Some(combo) = properties_get(props, "combo") {
            property_list_clear(&combo);
            property_list_add_string(&combo, Some("First"), Some("1"));
            property_list_add_string(&combo, Some("Second"), Some("2"));
        }
        true
    }
    fn from_group_cb(props: &ObsProperties, _prop: &ObsProperty, _s: &SettingsHandle) -> bool {
        properties_add_bool(props, "fromgroup", Some("From group"));
        true
    }

    // D-6 — a property removed during apply is skipped
    #[test]
    fn d6_remove_during_apply_skipped() {
        let top = properties_create();
        let a = properties_add_bool(&top, "a", Some("A")).unwrap();
        let b = properties_add_bool(&top, "b", Some("B")).unwrap();
        // "b"'s callback removes "a" (which has a callback); gather order
        // a, b -> execution b, a; a must be skipped
        property_set_modified_callback(&a, Some(keep_a_cb));
        property_set_modified_callback(&b, Some(remove_a_cb));
        properties_apply_settings(&top, &SettingsHandle::NULL);
        assert_eq!(ORDER.lock().unwrap().clone(), vec!["b"]);
        let _ = (a, b);
    }
    static ORDER: Mutex<Vec<&'static str>> = Mutex::new(Vec::new());
    fn keep_a_cb(_props: &ObsProperties, _p: &ObsProperty, _s: &SettingsHandle) -> bool {
        ORDER.lock().unwrap().push("a");
        true
    }
    fn remove_a_cb(props: &ObsProperties, _p: &ObsProperty, _s: &SettingsHandle) -> bool {
        ORDER.lock().unwrap().push("b");
        properties_remove_by_name(props, "a");
        true
    }
}
