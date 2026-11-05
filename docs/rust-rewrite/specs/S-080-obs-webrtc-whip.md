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

The WHIP (Web-Initiated HTTP Streaming) output: the service (URL + encoder settings), the
output (WebRTC peer connection driven over a libcurl HTTP session), SDP offer/answer, the
HTTP 401/`WWW-Authenticate` challenge flow, and the `Link` header (resource URL) parsing.
The full WebRTC stack (libdatachannel) stays a native dependency; this spec pins the
plugin's contract around it.

### Reference files (all cited)

- `plugins/obs-webrtc/obs-webrtc.cpp`
- `plugins/obs-webrtc/whip-service.cpp`
- `plugins/obs-webrtc/whip-output.cpp`
- `plugins/obs-webrtc/whip-utils.h`

## 2. Conventions

RFC 2119. The output is an `obs_output_t` (`WHIP_OUTPUT`); the service is an
`obs_service_t` carrying the server URL. The WebRTC layer (`rtc::PeerConnection`) is a
kept native dependency — the spec pins the call sequence and the HTTP contract, not the
WebRTC internals.

## 3. Service (`whip-service.cpp`)

- **S-080-R01** `register_whip_service`: registers the WHIP service type. `Properties`:
  the service settings (server URL, and the encoder settings the output will apply).
  `Update`: re-read the settings into the service state.
- **S-080-R02** `ApplyEncoderSettings`: the WHIP output applies its encoder configuration
  (video/audio encoder + their settings) to the stream's encoders when it starts — the
  mapping from service settings to encoder creation is pinned here; the encoder behaviour
  itself is the encoders' specs.

## 4. Output (`whip-output.cpp`)

- **S-080-R03** Lifecycle: `WHIPOutput` (ctor) / `~WHIPOutput` (dtor) wrap the libcurl easy
  handle + the peer connection; `register_whip_output` registers the output. `Start` /
  `Stop` drive the sequence below; `Data` is the obs data callback feeding audio/video
  packets into the media tracks.
- **S-080-R04** Start sequence: `Init` (build the peer connection from the service URL +
  settings) → `Setup` (configure the media tracks: `ConfigureAudioTrack` /
  `ConfigureVideoTrack` from the encoder formats) → `Connect` (create the local
  description = **offer SDP**) → HTTP **POST** of the offer (`CURLOPT_POST` + the offer
  body) to the server URL.
- **S-080-R05** Auth challenge: the server answers `401` with a `WWW-Authenticate` header
  (the challenge). The output retries the POST with the credential; `ParseLinkHeader`
  extracts the `Link` header values (multiple, comma-separated) and picks the WHIP
  resource URL the session posts to. A non-401 non-2xx response fails the connection
  (output error + stop).
- **S-080-R06** Success path: the 2xx response body is the **answer SDP**
  (`rtc::Description answer(response, "answer")`), applied via `setRemoteDescription`.
  `simulcast_layers_in_answer` (whip-utils) inspects the answer for the accepted
  simulcast layers (the output then enables only those layers).
- **S-080-R07** Running: `StartThread`/`StopThread` own the media-send loop; `Send`
  pushes a packet to the track. On stop/error, `SendDelete` issues the WHIP DELETE
  (session teardown) to the resource URL before the connection is torn down.

## 5. Utilities (`whip-utils.h`)

- **S-080-R08** `generate_random_u32` (SDP/ICE randomness), `trim_string` (header values),
  `value_for_header` (case-insensitive header lookup), `curl_writefunction` /
  `curl_header_function` (the libcurl response body/header collectors),
  `generate_user_agent` (the `User-Agent` string sent with the WHIP requests),
  `simulcast_layers_in_answer` (R06).

## 6. Concurrency

The media-send thread (R07) runs independently of the obs output thread; the curl handle
is used only during connect (R04–R06), then the peer connection is the sole I/O owner.
Stop joins the thread before freeing.

## 7. Open questions

1. The exact credential format sent after the 401 (the reference follows the WHIP spec's
   challenge grammar; the concrete string construction is not re-derived).
2. Retries/backoff on transport failure (the reference's reconnect policy is not pinned —
   treat as: the output stops with an error, the user restarts).
