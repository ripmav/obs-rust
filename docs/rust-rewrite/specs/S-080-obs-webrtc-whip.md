# S-080 obs-webrtc / WHIP output

| | |
|---|---|
| Status | Draft (round 9 — behaviour catalog written from C reference; no vectors yet) |
| Spec version | initial |
| Reference | OBS Studio 33.0.1, `plugins/obs-webRTC/` (WHIP subset) |
| Target crate | `crates/obs-webrtc` |
| Depends on | S-031 (RTMP protocol — the transport patterns), S-041 (output object) |
| Conformance | planned `test/conformance/scripts/whip/` — none yet |

## 1. Scope

The WHIP (Web-Initiated HTTP Streaming) output: the service (URL + optional bearer
token), the output (WebRTC peer connection driven over a libcurl HTTP session), SDP
offer/answer, the optional bearer-token auth header, and the `Location` (resource URL)
and `Link` (ICE servers) header parsing.
The full WebRTC stack (libdatachannel) stays a native dependency; this spec pins the
plugin's contract around it.

### Reference files (all cited)

- `plugins/obs-webrtc/obs-webrtc.cpp`
- `plugins/obs-webrtc/whip-service.cpp`
- `plugins/obs-webrtc/whip-output.cpp`
- `plugins/obs-webrtc/whip-utils.h`

## 2. Conventions

RFC 2119. The output is an `obs_output_t`; the service is an `obs_service_t` (id
`whip_custom`, protocol `"WHIP"`, output type `"whip_output"`) carrying the server URL
and an optional bearer token. The WebRTC layer (`rtc::PeerConnection`) is a kept native
dependency — the spec pins the call sequence and the HTTP contract, not the WebRTC
internals.

## 3. Service (`whip-service.cpp`)

- **S-080-R01** `register_whip_service`: registers the WHIP service type (id
  `whip_custom`, protocol `"WHIP"`, output type `"whip_output"`; supported codecs: video
  `h264`/`hevc`/`av1`, audio `opus`). `Properties`: exactly two text fields — `server`
  (URL) and `bearer_token` (password). `Update`: re-reads both into the service state;
  `GetConnectInfo` exposes `SERVER_URL` and `BEARER_TOKEN`; `CanTryToConnect` = non-empty
  server.
- **S-080-R02** `ApplyEncoderSettings(video_settings, audio_settings)`: for
  "maximum compatibility with webrtc peers" it sets `bf = 0` (no B-frames) and
  `repeat_headers = true` on the video settings; the audio settings are left untouched.
  It does not create or select encoders (those come from the output settings).

## 4. Output (`whip-output.cpp`)

- **S-080-R03** Lifecycle: `WHIPOutput` (ctor) / `~WHIPOutput` (dtor) wrap the libcurl easy
  handle + the peer connection; `register_whip_output` registers the output. `Start` /
  `Stop` drive the sequence below; `Data` is the obs data callback feeding audio/video
  packets into the media tracks.
- **S-080-R04** Start sequence (on the start/stop thread): `Init` (read the service URL
  + bearer token via `GetConnectInfo`; empty URL ⇒ `OBS_OUTPUT_BAD_PATH`) → `Setup`
  (create the `rtc::PeerConnection` with state-change callbacks — `Disconnected`/
  `Failed` stop the output — configure the media tracks: `ConfigureAudioTrack` /
  `ConfigureVideoTrack`, then `setLocalDescription()` creates the **offer SDP**) →
  `Connect` (HTTP **POST** of the offer: `CURLOPT_POST` +
  `CURLOPT_COPYPOSTFIELDS(offer)`, headers `Content-Type: application/sdp`, optional
  `Authorization: Bearer <token>`, the user agent; `FOLLOWLOCATION` +
  `UNRESTRICTED_AUTH` enabled, 8 s timeout) to the service URL.
- **S-080-R05** Response handling: the POST must return **exactly 201** — any other code
  fails the connect (`OBS_OUTPUT_INVALID_STREAM` / connect failed). There is **no
  401/`WWW-Authenticate` challenge flow** in 33.0.1; authentication is the optional
  bearer-token header (R04) followed across redirects by `UNRESTRICTED_AUTH`. The
  **resource URL** comes from the `Location` header (a relative `Location` is resolved
  against the effective URL with `curl_url`; missing ⇒ error). The `Link` header values
  (multiple, comma-separated) are parsed by `ParseLinkHeader` into **ICE servers
  (STUN/TURN)** and used for `gatherLocalCandidates` after the answer is applied.
- **S-080-R06** Success path: the 201 response body is the **answer SDP** — trimmed to
  start at `"v=0"` — applied as `rtc::Description answer(response, "answer")` via
  `setRemoteDescription` (invalid SDP ⇒ `Error.InvalidSDP` last-error + fail). With more
  than one video layer (simulcast), `simulcast_layers_in_answer` (whip-utils: number of
  `;`-separated groups in the `a=simulcast` line) must equal the layer count, else the
  connect fails with `Error.SimulcastLayersRejected` — layers are not silently reduced.
- **S-080-R07** Running: `StartThread`/`StopThread` own the media-send loop; `Send`
  pushes a packet to the track. On stop/error, `SendDelete` issues the WHIP DELETE
  (session teardown) to the resource URL before the connection is torn down.

## 5. Utilities (`whip-utils.h`)

- **S-080-R08** `generate_random_u32` (mt19937 seeded from `random_device`, range
  `1..UINT32_MAX-1`), `trim_string` (strips ` \n\r\t` from both ends — each collected
  header line is trimmed), `value_for_header(header, line)` (case-insensitive prefix
  match of `header` against a full header line; returns the substring after the first
  space, or "" if not present), `curl_writefunction` /
  `curl_header_function` (the libcurl response body / per-line header collectors),
  `generate_user_agent` (`"User-Agent: Mozilla/5.0 (OBS-Studio/<version>; <os>;
  <locale>)"`, sent with every WHIP request), `simulcast_layers_in_answer` (R06).

## 6. Concurrency

The media-send thread (R07) runs independently of the obs output thread; the curl handle
is used during connect (R04–R06) and again for the final `SendDelete` (R07); between
connects the peer connection is the sole I/O owner. Stop joins the thread before freeing.

## 7. Open questions

1. (resolved) There is no post-401 credential flow in 33.0.1 — authentication is the
   optional `Authorization: Bearer` header from the service (R01/R04/R05).
2. Retries/backoff on transport failure (the reference's reconnect policy is not pinned —
   treat as: the output stops with an error, the user restarts).

Round 10 second-author check (2026-11-05): corrected R01 (properties are `server` +
`bearer_token` only — no encoder settings; added id/protocol/codec facts), R02
(`ApplyEncoderSettings` only sets `bf=0` + `repeat_headers=true` on video), R04 (the
offer is created in `Setup` via `setLocalDescription`, not in `Connect`; added header
set + `FOLLOWLOCATION`/`UNRESTRICTED_AUTH`), R05 (the 401/`WWW-Authenticate` challenge
description was wrong — 33.0.1 has no 401 retry; success requires exactly 201, the
resource URL comes from `Location` (relative resolved via `curl_url`), `Link` headers
carry ICE servers for `gatherLocalCandidates`), R06 (201-only, body trimmed at
`"v=0"`, simulcast mismatch is a hard `Error.SimulcastLayersRejected` failure, not layer
reduction) and R08 (exact helper semantics). OQ1 resolved.
