# S-073 Frontend: output pipeline UI (simple/advanced output, multitrack, remux)

| | |
|---|---|
| Status | Draft (round 8 — behaviour catalog written from C reference; no vectors yet) |
| Spec version | initial |
| Reference | OBS Studio 33.0.1 (`frontend/utility/`, `frontend/components/`, `frontend/dialogs/`) |
| Target crate | `crates/frontend` (output-handler module) |
| Depends on | S-046 (obs-ffmpeg outputs: `ffmpeg_muxer`/`mp4_output`/`mov_output`/`replay_buffer`), S-041 (output delay), S-035 (services), S-077 (frontend API, `OBSWeakSource`-style refs) |
| Conformance | planned, none yet |

## 1. Scope

In scope: everything between the Output dialog and `obs_output_start` — the `BasicOutputHandler`
base, the `SimpleOutput` and `AdvancedOutput` strategies, multitrack (enhanced broadcasting)
output + WHIP simulcast encoders, the auto-remux queue + worker, media-source playback controls,
and the ffmpeg codec/format helper tables. Out of scope: the encoders/muxers themselves
(separate specs), the Output *dialog* widgets (S-068 settings), GoLive API HTTP details
(S-074 owns `GoLiveAPI_*` files).

### Reference files (26, full paths relative to `obs-studio/`)

- `frontend/utility/BasicOutputHandler.cpp` — shared base: signals, `GetStreamOutputType`, auto-remux, multitrack bootstrap
- `frontend/utility/BasicOutputHandler.hpp` — base class contract, `OBSBasic` handler accessors
- `frontend/utility/SimpleOutput.cpp` — simple output strategy (encoder presets, record/stream start)
- `frontend/utility/AdvancedOutput.cpp` — advanced output strategy (JSON encoder config, service settings)
- `frontend/utility/MultitrackVideoOutput.cpp` — enhanced-broadcasting (multitrack) stream output
- `frontend/utility/MultitrackVideoOutput.hpp` — `MultitrackVideoOutput` contract, `OBSOutputObjects`
- `frontend/utility/MultitrackVideoError.cpp` — multitrack error dialog (`MultitrackVideoError`)
- `frontend/utility/WHIPSimulcastEncoders.hpp` — WHIP simulcast layer encoders
- `frontend/utility/StartMultiTrackVideoStreamingGuard.hpp` — promise-based start guard
- `frontend/widgets/OBSBasic_OutputHandler.cpp` — `OBSBasic::ResetOutputs`, output-path helpers
- `frontend/components/MediaControls.cpp` — media source playback bar
- `frontend/components/SilentUpdateCheckBox.hpp` — signal-silent `setChecked`
- `frontend/components/SilentUpdateSpinBox.hpp` — signal-silent `setValue`
- `frontend/utility/audio-encoders.cpp` — audio bitrate enumeration + closest-bitrate selection
- `frontend/utility/FFmpegCodec.cpp` — codec enumeration, codec↔format compatibility
- `frontend/utility/FFmpegCodec.hpp` — `FFmpegCodec` value + `FFmpegCodecType`
- `frontend/utility/FFmpegFormat.cpp` — muxer enumeration, default-encoder lookup
- `frontend/utility/FFmpegFormat.hpp` — `FFmpegFormat` value
- `frontend/utility/FFmpegShared.hpp` — `strequal` NULL/empty-tolerant comparator
- `frontend/utility/QuickTransition.cpp` — transition hotkey label refresh
- `frontend/utility/QuickTransition.hpp` — `QuickTransition` value (source, duration, fade-to-black)
- `frontend/utility/models/multitrack-video.hpp` — multitrack dialog value model
- `frontend/utility/RemuxQueueModel.cpp` — remux queue model + entry states
- `frontend/utility/RemuxWorker.cpp` — `media_remux_job` worker thread
- `frontend/utility/RemuxEntryPathItemDelegate.cpp` — per-cell path editor (browse/clear)
- `frontend/dialogs/OBSRemux.cpp` — remux queue dialog (manual + auto-remux mode)

## 2. Conventions

RFC 2119 words. Config keys are `basic.ini` (profile) names; `[SimpleOutput]`/`[AdvOut]`/
`[Stream1]`/`[Output]`/`[Video]` are the sections. "The handler" = the active
`BasicOutputHandler` instance. Output *signals* are the libobs output signals
(`start`/`stop`/`starting`/`stopping`/`saved`/`file_changed`). W = Windows-only keys.

## 3. Behaviour

### 3.1 Shared handler base (`BasicOutputHandler.*`, `OBSBasic_OutputHandler.cpp`)

| # | component/file | trigger | behaviour | state changed |
|---|---|---|---|---|
| S-073-R01 | `OBSBasic_OutputHandler.cpp` | `OBSBasic::ResetOutputs` | Rebuild the handler **only if** `(!outputHandler \|\| !Active())` **and** the `setupStreamingGuard` is invalid or already ready (`wait_for(0 s) == ready`); mode from `[Output] Mode` (case-insensitive `"Advanced"` → `CreateAdvancedOutputHandler`, else simple). After rebuild: emit `ReplayBufEnabled`, update sys-tray replay-buffer enable, `UpdateIsRecordingPausable`. Otherwise: `outputHandler->Update()` (in-place reconfigure). | `outputHandler` |
| S-073-R02 | `BasicOutputHandler.cpp` | ctor | If `main->vcamEnabled` → create `virtualcam` output, connect `start`/`stop`/`deactivate` signals. `multitrackVideo` created iff `[Stream1] EnableMultitrackVideo` **and** (service settings have a user value `multitrack_video_configuration_url` **or** service id == `rtmp_custom`). `whipSimulcastEncoders` created iff `[Stream1] WHIPSimulcastTotalLayers > 1`. | handler fields |
| S-073-R03 | `BasicOutputHandler.cpp` | `GetStreamOutputType(service)` | No protocol → `LOG_WARNING "The service '%s' has no protocol set"` + `nullptr`. Protocol not registered → `LOG_WARNING "The protocol '%s' is not registered"` + `nullptr`. Preferred output type: used iff it exists and has `OBS_OUTPUT_SERVICE` flag (else `LOG_WARNING "The output '%s' is not registered, fallback to another one"`). Otherwise first-party order: `rtmp_output` (protocols RTMP/RTMPS) → `ffmpeg_hls_muxer` (HLS) → `ffmpeg_mpegts_muxer` (SRT/RIST). Else first type from `obs_enum_output_types_with_protocol`. Else `LOG_WARNING "No output compatible with the service '%s' is registered"` + `nullptr`. | — |
| S-073-R04 | `BasicOutputHandler.cpp` | `SetupAutoRemux` | `[Video] AutoRemux` (bool) in effect: an `mp4` recording is delivered as `mp4` but after stop the file is queued for background remux to `mkv` (container pointer swapped `mp4` → `mkv` for the remuxed copy); the *original* `lastRecordingPath` stays the mp4 path until the remuxed file is written (R15 `file_changed`). | remux queue |
| S-073-R05 | `BasicOutputHandler.cpp` | `file_changed` signal on recording output | Queued to UI thread: `lastRecordingPath` swapped to the new file, `recordingFileChanged` emitted with old+new paths. | `lastRecordingPath` |
| S-073-R06 | `BasicOutputHandler.cpp` | `StartVirtualCam` / `StopVirtualCam` | Start: requires `vcamEnabled`; non-program mode creates an `obs_view` (lazy), `UpdateVirtualCamOutputSource()` picks the active view/scene; video = `obs_get_video()` for program mode else `obs_view_add(view)`. Output updated with view settings, started; `deactivate` signal tears the view down but keeps the output for restart. Stop: `obs_output_stop`. | vcam output |
| S-073-R07 | `StartMultiTrackVideoStreamingGuard.hpp` | guard lifetime | RAII `std::promise<void>`: dtor sets the value; `GetFuture()` returns a shared future. `MakeReadyFuture()` returns an already-ready shared future (used when streaming setup fails *before* blocking work — R25). ResetOutputs (R01) waits on this so a rebuild never races an in-flight multitrack start. | — |
| S-073-R08 | `BasicOutputHandler.cpp` | `SetupMultitrackVideo(service, audio_encoder_id, main_mixer, vod_mixer, continuation)` | No `multitrackVideo` → `continuation(nullopt)`, ready future. Else: `multitrackVideoActive = false`; **disconnect all four stream signals** (re-wired per type); `is_custom` = service *type* starts with `rtmp_custom`; custom config from `[Stream1] MultitrackVideoConfigOverride` (JSON string, deserialized) iff `MultitrackVideoConfigOverrideEnabled`; extra canvas UUID from `[Stream1] MultitrackExtraCanvas`; stream key from service settings; `service_name` = user value `service_name` (custom) or `service` (else); `server` starting with `auto` → RTMPS decided by substring `rtmps`, else the URL is the custom RTMP URL; max aggregate bitrate / max video tracks from `MultitrackVideo*` keys (`*Auto` bool → `nullopt`); then `PrepareStreaming` on a **blocking** connection; a `StartMultitrackVideoStreamingGuard`'s future is returned to the caller so UI code can wait for completion. | multitrack state |

### 3.2 Simple output (`SimpleOutput.cpp`)

| # | component/file | trigger | behaviour | state changed |
|---|---|---|---|---|
| S-073-R09 | `SimpleOutput.cpp` | ctor | Read `[SimpleOutput] StreamEncoder` + `StreamAudioEncoder`. `LoadStreamingPreset_Lossy(get_simple_output_encoder(encoder))`. Audio: `StreamAudioEncoder == "opus"` → `simple_opus` encoder, else `simple_aac`, both created at `GetAudioBitrate()`; `audioStreaming` on mixer 0, `audioArchive` on mixer 1 (name `SIMPLE_ARCHIVE_NAME`). `LoadRecordingPreset()` (see R10). If not ffmpeg: `[SimpleOutput] RecRB` → create `replay_buffer` output + connect `start`/`stop`/`stopping`/`saved`; `fileOutput` = `ffmpeg_muxer`, except `RecFormat2 == "hybrid_mp4"` → `mp4_output`, `"hybrid_mov"` → `mov_output`; connect `start`/`stop`/`stopping`. Any creation failure **throws** (e.g. `"Failed to create audio streaming encoder (simple output)"`). | encoders, outputs |
| S-073-R10 | `SimpleOutput.cpp` | `LoadStreamingPreset_Lossy` / `LoadRecordingPreset` | Per-encoder preset tables (`Preset`/`QSVPreset`/`AMDPreset`/`NVENCPreset2`/`AMDAV1Preset`, `x264Settings` custom for x264). Recording: `RecQuality == "Lossless"` → lossless preset (per encoder: x264 lossless, qsv11 lossless, nvenc lossless, apple/AMD equivalents); else lossy preset; ffmpeg recording preset from `[SimpleOutput]` keys. Stream bitrate from `VBitrate`. | encoder settings |
| S-073-R11 | `SimpleOutput.cpp` | `GetAudioBitrate` | `[SimpleOutput] ABitrate`; `opus` → `FindClosestAvailableSimpleOpusBitrate`, else `FindClosestAvailableSimpleAACBitrate` (R39). | — |
| S-073-R12 | `SimpleOutput.cpp` | `Update()` | `UseAdvanced` flag gates whether the *streaming* video encoder is reconfigured from the simple dialog (advanced user keeps `streamEncoder.json`). Stream settings: `VBitrate` → `bitrate`, keyint 2 s, per-encoder preset key (R10), `x264Settings` → `x264opts`; `enforceBitrate = ![Stream1] IgnoreRecommended` keeps the simple bitrate (service recommendations ignored in simple mode). Audio track encoders: `audioTrack[i]` get audio only for mixes selected by `RecTracks` bitmask (else all to `audioRecording`). | encoder settings |
| S-073-R13 | `SimpleOutput.cpp` | `StartStreaming(service)` | Exact order: (1) build settings from `[Output]` `BindIP`/`IPFamily` (+ W: `NewSocketLoopEnable`→`new_socket_loop_enabled`, `LowLatencyEnable`→`low_latency_mode_enabled`), `DynamicBitrate`→`dyn_bitrate` → `obs_output_update`; (2) `reconnect==false` → `maxRetries=0`; `obs_output_set_delay(useDelay ? delaySec : 0, DelayPreserve ? OBS_OUTPUT_DELAY_PRESERVE : 0)`; (3) `obs_output_set_reconnect_settings(maxRetries, retryDelay)`; (4) `SetupVodTrack` **only when** `!(multitrackVideo && multitrackVideoActive)`; (5) `obs_output_start`. Success + multitrack active → `multitrackVideo->StartedStreaming()`. Failure: `multitrackVideoActive = false`; `lastError` = `obs_output_get_last_error` (or empty); `LOG_WARNING "Stream output type '%s' failed to start!%s%s"` (with `  Last Error: <err>` when present); return false. | `lastError`, output |
| S-073-R14 | `SimpleOutput.cpp` | `IsVodTrackEnabled` / `SetupVodTrack` | Simple: `UseAdvanced && VodTrackEnabled && ServiceSupportsVodTrack(service-name)`; `rtmp_custom` → instead `EnableCustomServerVodTrack` (user.ini `[General]`) gates `VodTrackEnabled`. Enabled → `obs_output_set_audio_encoder(streamOutput, audioArchive, 1)`; else `clear_archive_encoder`. | stream audio |
| S-073-R15 | `SimpleOutput.cpp` | `SetupStreaming(service, continuation)` | `SetupOutputs()` iff `!Active()`; `auth->OnStreamConfig()`; `GetStreamOutputType` → null: `continuation(false)`, ready future. Recreate `streamOutput` **only when the output type changed** (disconnect/reconnect `starting`/`stopping`/`start`/`stop`; creation failure → `LOG_WARNING "Creation of stream output type '%s' failed!"` + false). Set video encoder, `whipSimulcastEncoders->SetStreamOutput` (when present), `audioStreaming` on 0, service on output. Whole thing wrapped in `SetupMultitrackVideo` with audio encoder = `GetSimpleAACEncoderForBitrate(GetAudioBitrate())`, main mixer 0, vod track `1` iff R14. | stream output |
| S-073-R16 | `SimpleOutput.cpp` | `UpdateRecording` | Guard: `replayBufferActive \|\| recordingActive` → return. `usingRecordingPreset && !ffmpegOutput` → `UpdateRecordingSettings()`, else if `!obs_output_active(streamOutput)` → `Update()`. `SetupOutputs()` iff `!Active()`. Audio wiring for **both** `fileOutput` and (if enabled) `replayBuffer`: `videoRecording` always; `RecFormat2 == "flv"` **or** `RecQuality == "Stream"` → single `audioRecording` on track 0, else per-mixer `audioTrack[i]` for each bit of `RecTracks`. `recordingConfigured = true`. | output wiring |
| S-073-R17 | `SimpleOutput.cpp` | `ConfigureRecording(updateReplayBuffer)` | Replay-buffer branch: format = `GetFormatString(FilenameFormatting, RecRBPrefix, RecRBSuffix)`, `max_time_sec = RecRBTime`, `max_size_mb = RecRBSize` (0 when not using a recording preset). File branch: `GetRecordingFilename(path, ffmpegOutput ? "avi" : RecFormat2, FileNameWithoutSpace, OverwriteIfExists, FilenameFormatting, ffmpegOutput)`; ffmpeg → setting `url`, `obs_output_set_mixers(tracks)`; else `path`. Muxer: `RecFormat2` starting with `"fragmented"` **and** not lossless **and** `MuxerCustom` has no `movflags` → `muxer_settings = "movflags=frag_keyframe+empty_moov+delay_moov[ <custom>]"`; fragmented+lossless or custom-with-movflags → `LOG_WARNING "User enabled fragmented recording, but custom muxer settings contained movflags."` (custom passed through); else `muxer_settings = MuxerCustom`. Then `obs_output_update`. | output settings |
| S-073-R18 | `SimpleOutput.cpp` | `StartRecording` | `UpdateRecording()` → `ConfigureRecording(false)` → `obs_output_start(fileOutput)`. Failure → `QMessageBox::critical` with `obs_output_get_last_error` or `Output.StartFailedGeneric`. | output |

### 3.3 Advanced output (`AdvancedOutput.cpp`)

| # | component/file | trigger | behaviour | state changed |
|---|---|---|---|---|
| S-073-R19 | `AdvancedOutput.cpp` | ctor | `ffmpegOutput = [AdvOut] RecType == "FFmpeg"`; `ffmpegRecording = ffmpegOutput && FFOutputToFile`; `useStreamEncoder = RecEncoder == "none"`; `useStreamAudioEncoder = RecAudioEncoder == "none"`. Encoder settings loaded from **`streamEncoder.json` / `recordEncoder.json`** (frontend dir) via `GetDataFromJsonFile` + `ApplyEncoderDefaults` (missing keys ← encoder defaults). ffmpeg: `fileOutput = ffmpeg_output`. Else: `RecRB` → replay_buffer (+4 signals); `fileOutput = ffmpeg_muxer` (hybrid rules as R09); `videoRecording` created only when `!useStreamEncoder` (`advanced_video_recording`). `videoStreaming` always created (`advanced_video_stream`). macOS: legacy `macvth264` encoder ids translated. Failures **throw**. | encoders, outputs |
| S-073-R20 | `AdvancedOutput.cpp` | `UpdateStreamSettings` | `ApplyServiceSettings == true`: `obs_service_apply_encoder_settings` into the JSON settings; if `!enforceBitrate` (`[Stream1] IgnoreRecommended`) → keep the user `bitrate` and log `LOG_INFO "User is ignoring service bitrate limits.\nService Recommendations:\n\tvideo bitrate: %s\n\taudio bitrate: %s"` (limits or `None`); `keyint_sec` may only be **lowered** (user value wins when smaller and non-zero). Else → `LOG_WARNING "User is ignoring service settings."`. `DynamicBitrate && id contains "nvenc"` → `lookahead = false`. Preferred video format: keep current output format if I420/NV12/I010/P010, else force `VIDEO_FORMAT_NV12`. `obs_encoder_update`, then `whipSimulcastEncoders->Update(settings, bitrate)` when present. | stream encoder |
| S-073-R21 | `AdvancedOutput.cpp` | `UpdateRecordingSettings` / `Update()` | Recording: `recordEncoder.json` → `obs_encoder_update(videoRecording)`. `Update()`: stream settings always; recording settings only when `!useStreamEncoder && !ffmpegOutput`; audio settings (per-track) always. | encoders |
| S-073-R22 | `AdvancedOutput.cpp` | `GetAudioBitrate(i, id)` | `[AdvOut] Track{i+1}Bitrate` → `FindClosestAvailableAudioBitrate(id, bitrate)` (R39). | — |
| S-073-R23 | `AdvancedOutput.cpp` | `VodTrackMixerIdx(service)` | `rtmp_custom` → `VodTrackEnabled` gated by `EnableCustomServerVodTrack`; else `VodTrackEnabled && ServiceSupportsVodTrack(service-name)`. Only when enabled **and** `TrackIndex != VodTrackIndex` → return `VodTrackIndex - 1` (0-based mixer). Else `nullopt` → `clear_archive_encoder`. | — |
| S-073-R24 | `AdvancedOutput.cpp` | `SetupStreaming(service, continuation)` | `UpdateStreamSettings()` iff `!useStreamEncoder \|\| (!ffmpegOutput && !obs_output_active(fileOutput))`; audio always; `SetupOutputs()` iff `!Active()`; `auth->OnStreamConfig()`; `GetStreamOutputType` (R03). Audio wiring: non-multitrack (`allowsMultiTrack()` false) → `streamAudioEnc` on 0; else per-bit `StreamMultiTrackAudioMixes` bitmask onto `streamTrack[i]`. Rescale: `RescaleRes` parsed `%ux%u` (bad → 0×0), `RescaleFilter` → `obs_encoder_set_scaled_size` + `obs_encoder_set_gpu_scale_type`. `rtmp_custom`: service encoder settings applied to the stream encoder. Output recreated only on type change (as R15). Wrapped in `SetupMultitrackVideo` with main mixer = `TrackIndex - 1`, vod from R23. | stream output |
| S-073-R25 | `AdvancedOutput.cpp` | `StartStreaming(service)` | As R13, plus: `obs_output_set_service` first; `SetupVodTrack` only when protocol starts with `RTMP`; `RetryDelay`/`MaxRetries` read as `int` here (vs `uint` in simple). Same failure log (identical string). | `lastError`, output |
| S-073-R26 | `AdvancedOutput.cpp` | `StartRecording` | Refresh: `!useStreamEncoder && !ffmpegOutput` → recording settings, else `!obs_output_active(stream)` → stream settings; audio; `SetupOutputs()` iff needed. Non-ffmpeg (or ffmpeg-to-file): path from `RecFilePath` (or `FFFilePath`), format `RecFormat2`/`FFExtension`, `noSpace` from `RecFileNameWithoutSpace`/`FFFileNameWithoutSpace`, `splitFile` → `RecSplitFileType` (`Time` → `RecSplitFileTime`, else size `RecSplitFileSize`); `obs_output_start`. ffmpeg-to-URL: `url` = `FFOutputURL`. | output |

### 3.4 Multitrack video + WHIP simulcast (`MultitrackVideoOutput.*`, `WHIPSimulcastEncoders.hpp`, `MultitrackVideoError.cpp`)

| # | component/file | trigger | behaviour | state changed |
|---|---|---|---|---|
| S-073-R27 | `MultitrackVideoOutput.cpp` | `PrepareStreaming` | Logs full config summary (`LOG_INFO "Preparing enhanced broadcasting stream..."`). Non-custom service: config from `DownloadGoLiveConfig(auto_config_url, ...)` (GoLive API post built from key + max bitrate/tracks + vod + canvases). `rtmp_custom`: parse `custom_config` with nlohmann JSON; parse error → `LOG_WARNING "Failed to parse custom config: %s"` + throw `critical(InvalidCustomConfig)`. Neither → `LOG_ERROR "MultitrackVideoOutput: no config set, this should never happen"` + throw `warning(NoConfigSupplied)`. `SetupOBSOutput` (R28) → `rtmp_output` output `"rtmp multitrack video"`. `bpm_inject` registered as packet callback. Reconnect callback: `code != OBS_OUTPUT_INVALID_STREAM \|\| reconnect_attempts++ < MAX_RECONNECT_ATTEMPTS` (5, ~30 s with default delay) — i.e. an invalid-key error retries at most 5 times then gives up. Optional stream-dump recording output gets its own signal handlers + ref'd audio encoders + canvases. | `current`, `current_stream_dump` |
| S-073-R28 | `MultitrackVideoOutput.cpp` | `create_video_encoders` / `create_audio_encoders` / `create_service` | Encoders: empty `encoder_configurations` → `LOG_WARNING "MultitrackVideoOutput: Missing video encoder configurations"` + throw `warning(MissingEncoderConfigs)`; canvas index out of range → `LOG_ERROR "MultitrackVideoOutput: Invalid canvas index: %u"` + throw `warning(InvalidEncoderConfig)`; each encoder into one `obs_encoder_group`, `obs_output_set_video_encoder2(output, encoder, i)` (+ recording output). Audio: layouts from `obs_get_audio_info`; main mixer + vod mixer wired per config. Service: first ingest endpoint with protocol starting `RTMP` (case-insensitive) matching `use_rtmps` (RTMPS iff set); custom `rtmp_url` empty → throw `warning(NoCustomRTMPURLInSettings)`; no URL → `LOG_ERROR "No RTMP URL in go live config"` + throw `warning(NoRTMPURLInConfig)`; `/{stream_key}` removed from URL template; user-key query params carried over; `clientConfigId` query item added when `config_id` present; `obs_service_create("rtmp_custom", "multitrack video service")`. | encoders, service |
| S-073-R29 | `MultitrackVideoOutput.cpp` | `StartedStreaming` / `StopStreaming` | Start: start the stream-dump recording output if present (`LOG_INFO "MultitrackVideoOutput: starting recording%s"`). Stop: `restart_on_error = false`; stop current output and dump output (both under their mutexes, refs taken out). `StreamingSignalHandler()` exposes the current output's handler for UI wiring. | output |
| S-073-R30 | `MultitrackVideoError.cpp` | `ShowDialog(parent, multitrack_video_name)` | `Warning`: title `Output.StartStreamFailed`, rich text `error + FailedToStartStream.WarningRetryNonMultitrackVideo.arg(name)`, buttons Yes/No → **true iff Yes** (caller falls back to the default single-track stream). `Critical`: same title, OK only → false. `Cancel`/other → false, no dialog. | — |
| S-073-R31 | `WHIPSimulcastEncoders.hpp` | `Create(encoderId, rescaleFilter, layers, w, h)` | `OBS_SCALE_DISABLE` → `OBS_SCALE_BICUBIC`. `layers <= 1` → no-op. For `i = layers-1 … 1`: width = `(w/layers)*i` made even, height = `(h/layers)*i` made even; name = `"whip_simulcast_<i>"` (single-digit digit substitution); encoder bound to `obs_get_video()`, scaled size + gpu scale type set, kept in vector. `Update` propagates stream settings + per-layer bitrate split; `SetStreamOutput` wires the output. | simulcast encoders |

### 3.5 Media controls (`MediaControls.cpp`)

| # | component/file | trigger | behaviour | state changed |
|---|---|---|---|---|
| S-073-R32 | `MediaControls.cpp` | source signals | `stopped` → `SetRestartState`, `play`/`started` → `SetPlayingState`, `pause` → `SetPausedState`, `next`/`previous` → `UpdateSlideCounter` — all via `QMetaObject::invokeMethod` (queued to the UI thread; the source signal may fire off-thread). | button states, slide counter |
| S-073-R33 | `MediaControls.cpp` | slider interaction | Click: pause if playing (`prevPaused` remembered), `seekTimer.start(100)`. 100 ms `SeekTimerCallback`: if `seek != lastSeek` → `obs_source_media_set_time(percent * duration)`. Move: update `seek` + labels while dragging. Release: stop seek timer, final `set_time`, `UpdateLabels`, resume playback iff `prevPaused` false. Hover: tooltip with formatted time. `GetSliderTime(val) = val/maximum * media_duration`. | source time |
| S-073-R34 | `MediaControls.cpp` | playback timer | `mediaTimer` at **16 ms** drives `SetSliderPosition` (slider ← `obs_source_media_get_time`); started for playing state, skipped entirely for `isSlideshow`; stopped on pause/stop/seek. Restart/Play/Pause buttons map to `obs_source_media_restart/play/pause` with strong-ref resolution (dead source → no-op). | slider |

### 3.6 Remux queue (`RemuxQueueModel.cpp`, `RemuxWorker.cpp`, `RemuxEntryPathItemDelegate.cpp`, `OBSRemux.cpp`)

| # | component/file | trigger | behaviour | state changed |
|---|---|---|---|---|
| S-073-R35 | `RemuxQueueModel.cpp` | model | Entry states: `Empty, Ready, Pending, InProgress, Complete, InvalidPath, Error`. Columns: input path, output path, state icon. `checkInputPath`: missing input file/dir → `InvalidPath`. `checkForOverwrites` (QFileInfoList of existing outputs) prompts once before start; `checkForErrors` → dialog if any non-Ready. `clearFinished` removes Complete/Error rows; `clearAll` clears. | rows |
| S-073-R36 | `RemuxQueueModel.cpp` | processing loop | `beginProcessing`: all `Ready` → `Pending`. `beginNextEntry(&in, &out)`: first `Pending` row → `InProgress` (false when none left). `finishEntry(success)`: `Complete` / `Error`, then re-run `beginNextEntry` so the queue drains sequentially. | row states |
| S-073-R37 | `RemuxWorker.cpp` | `remux(source, target)` | `media_remux_job_create` → `media_remux_job_process(job, cb, this)` → destroy. Progress callback (job thread) → `UpdateProgress`: emit only when `|last - new| >= 0.1f`. `isWorking` is the stop flag (read by the callback); `remuxFinished(!stopped && success)` emitted at the end. Runs on a `QThread` started by the dialog. | job |
| S-073-R38 | `RemuxEntryPathItemDelegate.cpp` | per-cell editing | Editor = line edit + browse/clear buttons. `handleBrowse`: directory picker seeded with the model's `defaultPath` (output column) or the input file's dir; `handleClear` empties the cell. `updateText` on `editingFinished` writes back; commit → `setData` (which re-validates the pair). | row |
| S-073-R39 | `OBSRemux.cpp` | dialog / auto-remux mode | Normal: queue table + OK/Reset/Close; OK → overwrite check → start worker per row (R36/R37); drop-event accepts file paths into the input column. `autoRemux` mode (created after a recording stop, R04): `resize(280, 40)`, hidden until it has work, single implicit entry (old mp4 → new mkv), no user controls; on `remuxFinished` failure → `OBSBasic::OutputPathInvalidMessage`-style error path. `stopRemux` sets the worker stop flag; dtor waits for the thread. | worker |

### 3.7 Audio bitrate + ffmpeg helpers (`audio-encoders.cpp`, `FFmpeg*.cpp`)

| # | component/file | trigger | behaviour | state changed |
|---|---|---|---|---|
| S-073-R40 | `audio-encoders.cpp` | one-time population | `call_once`: for every audio encoder (skip `ffmpeg_aac`/`ffmpeg_opus` themselves) read its *default* properties: `samplerate` property gets the app's `[Audio] SampleRate` applied (`obs_property_modified`), then `bitrate` property: INT → min..max step; LIST(INT format) → enabled items; other formats → `LOG_ERROR "...unhandled..."`. `ffmpeg_aac` lists double as `fallbackBitrates` (opus reuses them). Empty results logged `LOG_ERROR "Could not enumerate %s encoder bitrates"`. | bitrate maps |
| S-073-R41 | `audio-encoders.cpp` | `FindClosestAvailableAudioBitrate(id, bitrate)` / simple variants | Selection over the (fallback) list: exact match wins; else the **smallest available above** requested; else the **largest below**; else constant `192`. Simple maps: `bitrate → encoder id` for AAC (`ffmpeg_aac`, `libfdk_aac`, + other aac-codec encoders) and Opus (ffmpeg_opus + opus-codec encoders); `GetSimple{AAC,Opus}EncoderForBitrate(bitrate)` returns the id or NULL. | — |
| S-073-R42 | `FFmpegFormat.cpp` | `GetSupportedFormats` | Iterate `av_muxer_iterate`; skip output **devices** (AV_CLASS_CATEGORY_DEVICE_*_OUTPUT); wrap the rest as `FFmpegFormat`. `GetDefaultEncoder(type)`: format's `video_codec`/`audio_codec` id → `avcodec_find_encoder`; miss → fall back to `FFmpegCodec{format-name, id}` (works for e.g. FLV). | — |
| S-073-R43 | `FFmpegCodec.cpp` | `GetFormatCodecs(format, ignore_compatibility)` | `av_codec_iterate` → encoders only; unless `ignore_compatibility`, codec must have a tag in `format.codec_tags`. `FFCodecAndFormatCompatible(codec, format)` via `avformat_query_codec(FF_COMPLIANCE_NORMAL)`. `ContainerSupportsCodec`: static table — `flv {h264, aac}`, `mpegts {h264, hevc, aac, opus}`, `hls {h264, hevc, aac}`, `mp4`/`fragmented_mp4`/`hybrid_mp4 {h264, hevc, av1, aac, opus, alac, flac, pcm_s16le/s24le/f32le}`, `mov`/`fragmented_mov`/`hybrid_mov {h264, hevc, prores, aac, alac, pcm_*}`, `mkv {}` = everything. `IsBuiltinCodec` = fixed set (h264/hevc/av1/prores/aac/opus/alac/flac/pcm_s16le/pcm_s24le/pcm_f32le). | — |
| S-073-R44 | `FFmpegShared.hpp` | `strequal(a, b)` | NULL/empty-tolerant: `("" == NULL)`, `(NULL == "")`, `(NULL == NULL)` all true; otherwise `strcmp`. Required because UI settings store "" for "no codec". | — |

### 3.8 Small helpers (`QuickTransition.*`, `SilentUpdate*`, `models/multitrack-video.hpp`)

| # | component/file | trigger | behaviour | state changed |
|---|---|---|---|---|
| S-073-R45 | `QuickTransition.cpp` | source `rename` signal | Hotkey description updated to `"QuickTransitions.HotkeyName".arg(label)`; label = source name (or `"FadeToBlack"` when `fadeToBlack`), plus `" (%1ms)"` suffix when the transition is not fixed-duration. The struct holds `source` (strong ref), `duration`, `id`, `hotkey`, `button`. | hotkey label |
| S-073-R46 | `SilentUpdateCheckBox.hpp` / `SilentUpdateSpinBox.hpp` | programmatic set | `setCheckedSilently` / `setValueSilently`: `blockSignals(true)` → set → restore previous block state. Used so restoring dialog state never re-triggers property writes. | — |
| S-073-R47 | `models/multitrack-video.hpp` | dialog model | Pure value model for the multitrack stream-settings widget (name, configuration URL, max aggregate bitrate + auto flag, max video tracks + auto flag, custom RTMP URL, config-override text) — no behaviour beyond getters/setters. | — |

## 4. Reference defects and quirks

- **Q-1** (`SimpleOutput.cpp` / `AdvancedOutput.cpp`): the same `"Stream output type '%s' failed to start!"`
  warning is emitted by *both* strategies — logs do not identify which mode was used.
- **Q-2** (`SimpleOutput.cpp` R13): `SetupVodTrack` is skipped while a multitrack stream is active
  because the multitrack output owns its own audio layout — do not re-apply the simple audio
  wiring after `multitrackVideo->StartedStreaming()`.
- **Q-3** (`AdvancedOutput.cpp` R24): the "recreate output on type change" block is duplicated
  verbatim in `SimpleOutput.cpp` (source comment: *"this is messy and disgusting and should be
  refactored"*). The rewrite MAY share one helper, but behaviour (signal disconnect/reconnect order,
  failure log) must stay identical.
- **Q-4** (`audio-encoders.cpp`): closest-bitrate selection is *not* nearest — it prefers the
  smallest available value **at or above** the requested bitrate (next-first), falling back down.
  A requested 128 kbps with list {96, 160} yields 160, not 96.
- **Q-5** (`MultitrackVideoOutput.cpp`): `reconnect_attempts` is never reset except by a fresh
  `PrepareStreaming`; the 5-attempt cap is per prepared session.

## 5. Concurrency (summary)

- Handler methods run on the UI thread; output signals are queued to the UI thread where needed
  (`file_changed` R05, media controls R32).
- `SetupMultitrackVideo` blocks the UI thread during `PrepareStreaming` (GoLive download,
  encoder creation) and hands out a guard future so `ResetOutputs` can wait instead of tearing
  down a live stream.
- `RemuxWorker` runs on its own `QThread`; progress is funnelled through a mutex + signal.
- Bitrate maps are `call_once` globals — populated lazily on first use, immutable afterwards.

## 6. Open questions

1. `SetupAutoRemux` (R04): confirm the exact remux target semantics (mp4→mkv background job vs
   container swap) against S-046/S-022 before conformance vectors; the reference triggers it from
   `OBSBasic` on recording stop, which lives outside this file list.
2. WHIP simulcast `Update` bitrate split across layers is not pinned here (only `Create` is);
   confirm the per-layer allocation formula from `WHIPSimulcastEncoders.hpp` lines 53–59 during
   vector generation.
3. Does the rewrite keep `streamEncoder.json`/`recordEncoder.json` as on-disk files (user-editable
   in the config dir) or move to structured settings? The reference reads them relative to the
   frontend binary directory.
