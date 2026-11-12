//! S-015 §4 state model: the container, the properties, the kind data,
//! and the tree algorithms (§6.1 lookup/uniqueness, §6.2 apply).
//!
//! The tree is `Rc`-based (S-015 §9): a property holds a **weak** link back
//! to its owner container and a group's content container holds a **weak**
//! link to the group property, so no reference cycle can leak; destroying
//! the container drops the properties in insertion order (R06) and a group
//! property drops its content with it.
//!
//! Pointer stability (S-015 §3.3): a property is one `Rc` allocation that
//! never moves, so two handles naming the same property share an identity
//! (`Rc::ptr_eq`) — the Rust analogue of the C pointer-stability contract.

use std::cell::RefCell;
use std::os::raw::c_void;
use std::rc::{Rc, Weak};

use crate::enums::*;

/// A callback that destroys a container `param` (S-015 R03).
pub type DestroyCb = fn(*mut c_void);
/// `obs_property_modified_t`: returns "the UI must rebuild the properties".
pub type ModifiedCb = fn(&ObsProperties, &ObsProperty, &SettingsHandle) -> bool;
/// `obs_property_modified2_t` (carries `priv`).
pub type Modified2Cb = fn(*mut c_void, &ObsProperties, &ObsProperty, &SettingsHandle) -> bool;
/// `obs_property_clicked_t`: `data` is the property `priv` or the
/// object's private data pointer (S-015 §3.2, R18).
pub type ClickedCb = fn(&ObsProperties, &ObsProperty, *mut c_void) -> bool;

/// The opaque `obs_data_t` settings handle that modified callbacks receive
/// (S-015 §1: passed through, never read by this component).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SettingsHandle {
    ptr: *mut c_void,
}

impl SettingsHandle {
    pub const NULL: Self = Self {
        ptr: std::ptr::null_mut(),
    };

    /// Wrap an existing `obs_data_t` pointer (FFI side). The handle must
    /// stay valid for the callback call; it is never dereferenced here.
    ///
    /// # Safety
    /// The pointer must be a valid `obs_data_t` for the duration of the
    /// callback call that receives the handle. This component never
    /// dereferences it (S-015 §1).
    pub const unsafe fn from_ptr(ptr: *mut c_void) -> Self {
        Self { ptr }
    }

    pub const fn is_null(self) -> bool {
        self.ptr.is_null()
    }

    pub const fn as_ptr(self) -> *mut c_void {
        self.ptr
    }
}

impl From<&obs_data::model::ObsData> for SettingsHandle {
    fn from(d: &obs_data::model::ObsData) -> Self {
        Self { ptr: d.as_opaque() }
    }
}

/// A `param`/`destroy` slot (S-015 R02–R04).
#[derive(Clone, Copy)]
struct Param {
    ptr: *mut c_void,
    destroy: Option<DestroyCb>,
}

pub(crate) type ContainerRc = Rc<RefCell<Container>>;
pub(crate) type PropRc = Rc<RefCell<Prop>>;

/// A list item (S-015 §4 kind data).
#[derive(Debug)]
#[allow(dead_code)] // fields read by the kind getters (next commit)
pub(crate) struct ListItem {
    pub(crate) name: Option<String>,
    pub(crate) disabled: bool,
    pub(crate) value: ItemValue,
}

/// The item's value; which variant is legal depends on the list format.
#[derive(Debug)]
#[allow(dead_code)] // read by the kind getters (next commit)
pub(crate) enum ItemValue {
    Str(Option<String>),
    Int(i64),
    Float(f64),
    Bool(bool),
}

/// A frame-rate option (S-015 R36).
#[derive(Debug)]
#[allow(dead_code)] // read by the kind getters (next commit)
pub(crate) struct FpsOption {
    pub(crate) name: Option<String>,
    pub(crate) description: Option<String>,
}

/// A frame-rate fps range (S-015 R36).
#[derive(Debug)]
#[allow(dead_code)] // read by the kind getters (next commit)
pub(crate) struct FpsRange {
    pub(crate) min: MediaFps,
    pub(crate) max: MediaFps,
}

/// The kind-specific data (S-015 §4). Enum fields are raw `u32` so that
/// out-of-range values are stored and returned unchanged (S-015 §3.1).
#[allow(dead_code)] // read by the kind getters (next commit)
pub(crate) enum Kind {
    Bool,
    Int {
        min: i32,
        max: i32,
        step: i32,
        numtype: NumberType,
        suffix: Option<String>,
    },
    Float {
        min: f64,
        max: f64,
        step: f64,
        numtype: NumberType,
        suffix: Option<String>,
    },
    Text {
        #[allow(dead_code)]
        ty: u32,
        monospace: bool,
        info_type: u32,
        word_wrap: bool,
    },
    Path {
        ty: u32,
        filter: Option<String>,
        default_path: Option<String>,
    },
    List {
        combo_type: u32,
        format: u32,
        items: Vec<ListItem>,
    },
    Color,
    Button {
        callback: Option<ClickedCb>,
        ty: u32,
        url: Option<String>,
    },
    Font,
    EditableList {
        ty: u32,
        filter: Option<String>,
        default_path: Option<String>,
    },
    FrameRate {
        options: Vec<FpsOption>,
        ranges: Vec<FpsRange>,
    },
    Group {
        ty: u32,
        content: ContainerRc,
    },
    ColorAlpha,
}

/// An `obs_property_t` (S-015 §4).
#[allow(dead_code)] // fields read by the kind getters (next commit)
pub(crate) struct Prop {
    pub(crate) name: String,
    pub(crate) desc: Option<String>,
    pub(crate) long_desc: Option<String>,
    pub(crate) ty: PropType,
    pub(crate) visible: bool,
    pub(crate) enabled: bool,
    /// Weak back-reference to the owning container (S-015 §9: no cycle).
    pub(crate) owner: Weak<RefCell<Container>>,
    pub(crate) modified: Option<ModifiedCb>,
    pub(crate) modified2: Option<Modified2Cb>,
    /// ONE slot shared by `add_button2` and `set_modified_callback2`
    /// (QUIRK Q-4).
    pub(crate) priv_slot: *mut c_void,
    pub(crate) kind: Kind,
}

/// An `obs_properties_t` (S-015 §4).
pub(crate) struct Container {
    /// Declared before `props` on purpose: a manual `Drop` runs the body
    /// first (the outer param callback, R06) and then drops the fields in
    /// declaration order, so the properties are destroyed in insertion
    /// order after the param.
    param: Option<Param>,
    flags: u32,
    pub(crate) props: Vec<PropRc>,
    /// Number of GROUP properties directly in `props` (R09/R10 shortcut).
    group_count: usize,
    /// The GROUP property whose content this container is (R11), or none.
    parent: Option<Weak<RefCell<Prop>>>,
}

impl Drop for Container {
    fn drop(&mut self) {
        // R06: destroy(param) first, only when BOTH are present.
        if let Some(p) = self.param {
            // R06: destroy(param) only when BOTH are non-NULL.
            if let Some(destroy) = p.destroy.filter(|_| !p.ptr.is_null()) {
                destroy(p.ptr);
            }
        }
        // `props` (and with it every group's content container) drops in
        // declaration order after this body.
    }
}

/// A container handle (the `obs_properties_t` of the safe API).
///
/// `Clone` adds nothing — handles to a container are shared by identity;
/// drop releases nothing either (the container lives until the last handle
/// that was made from a C reference; in pure Rust code the `ObsProperties`
/// returned by `create` owns it).
#[derive(Clone)]
pub struct ObsProperties {
    pub(crate) inner: ContainerRc,
}

/// A property handle (the `obs_property_t` of the safe API).
#[derive(Clone)]
pub struct ObsProperty {
    pub(crate) inner: PropRc,
}

impl ObsProperty {
    pub(crate) fn new(r: PropRc) -> Self {
        ObsProperty { inner: r }
    }

    /// Pointer stability (S-015 §3.3): two handles name the same property
    /// iff they share the allocation.
    pub fn is_same(&self, other: &ObsProperty) -> bool {
        Rc::ptr_eq(&self.inner, &other.inner)
    }
}

// ---------------------------------------------------------------------------
// §6.1 lookup and uniqueness
// ---------------------------------------------------------------------------

/// `topmost(c)`: follow parent links until none (S-015 §6.1).
pub(crate) fn topmost(c: &ContainerRc) -> Option<ContainerRc> {
    let mut cur = c.clone();
    loop {
        let parent = {
            let g = cur.borrow();
            g.parent.as_ref().and_then(Weak::upgrade)
        };
        let parent = match parent {
            Some(p) => p,
            None => return Some(cur),
        };
        let owner = match parent.borrow().owner.upgrade() {
            Some(o) => o,
            None => return Some(cur),
        };
        cur = owner;
    }
}

/// The direct group contents of `c`, in insertion order.
fn group_contents(c: &ContainerRc) -> Vec<ContainerRc> {
    let g = c.borrow();
    if g.group_count == 0 {
        return vec![];
    }
    g.props
        .iter()
        .filter_map(|p| match &p.borrow().kind {
            Kind::Group { content, .. } => Some(content.clone()),
            _ => None,
        })
        .collect()
}

/// `find_direct(c, name)`: the first property of `c` with this exact name.
pub(crate) fn find_direct(c: &ContainerRc, name: &str) -> Option<PropRc> {
    c.borrow()
        .props
        .iter()
        .find(|p| p.borrow().name == name)
        .cloned()
}

/// `tree_contains(c, name)` (S-015 §6.1): direct hit first, then each group
/// content recursively. The reference logs `LOG_WARNING "Property
/// '<name>' exists"` on a hit at any level; this crate has no logger yet
/// (the FFI layer wires it, as in obs-data), so the log call is documented
/// here and in the test matrix.
fn tree_contains(c: &ContainerRc, name: &str) -> bool {
    if find_direct(c, name).is_some() {
        return true;
    }
    for g in group_contents(c) {
        if tree_contains(&g, name) {
            return true;
        }
    }
    false
}

/// `has_prop(c, name) = tree_contains(topmost(c), name)` — the R09 check
/// every `add_*` runs first.
pub(crate) fn has_prop(c: &ContainerRc, name: &str) -> bool {
    match topmost(c) {
        Some(t) => tree_contains(&t, name),
        None => false,
    }
}

/// `get(c, name)` (S-015 R10): own properties first, then each group
/// content in insertion order, recursively. No logging (unlike `add`).
fn get_inner(c: &ContainerRc, name: &str) -> Option<PropRc> {
    if let Some(p) = find_direct(c, name) {
        return Some(p);
    }
    for g in group_contents(c) {
        if let Some(p) = get_inner(&g, name) {
            return Some(p);
        }
    }
    None
}

// ---------------------------------------------------------------------------
// R01–R08: container lifecycle, iteration
// ---------------------------------------------------------------------------

fn new_container() -> ContainerRc {
    Rc::new(RefCell::new(Container {
        param: None,
        flags: 0,
        props: vec![],
        group_count: 0,
        parent: None,
    }))
}

/// S-015-R01: `obs_properties_create` — an empty container.
pub fn properties_create() -> ObsProperties {
    ObsProperties {
        inner: new_container(),
    }
}

/// S-015-R02: `obs_properties_create_param` — create followed by
/// `set_param(param, destroy)`.
pub fn properties_create_param(param: *mut c_void, destroy: Option<DestroyCb>) -> ObsProperties {
    let c = properties_create();
    properties_set_param(&c, param, destroy);
    c
}

/// S-015-R03: `obs_properties_set_param`. If the container currently has
/// **both** a param and a destroy callback, the old destroy runs first.
/// A NULL `destroy` means "never destroy"; a NULL param with a non-NULL
/// destroy is stored (the callback is stored but can never run).
pub fn properties_set_param(c: &ObsProperties, param: *mut c_void, destroy: Option<DestroyCb>) {
    let slot = Param {
        ptr: param,
        destroy,
    };
    let old = c.inner.borrow_mut().param.replace(slot);
    if let Some(p) = old {
        // R03: the old destroy runs only when BOTH param and callback
        // are non-NULL.
        if let Some(destroy) = p.destroy.filter(|_| !p.ptr.is_null()) {
            destroy(p.ptr);
        }
    }
}

/// S-015-R04: `obs_properties_get_param`.
pub fn properties_get_param(c: &ObsProperties) -> *mut c_void {
    c.inner
        .borrow()
        .param
        .map(|p| p.ptr)
        .unwrap_or(std::ptr::null_mut())
}

/// S-015-R05: `set_flags` stores the 32-bit value verbatim.
pub fn properties_set_flags(c: &ObsProperties, flags: u32) {
    c.inner.borrow_mut().flags = flags;
}

/// S-015-R05: `get_flags`.
pub fn properties_get_flags(c: &ObsProperties) -> u32 {
    c.inner.borrow().flags
}

/// S-015-R07: the first property, or none.
pub fn properties_first(c: &ObsProperties) -> Option<ObsProperty> {
    c.inner
        .borrow()
        .props
        .first()
        .map(|r| ObsProperty::new(r.clone()))
}

/// S-015-R08: `obs_property_next`. `None` input: returns false, unchanged.
/// Otherwise advances to the next property in the same container (or
/// `None` after the last) and reports whether one exists. Iteration does
/// not descend into groups.
pub fn property_next(p: &mut Option<ObsProperty>) -> bool {
    let Some(cur) = p.as_ref() else {
        return false;
    };
    let cur = cur.clone();
    let owner = match cur.inner.borrow().owner.upgrade() {
        Some(o) => o,
        // Orphaned (its container is gone): the C pointer would dangle.
        None => {
            *p = None;
            return false;
        }
    };
    let idx = owner
        .borrow()
        .props
        .iter()
        .position(|q| Rc::ptr_eq(q, &cur.inner));
    let next = {
        let g = owner.borrow();
        idx.and_then(|i| g.props.get(i + 1)).cloned()
    };
    let has_next = next.is_some();
    *p = next.map(ObsProperty::new);
    has_next
}

// ---------------------------------------------------------------------------
// R10–R12: lookup, parent, removal
// ---------------------------------------------------------------------------

/// S-015-R10: `obs_properties_get` (deep lookup, no logging).
pub fn properties_get(c: &ObsProperties, name: &str) -> Option<ObsProperty> {
    get_inner(&c.inner, name).map(ObsProperty::new)
}

/// S-015-R11: `obs_properties_get_parent` — the container holding the group
/// property whose content this container is; `None` for a top container.
pub fn properties_get_parent(c: &ObsProperties) -> Option<ObsProperties> {
    let parent = c.inner.borrow().parent.as_ref().and_then(Weak::upgrade)?;
    let owner = parent.borrow().owner.upgrade()?;
    Some(ObsProperties { inner: owner })
}

/// S-015-R12: `obs_properties_remove_by_name`. A direct match is unlinked
/// and destroyed (a group destroys its content and decrements the group
/// count). Otherwise every direct group's content is searched in order.
/// A missing name is silent.
pub fn properties_remove_by_name(c: &ObsProperties, name: &str) {
    remove_inner(&c.inner, name);
}

fn remove_inner(c: &ContainerRc, name: &str) {
    let idx = c
        .borrow()
        .props
        .iter()
        .position(|p| p.borrow().name == name);
    if let Some(i) = idx {
        let mut g = c.borrow_mut();
        let p = g.props.remove(i);
        if matches!(&p.borrow().kind, Kind::Group { .. }) {
            g.group_count -= 1;
        }
        // The group count fix and the Vec shrink happen before `p` drops,
        // so a group's content is destroyed after the unlink (R06/R12).
        // R12: a direct match ends the search — no recursion.
        return;
    }
    for g in group_contents(c) {
        remove_inner(&g, name);
    }
}

// ---------------------------------------------------------------------------
// R13: property construction
// ---------------------------------------------------------------------------

/// Append a property **without** the R09 name check (callers that must
/// order a refusal before/after R09 use this directly).
pub(crate) fn add_unchecked(
    c: &ContainerRc,
    name: &str,
    ty: PropType,
    desc: Option<String>,
    kind: Kind,
) -> PropRc {
    let r = Rc::new(RefCell::new(Prop {
        name: name.to_string(),
        desc,
        long_desc: None,
        ty,
        visible: true,
        enabled: true,
        owner: Weak::new(),
        modified: None,
        modified2: None,
        priv_slot: std::ptr::null_mut(),
        kind,
    }));
    c.borrow_mut().props.push(r.clone());
    if matches!(ty, PropType::GROUP) {
        c.borrow_mut().group_count += 1;
    }
    {
        let mut p = r.borrow_mut();
        p.owner = Rc::downgrade(c);
    }
    r
}

/// Create, name-check (R09) and append a property. Returns the new
/// property or `None` (name duplicate anywhere in the top-most tree).
pub(crate) fn add_checked(
    c: &ContainerRc,
    name: &str,
    ty: PropType,
    desc: Option<String>,
    kind: Kind,
) -> Option<PropRc> {
    if has_prop(c, name) {
        // R09: LOG_WARNING "Property '<name>' exists" — the FFI layer
        // wires the logger (crate has none yet, as in obs-data).
        return None;
    }
    Some(add_unchecked(c, name, ty, desc, kind))
}

/// R41 check 5: would adopting `group` under `target` create a cycle, i.e.
/// does the tree of `group` (transitively) contain `target` as the content
/// of one of its groups? (QUIRK Q-2: silent refusal.)
fn group_tree_contains(target: &ContainerRc, group: &ContainerRc) -> bool {
    for g in group_contents(group) {
        if Rc::ptr_eq(&g, target) {
            return true;
        }
        if group_tree_contains(target, &g) {
            return true;
        }
    }
    false
}

/// S-015-R41: `obs_properties_add_group` (QUIRKs Q-1, Q-2; D-7).
///
/// Rejections, in order: R09 duplicate name (logged); `group == props`;
/// a cycle (the group's tree already contains `props`, silent); the group
/// already adopted elsewhere (D-7); any **direct** child of the group
/// whose name exists in the top-most tree of `props` (logged).
pub fn properties_add_group(
    props: &ObsProperties,
    name: &str,
    desc: Option<&str>,
    ty: GroupType,
    group: &ObsProperties,
) -> Option<ObsProperty> {
    // R09 has priority over every other refusal (R41 check order).
    if has_prop(&props.inner, name) {
        // LOG_WARNING "Property '<name>' exists" — logger not wired yet.
        return None;
    }
    if Rc::ptr_eq(&props.inner, &group.inner) {
        return None;
    }
    if group_tree_contains(&props.inner, &group.inner) {
        return None;
    }
    // D-7: a container that is already a group's content cannot be
    // adopted again (the reference would corrupt the parent link and
    // double-free on destroy).
    if group.inner.borrow().parent.is_some() {
        return None;
    }
    // Q-1: only the group's DIRECT children are checked against the tree.
    for p in &group.inner.borrow().props {
        let n = p.borrow().name.clone();
        if has_prop(&props.inner, &n) {
            // R09 warning for the child name — logger not wired yet.
            return None;
        }
    }
    // The R09 check already ran above, so add_checked cannot refuse here.
    let Some(r) = add_checked(
        &props.inner,
        name,
        PropType::GROUP,
        desc.map(str::to_string),
        Kind::Group {
            ty: ty.as_raw(),
            content: group.inner.clone(),
        },
    ) else {
        unreachable!("R09 check ran first");
    };
    {
        let mut g = group.inner.borrow_mut();
        g.parent = Some(Rc::downgrade(&r));
    }
    Some(ObsProperty::new(r))
}

/// S-015-R42: `group_content`.
pub fn group_content(p: &ObsProperty) -> Option<ObsProperties> {
    match &p.inner.borrow().kind {
        Kind::Group { content, .. } => Some(ObsProperties {
            inner: content.clone(),
        }),
        _ => None,
    }
}

/// S-015-R42: `group_type` — the stored type, `INVALID` on a non-group.
pub fn group_type(p: &ObsProperty) -> GroupType {
    match p.inner.borrow().kind {
        Kind::Group { ty, .. } => GroupType::from_raw(ty),
        _ => GroupType::INVALID,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Mutex;

    /// A BOOL property (test stand-in for the `add_*` family, which lands
    /// in the next commit).
    fn add_bool(c: &ObsProperties, name: &str, desc: &str) -> Option<ObsProperty> {
        add_checked(
            &c.inner,
            name,
            PropType::BOOL,
            Some(desc.to_string()),
            Kind::Bool,
        )
        .map(ObsProperty::new)
    }

    fn names(c: &ObsProperties) -> Vec<String> {
        let g = c.inner.borrow();
        g.props.iter().map(|p| p.borrow().name.clone()).collect()
    }

    // vector 01 — container basics
    #[test]
    fn v01_container_basics() {
        let p = properties_create();
        assert_eq!(properties_get_flags(&p), 0);
        assert!(properties_get_param(&p).is_null());
        assert!(properties_first(&p).is_none());

        add_bool(&p, "zeta", "Z").unwrap();
        add_checked(
            &p.inner,
            "alpha",
            PropType::INT,
            Some("A".into()),
            Kind::Int {
                min: 0,
                max: 10,
                step: 1,
                numtype: NumberType::SCROLLER,
                suffix: None,
            },
        )
        .unwrap();
        add_bool(&p, "mid", "M").unwrap();
        add_bool(&p, "beta", "B").unwrap();
        assert_eq!(names(&p), vec!["zeta", "alpha", "mid", "beta"]);

        // first/next, incl. next at the end (stays None)
        let mut a = properties_first(&p);
        assert!(a.is_some());
        for _ in 0..3 {
            assert!(property_next(&mut a));
        }
        // at the end: false, stays None; further calls keep it None
        assert!(!property_next(&mut a));
        assert!(a.is_none());
        assert!(!property_next(&mut a));
        assert!(a.is_none());

        // flags are stored raw
        properties_set_flags(&p, 1);
        assert_eq!(properties_get_flags(&p), 1);
        properties_set_flags(&p, 0xdeadbeef);
        assert_eq!(properties_get_flags(&p), 0xdeadbeef);
    }

    // vector 02 — param and destroy order
    static CALLS: Mutex<Vec<&'static str>> = Mutex::new(Vec::new());

    fn destroy_token(p: *mut c_void) {
        CALLS
            .lock()
            .unwrap()
            .push(unsafe { *(p as *const &'static str) });
    }

    fn param(tok: &'static str) -> *mut c_void {
        Box::leak(Box::new(tok)) as *mut &'static str as *mut c_void
    }

    #[test]
    fn v02_param_and_destroy_order() {
        CALLS.lock().unwrap().clear();

        // plain param with destroy
        let t1 = param("t1");
        let a = properties_create_param(t1, Some(destroy_token));
        assert_eq!(properties_get_param(&a), t1);
        drop(a);
        assert_eq!(*CALLS.lock().unwrap(), ["t1"]);
        CALLS.lock().unwrap().clear();

        // param without destroy callback: stored, never called
        let t2 = param("t2");
        let b = properties_create_param(t2, None);
        assert_eq!(properties_get_param(&b), t2);
        drop(b);
        assert!(CALLS.lock().unwrap().is_empty());

        // set_param replaces and calls the old destroy (vector 02 exact)
        let c = properties_create_param(t1, None);
        let t2b = param("t2b");
        properties_set_param(&c, t2b, Some(destroy_token));
        // old (t1, nodestroy): nothing runs
        assert!(CALLS.lock().unwrap().is_empty());
        assert_eq!(properties_get_param(&c), t2b);
        let t3 = param("t3");
        properties_set_param(&c, t3, None);
        // old (t2b, destroy): the old destroy runs
        assert_eq!(*CALLS.lock().unwrap(), ["t2b"]);
        CALLS.lock().unwrap().clear();
        assert_eq!(properties_get_param(&c), t3);
        let t4 = param("t4");
        properties_set_param(&c, t4, Some(destroy_token));
        // old (t3, nodestroy): nothing runs
        assert!(CALLS.lock().unwrap().is_empty());
        // NULL param keeps the destroy callback; the old (t4, destroy) runs
        properties_set_param(&c, std::ptr::null_mut(), Some(destroy_token));
        assert_eq!(*CALLS.lock().unwrap(), ["t4"]);
        CALLS.lock().unwrap().clear();
        assert!(properties_get_param(&c).is_null());
        drop(c);
        assert!(
            CALLS.lock().unwrap().is_empty(),
            "no param -> no destroy call"
        );

        // order with groups: outer param first, then properties in order;
        // a group's content container is destroyed when the group is reached
        CALLS.lock().unwrap().clear();
        let top = param("top");
        let t = properties_create_param(top, Some(destroy_token));
        let g1p = param("g1p");
        let g1 = properties_create_param(g1p, Some(destroy_token));
        let g2p = param("g2p");
        let g2 = properties_create_param(g2p, Some(destroy_token));
        let nested = param("nested");
        let g3 = properties_create_param(nested, Some(destroy_token));
        properties_add_group(&g1, "inner", Some("Inner"), GroupType::NORMAL, &g3).unwrap();
        drop(g3);
        properties_add_group(&t, "first", Some("First"), GroupType::NORMAL, &g1).unwrap();
        properties_add_group(&t, "second", Some("Second"), GroupType::NORMAL, &g2).unwrap();
        drop(g1);
        drop(g2);
        drop(t);
        assert_eq!(*CALLS.lock().unwrap(), ["top", "g1p", "nested", "g2p"]);
    }

    // vector 03 — lookup, duplicates, remove
    #[test]
    fn v03_lookup_duplicates_remove() {
        let p = properties_create();
        add_bool(&p, "a", "A").unwrap();
        add_bool(&p, "b", "B").unwrap();
        add_bool(&p, "c", "C").unwrap();
        assert!(add_bool(&p, "a", "again").is_none(), "duplicate");
        assert!(
            add_checked(
                &p.inner,
                "b",
                PropType::INT,
                None,
                Kind::Int {
                    min: 0,
                    max: 1,
                    step: 1,
                    numtype: NumberType::SCROLLER,
                    suffix: None
                },
            )
            .is_none(),
            "duplicate"
        );
        add_bool(&p, "A", "case differs").unwrap();
        assert_eq!(names(&p), vec!["a", "b", "c", "A"]);

        let hb = properties_get(&p, "b").unwrap();
        assert_eq!(hb.inner.borrow().name, "b");
        assert!(properties_get(&p, "nope").is_none());
        assert!(properties_get(&p, "B").is_none(), "case sensitive");

        properties_remove_by_name(&p, "b");
        assert_eq!(names(&p), vec!["a", "c", "A"]);
        // removing a missing name is silent; removing twice is fine
        properties_remove_by_name(&p, "b");
        properties_remove_by_name(&p, "zzz");

        // re-add appends
        add_bool(&p, "b", "B2").unwrap();
        assert_eq!(names(&p), vec!["a", "c", "A", "b"]);

        properties_remove_by_name(&p, "a");
        properties_remove_by_name(&p, "b");
        assert_eq!(names(&p), vec!["c", "A"]);
        properties_remove_by_name(&p, "c");
        properties_remove_by_name(&p, "A");
        assert!(names(&p).is_empty());
        assert!(properties_first(&p).is_none());
        add_bool(&p, "x", "X").unwrap();
        assert_eq!(names(&p), vec!["x"]);
    }

    // vector 14 — groups, lookup, parent
    #[test]
    fn v14_groups() {
        let top = properties_create();
        let g1 = properties_create();
        let g2 = properties_create();
        add_bool(&g1, "a", "A").unwrap();
        add_bool(&g1, "b", "B").unwrap();
        add_bool(&g2, "c", "C").unwrap();

        let g1p =
            properties_add_group(&top, "g1", Some("Group 1"), GroupType::NORMAL, &g1).unwrap();
        add_bool(&top, "t", "T").unwrap();
        let g2p =
            properties_add_group(&top, "g2", Some("Group 2"), GroupType::CHECKABLE, &g2).unwrap();
        assert_eq!(group_type(&g1p), GroupType::NORMAL);
        assert_eq!(group_type(&g2p), GroupType::CHECKABLE);
        assert!(Rc::ptr_eq(&group_content(&g1p).unwrap().inner, &g1.inner));

        // lookup through groups (top-level first, then groups in order)
        assert_eq!(properties_get(&top, "a").unwrap().inner.borrow().name, "a");
        assert!(properties_get(&top, "c").is_some());
        assert!(properties_get(&top, "g2").is_some());
        assert!(properties_get(&top, "nope").is_none());
        // from a group content: its own properties only, no descent
        assert!(properties_get(&g1, "a").is_some());
        assert!(properties_get(&g1, "c").is_none());

        // parent
        assert!(properties_get_parent(&top).is_none());
        assert!(Rc::ptr_eq(
            &properties_get_parent(&g1).unwrap().inner,
            &top.inner
        ));
        assert!(Rc::ptr_eq(
            &properties_get_parent(&g2).unwrap().inner,
            &top.inner
        ));

        // adding to content after the group exists: unique across the tree
        add_bool(&g1, "late", "L").unwrap();
        assert!(add_bool(&g1, "a", "dup").is_none());
        assert!(add_bool(&top, "b", "dup of nested").is_none());
        assert!(add_bool(&g2, "a", "dup").is_none());
        assert!(add_bool(&g2, "t", "dup of top").is_none());

        // group names are unique too
        assert!(add_bool(&top, "g1", "same as group name").is_none());
        assert!(add_bool(&g1, "g2", "group name of sibling").is_none());
    }

    // vector 15 — group rejections
    #[test]
    fn v15_group_rejections() {
        let top = properties_create();
        add_bool(&top, "x", "X").unwrap();
        let g = properties_create();

        // self is a cycle refusal (and would be the same via ptr_eq)
        assert!(properties_add_group(&top, "self", None, GroupType::NORMAL, &top).is_none());

        // duplicate between the group's DIRECT children and the target tree
        add_bool(&g, "x", "x").unwrap();
        assert!(properties_add_group(&top, "g", Some("G"), GroupType::NORMAL, &g).is_none());
        // the rejected group was not adopted
        assert!(g.inner.borrow().parent.is_none());

        // duplicate group name
        let g2 = properties_create();
        assert!(
            properties_add_group(&top, "x", Some("dup name"), GroupType::NORMAL, &g2).is_none()
        );
        assert!(g2.inner.borrow().parent.is_none());

        // fix and retry
        properties_remove_by_name(&g, "x");
        add_bool(&g, "y", "Y").unwrap();
        assert!(properties_add_group(&top, "g", Some("G"), GroupType::NORMAL, &g).is_some());
        drop(g);

        // recursion cycles through nesting: A -> b(B) -> c(C)
        let a = properties_create();
        let b = properties_create();
        let c = properties_create();
        properties_add_group(&a, "b", Some("B"), GroupType::NORMAL, &b).unwrap();
        properties_add_group(&b, "c", Some("C"), GroupType::NORMAL, &c).unwrap();
        // A contains B contains C: C cannot adopt A, B cannot adopt A, A != A2
        assert!(properties_add_group(&c, "a", Some("A"), GroupType::NORMAL, &a).is_none());
        assert!(properties_add_group(&b, "a", Some("A"), GroupType::NORMAL, &a).is_none());
        assert!(properties_add_group(&a, "a2", Some("A2"), GroupType::NORMAL, &a).is_none());
        // not a cycle (sibling): Top can adopt A
        assert!(properties_add_group(&top, "a", Some("A"), GroupType::NORMAL, &a).is_some());
        drop(a);
        drop(b);
        drop(c);
    }

    // vector 16 — group remove
    #[test]
    fn v16_group_remove() {
        CALLS.lock().unwrap().clear();
        let top = properties_create_param(param("tp"), Some(destroy_token));
        let g1 = properties_create_param(param("gp1"), Some(destroy_token));
        let g2 = properties_create_param(param("gp2"), Some(destroy_token));
        add_bool(&g1, "a", "A").unwrap();
        add_bool(&g1, "b", "B").unwrap();
        add_bool(&g2, "c", "C").unwrap();
        properties_add_group(&g1, "inner", Some("Inner"), GroupType::NORMAL, &g2).unwrap();
        properties_add_group(&top, "g", Some("Group"), GroupType::NORMAL, &g1).unwrap();
        add_bool(&top, "t", "T").unwrap();
        drop(g1);
        drop(g2);

        // remove a nested leaf (through the top container)
        properties_remove_by_name(&top, "b");
        assert!(properties_get(&top, "b").is_none());
        assert!(properties_get(&top, "a").is_some());
        properties_remove_by_name(&top, "c");
        assert!(properties_get(&top, "c").is_none());

        // removing the inner group destroys its content (param callback)
        properties_remove_by_name(&top, "inner");
        assert!(properties_get(&top, "c").is_none());
        assert_eq!(*CALLS.lock().unwrap(), ["gp2"]);

        // add after remove
        add_bool(&top, "c", "C again").unwrap();
        // group with a NULL content is refused (C: NULL group) — in the
        // safe API `add_group` takes a reference, so this case is
        // unrepresentable; D-4 covers the NULL-name analogue in the API.

        // removing the group destroys its content
        properties_remove_by_name(&top, "g");
        assert!(properties_get(&top, "a").is_none());
        assert_eq!(*CALLS.lock().unwrap(), ["gp2", "gp1"]);
        drop(top);
        assert_eq!(*CALLS.lock().unwrap(), ["gp2", "gp1", "tp"]);
    }

    // vector 21 — quirks (Q-1)
    #[test]
    fn v21_quirks() {
        let top = properties_create();
        let g = properties_create();
        let h = properties_create();
        add_bool(&top, "x", "X top").unwrap();
        add_bool(&h, "x", "X deep").unwrap();
        properties_add_group(&g, "h", Some("H"), GroupType::NORMAL, &h).unwrap();
        drop(h);
        // a name nested two levels deep does NOT trip the direct-child check
        assert!(properties_add_group(&top, "g", Some("G"), GroupType::NORMAL, &g).is_some());
        drop(g);
        // get returns the first match in search order (top-level first)
        let x = properties_get(&top, "x").unwrap();
        assert_eq!(x.inner.borrow().desc.as_deref(), Some("X top"));

        // removing the top-level match leaves the nested one
        properties_remove_by_name(&top, "x");
        assert!(properties_get(&top, "x").is_some());

        // a group named like its own child
        let k = properties_create();
        let top2 = properties_create();
        add_bool(&k, "same", "child").unwrap();
        assert!(
            properties_add_group(&top2, "same", Some("group"), GroupType::NORMAL, &k).is_some()
        );
        drop(k);
        let s = properties_get(&top2, "same").unwrap();
        assert_eq!(s.inner.borrow().ty, PropType::GROUP);
    }

    // D-7 — double adoption is rejected, the original parent link survives
    #[test]
    fn d7_double_adoption_rejected() {
        let a = properties_create();
        let b = properties_create();
        let c = properties_create();
        properties_add_group(&a, "g", Some("g"), GroupType::NORMAL, &c).unwrap();
        // c already has a parent: a second adoption is refused
        assert!(properties_add_group(&b, "g2", Some("g2"), GroupType::NORMAL, &c).is_none());
        assert!(Rc::ptr_eq(
            &properties_get_parent(&c).unwrap().inner,
            &a.inner
        ));
        // b's destroy must not touch c (not adopted)
        drop(b);
        assert!(c.inner.borrow().parent.is_some());
        drop(a);
        drop(c);
    }

    // an orphaned property (container gone): safe no-ops
    #[test]
    fn orphaned_property() {
        let p = properties_create();
        let prop = add_bool(&p, "a", "A").unwrap();
        let inner = p.inner.clone();
        drop(p);
        // the handle still names a live allocation; tree walks stop
        let mut maybe = Some(prop.clone());
        assert!(!property_next(&mut maybe));
        assert!(maybe.is_none());
        assert!(properties_get_parent(&ObsProperties { inner }).is_none());
    }
}
