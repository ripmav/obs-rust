//! S-024 M1 — `shared/opts-parser` (P1-33.5).
//!
//! Option-string parsing for encoder-style option strings. The ported API
//! mirrors the C reference (`obs_parse_options` / `obs_free_options`):
//! words are separated by the single space character (0x20) only; a word is
//! an option iff it does not start with `=`, contains `=`, and has at least
//! one character after the first `=` (rules R01-R07). There is no quoting,
//! escaping or tab handling (Q-1) — everything else is an ignored word.
//!
//! The parsing is byte-oriented like the C reference (any byte is valid
//! data, R06/Q-1). The C-ABI bridge in `obs-ffi` copies the words into
//! `bmem` allocations with the C ownership layout: the `value` pointers
//! point *into* `input_words`, the `name`s are separate copies.

/// Result of [`parse_options`]; borrows the input bytes.
///
/// In the zero result (R01) `words` is empty.
#[derive(Debug, Default, PartialEq, Eq)]
pub struct ParsedOptions<'a> {
    /// Every word of the input, in order (R02/R04).
    pub words: Vec<&'a [u8]>,
    /// Per word: `Some(eq)` where `eq` is the byte offset of the first
    /// `=` when the word is an option (R03), `None` when it is an ignored
    /// word (R04).
    pub kind: Vec<Option<usize>>,
}

impl<'a> ParsedOptions<'a> {
    /// R01: the zero result (no non-space character).
    #[must_use]
    pub fn is_zero(&self) -> bool {
        self.words.is_empty()
    }

    /// `count`: number of options (R02).
    #[must_use]
    pub fn count(&self) -> usize {
        self.kind.iter().filter(|k| k.is_some()).count()
    }

    /// `ignored_word_count` (R04).
    #[must_use]
    pub fn ignored_count(&self) -> usize {
        self.kind.iter().filter(|k| k.is_none()).count()
    }

    /// Options in input order as `(name, value)` byte pairs (R03, R05):
    /// `name` is the text before the first `=`, `value` everything after it
    /// (further `=` belong to the value).
    pub fn options(&self) -> impl Iterator<Item = (&[u8], &[u8])> {
        self.words
            .iter()
            .zip(&self.kind)
            .filter_map(move |(word, kind)| {
                let eq = *kind.as_ref()?;
                Some((&word[..eq], &word[eq + 1..]))
            })
    }

    /// Ignored words in input order (R04).
    pub fn ignored_words(&self) -> impl Iterator<Item = &[u8]> {
        self.words
            .iter()
            .zip(&self.kind)
            .filter_map(
                move |(word, kind)| {
                    if kind.is_none() { Some(*word) } else { None }
                },
            )
    }
}

/// `obs_parse_options` (R01-R07).
///
/// Empty/all-space input yields the zero result (R01). Words are split on
/// the single space character only, empty words are dropped (R02); a word
/// is an option per R03, everything else is an ignored word (R04).
/// No quoting/escaping/tab handling exists (Q-1, R06).
#[must_use]
pub fn parse_options<'a>(options_string: &'a [u8]) -> ParsedOptions<'a> {
    let mut out = ParsedOptions::default();
    for word in options_string.split(|&b| b == b' ') {
        if word.is_empty() {
            continue; // runs of spaces and leading/trailing spaces vanish (R02)
        }
        // R03: option iff not `=`-leading (eq > 0) and the value is
        // non-empty (eq + 1 < len).
        let eq = word.iter().position(|&b| b == b'=');
        let kind = match eq {
            Some(e) if e > 0 && e + 1 < word.len() => Some(e),
            _ => None,
        };
        out.kind.push(kind);
        out.words.push(word);
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn opts<'p>(p: &'p ParsedOptions<'_>) -> Vec<(&'p [u8], &'p [u8])> {
        p.options().collect()
    }

    /// R01: empty/all-space input -> the zero result.
    #[test]
    fn r01_zero_result() {
        for input in [b"" as &[u8], b" ", b"     "] {
            let p = parse_options(input);
            assert!(p.is_zero(), "{input:?}");
            assert_eq!(p.count(), 0);
            assert_eq!(p.ignored_count(), 0);
        }
    }

    /// R02: only 0x20 separates; runs/leading/trailing spaces vanish;
    /// input order is kept.
    #[test]
    fn r02_split_and_order() {
        let p = parse_options(b" lead=1 trail=2 ");
        assert_eq!(p.words, [b"lead=1" as &[u8], b"trail=2"]);
        assert_eq!(
            opts(&p),
            [(b"lead" as &[u8], b"1" as &[u8]), (b"trail", b"2")]
        );
        let q = parse_options(b"a=1   b=2");
        assert_eq!(q.words.len(), 2);
        // tabs are data, not separators (Q-1)
        let t = parse_options(b"tab=a\tb");
        assert_eq!(opts(&t), vec![(b"tab" as &[u8], b"a\tb" as &[u8])]);
        assert_eq!(t.ignored_count(), 0);
    }

    /// R03: name = before the first `=`, value = everything after (further
    /// `=` in the value); the ignored cases.
    #[test]
    fn r03_option_shape() {
        let p = parse_options(b"eq=a=b==c");
        assert_eq!(opts(&p), vec![(b"eq" as &[u8], b"a=b==c" as &[u8])]);
        for input in [b"=value" as &[u8], b"key=", b"=", b"=="] {
            let p = parse_options(input);
            assert_eq!(p.count(), 0, "{input:?}");
            assert_eq!(
                p.ignored_words().collect::<Vec<_>>(),
                [input as &[u8]],
                "{input:?}"
            );
        }
        let no_eq = parse_options(b"flag");
        assert_eq!(no_eq.count(), 0);
        assert_eq!(
            no_eq.ignored_words().collect::<Vec<_>>(),
            [b"flag" as &[u8]]
        );
    }

    /// R04: ignored words and input words keep input order.
    #[test]
    fn r04_ignored_and_input_words() {
        let p = parse_options(b"a=1 flag b=2 =x c= d=4");
        assert_eq!(
            opts(&p),
            vec![(b"a" as &[u8], b"1" as &[u8]), (b"b", b"2"), (b"d", b"4")]
        );
        assert_eq!(
            p.ignored_words().collect::<Vec<_>>(),
            [b"flag" as &[u8], b"=x", b"c="]
        );
        assert_eq!(p.words.len(), 6);
    }

    /// R05: duplicate option names are all kept.
    #[test]
    fn r05_duplicates_kept() {
        let p = parse_options(b"dup=1 dup=2 dup=3");
        assert_eq!(
            opts(&p),
            vec![
                (b"dup" as &[u8], b"1" as &[u8]),
                (b"dup", b"2"),
                (b"dup", b"3")
            ]
        );
    }

    /// Q-1 (R06): no quoting/escaping — quotes, backslashes, newlines and
    /// UTF-8 are plain data.
    #[test]
    fn q1_no_quoting() {
        let p = parse_options(b"title=\"hello world\"");
        assert_eq!(opts(&p), vec![(b"title" as &[u8], b"\"hello" as &[u8])]);
        assert_eq!(p.ignored_words().collect::<Vec<_>>(), [b"world\"" as &[u8]]);
        let b = parse_options(b"back=a\\b c=d");
        assert_eq!(
            opts(&b),
            vec![(b"back" as &[u8], b"a\\b" as &[u8]), (b"c", b"d")]
        );
        let nl = parse_options(b"nl=a\nb");
        assert_eq!(opts(&nl), vec![(b"nl" as &[u8], b"a\nb" as &[u8])]);
        let u = parse_options(b"utf8=\xc3\xa4\xc3\xb6 \xe2\x82\xac=euro");
        assert_eq!(
            opts(&u),
            vec![
                (b"utf8" as &[u8], b"\xc3\xa4\xc3\xb6" as &[u8]),
                (b"\xe2\x82\xac", b"euro")
            ]
        );
    }

    /// The option `eq` offset points at the first `=` (the C `name` length
    /// and the `value` pointer both derive from it).
    #[test]
    fn eq_offset_first_assign() {
        let p = parse_options(b"x=a=b");
        assert_eq!(p.kind, [Some(1)]);
        assert_eq!(&p.words[0][..p.kind[0].unwrap()], b"x");
        assert_eq!(&p.words[0][p.kind[0].unwrap() + 1..], b"a=b");
    }
}
