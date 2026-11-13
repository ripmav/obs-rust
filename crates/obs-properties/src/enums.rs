//! S-015 §3.1 enumerations.
//!
//! The numeric values are ABI: each enum carries the C values verbatim and
//! an `Unknown` catch-all so that out-of-range values the reference accepts
//! ("enum arguments are never validated", S-015 §3.1; vectors `07`, `10`,
//! `25`) can be stored and returned unchanged.

macro_rules! abi_enum {
    ($(#[$m:meta])* $name:ident { $($v:ident = $n:tt),* $(,)? }) => {
        $(#[$m])*
        #[derive(Clone, Copy, Debug, PartialEq, Eq)]
        #[repr(u32)]
        pub enum $name {
            $($v = $n),*,
            /// An out-of-range value, stored and returned unchanged
            /// (S-015 §3.1: enum arguments are never validated).
            Unknown(u32),
        }

        impl $name {
            pub const fn as_raw(self) -> u32 {
                match self {
                    $($name::$v => $n),*,
                    $name::Unknown(v) => v,
                }
            }

            pub const fn from_raw(v: u32) -> Self {
                match v {
                    $($n => $name::$v),*,
                    _ => $name::Unknown(v),
                }
            }
        }
    };
}

abi_enum! {
    /// `obs_property_type`.
    #[allow(non_camel_case_types)]
    PropType {
        INVALID = 0,
        BOOL = 1,
        INT = 2,
        FLOAT = 3,
        TEXT = 4,
        PATH = 5,
        LIST = 6,
        COLOR = 7,
        BUTTON = 8,
        FONT = 9,
        EDITABLE_LIST = 10,
        FRAME_RATE = 11,
        GROUP = 12,
        COLOR_ALPHA = 13,
    }
}

abi_enum! {
    /// `obs_combo_format`.
    #[allow(non_camel_case_types)]
    ComboFormat {
        INVALID = 0,
        INT = 1,
        FLOAT = 2,
        STRING = 3,
        BOOL = 4,
    }
}

abi_enum! {
    /// `obs_combo_type`.
    #[allow(non_camel_case_types)]
    ComboType {
        INVALID = 0,
        EDITABLE = 1,
        LIST = 2,
        RADIO = 3,
    }
}

abi_enum! {
    /// `obs_editable_list_type`.
    #[allow(non_camel_case_types)]
    EditableListType {
        STRINGS = 0,
        FILES = 1,
        FILES_AND_URLS = 2,
    }
}

abi_enum! {
    /// `obs_path_type`.
    #[allow(non_camel_case_types)]
    PathType {
        FILE = 0,
        FILE_SAVE = 1,
        DIRECTORY = 2,
    }
}

abi_enum! {
    /// `obs_text_type`.
    #[allow(non_camel_case_types)]
    TextType {
        DEFAULT = 0,
        PASSWORD = 1,
        MULTILINE = 2,
        INFO = 3,
    }
}

abi_enum! {
    /// `obs_text_info_type`.
    #[allow(non_camel_case_types)]
    TextInfoType {
        NORMAL = 0,
        WARNING = 1,
        ERROR = 2,
    }
}

/// `obs_number_type`. Set only by this component (SCROLLER/SLIDER).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u32)]
pub enum NumberType {
    #[default]
    SCROLLER = 0,
    SLIDER = 1,
}

abi_enum! {
    /// `obs_group_type` (INVALID is named `OBS_COMBO_INVALID` in the header;
    /// QUIRK Q-7).
    #[allow(non_camel_case_types)]
    GroupType {
        INVALID = 0,
        NORMAL = 1,
        CHECKABLE = 2,
    }
}

abi_enum! {
    /// `obs_button_type`.
    #[allow(non_camel_case_types)]
    ButtonType {
        DEFAULT = 0,
        URL = 1,
    }
}

/// `OBS_PROPERTIES_DEFER_UPDATE`. Stored/returned like any flag; the library
/// never interprets flags.
pub const PROPERTIES_DEFER_UPDATE: u32 = 1 << 0;

/// Font flag constants, defined in the header but not used by this
/// component.
pub const FONT_BOLD: u32 = 1;
pub const FONT_ITALIC: u32 = 2;
pub const FONT_UNDERLINE: u32 = 4;
pub const FONT_STRIKEOUT: u32 = 8;

/// `media_frames_per_second` (media-io/frame-rate.h), 8 bytes, `#[repr(C)]`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct MediaFps {
    pub numerator: u32,
    pub denominator: u32,
}
