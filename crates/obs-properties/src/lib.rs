//! obs-properties: `obs_properties_t` / `obs_property_t` (S-015).
//!
//! A *properties object* describes, as plain data, the controls a user
//! interface should show for a source, encoder, output or service: an
//! ordered list of typed properties (bool, int, float, text, path, list,
//! colour, button, font, editable list, frame rate, group). It holds no
//! values: the values live in an `obs_data` settings object that the UI
//! passes to the callbacks (received here as the opaque
//! [`SettingsHandle`]).
//!
//! The safe Rust API is the contract: C-`NULL` container/property/name
//! cases are unrepresentable and noted per function; string getters return
//! owned copies (the C `const char *` stability into the property is an
//! FFI concern); the two logged messages of S-015 R09/R27 are not emitted
//! yet — this crate has no logger (it arrives with the platform crate,
//! S-023); the FFI layer will wire them.
//!
//! Properties are not thread-safe (S-015 §2): no operation locks
//! anything; callbacks run synchronously on the calling thread and may
//! call back into the same container.

pub mod enums;
pub mod model;

pub use enums::*;
pub use model::{
    ClickedCb, DestroyCb, Modified2Cb, ModifiedCb, ObsProperties, ObsProperty, SettingsHandle,
    group_content, group_type, properties_create, properties_create_param, properties_first,
    properties_get, properties_get_flags, properties_get_param, properties_get_parent,
    properties_remove_by_name, properties_set_flags, properties_set_param,
};
