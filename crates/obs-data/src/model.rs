//! S-014 §4–§5.1, §5.8–§5.9 — the data model: reference-counted objects,
//! ordered items with three value layers, item handles, arrays, and the
//! §6.1 storage-growth size model (QUIRK Q1: an item moves to the end of
//! the list whenever its modelled storage must grow).
//!
//! Reference counting follows the spec's primary suggestion (§9):
//! `Rc<RefCell<…>>` — `Clone` is addref, drop is release, and the counts
//! are `Rc::strong_count` (the conformance runner's `refs` command
//! peeks at them through [`ObsData::refcount`]).
//!
//! DEFECT fixes (S-014 §8):
//! - **D1**: `set_default_obj`/`set_default_array` on an existing item
//!   without a user layer always takes the reference (the C undercount).
//! - **D3**: a type change discards (releases) the layers that no longer
//!   match the new type instead of reinterpreting their bytes.
//! - **D4**: an item handle is an `Rc` to the item's storage, so growth
//!   never dangles the handle; all handles of an item stay valid.
//! - **D6**: out-of-range array indices are ignored, never a corruption.
//!
//! NULL arguments are unrepresentable in the safe API and noted where the
//! C no-op would apply.

// The public handles expose their `Rc<RefCell<T>>` interiors (crate-internal
// `T`) so the FFI layer and the conformance runner can take references.
#![allow(private_interfaces)]

use std::cell::RefCell;
use std::rc::{Rc, Weak};

/// The item/value type (S-014 §3.1 `obs_data_type` without NULL).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Ty {
    String,
    Number,
    Bool,
    Obj,
    Arr,
}

/// The number type of a stored number value (S-014 §3.1).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum NumType {
    /// NULL item, non-number item, or a number item without any layer.
    Invalid,
    Int,
    Double,
}

/// A value of one layer. Names and strings are byte strings (they may be
/// invalid UTF-8 once set through the FFI; the JSON writer then omits the
/// member, QUIRK Q5).
#[derive(Clone)]
pub(crate) enum Value {
    Str(Vec<u8>),
    Int(i64),
    Dbl(f64),
    Bool(bool),
    /// An object value; `None` is a **present** NULL object (R26).
    Obj(Option<DataRc>),
    /// An array value; `None` is a **present** NULL array (R26).
    Arr(Option<ArrRc>),
}

impl std::fmt::Debug for Value {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Value::Str(b) => write!(f, "Str({:?})", String::from_utf8_lossy(b)),
            Value::Int(i) => write!(f, "Int({i})"),
            Value::Dbl(d) => write!(f, "Dbl({d})"),
            Value::Bool(b) => write!(f, "Bool({b})"),
            Value::Obj(_) => write!(f, "Obj(..)"),
            Value::Arr(_) => write!(f, "Arr(..)"),
        }
    }
}

impl Value {
    fn ty(&self) -> Ty {
        match self {
            Value::Str(_) => Ty::String,
            Value::Int(_) | Value::Dbl(_) => Ty::Number,
            Value::Bool(_) => Ty::Bool,
            Value::Obj(_) => Ty::Obj,
            Value::Arr(_) => Ty::Arr,
        }
    }

    /// §6.1 layer size: string = byte length + 1; number = 16; boolean = 1;
    /// object or array = 8.
    fn size(&self) -> usize {
        match self {
            Value::Str(b) => b.len() + 1,
            Value::Int(_) | Value::Dbl(_) => 16,
            Value::Bool(_) => 1,
            Value::Obj(_) | Value::Arr(_) => 8,
        }
    }
}

pub(crate) type DataRc = Rc<RefCell<Data>>;
pub(crate) type ArrRc = Rc<RefCell<Arr>>;
pub(crate) type ItemRc = Rc<RefCell<Item>>;

/// A settings object (S-014 §3.1 `obs_data_t`).
///
/// `Clone` adds a reference (R01); drop releases one.
#[derive(Clone)]
pub struct ObsData {
    pub(crate) inner: DataRc,
}

impl ObsData {
    /// The current reference count (the conformance runner's `refs`
    /// command; S-014 §4.3 "observable through lifetime only" — the C has
    /// no API for it, the runner peeks at the memory).
    pub fn refcount(&self) -> usize {
        Rc::strong_count(&self.inner)
    }

    /// Identity token for the opaque `obs_data_t` handle that
    /// `obs_properties` callbacks pass around (S-015 §1: the properties
    /// component never dereferences it). Valid while at least one
    /// `ObsData` reference lives; never dereference.
    pub fn as_opaque(&self) -> *mut std::os::raw::c_void {
        Rc::as_ptr(&self.inner) as *mut std::os::raw::c_void
    }

    /// S-014-R01: `obs_data_create`, count 1.
    pub fn create() -> ObsData {
        ObsData {
            inner: Rc::new(RefCell::new(Data {
                items: vec![],
                last_json: None,
            })),
        }
    }
}

/// S-014-R01 inline `obs_data_newref`: addref and return, or a fresh
/// object when `None` (the C NULL case).
pub fn data_newref(d: Option<&ObsData>) -> ObsData {
    match d {
        Some(d) => d.clone(),
        None => ObsData::create(),
    }
}

/// An array (S-014 §3.1 `obs_data_array_t`). Elements are objects, never
/// NULL. `Clone` adds a reference (R01).
#[derive(Clone)]
pub struct ObsDataArray {
    pub(crate) inner: ArrRc,
}

impl ObsDataArray {
    pub fn refcount(&self) -> usize {
        Rc::strong_count(&self.inner)
    }

    /// S-014-R50: count 1, empty.
    pub fn create() -> ObsDataArray {
        ObsDataArray {
            inner: Rc::new(RefCell::new(Arr { elements: vec![] })),
        }
    }
}

/// An item handle (S-014 §3.3): owns one reference to the item; it stays
/// usable after the item is detached from its object (R41).
#[derive(Clone)]
pub struct ObsDataItem {
    pub(crate) item: ItemRc,
}

pub(crate) struct Data {
    /// List order — the source of iteration/JSON key order (§6.1).
    pub(crate) items: Vec<ItemRc>,
    /// The buffer of the most recent `get_json*` call (R81, QUIRK Q4) —
    /// written by the JSON layer.
    #[allow(dead_code)]
    pub(crate) last_json: Option<String>,
}

pub(crate) struct Arr {
    /// Elements are shared: `array_item` returns the same object.
    pub(crate) elements: Vec<DataRc>,
}

pub(crate) struct Item {
    pub(crate) name: Vec<u8>,
    pub(crate) ty: Ty,
    pub(crate) user: Option<Value>,
    pub(crate) default: Option<Value>,
    pub(crate) autoselect: Option<Value>,
    // §6.1 size model
    ds: usize,
    dl: usize,
    fs: usize,
    fl: usize,
    asz: usize,
    cap: usize,
    // ownership / detachment (R41–R42)
    owner: Weak<RefCell<Data>>,
    detached: bool,
    /// The item that followed at detach time (identity, R41).
    next_item: Option<Weak<RefCell<Item>>>,
}

impl Item {
    /// Create an item holding exactly one layer (R10/R11/R12 creation).
    /// §6.1: `cap = n`; the layer's region is set, all others are 0.
    fn new_with_layer(name: Vec<u8>, v: Value, layer: Layer, owner: Weak<RefCell<Data>>) -> ItemRc {
        let n = v.size();
        let mut it = Item {
            name,
            ty: v.ty(),
            user: None,
            default: None,
            autoselect: None,
            ds: 0,
            dl: 0,
            fs: 0,
            fl: 0,
            asz: 0,
            cap: n,
            owner,
            detached: false,
            next_item: None,
        };
        match layer {
            Layer::User => {
                it.user = Some(v);
                it.ds = n;
                it.dl = n;
            }
            Layer::Default => {
                it.default = Some(v);
                it.fs = n;
                it.fl = n;
            }
            Layer::Autoselect => {
                it.autoselect = Some(v);
                it.asz = n;
            }
        }
        Rc::new(RefCell::new(it))
    }

    /// §6.1 size update for a set on an existing item. Returns true when
    /// the item must move to the end of the list (its `cap` grew).
    fn apply_set_sizes(&mut self, layer: Layer, n: usize) -> bool {
        match layer {
            Layer::User => {
                self.ds = n;
                self.dl = if self.fs > 0 || self.asz > 0 {
                    a32(n)
                } else {
                    n
                };
            }
            Layer::Default => {
                self.fs = n;
                self.fl = if self.asz > 0 { a32(n) } else { n };
                self.dl = if self.ds > 0 { a32(self.ds) } else { 0 };
            }
            Layer::Autoselect => {
                self.asz = n;
                self.dl = if self.ds > 0 { a32(self.ds) } else { 0 };
                self.fl = if self.fs > 0 { a32(self.fs) } else { 0 };
            }
        }
        let need = self.dl + self.fl + self.asz;
        if self.cap < need {
            self.cap = need;
            true
        } else {
            false
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub(crate) enum Layer {
    User,
    Default,
    Autoselect,
}

fn a32(n: usize) -> usize {
    (n + 31) & !31
}

/// S-014-R42: effective = user, else default, else autoselect (§4.2).
pub(crate) fn effective_value(item: &Item) -> Option<&Value> {
    item.user
        .as_ref()
        .or(item.default.as_ref())
        .or(item.autoselect.as_ref())
}

fn find_item(items: &[ItemRc], name: &[u8]) -> Option<ItemRc> {
    items.iter().find(|it| it.borrow().name == name).cloned()
}

fn move_to_end(data: &mut Data, item: &ItemRc) {
    if let Some(idx) = data.items.iter().position(|it| Rc::ptr_eq(it, item)) {
        let it = data.items.remove(idx);
        data.items.push(it);
    }
}

/// R10/R11/R12 + D3, with the §6.1 size model.
///
/// - user: creates the item if missing; otherwise the type becomes the
///   value's type (mismatching layers are discarded, D3) and the user
///   layer is replaced (the old value's references released, R03).
/// - default: an existing item whose type differs is **ignored** (R11).
/// - autoselect: like user for the autoselect layer (R12).
pub(crate) fn set_layer_value(inner: &DataRc, layer: Layer, name: &[u8], v: Value) {
    let n = v.size();
    let mut move_item: Option<ItemRc> = None;
    {
        let mut data = inner.borrow_mut();
        match find_item(&data.items, name) {
            None => {
                let owner = Rc::downgrade(inner);
                data.items
                    .push(Item::new_with_layer(name.to_vec(), v, layer, owner));
            }
            Some(it) => {
                let mut b = it.borrow_mut();
                if b.ty != v.ty() {
                    if layer == Layer::Default {
                        return; // R11: ignored, no change
                    }
                    // D3 fix: discard the layers that no longer match
                    b.user.take();
                    b.default.take();
                    b.autoselect.take();
                    b.fs = 0;
                    b.fl = 0;
                    b.asz = 0;
                    b.ty = v.ty();
                }
                match layer {
                    Layer::User => b.user = Some(v),
                    Layer::Default => b.default = Some(v),
                    Layer::Autoselect => b.autoselect = Some(v),
                }
                if b.apply_set_sizes(layer, n) {
                    move_item = Some(it.clone());
                }
            }
        }
    }
    if let Some(it) = move_item {
        let mut data = inner.borrow_mut();
        move_to_end(&mut data, &it);
    }
}

/// R30: remove just one layer (releasing its value's references).
pub(crate) fn unset_layer(inner: &DataRc, layer: Layer, name: &[u8]) {
    let data = inner.borrow_mut();
    if let Some(it) = find_item(&data.items, name) {
        let mut b = it.borrow_mut();
        match layer {
            Layer::User => {
                b.user.take();
                b.ds = 0;
                b.dl = 0;
            }
            Layer::Default => {
                b.default.take();
                b.fs = 0;
                b.fl = 0;
            }
            Layer::Autoselect => {
                b.autoselect.take();
                b.asz = 0;
            }
        }
    }
}

/// R31: remove the item; handles to it stay valid (R41).
pub(crate) fn erase_inner(inner: &DataRc, name: &[u8]) {
    let mut data = inner.borrow_mut();
    if let Some(idx) = data.items.iter().position(|it| it.borrow().name == name) {
        let it = data.items.remove(idx); // releases the list's reference
        let mut b = it.borrow_mut();
        b.detached = true;
        b.next_item = data.items.get(idx).map(Rc::downgrade);
    }
}

/// R31: remove the **user** layer of every item, keeping the items
/// (with their default/autoselect layers, or as shells).
pub(crate) fn clear_inner(inner: &DataRc) {
    let data = inner.borrow_mut();
    for it in &data.items {
        let mut b = it.borrow_mut();
        if b.user.is_some() {
            b.user.take();
            b.ds = 0;
            b.dl = 0;
        }
    }
}

/// The number of items in list order (shells included).
pub fn data_count(d: &ObsData) -> usize {
    d.inner.borrow().items.len()
}

/// Find an item by name (byte-exact, case-sensitive) — a clone of the
/// item's `Rc` (the list's reference is untouched).
pub(crate) fn data_find(d: &ObsData, name: &str) -> Option<ItemRc> {
    let data = d.inner.borrow();
    find_item(&data.items, name.as_bytes())
}

/// A snapshot of the user layer of every item (list order), for
/// [`data_apply`]. The cloned values hold temporary references that are
/// released when the snapshots drop.
pub(crate) struct UserSnapshot {
    pub name: Vec<u8>,
    pub user: Value,
}

fn user_snapshots(d: &DataRc) -> Vec<UserSnapshot> {
    let data = d.borrow();
    data.items
        .iter()
        .filter_map(|it| {
            let b = it.borrow();
            b.user.as_ref().map(|v| UserSnapshot {
                name: b.name.clone(),
                user: v.clone(),
            })
        })
        .collect()
}

/// S-014-R32 `obs_data_apply(target, src)`: copies the user layers of
/// `src` into `target` (only user layers, recursively; NULL object/array
/// members skipped; nothing shared with the source; an existing same-named
/// target value is replaced, never merged). No-op for the same object.
pub fn data_apply(target: &ObsData, src: &ObsData) {
    if Rc::ptr_eq(&target.inner, &src.inner) {
        return;
    }
    apply_user_layers(&target.inner, &src.inner);
}

fn apply_user_layers(t: &DataRc, s: &DataRc) {
    for snap in user_snapshots(s) {
        match &snap.user {
            Value::Str(b) => set_layer_value(t, Layer::User, &snap.name, Value::Str(b.clone())),
            Value::Int(i) => set_layer_value(t, Layer::User, &snap.name, Value::Int(*i)),
            Value::Dbl(f) => set_layer_value(t, Layer::User, &snap.name, Value::Dbl(*f)),
            Value::Bool(b) => set_layer_value(t, Layer::User, &snap.name, Value::Bool(*b)),
            Value::Obj(None) => {} // NULL object member: skip
            Value::Obj(Some(o)) => {
                let copy = Rc::new(RefCell::new(Data {
                    items: vec![],
                    last_json: None,
                }));
                apply_user_layers(&copy, o); // deepcopy(x) = new + apply(new, x)
                set_layer_value(t, Layer::User, &snap.name, Value::Obj(Some(copy)));
            }
            Value::Arr(None) => {} // NULL array: skip
            Value::Arr(Some(a)) => {
                let elems: Vec<DataRc> = a
                    .borrow()
                    .elements
                    .iter()
                    .map(|e| {
                        let c = Rc::new(RefCell::new(Data {
                            items: vec![],
                            last_json: None,
                        }));
                        apply_user_layers(&c, e);
                        c
                    })
                    .collect();
                set_layer_value(
                    t,
                    Layer::User,
                    &snap.name,
                    Value::Arr(Some(Rc::new(RefCell::new(Arr { elements: elems })))),
                );
            }
        }
    }
}

/// S-014-R33 `obs_data_get_defaults`: a new object (count 1) with, for
/// each item of `d` in list order, one user value read from the default
/// layers (missing defaults read as the type's zero; number shells create
/// no member).
pub fn data_get_defaults(d: &ObsData) -> ObsData {
    let out = Rc::new(RefCell::new(Data {
        items: vec![],
        last_json: None,
    }));
    get_defaults_into(&out, &d.inner);
    ObsData { inner: out }
}

fn get_defaults_into(out: &DataRc, d: &DataRc) {
    // list order; the cloned values hold temporary references
    struct DefInfo {
        name: Vec<u8>,
        ty: Ty,
        default_v: Option<Value>,
        eff: Option<Value>,
    }
    let info: Vec<DefInfo> = {
        let data = d.borrow();
        data.items
            .iter()
            .map(|it| {
                let b = it.borrow();
                DefInfo {
                    name: b.name.clone(),
                    ty: b.ty,
                    default_v: b.default.clone(),
                    eff: effective_value(&b).cloned(),
                }
            })
            .collect()
    };
    for DefInfo {
        name,
        ty,
        default_v,
        eff,
    } in info
    {
        match ty {
            Ty::String => {
                let v = match &default_v {
                    Some(Value::Str(b)) => b.clone(),
                    _ => Vec::new(), // missing default reads as ""
                };
                set_layer_value(out, Layer::User, &name, Value::Str(v));
            }
            Ty::Bool => {
                let v = matches!(&default_v, Some(Value::Bool(true)));
                set_layer_value(out, Layer::User, &name, Value::Bool(v));
            }
            Ty::Number => {
                let numtype = match &eff {
                    Some(Value::Int(_)) => NumType::Int,
                    Some(Value::Dbl(_)) => NumType::Double,
                    _ => NumType::Invalid, // shell: no member is created
                };
                let v = match numtype {
                    NumType::Int => Value::Int(match &default_v {
                        Some(Value::Int(i)) => *i,
                        Some(Value::Dbl(f)) => f.trunc() as i64, // R22 + D5 saturating
                        _ => 0,
                    }),
                    NumType::Double => Value::Dbl(match &default_v {
                        Some(Value::Dbl(f)) => *f,
                        Some(Value::Int(i)) => *i as f64,
                        _ => 0.0,
                    }),
                    NumType::Invalid => continue,
                };
                set_layer_value(out, Layer::User, &name, v);
            }
            Ty::Obj => {
                let sub = Rc::new(RefCell::new(Data {
                    items: vec![],
                    last_json: None,
                }));
                if let Some(Value::Obj(Some(o))) = &default_v {
                    get_defaults_into(&sub, o);
                }
                set_layer_value(out, Layer::User, &name, Value::Obj(Some(sub)));
            }
            Ty::Arr => {
                let elems: Vec<DataRc> = if let Some(Value::Arr(Some(a))) = &default_v {
                    let arr = a.borrow();
                    arr.elements
                        .iter()
                        .map(|e| {
                            let c = Rc::new(RefCell::new(Data {
                                items: vec![],
                                last_json: None,
                            }));
                            get_defaults_into(&c, e);
                            c
                        })
                        .collect()
                } else {
                    vec![]
                };
                set_layer_value(
                    out,
                    Layer::User,
                    &name,
                    Value::Arr(Some(Rc::new(RefCell::new(Arr { elements: elems })))),
                );
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Item handles (R40–R43)
// ---------------------------------------------------------------------------

/// R40: a handle to the first item (list order), +1 reference; `None`
/// when the object is empty.
pub fn data_first(d: &ObsData) -> Option<ObsDataItem> {
    let data = d.inner.borrow();
    data.items.first().cloned().map(|item| ObsDataItem { item })
}

/// R40: a handle to the item with this name, +1 reference.
pub fn data_item_byname(d: &ObsData, name: &str) -> Option<ObsDataItem> {
    let data = d.inner.borrow();
    find_item(&data.items, name.as_bytes()).map(|item| ObsDataItem { item })
}

/// R40/R41: `it` becomes the successor (+1 reference on it, the reference
/// on the old item released). For a **detached** handle the successor is
/// the item that followed at detach time (the walk continues). Returns
/// true iff a successor exists; when false, `*it` is `None`.
pub fn data_item_next(it: &mut Option<ObsDataItem>) -> bool {
    let Some(h) = it.as_mut() else {
        return false; // NULL handle / NULL pointer: no effect
    };
    let (detached, next_item, owner) = {
        let b = h.item.borrow();
        (b.detached, b.next_item.clone(), b.owner.clone())
    };
    let Some(owner) = owner.upgrade() else {
        *it = None;
        return false; // the object is gone
    };
    let succ = {
        let data = owner.borrow();
        if detached {
            next_item.and_then(|w| w.upgrade())
        } else {
            data.items
                .iter()
                .position(|i| Rc::ptr_eq(i, &h.item))
                .and_then(|i| data.items.get(i + 1))
                .cloned()
        }
    };
    match succ {
        Some(item) => {
            h.item = item; // the old reference is released
            true
        }
        None => {
            *it = None;
            false
        }
    }
}

/// R43: release the handle's reference (the item is destroyed at zero,
/// releasing its values' references); the port always clears `*it`.
pub fn data_item_release(it: &mut Option<ObsDataItem>) {
    *it = None;
}

/// R41: detach the item from its object (if attached) and release the
/// handle's reference; `*it` is `None` afterwards (also a no-op for a
/// `None` handle).
pub fn data_item_remove(it: &mut Option<ObsDataItem>) {
    let Some(h) = it.take() else {
        return;
    };
    let (owner, attached) = {
        let b = h.item.borrow();
        (b.owner.clone(), !b.detached)
    };
    if attached {
        let Some(owner) = owner.upgrade() else {
            // `h` drops below: the handle's reference is released
            return;
        };
        let mut data = owner.borrow_mut();
        if let Some(idx) = data.items.iter().position(|i| Rc::ptr_eq(i, &h.item)) {
            let succ = data.items.get(idx + 1).map(Rc::downgrade);
            data.items.remove(idx); // releases the list's reference
            let mut b = h.item.borrow_mut();
            b.detached = true;
            b.next_item = succ;
        }
    }
    // `h` drops here: the handle's reference is released
}

/// R25: the item's type (a `None` handle would be NULL → OBS_DATA_NULL).
pub fn data_item_gettype(it: &ObsDataItem) -> Ty {
    it.item.borrow().ty
}

/// R25: the number type of the **effective** value; `Invalid` for
/// non-number items or number items without any layer.
pub fn data_item_numtype(it: &ObsDataItem) -> NumType {
    let b = it.item.borrow();
    if b.ty != Ty::Number {
        return NumType::Invalid;
    }
    match effective_value(&b) {
        Some(Value::Int(_)) => NumType::Int,
        Some(Value::Dbl(_)) => NumType::Double,
        _ => NumType::Invalid,
    }
}

/// R25: the item's name (byte strings may be invalid UTF-8 once set
/// through the FFI; the port renders them lossily in the safe API).
pub fn data_item_get_name(it: &ObsDataItem) -> String {
    String::from_utf8_lossy(&it.item.borrow().name).into_owned()
}

/// R24: the handle's item has this layer present (a present NULL object
/// value counts).
pub fn data_item_has_value(it: &ObsDataItem, layer: Layer) -> bool {
    let b = it.item.borrow();
    match layer {
        Layer::User => b.user.is_some(),
        Layer::Default => b.default.is_some(),
        Layer::Autoselect => b.autoselect.is_some(),
    }
}

/// R30 item form: remove just this layer from the handle's item.
pub fn data_item_unset(it: &ObsDataItem, layer: Layer) {
    let mut b = it.item.borrow_mut();
    match layer {
        Layer::User => {
            b.user.take();
            b.ds = 0;
            b.dl = 0;
        }
        Layer::Default => {
            b.default.take();
            b.fs = 0;
            b.fl = 0;
        }
        Layer::Autoselect => {
            b.autoselect.take();
            b.asz = 0;
        }
    }
}

/// R13: the item-level setter — exactly the name-based semantics applied
/// to the item the handle refers to, except that no item is ever created.
/// If the item has to grow (§6.1) it moves to the end; the handle still
/// refers to the same (now last) item (D4: all handles stay valid).
pub(crate) fn item_set_layer(item: &ItemRc, layer: Layer, v: Value) {
    let n = v.size();
    let moved = {
        let mut b = item.borrow_mut();
        if b.ty != v.ty() {
            if layer == Layer::Default {
                return; // R11: ignored, no change
            }
            // D3 fix: discard the layers that no longer match
            b.user.take();
            b.default.take();
            b.autoselect.take();
            b.fs = 0;
            b.fl = 0;
            b.asz = 0;
            b.ty = v.ty();
        }
        match layer {
            Layer::User => b.user = Some(v),
            Layer::Default => b.default = Some(v),
            Layer::Autoselect => b.autoselect = Some(v),
        }
        b.apply_set_sizes(layer, n)
    };
    if moved {
        let owner = {
            let b = item.borrow();
            b.owner.clone()
        };
        if let Some(owner) = owner.upgrade() {
            let mut data = owner.borrow_mut();
            move_to_end(&mut data, item);
        }
    }
}

/// Read one layer of the handle's item (`None` when absent or the item's
/// type differs — the caller matches the value's kind).
pub(crate) fn item_layer_value(it: &ObsDataItem, layer: Layer) -> Option<Value> {
    let b = it.item.borrow();
    match layer {
        Layer::User => b.user.clone(),
        Layer::Default => b.default.clone(),
        Layer::Autoselect => b.autoselect.clone(),
    }
}

// ---------------------------------------------------------------------------
// Arrays (R50–R53)
// ---------------------------------------------------------------------------

/// R50: the element count (`None` in C → 0; unrepresentable here).
pub fn array_count(a: &ObsDataArray) -> usize {
    a.inner.borrow().elements.len()
}

/// R51: append, taking one reference per element (R04); the index of the
/// new element. (The C NULL-argument no-op is unrepresentable here.)
pub fn array_push_back(a: &ObsDataArray, obj: &ObsData) -> usize {
    let mut arr = a.inner.borrow_mut();
    let idx = arr.elements.len();
    arr.elements.push(obj.inner.clone());
    idx
}

/// R51: insert at `idx` (`idx = count` appends); `idx > count` is outside
/// the contract and ignored (D6: no corruption).
pub fn array_insert(a: &ObsDataArray, idx: usize, obj: &ObsData) {
    let mut arr = a.inner.borrow_mut();
    if idx <= arr.elements.len() {
        arr.elements.insert(idx, obj.inner.clone());
    }
}

/// R52: +1 reference to the element, or `None` when `idx >= count`.
pub fn array_item(a: &ObsDataArray, idx: usize) -> Option<ObsData> {
    let arr = a.inner.borrow();
    arr.elements
        .get(idx)
        .cloned()
        .map(|inner| ObsData { inner })
}

/// R52: remove element `idx` and release it (`idx < count`; out of range
/// ignored, D6).
pub fn array_erase(a: &ObsDataArray, idx: usize) {
    let mut arr = a.inner.borrow_mut();
    if idx < arr.elements.len() {
        arr.elements.remove(idx);
    }
}

/// R52: append every element of `b` to `a` in order, taking one
/// reference per element (elements are shared).
pub fn array_push_back_array(a: &ObsDataArray, b: &ObsDataArray) {
    let elems = b.inner.borrow().elements.clone();
    a.inner.borrow_mut().elements.extend(elems);
}

/// R53: call `cb` for each element in order. The callback sees a
/// reference without a *net* reference change (the wrapper reference is
/// transient, unlike the C raw pointer it is observationally equivalent
/// except for a count read inside the callback itself).
pub fn array_enum(a: &ObsDataArray, mut cb: impl FnMut(&ObsData)) {
    let arr = a.inner.borrow();
    for e in &arr.elements {
        cb(&ObsData { inner: e.clone() });
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn d() -> ObsData {
        ObsData::create()
    }
    fn set_string(d: &ObsData, name: &str, v: &str) {
        set_layer_value(
            &d.inner,
            Layer::User,
            name.as_bytes(),
            Value::Str(v.as_bytes().to_vec()),
        );
    }
    fn set_int(d: &ObsData, name: &str, v: i64) {
        set_layer_value(&d.inner, Layer::User, name.as_bytes(), Value::Int(v));
    }
    fn set_obj(d: &ObsData, name: &str, v: Option<&ObsData>) {
        set_layer_value(
            &d.inner,
            Layer::User,
            name.as_bytes(),
            Value::Obj(v.map(|o| o.inner.clone())),
        );
    }
    fn user_str(d: &ObsData, name: &str) -> Option<String> {
        data_find(d, name).map(|it| {
            let b = it.borrow();
            match b.user.as_ref().expect("user layer") {
                Value::Str(s) => String::from_utf8_lossy(s).into_owned(),
                _ => panic!("string expected"),
            }
        })
    }
    fn names(d: &ObsData) -> Vec<String> {
        let data = d.inner.borrow();
        data.items
            .iter()
            .map(|it| data_item_get_name(&ObsDataItem { item: it.clone() }))
            .collect()
    }

    // -- R01–R05: reference counting ------------------------------------

    #[test]
    fn r01_create_addref_release() {
        let o = d();
        assert_eq!(o.refcount(), 1, "create returns count 1");
        let o2 = o.clone(); // addref
        assert_eq!(o.refcount(), 2);
        drop(o2);
        assert_eq!(o.refcount(), 1);
        drop(o);
        // destruction releases the object/array values of its layers:
        let p = d();
        let child = d();
        set_obj(&p, "c", Some(&child));
        assert_eq!(child.refcount(), 2); // p's layer holds one
        drop(p);
        assert_eq!(child.refcount(), 1);
    }

    #[test]
    fn r02_getters_return_new_reference() {
        let o = d();
        let child = d();
        set_obj(&o, "c", Some(&child));
        assert_eq!(child.refcount(), 2);
        // a getter would clone (tested through the API in api.rs); the
        // stored reference is exactly one besides the caller's:
        let found = {
            let data = o.inner.borrow();
            find_item(&data.items, b"c").and_then(|it| {
                it.borrow().user.clone().and_then(|v| match v {
                    Value::Obj(x) => x,
                    _ => None,
                })
            })
        };
        assert!(Rc::ptr_eq(found.as_ref().expect("stored"), &child.inner));
        drop(found);
        assert_eq!(child.refcount(), 2); // stored + caller
        drop(o);
        assert_eq!(child.refcount(), 1);
    }

    #[test]
    fn r03_setters_take_a_reference_replace_releases() {
        let o = d();
        let a = d();
        let b = d();
        set_obj(&o, "c", Some(&a));
        assert_eq!(a.refcount(), 2); // caller + stored
        set_obj(&o, "c", Some(&b)); // replaces: releases a's stored ref
        assert_eq!(a.refcount(), 1);
        assert_eq!(b.refcount(), 2);
        set_obj(&o, "c", None); // storing NULL releases b's stored ref
        assert_eq!(b.refcount(), 1);
        assert_eq!(o.refcount(), 1);
    }

    #[test]
    fn r04_erase_clear_unset_release() {
        let o = d();
        let child = d();
        set_obj(&o, "c", Some(&child));
        unset_layer(&o.inner, Layer::User, b"c");
        assert_eq!(child.refcount(), 1); // the unset released it
        // the item stays as a shell (R30)
        assert!(data_find(&o, "c").is_some());
        set_obj(&o, "c", Some(&child));
        clear_inner(&o.inner);
        assert_eq!(child.refcount(), 1);
        set_obj(&o, "c", Some(&child));
        erase_inner(&o.inner, b"c");
        assert_eq!(child.refcount(), 1);
    }

    #[test]
    fn d1_default_object_refcount_fixed() {
        // D1 (required behaviour): setting a default object on an
        // existing item without a user layer MUST take the reference
        // (the C reference undercounts by one here).
        let p = d();
        let child = d();
        set_layer_value(
            &p.inner,
            Layer::Default,
            b"k",
            Value::Obj(Some(child.inner.clone())),
        );
        assert_eq!(child.refcount(), 2, "item created with the default counts");
        // the D1 case: the item exists, has no user layer, re-set the default
        let c2 = d();
        set_layer_value(
            &p.inner,
            Layer::Default,
            b"k",
            Value::Obj(Some(c2.inner.clone())),
        );
        assert_eq!(child.refcount(), 1, "the old default's reference released");
        assert_eq!(
            c2.refcount(),
            2,
            "D1 fix: the new default took its reference"
        );
        drop(p);
        assert_eq!(c2.refcount(), 1);
    }

    // -- §6.1 size model / Q1 order ---------------------------------------

    #[test]
    fn q1_growth_moves_item_to_end() {
        let o = d();
        set_string(&o, "a", "abcd"); // ds = dl = 5, cap = 5
        set_string(&o, "b", "x");
        set_string(&o, "c", "y");
        assert_eq!(names(&o), vec!["a", "b", "c"]);
        // a default on "a": the user region becomes aligned to 32
        // (dl = A(5) = 32, fl = 5): need = 37 > cap = 5 -> a moves last
        set_layer_value(&o.inner, Layer::Default, b"a", Value::Str(b"z".to_vec()));
        assert_eq!(names(&o), vec!["b", "c", "a"]);
        // int over int: same size (16), no move
        set_int(&o, "a", 1);
        assert_eq!(names(&o), vec!["b", "c", "a"]);
        // a longer user string on "a": dl = A(ds) with the default
        // present: A(10) = 32 = dl already -> need unchanged, no move
        set_string(&o, "a", "0123456789");
        assert_eq!(names(&o), vec!["b", "c", "a"]);
    }

    #[test]
    fn q1_user_only_growth_moves() {
        let o = d();
        set_int(&o, "a", 1); // ds = dl = 16, cap = 16
        set_int(&o, "b", 2);
        // a user string of 20 bytes on "a" (no other layers):
        // dl = 21 > cap 16 -> moves
        set_string(&o, "a", "01234567890123456789");
        assert_eq!(names(&o), vec!["b", "a"]);
    }

    #[test]
    fn q1_reverse_order_user_after_default() {
        let o = d();
        set_layer_value(&o.inner, Layer::Default, b"a", Value::Str(b"z".to_vec())); // fs = fl = 2, cap = 2
        set_string(&o, "b", "x");
        // a user value on "a": dl = A(5) = 32 (default present): need 34 > 2
        set_string(&o, "a", "abcd");
        assert_eq!(names(&o), vec!["b", "a"]);
    }

    #[test]
    fn q1_r11_ignored_default_changes_nothing() {
        let o = d();
        set_string(&o, "a", "x");
        set_string(&o, "b", "y");
        // a default of a different type is ignored (R11): no size change,
        // no move
        set_layer_value(&o.inner, Layer::Default, b"a", Value::Int(7));
        assert_eq!(names(&o), vec!["a", "b"]);
        assert!(data_find(&o, "a").is_some());
        let b = o.inner.borrow();
        assert!(
            b.items[0].borrow().default.is_none(),
            "ignored default not stored"
        );
    }

    // -- shells, unset, clear, erase (R30/R31) -----------------------------

    #[test]
    fn r30_unset_leaves_shell() {
        let o = d();
        set_string(&o, "a", "x");
        unset_layer(&o.inner, Layer::User, b"a");
        let it = data_find(&o, "a").expect("shell stays");
        let b = it.borrow();
        assert_eq!(b.ty, Ty::String, "the shell keeps its type");
        assert!(b.user.is_none());
        assert_eq!(b.ds, 0);
        assert_eq!(b.dl, 0);
    }

    #[test]
    fn r31_clear_keeps_defaults_and_autoselect() {
        let o = d();
        set_string(&o, "a", "u");
        set_layer_value(&o.inner, Layer::Default, b"a", Value::Str(b"d".to_vec()));
        set_layer_value(
            &o.inner,
            Layer::Autoselect,
            b"a",
            Value::Str(b"as".to_vec()),
        );
        set_string(&o, "b", "v");
        clear_inner(&o.inner);
        let it = data_find(&o, "a").expect("item kept");
        let b = it.borrow();
        assert!(b.user.is_none());
        assert!(b.default.is_some());
        assert!(b.autoselect.is_some());
        let it = data_find(&o, "b").expect("item kept as shell");
        let b = it.borrow();
        assert!(b.user.is_none());
    }

    // -- item handles (R40–R43, D4) ----------------------------------------

    #[test]
    fn r40_first_byname_next_release() {
        let o = d();
        assert!(data_first(&o).is_none(), "empty object");
        set_string(&o, "a", "1");
        set_string(&o, "b", "2");
        set_string(&o, "c", "3");
        let mut it = data_first(&o);
        assert_eq!(data_item_get_name(it.as_ref().expect("first")), "a");
        assert!(data_item_next(&mut it));
        assert_eq!(data_item_get_name(it.as_ref().expect("second")), "b");
        assert!(data_item_next(&mut it));
        assert_eq!(data_item_get_name(it.as_ref().expect("third")), "c");
        assert!(!data_item_next(&mut it));
        assert!(it.is_none(), "*it is NULL after the end");
        let mut h = data_item_byname(&o, "b");
        assert_eq!(data_item_get_name(h.as_ref().expect("byname")), "b");
        assert!(data_item_byname(&o, "nope").is_none());
        data_item_release(&mut h);
        assert!(h.is_none());
        // NULL handle: no effect
        let mut none: Option<ObsDataItem> = None;
        assert!(!data_item_next(&mut none));
    }

    #[test]
    fn r41_append_during_walk_visited() {
        let o = d();
        set_string(&o, "a", "1");
        set_string(&o, "b", "2");
        let mut it = data_first(&o);
        let mut seen = vec![data_item_get_name(it.as_ref().unwrap())];
        // append while walking: the new item is visited
        set_string(&o, "c", "3");
        while data_item_next(&mut it) {
            seen.push(data_item_get_name(it.as_ref().unwrap()));
        }
        assert_eq!(seen, vec!["a", "b", "c"]);
    }

    #[test]
    fn r41_erase_during_walk_continues() {
        let o = d();
        set_string(&o, "a", "1");
        set_string(&o, "b", "2");
        set_string(&o, "c", "3");
        let mut it = data_first(&o);
        let mut seen = vec![data_item_get_name(it.as_ref().unwrap())];
        erase_inner(&o.inner, b"b"); // erase the *next* item
        while data_item_next(&mut it) {
            seen.push(data_item_get_name(it.as_ref().unwrap()));
        }
        assert_eq!(seen, vec!["a", "c"]);
    }

    #[test]
    fn r41_detached_handle_walk_continues() {
        let o = d();
        set_string(&o, "a", "1");
        set_string(&o, "b", "2");
        set_string(&o, "c", "3");
        let mut h = data_item_byname(&o, "b");
        let mut h2 = data_item_byname(&o, "b"); // a second handle keeps b alive
        assert!(Rc::ptr_eq(
            &h.as_ref().unwrap().item,
            &h2.as_ref().unwrap().item
        ));
        // remove h2: detaches b (the list's reference is released) and
        // releases h2's reference; h still points at the detached item
        data_item_remove(&mut h2);
        assert!(
            data_item_byname(&o, "b").is_none(),
            "b is detached from the list"
        );
        // next from the detached handle yields the item that followed at
        // detach time (the walk continues)
        assert!(data_item_next(&mut h));
        assert_eq!(data_item_get_name(h.as_ref().unwrap()), "c");
        assert!(!data_item_next(&mut h));
        assert!(h.is_none());
    }

    #[test]
    fn r42_detached_item_destroyed_with_last_handle() {
        let o = d();
        let child = d();
        set_obj(&o, "a", Some(&child));
        assert_eq!(child.refcount(), 2); // list + (below) handle
        let mut h = data_item_byname(&o, "a");
        assert_eq!(child.refcount(), 2); // the item's value holds it
        erase_inner(&o.inner, b"a"); // the list's reference is gone
        assert_eq!(
            child.refcount(),
            2,
            "R42: caller + the item's value keep it alive"
        );
        data_item_release(&mut h); // the last handle drops
        assert_eq!(
            child.refcount(),
            1,
            "the detached item is destroyed with its last handle (only the caller's reference remains)"
        );
    }

    #[test]
    fn d4_growth_does_not_dangle_handles() {
        let o = d();
        set_string(&o, "a", "x");
        set_string(&o, "b", "y");
        let h1 = data_item_byname(&o, "a").unwrap();
        let h2 = data_item_byname(&o, "a").unwrap(); // a second handle (R43/D4)
        // grow a while both handles are live: the item's storage is an
        // Rc, so no handle dangles (the C D4 defect is structurally gone)
        item_set_layer(&h1.item, Layer::User, Value::Str(vec![b'z'; 40]));
        assert!(Rc::ptr_eq(&h1.item, &h2.item));
        assert_eq!(data_item_get_name(&h2), "a");
        assert_eq!(names(&o), vec!["b", "a"], "the grown item moved last");
        // and both handles read the grown value
        let v = item_layer_value(&h2, Layer::User).unwrap();
        match v {
            Value::Str(b) => assert_eq!(b.len(), 40),
            _ => panic!(),
        }
    }

    #[test]
    fn r25_gettype_numtype_name() {
        let o = d();
        set_int(&o, "i", 1);
        set_layer_value(&o.inner, Layer::Default, b"i", Value::Dbl(2.0));
        set_string(&o, "s", "x");
        set_obj(&o, "o", None); // present NULL
        let it = data_item_byname(&o, "i").unwrap();
        assert_eq!(data_item_gettype(&it), Ty::Number);
        assert_eq!(
            data_item_numtype(&it),
            NumType::Int,
            "the effective (user) type"
        );
        let it = data_item_byname(&o, "s").unwrap();
        assert_eq!(data_item_gettype(&it), Ty::String);
        assert_eq!(data_item_numtype(&it), NumType::Invalid, "non-number");
        let it = data_item_byname(&o, "o").unwrap();
        assert_eq!(data_item_gettype(&it), Ty::Obj);
        assert_eq!(data_item_numtype(&it), NumType::Invalid);
        assert_eq!(data_item_get_name(&it), "o");
    }

    #[test]
    fn r13_item_set_semantics() {
        let o = d();
        set_string(&o, "a", "x");
        let it = data_item_byname(&o, "a").unwrap();
        // R11: a default of a different type is ignored
        item_set_layer(&it.item, Layer::Default, Value::Int(7));
        {
            let b = it.item.borrow();
            assert!(b.default.is_none());
        }
        // same type: replaced
        item_set_layer(&it.item, Layer::Default, Value::Str(b"y".to_vec()));
        assert!(it.item.borrow().default.is_some());
        // a type change overwrites the item's type (R10 through the item)
        item_set_layer(&it.item, Layer::User, Value::Int(3));
        assert_eq!(it.item.borrow().ty, Ty::Number);
    }

    // -- arrays (R50–R53) ---------------------------------------------------

    #[test]
    fn r50_r52_array_lifecycle() {
        let a = ObsDataArray::create();
        assert_eq!(a.refcount(), 1);
        assert_eq!(array_count(&a), 0);
        let o1 = d();
        let o2 = d();
        assert_eq!(array_push_back(&a, &o1), 0);
        assert_eq!(o1.refcount(), 2, "R04: push takes a reference");
        assert_eq!(array_push_back(&a, &o2), 1);
        assert_eq!(array_count(&a), 2);
        // item returns +1 reference, same object (shared elements)
        let got = array_item(&a, 0).expect("element");
        assert!(Rc::ptr_eq(&got.inner, &o1.inner));
        assert_eq!(o1.refcount(), 3); // caller + array + got
        assert!(array_item(&a, 2).is_none(), "idx >= count");
        drop(got);
        // insert at 0 / at the end; idx > count ignored (D6)
        let o3 = d();
        array_insert(&a, 0, &o3);
        assert_eq!(array_count(&a), 3);
        array_insert(&a, 3, &o3); // append
        array_insert(&a, 99, &o3); // ignored
        assert_eq!(array_count(&a), 4);
        // push_back_array shares the elements
        let b = ObsDataArray::create();
        array_push_back_array(&b, &a);
        assert_eq!(array_count(&b), 4);
        assert!(Rc::ptr_eq(&array_item(&b, 0).unwrap().inner, &o3.inner));
        // erase releases
        assert_eq!(o1.refcount(), 3, "caller + a + b");
        array_erase(&a, 1); // removes o1 (after the two inserts)
        assert_eq!(o1.refcount(), 2, "caller + b");
        array_erase(&b, 99); // ignored (D6)
        assert_eq!(array_count(&b), 4);
    }

    #[test]
    fn r53_array_enum_shares_elements() {
        let a = ObsDataArray::create();
        let o = d();
        array_push_back(&a, &o);
        let mut seen: Vec<bool> = vec![];
        array_enum(&a, |e| {
            seen.push(Rc::ptr_eq(&e.inner, &o.inner));
        });
        assert_eq!(seen, vec![true]);
        assert_eq!(o.refcount(), 2, "no net reference added by enum");
    }

    // -- apply (R32) ---------------------------------------------------------

    #[test]
    fn r32_apply_copies_user_layers_deep() {
        let src = d();
        set_string(&src, "s", "text");
        set_int(&src, "i", 7);
        set_layer_value(&src.inner, Layer::Default, b"i", Value::Int(70));
        // a nested object with user + default layers and a NULL member
        let inner = d();
        set_string(&inner, "u", "in");
        set_layer_value(
            &inner.inner,
            Layer::Default,
            b"u",
            Value::Str(b"default".to_vec()),
        );
        set_obj(&inner, "nul", None);
        set_obj(&src, "o", Some(&inner));
        // an array of objects
        let arr = ObsDataArray::create();
        let e1 = d();
        set_string(&e1, "e", "1");
        array_push_back(&arr, &e1);
        set_layer_value(
            &src.inner,
            Layer::User,
            b"ar",
            Value::Arr(Some(arr.inner.clone())),
        );

        let dst = d();
        data_apply(&dst, &src);

        assert_eq!(user_str(&dst, "s").as_deref(), Some("text"));
        assert_eq!(names(&dst), vec!["s", "i", "o", "ar"], "list order of src");
        // the default layer was NOT copied
        assert!(data_find(&dst, "i").is_some());
        let b = dst.inner.borrow();
        assert!(b.items[1].borrow().default.is_none());
        // the nested object: user layer only, the NULL member dropped
        let o = data_find(&dst, "o").expect("nested object");
        let stored = match o.borrow().user.clone().unwrap() {
            Value::Obj(Some(x)) => x,
            _ => panic!(),
        };
        assert!(!Rc::ptr_eq(&stored, &inner.inner), "deepcopy, not shared");
        assert_eq!(
            user_str(
                &ObsData {
                    inner: stored.clone()
                },
                "u"
            )
            .as_deref(),
            Some("in")
        );
        assert!(
            data_find(&ObsData { inner: stored }, "nul").is_none(),
            "NULL member dropped"
        );
        // the array: a new array of new objects with the user layers
        let ar = data_find(&dst, "ar").expect("array");
        let elems = match ar.borrow().user.clone().unwrap() {
            Value::Arr(Some(x)) => x.borrow().elements.clone(),
            _ => panic!(),
        };
        assert_eq!(elems.len(), 1);
        assert!(!Rc::ptr_eq(&elems[0], &e1.inner), "element deepcopied");
        assert_eq!(
            user_str(
                &ObsData {
                    inner: elems[0].clone()
                },
                "e"
            )
            .as_deref(),
            Some("1")
        );
        // the source is untouched
        assert_eq!(user_str(&src, "s").as_deref(), Some("text"));
        // apply(target, target) is a no-op
        let before = names(&dst);
        data_apply(&dst, &dst);
        assert_eq!(names(&dst), before);
    }

    // -- get_defaults (R33) ----------------------------------------------------

    #[test]
    fn r33_get_defaults() {
        let o = d();
        set_string(&o, "s", "u");
        set_layer_value(&o.inner, Layer::Default, b"s", Value::Str(b"d".to_vec()));
        set_layer_value(&o.inner, Layer::Default, b"i", Value::Int(5)); // default only, int
        set_layer_value(&o.inner, Layer::Default, b"d", Value::Dbl(2.5)); // default only, double
        set_layer_value(&o.inner, Layer::User, b"sh", Value::Int(1));
        unset_layer(&o.inner, Layer::User, b"sh"); // a number shell
        // nested default object / array
        let inner = d();
        set_layer_value(
            &inner.inner,
            Layer::Default,
            b"x",
            Value::Str(b"dx".to_vec()),
        );
        set_layer_value(
            &o.inner,
            Layer::Default,
            b"o",
            Value::Obj(Some(inner.inner.clone())),
        );
        let arr = ObsDataArray::create();
        let e = d();
        set_layer_value(&e.inner, Layer::Default, b"y", Value::Int(9));
        array_push_back(&arr, &e);
        set_layer_value(
            &o.inner,
            Layer::Default,
            b"ar",
            Value::Arr(Some(arr.inner.clone())),
        );

        let r = data_get_defaults(&o);
        assert_eq!(
            names(&r),
            vec!["s", "i", "d", "o", "ar"],
            "list order; the shell creates no member"
        );
        assert_eq!(user_str(&r, "s").as_deref(), Some("d"));
        // number: the effective (here: the default's own) number type decides
        let i = data_find(&r, "i").unwrap();
        match i.borrow().user.clone().unwrap() {
            Value::Int(5) => {}
            v => panic!("int 5 expected, got {v:?}"),
        }
        let d_ = data_find(&r, "d").unwrap();
        match d_.borrow().user.clone().unwrap() {
            Value::Dbl(2.5) => {}
            v => panic!("double 2.5 expected, got {v:?}"),
        }
        // the nested object carries the defaults of the default object
        let o_ = data_find(&r, "o").unwrap();
        let stored = match o_.borrow().user.clone().unwrap() {
            Value::Obj(Some(x)) => x,
            _ => panic!(),
        };
        assert!(!Rc::ptr_eq(&stored, &inner.inner));
        assert_eq!(
            user_str(&ObsData { inner: stored }, "x").as_deref(),
            Some("dx")
        );
        // the array holds get_defaults(e) for each element
        let ar = data_find(&r, "ar").unwrap();
        let elems = match ar.borrow().user.clone().unwrap() {
            Value::Arr(Some(x)) => x.borrow().elements.clone(),
            _ => panic!(),
        };
        assert_eq!(elems.len(), 1);
        let y = data_find(
            &ObsData {
                inner: elems[0].clone(),
            },
            "y",
        )
        .expect("element default");
        match y.borrow().user.clone().unwrap() {
            Value::Int(9) => {}
            v => panic!("Int(9) expected, got {v:?}"),
        }
        // a shell number: no member
        assert!(data_find(&r, "sh").is_none());
        // missing defaults read as zero values
        let only = d();
        set_string(&only, "a", "x"); // user only
        let r = data_get_defaults(&only);
        assert_eq!(user_str(&r, "a").as_deref(), Some(""), "no default -> \"\"");
    }
}
