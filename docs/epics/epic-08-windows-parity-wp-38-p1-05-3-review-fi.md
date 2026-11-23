# E8 — Windows parity (WP-38, P1-05.3 + review finding 5)

**Milestone:** M1 — Phase 1: code & recipe

Windows parity (WP-38, P1-05.3 + review finding 5)
**Why:** the "3-OS" Done-when of the threading/pipe/dl tasks cannot tick without it.

| Story | I want | Acceptance | Status |
|---|---|---|---|
| **E8S1** `threading-windows` port (P1-05.3) | events/semaphores as `HANDLE`s (`CreateEvent`/`CreateSemaphore`, `WaitForSingleObject`), no SRW locks | behaviour matches `threading-windows.c`; documented | open |
| **E8S2** Windows `dl`/`pipe`/`os_proc` variants (P1-11 3-OS) | the POSIX-only modules gain Windows implementations | Windows CI green for the three modules | open |
| **E8S3** Windows CI green (P1-05.3) | the whole threading area passes on the Windows runner | `test (windows-latest)` green incl. the new modules | open |

---
