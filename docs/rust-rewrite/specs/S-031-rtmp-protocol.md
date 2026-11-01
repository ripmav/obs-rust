# S-031 RTMP protocol engine (vendored librtmp)

| | |
|---|---|
| Status | Ready for review |
| Reference | libobs 33.0.1 tree, `plugins/obs-outputs/librtmp/{rtmp.c,rtmp.h,parseurl.c}` (+ `handshake.h` included by `rtmp.c`) |
| Target crate | `crates/obs-rtmp` (modules `url`, `handshake`, `chunk`, `engine`, `auth`) |
| Depends on | S-021 (AMF0 encoder/decoder, `AMFObject` model), S-024 (nothing required) |
| Conformance | `test/conformance/scripts/rtmp/` (35 scripts) |

## 1. Scope

The client-side RTMP engine OBS uses to publish a stream (and, because the code is shared, to play one): URL parsing, stream-key
rewriting, the plain and the SWF-digest handshake, chunk-stream writing (`RTMP_SendPacket`, `RTMP_Write`) and reading
(`RTMP_ReadPacket`), the command state machine of `RTMP_Connect` / `RTMP_ConnectStream`, control messages, acknowledgement
accounting, publisher authentication and close/error behaviour. AMF0 value encoding is S-021 and is only referenced.

Out of scope (a conforming implementation need not provide them): RTMPT/RTMPTE (HTTP tunnelling, `HTTP_Post`/`HTTP_read`, protocol flag
`RTMP_FEATURE_HTTP`); SOCKS4 (`Link.socksport`; `SocksNegotiate`); RTMPS TLS (`RTMP_Connect1` TLS branch, mbedTLS), the connector
(`happy_eyeballs`, name resolution) and `net-if` bind handling; `RTMP_Read` / `Read_1_Packet` (FLV re-assembly for playback);
RTMPE type 8/9 handshake signature scrambling (XTEA / Blowfish tables) which a server answering S0 = 8 or 9 to a digest-mode client would
trigger. `RTMP_SetOpt`, `RTMP_SetupStream`, `RTMP_Serve`, `hashswf.c` (`RTMP_HashSWF`) are **declared or absent in this tree**: there is
no server mode and no option-string parser (`parseAMF` is an unused static); a conforming implementation provides none of them
(S-031-R26). The engine is not thread-safe; OBS drives one `RTMP` object from one thread at a time (the sender thread), except that
`RTMP_Close` may be called from another thread after stopping the sender.

## 2. Terminology and conventions

RFC 2119 words. All integers on the wire are big-endian except the message stream id in a type-0 chunk header (little-endian).
`BE24`/`BE32` = big-endian 3/4 byte value. "AMF string/number/null" are AMF0 values (S-021 section: marker `02` + u16 length + bytes;
`00` + IEEE-754 double; `05`). "Packet" = one RTMP message with header fields (channel = chunk stream id `csid`, header type `fmt` 0..3,
message type, timestamp, stream id `info2`, body). Sizes `RTMP_MAX_HEADER_SIZE` = 18 and `RTMP_MAX_STREAMS` = 8.

## 3. Interface

### 3.1 Engine state (observable through `state`)

`RTMP` fields that influence behaviour (defaults after `RTMP_Init`): `m_inChunkSize` 128, `m_outChunkSize` 128, `m_bSendChunkSizeInfo` 1,
`m_nBufferMS` 30000, `m_nClientBW` 2500000, `m_nClientBW2` 2, `m_nServerBW` 2500000, `m_fAudioCodecs` 3191.0, `m_fVideoCodecs` 252.0,
`m_fEncoding` 0, `Link.receiveTimeout` 30, `Link.sendTimeout` 15, `Link.swfAge` 30, `m_stream_id` 0, `m_numInvokes` 0, `m_nBytesIn`/`m_nBytesInSent`
0, `m_bSendCounter` 0 (set to 1 by `RTMP_Connect` after the socket is up), socket handle "invalid" (-1). `Link` carries `hostname`, `port`
(u16), `app`, `tcUrl`, `swfUrl`, `pageUrl`, `flashVer`, `auth`, `usherToken`, `subscribepath`, `token`, `pubUser`, `pubPasswd`, `protocol`
(bit flags), `lFlags` (`RTMP_LF_AUTH` 1, `LIVE` 2, `SWFV` 4, `PLST` 8, `BUFX` 0x10, `FTCU` 0x20), `pFlags` (`NAME` 1, `RESP` 2, `ALLOC` 4, `CLEAN` 8,
`CLATE` 0x10), `streams[8]{id, playpath}`, `nStreams`, `curStreamIdx`, `playingStreams`, `SWFSize`, `SWFHash[32]`, `seekTime`, `stopTime`.
Protocol flags: `RTMP`=0, `RTMPT`=1 (HTTP), `RTMPE`=2 (ENC), `RTMPTE`=3, `RTMPS`=4 (SSL), `RTMPTS`=5, `RTMFP`=8, `WRITE`=0x10 (publish mode,
`RTMP_EnableWrite` ORs it in).

### 3.2 Functions (all return `1` = TRUE / `0` = FALSE unless stated)

`RTMP_ParseURL(url, &protocol, &host, &port, &app)`, `RTMP_ParsePlaypath(in, &out)`, `RTMP_SetupURL(r, url)`, `RTMP_AddStream(r, playpath) -> idx`,
`RTMP_EnableWrite`, `RTMP_Connect(r, NULL)`, `RTMP_ConnectStream(r, 0)`, `RTMP_ReadPacket(r, &pkt)`, `RTMP_ClientPacket(r, &pkt) -> 0|1|2`,
`RTMP_SendPacket(r, &pkt, queue)`, `RTMP_Write(r, buf, size, streamIdx) -> int`, `RTMP_SendCtrl`, `RTMP_SendPause`/`RTMP_Pause`/`RTMP_SendSeek`,
`RTMP_SendServerBW`/`RTMP_SendClientBW`, `RTMP_SendCreateStream`, `RTMP_UpdateBufferMS`, `RTMP_DeleteStream`, `RTMP_Close`, `RTMP_IsConnected`,
`RTMP_IsTimedout`, `RTMP_GetTime`.

### 3.3 Harness inputs (not part of the engine)

The conformance runner supplies: the transport (queued server bytes, EOF/timeout/reset on an empty queue, per-call byte cap, send failures),
`clock` (milliseconds returned by `RTMP_GetTime`), the random generator (31-bit LCG, `state = (state*1103515245 + 12345) mod 2^31`, seed
`seed N`, `rand()` returns the new state; the engine takes the *low 32 bits as a little-endian word* when it fills buffers) and the
connector (every `RTMP_Connect` yields a fresh connection; `hevfail` makes it fail). The random bytes of handshakes and the `cnonce`/`challenge`
of authentication are therefore deterministic *only because the generator is specified*; a real implementation uses any CSPRNG.

## 4. Data formats

### 4.1 Chunk basic header and message header

First byte: `fmt << 6 | id6`. `id6` 2..63 = csid; `id6 == 0`: csid = next byte + 64 (64..319); `id6 == 1`: csid = BE-less little-endian u16 (`b1 + b2*256`) + 64 (320..65599). Message header
after the basic header: fmt 0 = 11 bytes (`BE24 ts`, `BE24 length`, `u8 type`, `LE32 stream id`), fmt 1 = 7 (`ts delta, length, type`), fmt 2 = 3 (`ts delta`),
fmt 3 = 0. A 24-bit timestamp field of `0xffffff` is followed by a 4 byte extended timestamp (BE32) *after the message header*.

Worked example (vector `10-publish-flow`, `SetChunkSize(4096)` sent first): `02 000000 000004 01 00000000 | 00001000`. The connect invoke uses
`03 000000 0000ac 14 00000000`. A following `releaseStream` on csid 3 uses fmt 1: `43 000000 000025 14`.

### 4.2 Handshake blocks

C0 = `03`. C1 (plain mode) = `BE32 clock_ms`, `00 00 00 00`, 1528 random bytes (382 generator words stored little-endian, bytes 8..1535).
The runner's example (clock 1000, seed 3): `030000 03e8 00000000 801b5345 ...` (vector `30-handshake-plain`).
Digest mode (only when `Link.SWFSize != 0`): bytes 4..7 = `0a 00 2d 02` (version 10.0.45.2), the rest random as above, then a 32 byte digest is *overwritten*
into C1 at `pos0(C1)`.

Digest position schemes (block of 1536 bytes `b`): scheme 0: `pos = (b[8]+b[9]+b[10]+b[11]) mod 728 + 12`; scheme 1: `pos = (b[772]+b[773]+b[774]+b[775]) mod 728 + 776`
(sums of four *unsigned* bytes). `D(b,pos,key)` = HMAC-SHA256(key, `b[0..pos] ++ b[pos+32..1536]` (1504 bytes)).

Keys: `FMS` (68 bytes) = ASCII `Genuine Adobe Flash Media Server 001` (36 bytes) ++ `K`; `FP` (62 bytes) = ASCII `Genuine Adobe Flash Player 001` (30 bytes) ++ `K`;
`K` = `f0eec24a8068bee82e00d0d1029e7e576eec5d2d29806fab93b8e636cfeb31ae` (32 bytes).

### 4.3 Connect invoke body (publish mode, vector `10-publish-flow`)

AMF string `connect`, number txn (`++m_numInvokes`), object (`03`) with named properties in this order (AMF named string/number/boolean as in S-021):
`app` = `Link.app`; if publishing: `type` = `nonprivate`; (`customConnectEncode` hook: not used by OBS, not specified); `flashVer` if non-empty; `swfUrl`
if non-empty; `tcUrl` if non-empty; if **not** publishing: `fpad` = false, `capabilities` = 15, `audioCodecs`, `videoCodecs`, `videoFunction` = 1 and `pageUrl` if
non-empty; then `objectEncoding` = `m_fEncoding` if `m_fEncoding != 0 || m_bSendEncoding`. Object end `00 00 09`. After the object: if `Link.auth` is non-empty an AMF boolean
(`lFlags & RTMP_LF_AUTH`) and an AMF string with the auth text; then the `Link.extras` properties (none from OBS). The body is built in a 4096-byte buffer; if any
step does not fit, the connect fails (`RTMP_Connect1` logs "RTMP connect failed", closes, returns 0).

## 5. Behaviour

### 5.1 URL parsing (`RTMP_ParseURL`, `RTMP_SetupURL`)

- **S-031-R01** The URL must contain `"://"` (first occurrence); otherwise log ERROR and return 0 with `protocol = RTMP`, `port = 0`, `app = (NULL,0)`.
  The host/app outputs are *not* reset (they keep their previous value; the engine's `Link` starts zero).
- **S-031-R02** Scheme = text before `"://"`, compared ASCII case-insensitively: `rtmp`=0, `rtmpt`=1, `rtmpe`=2, `rtmpte`=3, `rtmps`=4, `rtmpts`=5, `rtmfp`=8. Exact length matters
  (`rtmpx`, `http` are unknown).
- **S-031-R03** Unknown scheme: log WARNING and continue; `protocol` stays `RTMP` (0).
- **S-031-R04** Empty text after `"://"` : WARNING, return 0 (outputs unchanged except `protocol`/`port`/`app` initialisation).
- **S-031-R05** `slash` = first `/` after `://`, `v6` = first `]`, `col` = first `:` searched from `p` or, when `v6` exists and lies before `slash` (compared as
  pointers; with no slash the comparison is false), from `v6`. Host length = (slash, else end) − start, shortened to the colon position if the colon lies before the host end.
  Host keeps brackets (`[::1]`).
- **S-031-R06** Host length >= 256: WARNING, `host` is not assigned (stays as it was), the call still succeeds.
- **S-031-R07** If the host is followed by `:`, `atoi` of the following text (digits, optional sign/leading blanks; `abc` → 0, `-5` → huge/invalid): a value > 65535 (or negative) logs
  WARNING and leaves `port = 0`; otherwise `port` = the value (0 allowed, `12ab` → 12).
- **S-031-R08** No slash: WARNING, return 1 with `app = (NULL,0)`.
- **S-031-R09** `app` = text after the first slash to the end of the string (query and further slashes included); a single trailing `/` is removed (`app//` → `app/`).
  Returns 1. `app` points into the URL (not NUL-terminated at its length).
- **S-031-R10** The port is returned separately from `Link.port` (see R12).
- **S-031-R11** `RTMP_SetupURL` calls `ParseURL`, **overwriting `Link.protocol`** (so `RTMP_EnableWrite` must follow it; Q-1), sets `Link.hostname`, `Link.app`, `Link.port`; a failing parse returns 0 but has already reset `Link.protocol` to `RTMP` (0) and `Link.app` to NULL
  (ParseURL initialises its outputs before failing); `Link.hostname` and `Link.port` are untouched.
- **S-031-R12** If the parsed port is 0: 443 for the SSL flag, else 80 for the HTTP flag, else 1935.
- **S-031-R13** If `Link.tcUrl` is empty: `tcUrl` = the original URL up to the end of `app` (so a trailing `/` of the URL is dropped); if `app` is empty (including the "no slash" case)
  `tcUrl` = the whole URL.
- **S-031-R14** A non-empty `Link.tcUrl` set earlier is kept (the second `SetupURL` in the same object replaces host/app/port only).
- **S-031-R15** If `lFlags & SWFV` and `swfUrl` is non-empty (checked after the tcUrl step), return 0 (SWF hashing is not compiled in).
- **S-031-R16** `sockshost` is cleared (`socksport = 0`) — SOCKS is not supported by OBS.

### 5.2 Stream keys (`RTMP_ParsePlaypath`, `RTMP_AddStream`)

Input `in` = (bytes, len). Output = newly allocated string; `NULL`/len 0 for NULL input.

- **S-031-R17** If the first byte is `?` and the text contains `slist=`, the playpath is the text after `slist=` up to the next `&` (or the end); otherwise the whole input.
- **S-031-R18** Extension test: let `q` = first `?` in the (sliced) text; the 4 bytes `ext` are the 4 bytes ending at `q` (before the query), or the last 4 bytes when no `?`, tested only when
  length >= 4. (Input of length 4 or more with `?` among its first 4 bytes reads before the buffer: D-1.)
- **S-031-R19** `.f4v` or `.mp4` (exact lower case) → prepend `mp4:` and remove the extension; `.flv` → remove the extension *only if the text was not sliced by slist*; `.mp3` → prepend
  `mp3:`, remove extension. An extension in front of a query (`x.f4v?a=b.mp4`) is the one before the `?` and the query is kept.
- **S-031-R20** If the text already starts with `mp4:` (resp. `mp3:`) no prefix is added **and the extension is not removed**.
- **S-031-R21** Copy loop: `%` followed by two characters is replaced by the byte `sscanf("%02x")` reads (a decoded `%00` ends the visible string, but the returned length counts it); `%` consumes 3 input
  bytes. Non-hex digits: D-2.
- **S-031-R22** Output length = bytes written; output is NUL-terminated.
- **S-031-R23** `RTMP_AddStream(r, key)` stores the rewritten playpath in `streams[nStreams]`, sets its `id = -1`, increments `nStreams`, returns the old count as index. `NULL` key → empty playpath (NULL value).
- **S-031-R24** There is no bound check: the 9th stream writes outside `streams[8]` (D-3). A conforming implementation returns -1 and changes nothing.

### 5.3 Absent API

- **S-031-R26** `RTMP_SetOpt`, `RTMP_SetupStream`, `RTMP_Serve`, `RTMP_HashSWF` are not implemented by the reference (declared only) and need not exist. There is no server-side
  handshake, no `connect` request parsing and no way to receive `publish`.

### 5.4 Handshake (`HandShake` as compiled with CRYPTO, called by `RTMP_Connect1`)

Reads in this section go through `ReadN` (rule R79: every byte counts into `m_nBytesIn`, so after a full handshake `m_nBytesIn == 3073`).

- **S-031-R27** If the protocol has the ENC flag (`rtmpe://`, `rtmpte://`) log ERROR "encrypted RTMP is not supported" and fail the handshake. RTMPE is therefore never negotiated (no DH/RC4 code runs).
- **S-031-R28** Digest mode iff `Link.SWFSize != 0`. Plain mode: send C0 (`03`) and C1 in one write (1537 bytes): `BE32 RTMP_GetTime()`, 4 zero bytes, 382 generator words little-endian.
- **S-031-R29** Read S0 (1 byte) then S1 (1536). A type byte differing from the client's `03` only logs WARNING. Fewer bytes → fail (R32).
- **S-031-R30** Plain mode: send S1 unchanged as C2 (1536 bytes), then read S2 (1536). If S2 differs from C1 log WARNING "client signature does not match"; always succeed.
- **S-031-R31** In digest mode, a server that answers `S0 == 3` with `S1[4] == 0` is treated as a plain server (the plain path above, no digest checks, but C1 was already sent in digest form).
- **S-031-R32** A failed read (timeout, EOF, short) or failed write fails the handshake; `RTMP_Connect1` then logs ERROR "handshake failed", closes the connection and returns 0. No connect invoke is sent.
- **S-031-R33** `RTMP_Connect1` after a successful handshake sends the connect (R40) and returns 1 (0 + Close if the connect did not fit).
- **S-031-R34** `RTMP_Connect` = connector + `RTMP_Connect0` (socket options; sets `m_bSendCounter = 1`) + `RTMP_Connect1`. A failing connector logs ERROR (ECONNREFUSED text names the host)
  and returns 0 with the engine closed. A host written `[v6]` is passed to the connector without brackets.

Digest mode (C1 as in 4.2; `clientsig` = C1 without C0):

- **S-031-R35** C1 digest: `pos0 = scheme0(C1)`; digest = `D(C1, pos0, FP[0..30])`, stored at `C1[pos0..+32]`. The digest is computed over the 1504 bytes *excluding* its own place.
- **S-031-R36** Server S1 verification: try scheme 0 with `D(S1, pos, FMS[0..36])`; if it differs log WARNING "Trying different position for server digest!" and try scheme 1; if that also differs log ERROR and fail.
  (Scheme used for later steps is the one that matched, but only `pos` of the server digest is used.)
- **S-031-R37** C2: 384 generator words (little-endian, 1536 bytes), then the last 32 bytes are replaced by `HMAC(key = HMAC(FP[0..62], S1[pos..pos+32]), C2[0..1504])`. Written as 1536 bytes.
  Also `SWFVerificationResponse = 01 01 | BE32 SWFSize | BE32 SWFSize | HMAC(key = S1[1504..1536], SWFHash[32])` (42 bytes).
- **S-031-R38** S2 verification: `signature = HMAC(key = HMAC(FMS[0..68], C1[pos0..pos0+32]), S2[0..1504])` must equal `S2[1504..1536]`; mismatch logs WARNING "Server not genuine Adobe!" and fails.
- **S-031-R39** If `S0` is 8 or 9 the reference additionally scrambles the 32-byte signatures in 8-byte blocks (XTEA / Blowfish with a per-block key chosen by `digest[i] mod 15`); **not specified** (open
  question O-2); an implementation MAY fail such handshakes.

### 5.5 Command state machine (publish mode)

`RTMP_Connect` leaves one `connect` queued (`calls=[connect:1]`). `RTMP_ConnectStream` loops `RTMP_ReadPacket` while `!m_bPlaying && connected && read succeeded`: for each ready packet
with a non-empty body: AUDIO/VIDEO/INFO packets are discarded with WARNING "Received FLV packet before play()! Ignoring."; all others go to `RTMP_ClientPacket` (R84) and the body is freed.
It returns `m_bPlaying`. A `RTMP_ReadPacket` failure (timeout, EOF, bad data) ends the loop (return 0; the connection stays open on a timeout, R78).

Sending uses `send(pkt, queue)` = `RTMP_SendPacket` (R54..R62); `queue` = remember (method, txn) until the response arrives. Message tables (all type 20 INVOKE unless noted;
`txn = ++m_numInvokes` unless noted; `S(x)` AMF string, `N(x)` AMF number, `Z` AMF null):

| message | csid | fmt | queue | body |
|---|---|---|---|---|
| SetChunkSize | 2 | 0 | no | type 1, `BE32 m_outChunkSize` — sent first iff publishing and `m_bSendChunkSizeInfo` |
| connect | 3 | 0 | yes | 4.3 |
| releaseStream | 3 | 1 | no | `S("releaseStream") N(txn) Z S(playpath)` |
| FCPublish | 3 | 1 | no | `S("FCPublish") N(txn) Z S(playpath)` |
| createStream | 3 | 1 | yes | `S("createStream") N(txn) Z` |
| publish | 4 | 0 | yes | `S("publish") N(txn) Z S(playpath) S("live")`; stream id (LE) = `streams[cur].id` |
| FCUnpublish | 3 | 1 | no | `S("FCUnpublish") N(txn) Z S(playpath)` |
| deleteStream | 3 | 1 | no | `S("deleteStream") N(txn) Z N(stream id)` |
| _checkbw | 3 | 0 | no | `S("_checkbw") N(txn) Z` |
| _result (bw check) | 3 | 1 | no | `S("_result") N(server txn) Z N(counter)`; message timestamp = `0x16 * counter`, then `counter++` |
| pong | 3 | 1 | no | `S("pong") N(server txn) Z`; timestamp `0x16 * m_nBWCheckCounter` |
| secureTokenResponse | 3 | 1 | no | `S("secureTokenResponse") N(0) Z S(response)` |
| ack | 2 | 1 | no | type 3, `BE32 m_nBytesIn` |
| user control | 2 | 1 | no | type 4, see 5.9 |
| server BW / client BW | 2 | 0 | no | type 5 `BE32 m_nServerBW`; type 6 `BE32 m_nClientBW, u8 m_nClientBW2` |

- **S-031-R40** After the connect `_result` (txn matches the queued `connect`; the queue entry is removed): if `Link.token` is set and the result object contains a `secureToken` property (searched
  recursively, first match) → R113. Publishing: for `i` in 0..nStreams send releaseStream(i); then for each stream FCPublish(i); (each increments the txn counter); then for each stream createStream.
  Not publishing: R104.
- **S-031-R41** Result for `createStream`: `id = (int)number at index 3` of the decoded message; `streams[curStreamIdx].id = id`; publishing: send `publish` for that stream; else R105; `curStreamIdx++`.
- **S-031-R42** Result for `publish` or `play`: `m_bPlaying = 1`, `playingStreams++`. The queue entry is removed. (`m_stream_id` is never set from the createStream result: it stays 0 until a close sets -1; Q-9.)
- **S-031-R43** A `_result` whose txn (cast to int) matches no queued call is ignored (DEBUG log); the first queued call with an equal txn is used.
- **S-031-R44** `onBWDone`: if `m_nBWCheckCounter == 0` send `_checkbw`. The counter is reset to 0 on close.
- **S-031-R45** `_onbwcheck(txn)`: send the `_result` of the table with the server's txn; counter increments.
- **S-031-R46** `_onbwdone`: remove the first queued `_checkbw` call if any (never queued by the engine, so a no-op).
- **S-031-R47** `ping` invoke: send `pong` with the server's txn. `onFCSubscribe`, `playlist_ready` (removes the first queued `set_playlist`) and unknown methods are ignored. An invoke whose first byte is not `02` logs
  WARNING and is ignored; undecodable AMF logs ERROR.
- **S-031-R48** `onStatus` (code at object index 3, property `code`; `level`/`description` also read): codes `NetStream.Failed`, `NetStream.Play.Failed`, `NetStream.Play.StreamNotFound`,
  `NetConnection.Connect.InvalidApp`, `NetStream.Publish.Rejected`, `NetStream.Publish.Denied`, `NetStream.Publish.BadName` set `m_stream_id = -1` and close the engine (R52), then log ERROR
  `"<tcUrl>:\n<code> (<description>)"` (description only if non-empty).
- **S-031-R49** `NetStream.Publish.Start`: `m_bPlaying = 1` and remove the first queued `publish`; `NetStream.Play.Start` / `Play.PublishNotify`: same with `play`.
- **S-031-R50** `NetStream.Play.Complete`, `Play.Stop`, `Play.UnpublishNotify`: close the engine and make `RTMP_ClientPacket` return 1. `Seek.Notify` clears the seeking flag; `Pause.Notify` sends an unpause when
  pausing (`m_pausing` 1 or 2) — not reachable from OBS.
- **S-031-R51** Any other code logs WARNING `"Unhandled: <tcUrl>:\n<code>"` and is ignored.
- **S-031-R52** Close side effect (see R114) means a rejected publish produces FCUnpublish + deleteStream on the wire before the socket is closed whenever the stream id is > 0.
- **S-031-R53** `_error`: only in publish mode and only for a queued txn: the call is removed; for `connect` the object at index 3 supplies `description` and PublisherAuth runs (R94); other errors are
  ignored. In play mode only logs ERROR "rtmp server sent error". An `_error` with an unqueued txn is ignored.

### 5.6 Writing (`RTMP_SendPacket`, `RTMP_Write`)

State per outgoing channel: the last sent packet (type, body size, absolute timestamp, wire delta `lastWire`). All durations are `u32` wrapping.

- **S-031-R54** Header-type selection. Let `prev` = last packet sent on this csid. If `prev` exists and the requested `fmt != 0`: (a) if `fmt == 1` and `prev.size == size` and `prev.type == type` → `fmt = 2`;
  (b) then if `fmt == 2` and `(ts - prev.ts) == prev.lastWire` → `fmt = 3`; `last = prev.ts`. Otherwise `last = 0`. `fmt > 3` logs ERROR "sanity failed" and returns 0 *before* sending.
- **S-031-R55** `t = ts - last` (wrapping). The 24-bit field is `min(t, 0xffffff)`; the extended 4-byte BE value `t` follows the message header iff `fmt != 3 && t >= 0xffffff` (so `t == 0xffffff` has the extended field).
  `lastWire` is remembered as `t`.
- **S-031-R56** Basic header: csid < 64: one byte `fmt<<6|csid`; 64..319: `fmt<<6`, `csid-64`; >= 320: `fmt<<6|1`, `(csid-64)&255`, `(csid-64)>>8`.
- **S-031-R57** Body is cut into chunks of `m_outChunkSize` (as currently set; changes apply immediately, also if never announced — Q-2); chunk 1 has the full header, the others a one-byte-type-3 header
  `0xc0 | code` (`code` = csid for csid < 64, 0 for 64..319, 1 for >= 320) followed by the same csid extension bytes as the first chunk's basic header. A zero-length body sends the header only.
- **S-031-R58** Every continuation chunk (including those of fmt 3 and of messages whose `t >= 0xffffff`) is followed by the 4-byte extended timestamp `t` iff `t >= 0xffffff`, but a *fmt-3 first chunk
  has no extended timestamp* (Q-4: the stream the engine writes is not RFC-conformant there).
- **S-031-R59** Message-length field = body size; type field = packet type; fmt-0 stream id is written little-endian from `info2`.
- **S-031-R60** If `queue` and the type is INVOKE (20): the method name and the number following it (AMF string then number: skip marker bytes) are stored in the call queue. (Non-invoke packets are never queued.)
- **S-031-R61** A write failure returns 0 (the connection is closed by R122); the per-channel bookkeeping is not updated on failure.
- **S-031-R62** After a successful send the packet header (incl. the absolute `ts`, size, type, `lastWire`) replaces the channel's bookkeeping.
- **S-031-R63** `RTMP_Write(buf, size, idx)`: while bytes remain: if no tag is in progress: if `size < 11` return 0; if the buffer starts with `FLV` skip 13 bytes; read tag header: type, `BE24` length, `BE24` timestamp + 1 byte
  extension (`ts = low24 | ext<<24`), skip 3 stream-id bytes; header type: AUDIO(8)/VIDEO(9) with `ts == 0` or INFO(0x12) → `fmt 0`, everything else → `fmt 1`; `csid = 4`; `info2 = streams[idx].id`.
- **S-031-R64** Copy body bytes (across calls if split); when complete: `RTMP_SendPacket(pkt, queue=0)`, then skip the 4-byte previous-tag-size that follows.
- **S-031-R65** Return value = `size + s2` where `s2` is the number of input bytes not consumed counted negatively if the 4-byte trailer is missing (`size - 4` for a tag without trailer, `size - 2` for a 2 byte trailer; 0 for
  `size < 11`; -1 if `RTMP_SendPacket` failed). Tag types other than 8/9/18 are sent unchanged.
- **S-031-R66** A tag can be split across calls only after its header (the header must be fully inside the first call of a tag); `m_write` keeps the partial body.
- **S-031-R67** Timestamps: absolute 32-bit from the tag (`16777215`, `16777216`, `4000000000` all valid); consecutive frames produce fmt 2/3 per R54; fmt 0 tags always send the absolute timestamp.
- **S-031-R68** The first tag after `publish` (fmt 1, first message for this packet shape) uses `last = prev.ts` of the publish packet (0).
- **S-031-R69** Sending on a closed engine fails with a send error (ERROR log, return 0 / -1 for Write).

### 5.7 Reading (`RTMP_ReadPacket`)

Per input channel the engine keeps the last packet header (`fmt`-inherited fields, partial body) and `m_channelTimestamp[csid]` (i32, starts 0). The caller passes one `RTMPPacket` struct repeatedly.

- **S-031-R70** Read 1 byte (timeout/EOF → return 0 without error log if it is the first byte). `fmt = b>>6`, `csid` per 4.1 (2- and 3-byte forms read 1 resp. 2 more bytes). The channel tables grow to `csid+10` entries.
- **S-031-R71** fmt 0: `hasAbsTimestamp = 1`. fmt 1/2/3: the packet struct is first overwritten by the stored header of this csid (if any; otherwise the caller's struct contents are used). Then fields are read:
  fmt 0..2: timestamp field (24 bit); fmt 0/1: length (24 bit, resets `bytesRead` to 0) and type; fmt 0: stream id (little-endian, `info2`).
- **S-031-R71a** The `fmt` value reported for a ready/partial packet is the one stored with it: when a type 1/2/3 chunk starts a new message (the stored body is empty) it is the fmt of that
  chunk; for later chunks of the same message it stays the fmt of the first chunk (the stored header is copied back over the caller's struct each time). Fields of the caller's struct that the
  chunk does not carry (e.g. type/length for fmt 3 with no stored header for the csid) keep the value from the previous call.
- **S-031-R72** If the field is `0xffffff` (fmt 0/1/2 only) a 4-byte extended timestamp replaces it. At completion of a message with `hasAbs == 0`, `ts += m_channelTimestamp[csid]`; then
  `m_channelTimestamp[csid] = ts`. The stored header keeps the *field* value as read (`0xffffff` if extended) and `hasAbs` is cleared on completion.
- **S-031-R73** Chunk payload = `min(remaining, m_inChunkSize)` bytes appended to the message body; the packet is ready when `bytesRead == length`. A not-ready read returns the packet with `body = NULL`
  (the engine keeps the buffer). A message with length 0 never allocates a body.
- **S-031-R74** `m_inChunkSize` changes only through `RTMP_ClientPacket` (type 1 with body >= 4: `BE32`; no range check). `read` alone does not apply it.
- **S-031-R75** A type-3 header (fmt 3) takes the stored timestamp field (the previous message's delta, or — after a type-0 message — its *absolute* value; vector `20-read-headers` shows `ts` 1000→1040→1080→1120).
- **S-031-R76** (Q-5) A type-3 *continuation* chunk of a message whose timestamp was extended does **not** consume the 4-byte extended field (although the engine writes it): the stream desynchronises
  (vector `23-read-ext-continuation-quirk`). Required: same.
- **S-031-R77** Per-channel timestamp table is observable (`chts`).
- **S-031-R78** Timeout (`EAGAIN`): the read returns 0, `RTMP_IsTimedout` = 1, the connection stays open, and — if the timeout hit in the middle of a chunk — the bytes already consumed are lost
  (Q-6: continuing is undefined; implementations should treat a mid-packet timeout as fatal). EOF or a socket error: ERROR log, the engine closes itself (R114) and the read returns 0.

### 5.8 Byte accounting and acknowledgements

- **S-031-R79** `m_nBytesIn` += every byte delivered to `ReadN` (handshake included).
- **S-031-R80** In `ReadN`, after each copy step: if `m_bSendCounter` and `m_nBytesIn > m_nBytesInSent + m_nClientBW/10` (integer division, signed) send the acknowledgement; if that send fails the read fails.
- **S-031-R81** Acknowledgement: type 3, csid 2, fmt 1 (then R54), body `BE32 m_nBytesIn`; `m_nBytesInSent = m_nBytesIn` *before* the send. It can be sent in the middle of reading a message.
- **S-031-R82** `m_nClientBW` is set by **Set Peer Bandwidth** (type 6; `BE32`, `u8 limit-type` into `m_nClientBW2`, or 255 when the body has only 4 bytes), `m_nServerBW` by **Window Ack Size** (type 5, `BE32`).
  The acknowledgement threshold uses `m_nClientBW` (Q-7: Window Ack Size does not influence acks).
- **S-031-R83** `RTMP_Close` resets `m_nBytesIn` and `m_nBytesInSent` to 0; `RTMP_Connect` sets `m_bSendCounter = 1` (`RTMP_Connect0/1` alone do not).

### 5.9 Control messages (`RTMP_ClientPacket`)

- **S-031-R84** Dispatch on the packet type: 1 chunk size (R74), 3 bytes-read report (log), 4 user control (R85), 5 server BW (R82), 6 client BW (R82), 8/9 audio/video (return 1, set `m_mediaChannel`, `m_mediaStamp`),
  15/16/17 flex messages (17: handled as invoke after skipping 1 byte), 18 INFO (metadata; return 1 if `onMetaData`), 19 shared object (ignored), 20 invoke, 22 aggregate "FLV tags" (return 1), others (DEBUG log).
  Return value 2 = invoke handler asked to stop (R50), 1 = media, 0 otherwise. Zero-length bodies are skipped by callers (`ConnectStream`, `read`'s client mode).
- **S-031-R85** User control: body < 2 bytes → type -1 (log only). With >= 6 bytes: type 0 stream begin, 1 EOF (pause bookkeeping), 2 dry, 4 recorded, 31 BufferEmpty (only with `RTMP_LF_BUFX`), 32 BufferReady, others: logged.
  Type 6 (ping): send user control type 7 with the same `BE32` value (R130 body layout, 6 bytes).
- **S-031-R86** User control body layout written by `RTMP_SendCtrl(type, obj, time)`: `BE16 type, BE32 obj` (6 bytes); type 3 (set buffer length): 10 bytes `BE16 3, BE32 obj, BE32 time`; type 0x1A (SWF verify request): 3 bytes `BE16, u8 obj`; type
  0x1B: 44 bytes = `BE16 0x1B` ++ `SWFVerificationResponse[0..42]`. Always csid 2, fmt 1, type 4, `ts = 0`.
- **S-031-R87** SWF verification request (type 0x1A): body byte 2 > 1 → ERROR; else if `SWFSize != 0` send the 0x1B response, else ERROR "Ignoring SWFVerification request".
- **S-031-R88** `HandleServerBW`: `m_nServerBW = BE32(body[0..4])` with no length check (D-4: bodies shorter than 4 over-read; required: ignore).
- **S-031-R89** `HandleClientBW`: `m_nClientBW = BE32`, `m_nClientBW2 = body[4]` if body > 4 bytes else 255 (same D-4).
- **S-031-R90** `HandleChangeChunkSize`: only if body >= 4.
- **S-031-R91** INFO (18) packets: AMF decode; if the first value is `onMetaData`: log INFO lines, set `m_fDuration` from a property named `duration` (searched recursively), set `m_read.dataType` bits 1/4 for properties
  with prefix `video`/`audio`. FLV aggregate (22) packets: scan tags (see reference only for playback; not exercised by OBS).
- **S-031-R92** Unknown packet types are ignored.
- **S-031-R93** (OBS call profile) The streaming-time reader of OBS (`rtmp-stream.c`) calls only `RTMP_ReadPacket` — never `RTMP_ClientPacket` — so server pings and invokes after publish start are neither answered nor interpreted;
  only acknowledgements (R80) are produced (vector `26-drain-no-clientpacket`).

### 5.10 Publisher authentication (`PublisherAuth`, from the `_error` of `connect`, publish mode only)

`desc` = the `description` string of the error object (index 3). Let `app0` = `Link.app`, `tc0` = `Link.tcUrl`. All branches below *return 0 without closing* unless stated; `pFlags` bits are set as listed.

- **S-031-R94** `desc` contains `authmod=adobe`:
  (a) contains `code=403 need auth`: if `Link.app` already contains `authmod=adobe` → ERROR "wrong pubUser & pubPasswd", `pFlags |= CLEAN`, return 0; else if both `pubUser` and `pubPasswd` are non-empty
  `token = "?authmod=adobe&user=" + pubUser`, `pFlags |= NAME`; else ERROR "need to set pubUser & pubPasswd", `CLEAN`, return 0.
  (b) contains `?reason=needauth`: parse the query after that `?` (`&`-separated `key=value`): `user`, `salt`, `opaque`, `challenge` (values end at the next `&`). Compute
  `h1 = base64(md5(user ++ salt ++ pubPasswd))` (24 chars, padding), `c2 = base64(4 bytes of rand() as native int)` (8 chars), `resp = base64(md5(h1 ++ opaque ++ c2))` if `opaque` is non-empty, else
  `md5(h1 ++ challenge ++ c2)`; base64 is standard with padding. `token = "&challenge=" + c2 + "&response=" + resp + "&opaque=" + opaque` (empty if absent); `pFlags |= RESP | CLATE`.
  (c) `?reason=authfailed` → ERROR "wrong password", `CLEAN`, 0; (d) `?reason=nosuchuser` → ERROR "no such user", `CLEAN`, 0; (e) anything else → ERROR "unknown auth mode", `CLEAN`, 0.
- **S-031-R95** `desc` contains `authmod=llnw` (checked only if not adobe): (a) `code=403 need auth` as above with `authmod=llnw` (token `"?authmod=llnw&user=" + pubUser`); (b) `?reason=needauth`: parameters `user`, `nonce`;
  `nc = "00000001"`, `cnonce = 8 hex digits of rand()` (`%08x`), `h1 = hex(md5(user:live:pass))`, `app'` = `Link.app` cut at the first `?`, `h2 = hex(md5("publish:/" ++ app' ++ ("/_definst_" if app' contains no `/`)))`,
  `h3 = hex(md5(h1:nonce:nc:cnonce:auth:h2))`, `token = "&nonce=" + nonce + "&cnonce=" + cnonce + "&nc=" + nc + "&response=" + h3`; `pFlags |= RESP | CLATE`. (c) `?reason=authfail` → ERROR "Authentication failed", `CLEAN`, 0;
  (d) `nosuchuser`, (e) unknown as above.
- **S-031-R96** Neither authmod → return 0 silently.
- **S-031-R97** On success of (a)/(b): `Link.app = app0 + token` and `Link.tcUrl = tc0 + token` (new strings, lengths extended; `pFlags |= ALLOC`; **not NUL-terminated**, D-6).
- **S-031-R98** The caller (`HandleInvoke`) then — when PublisherAuth returned 1 — closes the engine (R114: FCUnpublish/deleteStream for open streams), sets `pFlags |= CLEAN` if `CLATE`, calls `RTMP_Connect(r, NULL)` and `RTMP_ConnectStream(r, 0)`
  recursively (a second `_error` can recurse again: the 403 → needauth sequence uses three connections in vector `32-auth-adobe`); `Link.streams` are *kept* across the close while publishing and `pFlags & CLEAN` is 0 (R115).
- **S-031-R99** A failure inside the nested connect leaves the engine closed; the outer `ConnectStream` loop then ends (the connection is gone).
- **S-031-R100** The connect object of the re-connect carries the new `app` and `tcUrl`; `txn` restarts at 1 (the close reset `m_numInvokes`); the SetChunkSize is sent again.
- **S-031-R101** `m_nBWCheckCounter`, ack counters and channel tables are reset by the close, so the re-connect starts from a clean engine.
- **S-031-R102** (`llnw`) see R95; the response differs from `adobe`.
- **S-031-R103** After a failing PublisherAuth the engine keeps the connection open and keeps reading (a later timeout ends `ConnectStream` with 0; vector `34-auth-failures`).

### 5.11 Play mode (non-publish; shared code, not used by OBS)

- **S-031-R104** After the connect `_result`: `RTMP_SendServerBW` (type 5, csid 2, fmt 0), `RTMP_SendCtrl(3, 0, 300)`; createStream per stream; then if `usherToken` is set a `NetStream.Authenticate.UsherToken` invoke (`txn++`, not queued);
  `FCSubscribe` (queued) with `subscribepath`, or — if `RTMP_LF_LIVE` — with each stream's playpath.
- **S-031-R105** Result of createStream: optional `set_playlist` if `RTMP_LF_PLST` (csid 8 fmt 0, `info2 = id`, queued; body = `S("set_playlist") N(0) Z` ++ the 4 bytes `08 00 00 00` ++ `03` ++ named string property `"0"` = playpath ++ `00 00 09`; note the array marker is followed by only three zero bytes, not four),
  then `play` (csid 8, fmt 0, `info2 = id`, `S("play") N(txn) Z S(playpath)` + start (`-1000` if LIVE, else `seekTime` if > 0 else 0) + `stopTime - seekTime` if `stopTime != 0`; queued), then `RTMP_SendCtrl(3, id, m_nBufferMS)`.
- **S-031-R106** The connect object of play mode: R40/4.3 non-publish branch; no SetChunkSize.
- **S-031-R107..R112** `Play.Start`/`PublishNotify`, failure codes, `Play.Stop/Complete` and the FLV-before-play rule behave as in R48..R51 and 5.5. `ping`/`FCSubscribe` unsupported cases ignored.
- **S-031-R113** SecureToken: if `Link.token` is non-empty and the connect `_result` contains `secureToken` (hex text): decrypt in place with XXTEA-style TEA described in 6.3, then send `secureTokenResponse` with the decrypted text (half the hex length).

### 5.12 Close and failures

- **S-031-R114** `RTMP_Close` on a connected engine: for every stream with `id > 0` (in index order) set `id = 0`, send `FCUnpublish` (publishing only) then `deleteStream(id)` (both with the normal fmt/csid rules, tolerating write errors), then close the socket.
- **S-031-R115** Close state reset: all stream ids -1, `m_stream_id = -1`, `m_nBWCheckCounter`, `m_nBytesIn`, `m_nBytesInSent` = 0, channel tables freed (so the next connection starts with fmt-0 headers and `lastWire` 0), call queue cleared, `m_numInvokes = 0`,
  `m_bPlaying = 0`, `Link.playingStreams = 0`, bind address and custom send cleared. When not publishing, or when publishing with `pFlags & CLEAN`, the stream table and `curStreamIdx` are cleared too; with a freed tcUrl/app when allocated.
  Close of a closed engine is a no-op apart from the reset.
- **S-031-R116** EOF (`recv` = 0): ERROR "remote host closed connection"; socket error (e.g. ECONNRESET): ERROR; both close the engine inside `ReadN` (R114 writes are attempted on the dead connection) and the read fails.
- **S-031-R117** `RTMP_ConnectStream` after the engine closed returns 0 without reading.
- **S-031-R118** A server `close` invoke logs ERROR and closes (no reconnect). `onFCUnsubscribe` closes and makes `ClientPacket` return 1.
- **S-031-R119** `RTMP_DeleteStream(idx)`: no-op if `m_stream_id < 0`; else `m_bPlaying = 0`, FCUnpublish (publishing), `deleteStream(m_stream_id)` (the never-assigned `m_stream_id`, i.e. 0.0), `m_stream_id = -1`.
- **S-031-R120** `RTMP_IsConnected` = socket valid; `RTMP_IsTimedout` = last read timed out.
- **S-031-R121** After close, sending fails (R69) without crash.
- **S-031-R122** Send error: ERROR log "RTMP send error N (L bytes)"; `EINTR` is retried (unless the global interrupt flag); otherwise `last_error_code` = errno, the socket gets an abortive close (SO_LINGER 0) and the engine closes
  itself (R114 attempted sends fail again, ids already 0); `WriteN` returns FALSE. A zero-byte send ends the loop as failure.
- **S-031-R123** A failed handshake write fails `Connect`.
- **S-031-R124** The connect body buffer is 4096 bytes minus the 18 byte header reserve (about 4078 bytes available); a connect object that does not fit (exact threshold not vectored; vector uses a 4100 byte app) fails (`Connect1` ERROR, return 0, closed).
- **S-031-R125** The connector failing returns 0 (R34).
- **S-031-R126** A failed `RTMP_SendPacket` inside `ConnectStream` handlers is ignored by the handlers (return values unused) except `SendBytesReceived` (R80).
- **S-031-R127** `RTMP_Connect` on an engine without `Link.hostname` is invalid input (precondition).
- **S-031-R128** `RTMP_Init` allocates TLS state (no observable effect without RTMPS).
- **S-031-R129** `RTMP_GetTime()` = millisecond tick counter (`times()*1000/CLK_TCK`, i.e. 10 ms resolution on Linux).
- **S-031-R130** `RTMP_SendCreateStream`: R40 table. `RTMP_SendPause(pause, t)`: csid 8 fmt 1 `S("pause") N(txn) Z B(pause) N(t)`, queued; `RTMP_Pause(1)` uses the channel timestamp of the media channel (0 if none).
- **S-031-R131** `RTMP_SendSeek(t)`: csid 8 fmt 1 `S("seek") N(txn) Z N(t)`, queued, sets the seeking flag.
- **S-031-R132** `RTMP_SendServerBW`: type 5 csid 2 fmt 0 `BE32 m_nServerBW`; `RTMP_SendClientBW`: type 6 `BE32 m_nClientBW, u8 m_nClientBW2`.
- **S-031-R133** `RTMP_UpdateBufferMS` = `SendCtrl(3, m_stream_id, m_nBufferMS)`.
- **S-031-R134** `RTMP_SendCtrl` bodies per R86 (type 0x1B copies the 42 byte SWF response; an unset response is 42 zero bytes).
- **S-031-R135** The call queue of sent-and-waiting methods is a list; `RTMP_DropRequest` removes entry i.
- **S-031-R136** `m_numInvokes` is the txn source for all requests above except `secureTokenResponse` (txn 0) and the bw-check `_result` / `pong` (server's txn).

## 6. Algorithms

### 6.1 Header compression (R54) in pseudocode

```
fn choose(prev, pkt):           # pkt.fmt is the requested type
  last = 0
  if prev and pkt.fmt != 0:
     if pkt.fmt == 1 and prev.size == pkt.size and prev.type == pkt.type: pkt.fmt = 2
     if pkt.fmt == 2 and pkt.ts - prev.ts == prev.lastWire:              pkt.fmt = 3
     last = prev.ts
  t = pkt.ts - last; pkt.lastWire = t
```
A requested fmt 2 or 3 is compressed further/kept as given (they are not "decompressed").

### 6.2 Reading loop (R70..R73)

```
fn read_packet(pkt):
  b = readn(1) or return false
  fmt = b>>6; csid = ...; ensure tables(csid)
  if fmt == 0: pkt.abs = true
  elif stored[csid]: *pkt = *stored[csid]            # fmt 1,2,3
  read fmt-dependent header; ext ts if field == 0xffffff (fmt<=2)
  if pkt.size > 0 and pkt.body == none: allocate
  n = min(pkt.size - pkt.read, in_chunk); readn(body+read, n) or return false
  pkt.read += n; stored[csid] = *pkt (ext: field=0xffffff)
  if pkt.read == pkt.size: if !abs: ts += chts[csid]; chts[csid] = ts; stored.body = none; stored.read = 0; stored.abs = false
  else: pkt.body = none
  return true
```

### 6.3 TEA decrypt of the secure token (R113)

Key: first 16 bytes of `Link.token` packed as four little-endian u32 (`k[i]`); fewer bytes → zero padded (trailing partial word kept only if non-zero). Text: hex pairs, `n = (len+7)/8` words ... see vector `36-secure-token`
(input hex `00112233445566778899aabbccddeeff`, token `0123456789abcdefXY`); standard XXTEA decryption with `DELTA = 0x9e3779b9`, `rounds = 6 + 52/n`, in 32-bit little-endian words; the result is the first `len/2` bytes.

## 7. Concurrency

One `RTMP` object is single-threaded. OBS' sender thread owns it after start; only `Close` may come from the control thread after the sender stopped. No locks are taken.

## 8. Reference defects and quirks

| ID | Kind | Description | Required |
|---|---|---|---|
| Q-1 | QUIRK | `SetupURL` overwrites `Link.protocol`, dropping the WRITE flag if set earlier | match; call order `SetupURL` then `EnableWrite` |
| Q-2 | QUIRK | `m_outChunkSize` takes effect immediately; announcing it (SetChunkSize) is a separate flag | match (script 17 announces explicitly) |
| Q-3 | QUIRK | for `fmt != 0` with no previous packet, the "delta" is the absolute timestamp (last = 0) | match |
| Q-4 | QUIRK | fmt-3 first chunk with `t >= 0xffffff` has no extended field; continuation chunks do | match |
| Q-5 | QUIRK | reader does not consume the extended timestamp of type-3 continuation chunks | match |
| Q-6 | QUIRK | mid-chunk timeout loses consumed bytes | treat as fatal |
| Q-7 | QUIRK | ack threshold uses peer-bandwidth value; Window Ack Size ignored | match |
| Q-8 | QUIRK | `RTMP_Write` puts audio, video and data on csid 4 | match |
| Q-9 | QUIRK | `m_stream_id` is never assigned from createStream; `DeleteStream`/`UpdateBufferMS` use 0 | match |
| Q-10 | QUIRK | Close on EOF/error writes FCUnpublish/deleteStream to the dead connection | match |
| Q-11 | QUIRK | PublisherAuth failure branches keep the connection open | match |
| Q-12 | QUIRK | `rtmp://[::1]` without slash: host `[`, port 0 (colon search starts at the host start) | match |
| Q-13 | QUIRK | `Publish.Start` of any stream ends `ConnectStream` (multi-stream) | match |
| D-1 | DEFECT | `ParsePlaypath` reads 4 bytes before the input when `?` is within the first 4 bytes of an input >= 4 bytes (`ab?cdef`, `?other=1`) | no OOB: treat as "no extension" |
| D-2 | DEFECT | `%` not followed by two hex digits stores an uninitialised value | emit `%` literally and copy |
| D-3 | DEFECT | `AddStream` with 8 streams already present overflows | return -1 |
| D-4 | DEFECT | Window-Ack-Size / Peer-Bandwidth bodies < 4 bytes read past the body | ignore the message |
| D-5 | DEFECT | signed left shifts (`ext<<24` in `RTMP_Write`, TEA word build) | use unsigned 32-bit arithmetic (results identical on two's complement) |
| D-6 | DEFECT | `app`/`tcUrl` after PublisherAuth are not NUL-terminated and later passed to `strstr` | use lengths |

Vectors: `04`/`05` (D-1, D-2 `ignore`), `06` (D-3), `27`/`28` (D-4), `16`/`36` (D-5).

## 9. Implementation notes

- Model `Packet {fmt, csid, type, ts, abs, info2, size, read, body}`, `Channels{out: Vec<Option<Prev>>, in: Vec<Option<Stored>>}`.
- Do the I/O behind a trait (`read(&mut [u8]) -> Result<usize, Err::Timeout|Eof|Io>`, `write`). The conformance runner drives it with queued bytes.
- Use a real SHA-256/HMAC/MD5/base64 crate; the vectors check the exact digests.
- URL slices must use lengths, not NUL terminators.
- The decoder in the runner is the contract for output: see README. An implementation's own writer is free in write granularity (the reference batches header+first chunk).

## 10. Conformance coverage

| Rules | Scripts |
|---|---|
| R01..R10 | 01-parseurl, 02-parseurl-limits |
| R11..R16 | 03-setupurl |
| R17..R24, D-1, D-2 | 04-playpath, 05-playpath-defects |
| R23..R25, D-3 | 06-addstream |
| R26 | (absence; no script) |
| R27..R34 | 30-handshake-plain, 10-publish-flow, 38-send-errors |
| R35..R39 | 31-handshake-digest |
| R40..R43 | 10-publish-flow, 14-result-matching |
| R44..R47 | 13-bwcheck-ping |
| R48..R53 | 11-publish-rejected, 12-status-codes, 15-error-nonauth |
| R54..R62 | 19-sendpacket, 16-write-tags, 17-write-chunking |
| R63..R69 | 16-write-tags, 18-write-edge, 38-send-errors |
| R70..R78 | 20-read-headers, 21-read-chunks, 22-read-extended-ts, 23-read-ext-continuation-quirk |
| R79..R83 | 24-ack-accounting, 27-bandwidth-messages |
| R84..R93 | 25-control-messages, 26-drain-no-clientpacket, 27, 28 (D-4) |
| R94..R103 | 32-auth-adobe, 33-auth-llnw, 34-auth-failures |
| R104..R113 | 35-play-mode, 36-secure-token |
| R114..R129 | 37-close-eof-timeout, 38-send-errors |
| R130..R136 | 39-direct-sends |

## 11. Open questions

- O-1: RTMPT/HTTP tunnelling, SOCKS4 and RTMPS are not covered by vectors.
- O-2: S0 = 8/9 signature scrambling in digest mode (needs the XTEA/Blowfish key tables).
- O-3: `customConnectEncode` hook is never set by OBS; its bytes are not specified.
- O-4: Multi-stream publishing (`nStreams > 1`) is implemented by the reference but OBS adds exactly one stream; only the single-stream sequence is vectored.
