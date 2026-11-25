//! S-024 M9 — `shared/happy-eyeballs` address ordering (P1-33.3).
//!
//! The part of happy-eyeballs that is deterministic and specified: the
//! resolver hints (`build_addr_list` family selection per the configured
//! bind address) and the in-place reordering of the resolver result list
//! that interleaves IPv4/IPv6. The connection race (sockets, timers,
//! worker threads) is **not specified** (S-024 §1) and stays in C.

/// `AF_UNSPEC` (POSIX) — no family hint.
pub const AF_UNSPEC: i32 = 0;
/// `AF_INET` (POSIX) — IPv4.
pub const AF_INET: i32 = 2;
/// `AF_INET6` (POSIX) — IPv6.
pub const AF_INET6: i32 = 10;

/// `HAPPY_EYEBALLS_DELAY_MS` — delay between attempts (same default as
/// libcurl).
pub const HAPPY_EYEBALLS_DELAY_MS: u32 = 200;
/// `HAPPY_EYEBALLS_MAX_ATTEMPTS` — maximum number of attempts.
pub const HAPPY_EYEBALLS_MAX_ATTEMPTS: u32 = 6;
/// `HAPPY_EYEBALLS_CONNECTION_TIMEOUT_MS` — total time to wait for
/// sockets or to finish trying; whichever is shorter.
pub const HAPPY_EYEBALLS_CONNECTION_TIMEOUT_MS: u32 = 25000;

/// `sizeof(struct sockaddr_in)` on the 64-bit targets of S-024 §2 (16).
pub const SOCKADDR_IN_LEN: usize = 16;
/// `sizeof(struct sockaddr_in6)` on the 64-bit targets of S-024 §2 (28).
pub const SOCKADDR_IN6_LEN: usize = 28;

/// The resolver family hint from the configured bind address length
/// (S-024 §4.9): `AF_INET` if the length equals 16, `AF_INET6` if it
/// equals 28, else `AF_UNSPEC`.
#[must_use]
pub fn hint_family(bind_addr_len: usize) -> i32 {
    if bind_addr_len == SOCKADDR_IN_LEN {
        AF_INET
    } else if bind_addr_len == SOCKADDR_IN6_LEN {
        AF_INET6
    } else {
        AF_UNSPEC
    }
}

/// The happy-eyeballs reordering of the resolver result (S-024 §4.9).
///
/// `families` is the resolver result in order (each entry an address
/// family). The C algorithm walks the linked list in a single pass:
/// `prev = L[0]; cur = L[1]; while cur:` — if `prev` and `cur` share a
/// family and that family is INET or INET6, find the first node `X` after
/// `cur` whose family is the *other* one; if none exists, stop the whole
/// loop; else move `X` between `prev` and `cur` (in the linked list:
/// `prev.next = X; X.next = cur`, and `X`'s old predecessor links over
/// `X`). Then `prev = cur; cur = cur.next` — after the move `cur` is the
/// node that was shifted back, and the moved node `X` is skipped.
///
/// Nodes of other families (e.g. `AF_UNIX`) never trigger a move and are
/// never moved. The result is a permutation of the input; the head
/// (`families[0]`) is never moved.
#[must_use]
pub fn reorder(families: &[i32]) -> Vec<usize> {
    let n = families.len();
    if n < 2 {
        return (0..n).collect();
    }

    // next[i]: the index of the node after i in the (mutated) list, or
    // None at the tail.
    let mut next: Vec<Option<usize>> = (0..n).map(|i| (i + 1 < n).then_some(i + 1)).collect();

    let chain = |next: &[Option<usize>]| -> Vec<usize> {
        let mut order = Vec::with_capacity(n);
        let mut i = 0;
        while let Some(j) = next[i] {
            order.push(i);
            i = j;
        }
        order.push(i);
        order
    };

    let mut prev = 0usize;
    let mut cur: Option<usize> = Some(1);
    while let Some(c) = cur {
        if families[prev] == families[c]
            && (families[prev] == AF_INET || families[prev] == AF_INET6)
        {
            let target = if families[prev] == AF_INET {
                AF_INET6
            } else {
                AF_INET
            };
            let mut it = next[c];
            let mut prev_it = c;
            while let Some(i) = it {
                if families[i] == target {
                    break;
                }
                prev_it = i;
                it = next[i];
            }
            match it {
                // No node of the wanted family left: the C breaks out of
                // the whole loop; the rest of the list stays as is.
                None => return chain(&next),
                Some(x) => {
                    // Move x between prev and c.
                    let x_next = next[x];
                    next[prev] = Some(x);
                    next[prev_it] = x_next;
                    next[x] = Some(c);
                }
            }
        }
        prev = c;
        // cur's own next is untouched by the move above (C: `cur =
        // cur->ai_next` where cur is the shifted-back node).
        cur = next[c];
    }
    chain(&next)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// S-024 §4.9: the family hint follows the bind address length.
    #[test]
    fn hint_family_values() {
        assert_eq!(hint_family(0), AF_UNSPEC);
        assert_eq!(hint_family(5), AF_UNSPEC);
        assert_eq!(hint_family(SOCKADDR_IN_LEN), AF_INET);
        assert_eq!(hint_family(SOCKADDR_IN6_LEN), AF_INET6);
        assert_eq!(hint_family(32), AF_UNSPEC);
    }

    /// The constants (S-024 §4.9, vector `he-resolve-args`).
    #[test]
    fn constants() {
        assert_eq!(HAPPY_EYEBALLS_DELAY_MS, 200);
        assert_eq!(HAPPY_EYEBALLS_MAX_ATTEMPTS, 6);
        assert_eq!(HAPPY_EYEBALLS_CONNECTION_TIMEOUT_MS, 25000);
    }

    /// The `he-order` vector cases: the interleaving of IPv4/IPv6.
    #[test]
    fn order_vectors() {
        let a = AF_INET;
        let b = AF_INET6;
        let cases: &[(Vec<i32>, Vec<usize>)] = &[
            (vec![b, b, a, a], vec![0, 2, 1, 3]),
            (vec![a, a, b, b], vec![0, 2, 1, 3]),
            (vec![b, b, b, a, a, a], vec![0, 3, 1, 4, 2, 5]),
            (vec![a, a, a, a, b], vec![0, 4, 1, 2, 3]),
            (vec![b, a, b, a], vec![0, 1, 2, 3]),
            (vec![a], vec![0]),
            (vec![b, b, b], vec![0, 1, 2]),
            (vec![a, b, b, a, a, b], vec![0, 1, 3, 2, 4, 5]),
        ];
        for (fam, want) in cases {
            assert_eq!(&reorder(fam), want, "{fam:?}");
        }
    }

    /// The `he-order-edge` vector cases: early exit when the wanted family
    /// is exhausted, other families neither trigger nor get moved, single
    /// pass (no full alternation).
    #[test]
    fn order_edge_vectors() {
        let a = AF_INET;
        let b = AF_INET6;
        let x = 1; // "other" family (the vectors use AF_UNIX)
        let cases: &[(Vec<i32>, Vec<usize>)] = &[
            (vec![b, b, b, b, a], vec![0, 4, 1, 2, 3]),
            (vec![a, a, x, b, b], vec![0, 3, 1, 2, 4]),
            (vec![x, x, a, a, b, b], vec![0, 1, 2, 4, 3, 5]),
            (vec![b, b, x, a], vec![0, 3, 1, 2]),
            (vec![a, a, b, b, b, a, a], vec![0, 2, 1, 3, 5, 4, 6]),
            (
                vec![b, b, a, b, b, a, a, a, b],
                vec![0, 2, 1, 5, 3, 6, 4, 7, 8],
            ),
            (vec![x], vec![0]),
            (vec![a, x, a, a, b], vec![0, 1, 2, 4, 3]),
        ];
        for (fam, want) in cases {
            assert_eq!(&reorder(fam), want, "{fam:?}");
        }
    }

    /// The head is never moved and the result is always a permutation.
    #[test]
    fn permutation_and_head() {
        let fams = vec![
            AF_INET, AF_INET6, AF_INET, AF_INET, AF_INET6, AF_INET, AF_INET6,
        ];
        let order = reorder(&fams);
        assert_eq!(order.first(), Some(&0));
        let mut sorted = order.clone();
        sorted.sort_unstable();
        assert_eq!(sorted, (0..fams.len()).collect::<Vec<_>>());
    }

    #[test]
    fn empty() {
        assert!(reorder(&[]).is_empty());
    }
}
