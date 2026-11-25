# happy_eyeballs (S-024 M9, P1-33.3) — test matrix

Test files: `crates/obs-netutil/src/happy_eyeballs.rs`
Issue: WP-34 (P1-33.3)

Deterministic half of `shared/happy-eyeballs` in
`crates/obs-netutil` (S-024 M9): the resolver family hint from the
configured bind address length and the C linked-list reorder algorithm
(single pass: when `prev`/`cur` share an INET/INET6 family, the first
later node of the *other* family moves between them; no other node left
→ stop; the head is never moved; foreign families never trigger a move
and are never moved). The connection race (sockets, timers, worker
threads) is unspecified (S-024 §1) and stays in C.

| Area                                                                                                                                                      | Pinned by test         | Conformance vector                         |
| --------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------- | ------------------------------------------ |
| **hint family (S-024 §4.9)**                                                                                                                              |                        |                                            |
| bind length 16 → `AF_INET`, 28 → `AF_INET6`, anything else (0, 12, 64…) → `AF_UNSPEC`                                                                     | `hint_family_values`   | `helpers/he-resolve-args`                  |
| **constants**                                                                                                                                             |                        |                                            |
| `HAPPY_EYEBALLS_DELAY_MS=200`, `HAPPY_EYEBALLS_MAX_ATTEMPTS=6`, `HAPPY_EYEBALLS_CONNECTION_TIMEOUT_MS=25000`, `SOCKADDR_IN_LEN=16`, `SOCKADDR_IN6_LEN=28` | `constants`            | `helpers/he-resolve-args` (he_consts line) |
| **reorder (S-024 §4.9)**                                                                                                                                  |                        |                                            |
| C walk algorithm on the reference family sequences (same family adjacent → next opposite node moves in between)                                           | `order_vectors`        | `helpers/he-order`                         |
| edge cases: empty lists, single-family lists, uneven groups, foreign families (`AF_UNIX`), no-op when the opposite family is exhausted                    | `order_edge_vectors`   | `helpers/he-order-edge`                    |
| `reorder` is a permutation (same multiset) and the head is never moved                                                                                    | `permutation_and_head` | `helpers/he-order`                         |
| empty input list stays empty                                                                                                                              | `empty`                | —                                          |
