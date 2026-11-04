# S-062 obs-websocket plugin (RPC over WebSocket, protocol 5.x, vendor API v3)

| | |
|---|---|
| Status | Draft (round 8 — behavioural catalog written from C reference; no vectors yet) |
| Spec version | initial |
| Reference | OBS Studio 33.0.1, plugin `obs-websocket` (5.x protocol implementation) |
| Target crate | `crates/obs-websocket` |
| Depends on | S-002 (calldata / proc handlers — the vendor C API rides on the global proc handler), S-014 (obs_data JSON shapes of `requestData`/`eventData`), S-048 (frontend events this plugin mirrors) |
| Conformance | planned `test/conformance/scripts/wsproto/` — none yet |

## 1. Scope

A local WebSocket RPC server embedded in OBS: clients `Identify` (optionally authenticate),
send typed `Request`s (or `RequestBatch`), and receive server-pushed `Event`s filtered by a
per-session subscription bitmask. The plugin also exposes a C **vendor API** (version 3)
(`obs_websocket-api.h`) that third-party plugins use to register request handlers and event
callbacks through the global obs proc handler.

In scope: wire protocol, identify/authenticate, request dispatch and status model, event
broadcast, server lifecycle and settings persistence, the vendor C API surface.
Out of scope (described only): the Qt settings dialog and connect-info UI (forms/*),
tray-notification text, per-request parameter schemas of every 5.x request type (the request
handlers map 1:1 to the published 5.x protocol; §6 lists what each handler file covers,
parameter schemas are open questions Q4).

### Reference files (all cited; behaviour sections below name each file)

- `plugins/obs-websocket/lib/obs-websocket-api.h`, `plugins/obs-websocket/lib/example/simplest-plugin.c`
- `plugins/obs-websocket/src/obs-websocket.cpp`, `plugins/obs-websocket/src/WebSocketApi.cpp`, `plugins/obs-websocket/src/WebSocketApi.h`, `plugins/obs-websocket/src/Config.cpp`
- `plugins/obs-websocket/src/eventhandler/EventHandler.cpp`, `plugins/obs-websocket/src/eventhandler/EventHandler_Inputs.cpp`, `plugins/obs-websocket/src/eventhandler/EventHandler_Outputs.cpp`, `plugins/obs-websocket/src/eventhandler/EventHandler_SceneItems.cpp`, `plugins/obs-websocket/src/eventhandler/EventHandler_Config.cpp`, `plugins/obs-websocket/src/eventhandler/EventHandler_Scenes.cpp`, `plugins/obs-websocket/src/eventhandler/EventHandler_Transitions.cpp`, `plugins/obs-websocket/src/eventhandler/EventHandler_Filters.cpp`, `plugins/obs-websocket/src/eventhandler/EventHandler_MediaInputs.cpp`, `plugins/obs-websocket/src/eventhandler/EventHandler_Canvases.cpp`, `plugins/obs-websocket/src/eventhandler/EventHandler_Ui.cpp`
- `plugins/obs-websocket/src/forms/SettingsDialog.cpp`, `plugins/obs-websocket/src/forms/ConnectInfo.cpp`
- `plugins/obs-websocket/src/requesthandler/RequestHandler.cpp`, `plugins/obs-websocket/src/requesthandler/RequestBatchHandler.cpp`, `plugins/obs-websocket/src/requesthandler/RequestHandler_Inputs.cpp`, `plugins/obs-websocket/src/requesthandler/RequestHandler_Outputs.cpp`, `plugins/obs-websocket/src/requesthandler/RequestHandler_SceneItems.cpp`, `plugins/obs-websocket/src/requesthandler/RequestHandler_Config.cpp`, `plugins/obs-websocket/src/requesthandler/RequestHandler_Scenes.cpp`, `plugins/obs-websocket/src/requesthandler/RequestHandler_Sources.cpp`, `plugins/obs-websocket/src/requesthandler/RequestHandler_Filters.cpp`, `plugins/obs-websocket/src/requesthandler/RequestHandler_General.cpp`, `plugins/obs-websocket/src/requesthandler/RequestHandler_Ui.cpp`, `plugins/obs-websocket/src/requesthandler/RequestHandler_Record.cpp`, `plugins/obs-websocket/src/requesthandler/RequestHandler_Transitions.cpp`, `plugins/obs-websocket/src/requesthandler/RequestHandler_MediaInputs.cpp`, `plugins/obs-websocket/src/requesthandler/RequestHandler_Stream.cpp`, `plugins/obs-websocket/src/requesthandler/RequestHandler_Canvases.cpp`, `plugins/obs-websocket/src/requesthandler/rpc/Request.cpp`, `plugins/obs-websocket/src/requesthandler/rpc/RequestBatchRequest.cpp`, `plugins/obs-websocket/src/requesthandler/rpc/RequestResult.cpp`
- `plugins/obs-websocket/src/utils/Obs_ArrayHelper.cpp`, `plugins/obs-websocket/src/utils/Obs_VolumeMeter.cpp`, `plugins/obs-websocket/src/utils/Obs_VolumeMeter_Helpers.h`, `plugins/obs-websocket/src/utils/Json.cpp`, `plugins/obs-websocket/src/utils/Obs_StringHelper.cpp`, `plugins/obs-websocket/src/utils/Crypto.cpp`, `plugins/obs-websocket/src/utils/Obs_NumberHelper.cpp`, `plugins/obs-websocket/src/utils/Platform.cpp`, `plugins/obs-websocket/src/utils/Obs_ActionHelper.cpp`, `plugins/obs-websocket/src/utils/Obs_ObjectHelper.cpp`, `plugins/obs-websocket/src/utils/Obs_SearchHelper.cpp`, `plugins/obs-websocket/src/utils/Obs.h`
- `plugins/obs-websocket/src/websocketserver/WebSocketServer.cpp`, `plugins/obs-websocket/src/websocketserver/WebSocketServer_Protocol.cpp`, `plugins/obs-websocket/src/websocketserver/rpc/WebSocketSession.h`

## 2. Conventions

RFC 2119. Every wire message is a single JSON object (MsgPack encoding is also
compiled in, `WebSocketEncoding { Json, MsgPack }`): `{"op": <opcode string>, "d": <object>}`.
`op` values are the identifier names of the opcode enum; `d` is the payload object.

## 3. Wire protocol

### 3.1 Opcodes (`WebSocketOpCode.h`)

| op | value | direction |
|---|---|---|
| `Hello` | 0 | server → client (first message on connect) |
| `Identify` | 1 | client → server |
| `Identified` | 2 | server → client |
| `Reidentify` | 3 | client → server |
| `Event` | 5 | server → client |
| `Request` | 6 | client → server |
| `RequestResponse` | 7 | server → client |
| `RequestBatch` | 8 | client → server |
| `RequestBatchResponse` | 9 | server → client |
| `Disconnect` | 10 (Q5) | server → client (final message, with close) |

S-062-R01: On connect the server sends `Hello`; the session is *not identified* until it
receives a valid `Identify` (or the client disconnects).
S-062-R02: A non-`Identify` message from a non-identified session closes the connection with
code 4007 `NotIdentified` ("You attempted to send a non-Identify message while not identified.").
S-062-R03: A payload whose `d` is missing/null closes with 4003 `MissingDataField`; a `d`
that is not an object closes with 4004 `InvalidDataFieldType`.

### 3.2 Identify

`d` fields: `rpcVersion` (unsigned, MUST be `CURRENT_RPC_VERSION` = 1, else close 4010
`UnsupportedRpcVersion`); `eventSubscriptions` (unsigned u64 bitmask, else 4004);
`authentication` (string; only inspected when authentication is required — missing or
wrong type → close 4009 `AuthenticationFailed`).
S-062-R04: Successful identify marks the session identified, applies the subscription mask,
and replies `Identified` with `d.negotiatedRpcVersion = 1`. A second `Identify` closes with
4008 `AlreadyIdentified`.
S-062-R05: `Reidentify` MUST NOT re-authenticate; it only swaps the `eventSubscriptions`
mask (unsubscribe old → subscribe new, announced to the subscription callback) and replies
`Identified` with the negotiated version.

Event subscription mask (`EventSubscription`): `None=0, General=1<<0, Config=1<<1,
Scenes=1<<2, Inputs=1<<3, Transitions=1<<4, Filters=1<<5, Outputs=1<<6, SceneItems=1<<7,
MediaInputs=1<<8, Vendors=1<<9, Ui=1<<10, Canvases=1<<11, All=0xFFF,
InputVolumeMeters=1<<16, InputActiveStateChanged=1<<17, InputShowStateChanged=1<<18,
SceneItemTransformChanged=1<<19`. A session defaults to `All` (bits 0–11).

### 3.3 Requests and responses

S-062-R06: A `Request` MUST carry `requestId` (missing → 4003) and `requestType`
(string; non-string → 4004). The response is `RequestResponse` with `d.requestType`,
`d.requestId`, `d.requestStatus = {result: bool, code: int, comment?: string}` and
`d.responseData` (object, present only when the handler produced one).
S-062-R07: If OBS is not yet ready (`_obsReady` false) the request is answered without
dispatch: status `NotReady`, comment "OBS is not ready to perform the request."
S-062-R08: `RequestBatch` carries `requestId` plus a batch of `RequestBatchRequest`s;
`executionType` (optional, unsigned, `None` rejected with 4005 `InvalidDataFieldValue`)
selects serial-realtime / serial-as-fast-as-possible / parallel execution; `Parallel`
MUST fail when the worker thread pool has fewer than 2 threads (deadlock guard).

### 3.4 Close codes (`WebSocketCloseCode`)

`DontClose=0, UnknownReason=4000, MessageDecodeError=4002, MissingDataField=4003,
InvalidDataFieldType=4004, InvalidDataFieldValue=4005, UnknownOpCode=4006,
NotIdentified=4007, AlreadyIdentified=4008, AuthenticationFailed=4009,
UnsupportedRpcVersion=4010, SessionInvalidated=4011, UnsupportedFeature=4012`
— always sent with a human-readable `closeReason`.

### 3.5 Authentication (challenge/secret, `Crypto.cpp`)

- Server secret = `base64( SHA256(password ‖ salt) )`; salt = base64 of 32 random bytes.
- Per session a `challenge` is generated; the client answers `authentication =
  base64( SHA256(secret ‖ challenge) )`; comparison is exact-string (S-062-R09).
- S-062-R10: A failed authentication closes 4009 and, when alerts are enabled, shows a
  tray notification including the client's remote address.

### 3.6 Events

S-062-R11: `BroadcastEvent(requiredIntent, eventType, eventData, rpcVersion)` sends
`{op: Event, d: {eventName, eventData}}` to every session whose subscription mask covers
`requiredIntent` (and whose negotiated rpcVersion admits the event). Events are produced
from obs/frontend callbacks by the `EventHandler_*.cpp` files (§6).

## 4. Vendor C API (`lib/obs-websocket-api.h`, `OBS_WEBSOCKET_API_VERSION 3`)

- `obs_websocket_get_api_version()` → 3, or 0 when the plugin is not loaded (proc handler
  `obs_websocket_api_get_ph` missing). All calls are dispatched through the global obs
  proc handler; NULL/empty arguments fail closed (S-062-R12).
- Request handlers: third parties register `obs_websocket_request_callback_function
  (obs_data_t *request, obs_data_t *response, void *priv)` per request type; the plugin
  routes matching `Request`s to registered vendor handlers before/alongside built-ins.
- Event handlers: `obs_websocket_event_callback_function (uint64_t seqNum, const char
  *eventName, const char *eventData, void *priv)`.
- `obs_websocket_request_response {unsigned int status_code; char *comment; char
  *response_data;}` — `response_data` is a **JSON string** (not obs_data) because the
  C API cannot pass `array<object>` (QUIRK Q1, kept in the conforming implementation).
- `lib/example/simplest-plugin.c` is the reference usage (register one handler, one event
  callback) — behaviour: load → register, unload → unregister.

## 5. Server lifecycle and settings (`obs-websocket.cpp`, `WebSocketServer.cpp`, `Config.cpp`)

- S-062-R13: Settings persist as config values `FirstLoad / ServerEnabled / ServerPort /
  AlertsEnabled / AuthRequired / ServerPassword`, mirrored in the JSON API as
  `first_load / server_enabled / server_port / alerts_enabled / auth_required /
  server_password` (booleans except port = unsigned, password = string). Missing keys keep
  defaults (Q3).
- S-062-R14: `WebSocketServer::Start()` binds the port and enables the listener; `Stop()`
  drops all sessions. Sessions track `remoteAddress, connectedAt, incoming/outgoing
  message counts, isIdentified` (observable via the session-state API).
- `forms/SettingsDialog.cpp` + `forms/ConnectInfo.cpp`: the Qt settings dialog (port/auth
  toggles, regenerate-password button) and the client-facing connect info (address,
  challenge, secret on first load); UI-only, behaviour = read/write §5 settings, no
  protocol effects beyond that.

## 6. Request dispatch and per-domain handlers

`RequestHandler.cpp` holds the requestType → handler table and the error model (unknown
request type → error status with a comment; handler exceptions never crash the server —
S-062-R15). `Request.cpp` / `RequestResult.cpp` / `RequestBatchRequest.cpp` are the RPC
value types (construction, status, response payload). Per-domain handler files and their
scope (each maps its 5.x request types to obs/frontend calls, serialising results to
`obs_data`):

| file | request domain |
|---|---|
| `RequestHandler_General.cpp` | version/heartbeat/OS info, recording + streaming state |
| `RequestHandler_Config.cpp` | config file / frontend API access (largest handler) |
| `RequestHandler_Scenes.cpp`, `RequestHandler_SceneItems.cpp`, `RequestHandler_Sources.cpp` | scene graph, item transforms, source CRUD + signals |
| `RequestHandler_Filters.cpp` | filter CRUD on sources |
| `RequestHandler_Outputs.cpp`, `RequestHandler_Stream.cpp`, `RequestHandler_Record.cpp` | output CRUD/start/stop, replay buffer, stream/record state |
| `RequestHandler_Transitions.cpp` | list/set/trigger transitions |
| `RequestHandler_MediaInputs.cpp` | media input control (play/pause/seek/stop) |
| `RequestHandler_Ui.cpp` | UI control (frontmost, preview selection) |
| `RequestHandler_Canvases.cpp` | canvas source list/set |
| `RequestHandler_Inputs.cpp` | input source helpers |

Event handlers (`EventHandler.cpp` central dispatcher + one file per domain) mirror the
same domains to `Event` pushes: `EventHandler_Inputs` (input create/remove/rename,
volume/active/show state per the 1<<16..18 bits), `EventHandler_SceneItems` (incl.
`SceneItemTransformChanged` 1<<19), `EventHandler_Scenes`, `EventHandler_Filters`,
`EventHandler_Outputs`, `EventHandler_Transitions`, `EventHandler_MediaInputs`,
`EventHandler_Config`, `EventHandler_Canvases`, `EventHandler_Ui`.

## 7. Utility layer

| file | behaviour |
|---|---|
| `utils/Obs_ArrayHelper.cpp` | obs_data array primitives used by handlers: first/last/nth, index-of, filter/sort/reverse/copy, append |
| `utils/Obs_VolumeMeter.cpp` + `Obs_VolumeMeter_Helpers.h` | per-input volume metering feeding the `InputVolumeMeters` events (peak/RMS over a window) |
| `utils/Obs_ObjectHelper.cpp`, `Obs_SearchHelper.cpp` | obs_data/obs_source lookup + JSON object search helpers |
| `utils/Obs_StringHelper.cpp`, `Obs_NumberHelper.cpp`, `Json.cpp` | string/number conversions and JSON ↔ obs_data bridging |
| `utils/Obs_ActionHelper.cpp` | action (hotkey) name ↔ id helpers |
| `utils/Crypto.cpp` | §3.5 primitives (salt/secret/auth-string, random password generator) |
| `utils/Platform.cpp` | OS-specific glue: tray notifications (SendTrayNotification), platform paths |
| `utils/Obs.h` | shared obs include wrapper for the utils |

## 8. Concurrency

Per-session `OperationMutex` guards identify/reidentify/subscription changes;
`BroadcastEvent` may be called from obs callback threads and is delivered under each
session's lock; batch parallel execution uses the plugin's QThreadPool (S-062-R08);
volume-meter events are rate-limited per input (Q2).

## 9. Quirks and defects

- Q1: vendor API `response_data` is a JSON string by API design (obs_data cannot hold
  `array<object>`); the conforming crate keeps the same shape.
- Q2: volume-meter event emission rate/window — window constants not re-derived this round.
- Q3: `Config.cpp` default values (port default, `ServerEnabled` default on first load).
- Q4: full per-request parameter schemas for every 5.x request type (deferred: they are
  the published 5.x protocol; handler files above are the authoritative list of covered
  domains).
- Q5: `Disconnect` opcode wire value (10 in the published 5.x protocol; confirm against
  `WebSocketOpCode.h`).
- Q6: exact `requestId` type validation beyond presence; `RequestStatus` numeric values.
