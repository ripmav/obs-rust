//! S-024 M3: `os_process_args` — the ordered argument list of the
//! `os_process_pipe_create2` form (S-055-R10).
//!
//! An ordered list of strings plus the implicit trailing NULL slot:
//! `create(exe)` stores a copy of `exe` as element 0, `add_arg` appends
//! a copy, `add_argf` appends a `printf`-formatted string, `argc` is the
//! number of stored elements (the terminator is not counted), `argv` is
//! the vector of strings. No quoting, splitting or escaping is done
//! anywhere.
//!
//! Deviations from the C reference:
//! - C-NULL arguments (the S-024 M3 quirk: `create(NULL)` /
//!   `add_arg(NULL)` store a NULL entry that truncates C consumers) are
//!   unrepresentable in the safe API; the FFI layer passes `""` for
//!   NULL, which behaves like `add_arg("")`.
//! - `add_argf` supports the conversion specifiers pinned by the
//!   conformance vectors: `%d`/`%i`, `%s`, `%x`, `%X`, decimal width and
//!   zero padding (`%05d`), and `%%`; other specifiers are left verbatim
//!   in the output (the C `printf` semantics are not reimplemented).
//! - S-024-R16 (DEFECT D-4 fixed): a format that expands to the empty
//!   string stores `""`, never NULL.

/// One argument of [`ProcessArgs::add_argf`].
pub enum ArgfArg {
    Int(i64),
    Str(String),
}

/// An ordered list of process arguments (S-024 M3).
#[derive(Debug, Default, Clone)]
pub struct ProcessArgs {
    args: Vec<String>,
}

impl ProcessArgs {
    /// `os_process_args_create(exe)`: the list starts holding a copy of
    /// `exe` as element 0.
    pub fn create(exe: &str) -> Self {
        Self {
            args: vec![exe.to_string()],
        }
    }

    /// `os_process_args_add_arg`: appends a copy of `arg`.
    pub fn add_arg(&mut self, arg: &str) {
        self.args.push(arg.to_string());
    }

    /// `os_process_args_add_argf(fmt, ...)`: formats `fmt` with the
    /// arguments and appends the result — always exactly one element,
    /// even when the result is the empty string (S-024-R16).
    pub fn add_argf(&mut self, fmt: &str, args: &[ArgfArg]) {
        self.args.push(format_argf(fmt, args));
    }

    /// `os_process_args_get_argc`: the number of strings without the
    /// implicit NULL terminator.
    pub fn argc(&self) -> usize {
        self.args.len()
    }

    /// `os_process_args_get_argv`: the stored strings in order. The C
    /// NULL terminator at index `argc` is implicit (a `Vec<String>` has
    /// no NULL slot); the FFI layer appends it.
    pub fn argv(&self) -> &[String] {
        &self.args
    }
}

/// Format one `add_argf` string (the pinned specifier subset — see the
/// module docs).
fn format_argf(fmt: &str, args: &[ArgfArg]) -> String {
    let mut out = String::new();
    let mut rest = fmt;
    let mut idx = 0usize;
    while let Some(pos) = rest.find('%') {
        out.push_str(&rest[..pos]);
        let tail = &rest[pos + 1..];
        if tail.is_empty() {
            // a trailing '%' with nothing after it
            out.push('%');
            break;
        }
        if tail.starts_with('%') {
            out.push('%');
            rest = &tail[1..];
            continue;
        }
        // parse [0][width]conversion
        let mut t = tail;
        let zero_padded = t.starts_with('0');
        if zero_padded {
            t = &t[1..];
        }
        let width_digits = t
            .as_bytes()
            .iter()
            .position(|b| !b.is_ascii_digit())
            .unwrap_or(t.len());
        let width: usize = t[..width_digits].parse().unwrap_or(0);
        t = &t[width_digits..];
        let width = if zero_padded { width } else { 0 };
        let (conv, after) = match t.as_bytes().first() {
            Some(c) => ((*c as char), &t[1..]),
            None => continue,
        };
        let spec = match (conv, idx < args.len()) {
            ('d', true) | ('i', true) => {
                let v = match &args[idx] {
                    ArgfArg::Int(v) => *v,
                    ArgfArg::Str(_) => 0,
                };
                idx += 1;
                let text = v.to_string();
                if zero_padded && width > text.len() && v >= 0 {
                    format!("{v:0width$}")
                } else {
                    text
                }
            }
            ('x', true) | ('X', true) => {
                let v = match &args[idx] {
                    ArgfArg::Int(v) => *v as u64,
                    ArgfArg::Str(_) => 0,
                };
                idx += 1;
                if conv == 'x' {
                    format!("{v:x}")
                } else {
                    format!("{v:X}")
                }
            }
            ('s', true) => {
                let v = match &args[idx] {
                    ArgfArg::Str(v) => v.clone(),
                    ArgfArg::Int(v) => v.to_string(),
                };
                idx += 1;
                v
            }
            _ => {
                // unparsable/unmapped: keep the whole spec verbatim
                let end = after.find('%').map_or(t.len(), |p| p + 1);
                out.push_str(&tail[..end]);
                rest = &tail[end..];
                continue;
            }
        };
        out.push_str(&spec);
        rest = after;
    }
    out.push_str(rest);
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    // vector args-basic: create stores the executable, add_arg appends
    // copies verbatim (spaces, empty, quotes, backslashes), the
    // terminator is implicit, argc does not count it
    #[test]
    fn args_basic() {
        let mut a = ProcessArgs::create("/usr/bin/tool");
        assert_eq!(a.argc(), 1);
        assert_eq!(a.argv(), ["/usr/bin/tool"]);

        a.add_arg("--flag");
        a.add_arg("value");
        a.add_arg("with space");
        a.add_arg("");
        a.add_arg("--key=val ue");
        a.add_arg(r#"quote"d \ back"#);
        assert_eq!(a.argc(), 7);
        assert_eq!(
            a.argv(),
            [
                "/usr/bin/tool",
                "--flag",
                "value",
                "with space",
                "",
                "--key=val ue",
                r#"quote"d \ back"#
            ]
        );
    }

    // vector args-multiple: independent lists, insertion order,
    // add_argf int
    #[test]
    fn args_multiple() {
        let mut a = ProcessArgs::create("first");
        let mut b = ProcessArgs::create("second");
        a.add_arg("a");
        b.add_arg("x");
        a.add_arg("b");
        b.add_arg("y");
        a.add_argf("n=%d", &[ArgfArg::Int(3)]);
        assert_eq!(a.argv(), ["first", "a", "b", "n=3"]);
        assert_eq!(b.argv(), ["second", "x", "y"]);
    }

    // vector args-argf: the pinned specifier subset
    #[test]
    fn args_argf() {
        let mut a = ProcessArgs::create("prog");
        a.add_argf("--width=%d", &[ArgfArg::Int(1920)]);
        a.add_argf("--height=%d", &[ArgfArg::Int(-1080)]);
        a.add_argf("--name=%s", &[ArgfArg::Str("my stream".to_string())]);
        a.add_argf("%05d", &[ArgfArg::Int(42)]);
        a.add_argf("%x", &[ArgfArg::Int(255)]);
        // plain append (the runner's args_addf_plain): no specifiers
        a.add_arg("100%% literal is not applied to the argument");
        // literal-only format
        a.add_argf("literal-only", &[]);
        assert_eq!(
            a.argv(),
            [
                "prog",
                "--width=1920",
                "--height=-1080",
                "--name=my stream",
                "00042",
                "ff",
                "100%% literal is not applied to the argument",
                "literal-only",
            ]
        );
        assert_eq!(a.argc(), 8);
    }

    // S-024-R16 (DEFECT D-4 fixed): a format that expands to the empty
    // string stores "" (the reference stored NULL)
    #[test]
    fn argf_empty_is_empty_string() {
        let mut a = ProcessArgs::create("prog");
        a.add_argf("", &[]);
        a.add_arg("");
        a.add_argf("", &[]);
        a.add_arg("tail");
        assert_eq!(a.argc(), 5);
        assert_eq!(a.argv(), ["prog", "", "", "", "tail"]);
    }

    // C-NULL arguments (the M3 quirk) are unrepresentable; "" behaves
    // like a stored empty string and never truncates
    #[test]
    fn null_quirk_documented() {
        let mut a = ProcessArgs::create("");
        a.add_arg("after");
        a.add_arg("");
        a.add_arg("last");
        assert_eq!(a.argc(), 4);
        assert_eq!(a.argv(), ["", "after", "", "last"]);
    }
}
