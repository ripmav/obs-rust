//! P1-33.1 (WP-34): the C ABI for `shared/bpm` (S-024 M8).
//!
//! The per-`(output, track)` state machine of `bpm.c` (counters, timestamp
//! validity, per-codec injection) on top of the C services still linked in
//! Phase 1: the output/encoder/video counters and codec/name accessors
//! (the conformance runner links fakes; production links libobs),
//! `obs_encoder_packet_release`, `os_nstime_to_timespec` (the injected
//! wall-clock base) and `blog`. The pure framing logic (SEI NALs with
//! emulation prevention, the three payloads, the AV1 metadata OBU, the
//! RFC3339 rendering) is [`obs_bpm::bpm`].

use std::ffi::CStr;
use std::os::raw::{c_char, c_int, c_long, c_void};
use std::sync::Mutex;

use obs_bpm::bpm;

use crate::abi_common::{bfree, bmalloc_c, bstrdup_bytes};
use crate::{LOG_DEBUG, LOG_ERROR};

const OBS_ENCODER_VIDEO: c_int = 1;

/// `MAX_OUTPUT_VIDEO_ENCODERS` (obs-internal.h): the per-output track count
/// (the spec scopes the state to track < 10).
const MAX_TRACKS: usize = 10;

/// `struct counter_data` (bpm-internal.h).
#[repr(C)]
#[derive(Clone, Copy, Default)]
struct Counter {
    diff: u32,
    ref_: u32,
    curr: u32,
}

/// A rendered timestamp (the `struct metrics_time` of bpm-internal.h):
/// the RFC3339 string once valid.
#[derive(Clone, Copy)]
struct MTime {
    rfc3339: [u8; 64],
    valid: bool,
}

impl Default for MTime {
    fn default() -> Self {
        Self {
            rfc3339: [0; 64],
            valid: false,
        }
    }
}

/// `struct metrics_data` (bpm-internal.h) minus the payload buffers (the
/// payloads are built per packet and never outlive the call — the
/// reference frees them in `process_metrics` before the next render).
#[derive(Default)]
struct MetricsTrack {
    rendition_frames_input: Counter,
    rendition_frames_output: Counter,
    rendition_frames_skipped: Counter,
    session_frames_rendered: Counter,
    session_frames_output: Counter,
    session_frames_dropped: Counter,
    session_frames_lagged: Counter,
    cts: MTime,
    ferts: MTime,
    fercts: MTime,
    pirts: MTime,
}

struct OutputLink {
    output: usize,
    tracks: Vec<MetricsTrack>,
}

/// The global `bpm_metrics` DARRAY of the reference (output -> tracks),
/// guarded by the global `bpm_metrics_mutex`. The reference locks a
/// per-track mutex around `process_metrics`; the port holds this single
/// lock for the call, which keeps the same serialisation per track.
static BPM_METRICS: Mutex<Option<Vec<OutputLink>>> = Mutex::new(None);

/// `struct encoder_packet` (obs-encoder.h) — full layout mirror (the
/// conformance runner builds packets with the real header).
#[repr(C)]
pub struct encoder_packet {
    data: *mut u8,
    size: usize,
    pts: i64,
    dts: i64,
    timebase_num: i32,
    timebase_den: i32,
    r#type: c_int,
    keyframe: bool,
    dts_usec: i64,
    sys_dts_usec: i64,
    priority: i32,
    drop_priority: i32,
    track_idx: usize,
    encoder: *mut c_void,
}

/// `struct encoder_packet_time` (obs-encoder.h).
#[repr(C)]
pub struct encoder_packet_time {
    pts: i64,
    cts: u64,
    fer: u64,
    ferc: u64,
    pir: u64,
}

/// `struct timespec` (the layout the platform API uses).
#[repr(C)]
#[derive(Default)]
struct Timespec {
    tv_sec: i64,
    tv_nsec: c_long,
}

unsafe extern "C" {
    // base-logging.c.
    fn blog(level: c_int, format: *const c_char, ...) -> c_int;

    // The output / encoder / video counter surface (the conformance
    // runner links fakes; production links libobs).
    fn obs_output_get_total_frames(output: *mut c_void) -> c_int;
    fn obs_output_get_frames_dropped(output: *mut c_void) -> c_int;
    fn obs_get_total_frames() -> u32;
    fn obs_get_lagged_frames() -> u32;
    fn obs_encoder_video(encoder: *mut c_void) -> *mut c_void;
    fn video_output_get_total_frames(video: *mut c_void) -> u32;
    fn video_output_get_skipped_frames(video: *mut c_void) -> u32;
    fn obs_encoder_get_encoded_frames(encoder: *mut c_void) -> u32;
    fn obs_encoder_get_name(encoder: *mut c_void) -> *const c_char;
    fn obs_encoder_get_codec(encoder: *mut c_void) -> *const c_char;
    fn obs_encoder_packet_release(packet: *mut encoder_packet);

    // The injected wall-clock base (the conformance runner links the
    // identity model; production links libobs).
    fn os_nstime_to_timespec(timestamp: u64, storage: *mut Timespec) -> *mut Timespec;
}

/// The live C string at `p` as a lossy Rust `String` ("" for NULL).
fn c_str(p: *const c_char) -> String {
    if p.is_null() {
        return String::new();
    }
    // SAFETY: p is a live NUL-terminated C string (the callers guarantee it).
    unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned()
}

/// `blog(level, "%s", msg)` of the reference (the port formats the
/// per-message strings itself).
fn blog_msg(level: c_int, msg: &str) {
    // SAFETY: a fresh bmem allocation; freed below.
    let msg_c = unsafe { bstrdup_bytes(msg.as_bytes()) };
    // SAFETY: msg_c is a live NUL-terminated C string for the call.
    unsafe { blog(level, c"%s".as_ptr(), msg_c) };
    // SAFETY: msg_c came from the C bmem allocator.
    unsafe { bfree(msg_c) };
}

/// `render_metrics_time` (S-024-R52): the RFC3339 string of the wall-clock
/// time of `ns` (via the injected `os_nstime_to_timespec` base), always
/// valid once rendered.
fn render_metrics_time(mt: &mut MTime, ns: u64) {
    let mut tspec = Timespec::default();
    // SAFETY: tspec is live storage for the conversion.
    unsafe { os_nstime_to_timespec(ns, &mut tspec) };
    let string = bpm::rfc3339_utc(tspec.tv_sec, tspec.tv_nsec as u64);
    let bytes = string.as_bytes();
    let n = bytes.len().min(63);
    mt.rfc3339[..n].copy_from_slice(&bytes[..n]);
    mt.valid = true;
}

/// The NUL-terminated string of a rendered timestamp.
fn mt_string(mt: &MTime) -> &[u8] {
    let end = mt.rfc3339.iter().position(|&b| b == 0).unwrap_or(64);
    &mt.rfc3339[..end]
}

/// `update_metrics` (S-024 §4.8): read the seven counters, derive the
/// diffs (u32 wrapping; all-zero for `pts == 0`, which still stores the
/// references), render the four timestamps with their validity rules and
/// log the metrics summary. Returns false on NULL arguments.
fn update_metrics(
    output: *mut c_void,
    pkt: &encoder_packet,
    ept: &encoder_packet_time,
    track: &mut MetricsTrack,
) -> bool {
    if output.is_null() {
        // (unreachable via bpm_inject, which checks first — kept for parity)
        blog_msg(LOG_DEBUG, "update_metrics: Null encoder_packet pointer");
        return false;
    }
    // SAFETY: the counter accessors take the live output/encoder pointers.
    let (session_output, dropped) = unsafe {
        (
            obs_output_get_total_frames(output) as u32,
            obs_output_get_frames_dropped(output) as u32,
        )
    };
    let (rendered, lagged) =
        // SAFETY: global counters.
        unsafe {
            (obs_get_total_frames(), obs_get_lagged_frames())
        };

    // SAFETY: the encoder pointer is live for the call.
    let video = unsafe { obs_encoder_video(pkt.encoder) };
    if video.is_null() {
        blog_msg(LOG_ERROR, "update_metrics(): *video_t==null");
    }
    let (rend_input, rend_skipped, rend_output) = if !video.is_null() {
        // SAFETY: the video/encoder pointers are live.
        unsafe {
            (
                video_output_get_total_frames(video),
                video_output_get_skipped_frames(video),
                obs_encoder_get_encoded_frames(pkt.encoder),
            )
        }
    } else {
        (0, 0, 0)
    };

    track.session_frames_output.curr = session_output;
    track.session_frames_dropped.curr = dropped;
    track.session_frames_rendered.curr = rendered;
    track.session_frames_lagged.curr = lagged;
    track.rendition_frames_input.curr = rend_input;
    track.rendition_frames_skipped.curr = rend_skipped;
    track.rendition_frames_output.curr = rend_output;

    if pkt.pts == 0 {
        // "update_metrics(): Setting diffs to 0"
        track.session_frames_output.diff = 0;
        track.session_frames_dropped.diff = 0;
        track.session_frames_rendered.diff = 0;
        track.session_frames_lagged.diff = 0;
        track.rendition_frames_input.diff = 0;
        track.rendition_frames_skipped.diff = 0;
        track.rendition_frames_output.diff = 0;
        blog_msg(LOG_DEBUG, "update_metrics(): Setting diffs to 0");
    } else {
        // diff = curr - ref (u32, wraps)
        track.session_frames_output.diff = track
            .session_frames_output
            .curr
            .wrapping_sub(track.session_frames_output.ref_);
        track.session_frames_dropped.diff = track
            .session_frames_dropped
            .curr
            .wrapping_sub(track.session_frames_dropped.ref_);
        track.session_frames_rendered.diff = track
            .session_frames_rendered
            .curr
            .wrapping_sub(track.session_frames_rendered.ref_);
        track.session_frames_lagged.diff = track
            .session_frames_lagged
            .curr
            .wrapping_sub(track.session_frames_lagged.ref_);
        track.rendition_frames_input.diff = track
            .rendition_frames_input
            .curr
            .wrapping_sub(track.rendition_frames_input.ref_);
        track.rendition_frames_skipped.diff = track
            .rendition_frames_skipped
            .curr
            .wrapping_sub(track.rendition_frames_skipped.ref_);
        track.rendition_frames_output.diff = track
            .rendition_frames_output
            .curr
            .wrapping_sub(track.rendition_frames_output.ref_);
    }

    // ref = curr for all seven
    track.session_frames_output.ref_ = track.session_frames_output.curr;
    track.session_frames_dropped.ref_ = track.session_frames_dropped.curr;
    track.session_frames_rendered.ref_ = track.session_frames_rendered.curr;
    track.session_frames_lagged.ref_ = track.session_frames_lagged.curr;
    track.rendition_frames_input.ref_ = track.rendition_frames_input.curr;
    track.rendition_frames_skipped.ref_ = track.rendition_frames_skipped.curr;
    track.rendition_frames_output.ref_ = track.rendition_frames_output.curr;

    // Timestamp validity: CTS and PIR always; FER iff fer != 0 && fer >
    // cts; FERC iff FER valid and ferc != 0 && ferc > fer.
    track.cts = MTime::default();
    track.ferts = MTime::default();
    track.fercts = MTime::default();
    track.pirts = MTime::default();
    render_metrics_time(&mut track.cts, ept.cts);
    if ept.fer != 0 && ept.fer > ept.cts {
        render_metrics_time(&mut track.ferts, ept.fer);
        if ept.ferc != 0 && ept.ferc > ept.fer {
            render_metrics_time(&mut track.fercts, ept.ferc);
        }
    }
    render_metrics_time(&mut track.pirts, ept.pir);

    // The metrics summary (the reference's --verbose debug line; the
    // fer/ferc/cts subtractions wrap, as in the reference).
    // SAFETY: the encoder pointer is live; the name is a live C string.
    let name = c_str(unsafe { obs_encoder_get_name(pkt.encoder) });
    let msg = format!(
        "BPM: {name}, trk {}: [CTS|FER-CTS|FERC-FER|PIR-CTS]:[{} ms|{} ms|{} us|{} ms], [dts|pts]:[{}|{}], S[R:O:D:L],R[I:S:O]:{}:{}:{}:{}:{}:{}:{}",
        pkt.track_idx,
        ept.cts / 1_000_000,
        ept.fer.wrapping_sub(ept.cts) / 1_000_000,
        ept.ferc.wrapping_sub(ept.fer) / 1_000,
        ept.pir.wrapping_sub(ept.cts) / 1_000_000,
        pkt.dts,
        pkt.pts,
        // The reference passes the u32 diffs to %d (signed interpretation).
        track.session_frames_rendered.diff as i32,
        track.session_frames_output.diff as i32,
        track.session_frames_dropped.diff as i32,
        track.session_frames_lagged.diff as i32,
        track.rendition_frames_input.diff as i32,
        track.rendition_frames_skipped.diff as i32,
        track.rendition_frames_output.diff as i32,
    );
    blog_msg(LOG_DEBUG, &msg);
    true
}

/// `process_metrics` (S-024 §4.8): update the metrics, build the three
/// payloads, frame them per codec and replace the packet data with
/// `original | appended` (the original buffer is released; the new buffer
/// is bmem-allocated with the reference's 8-byte `long ref` prefix, which
/// the conformance runner frees as `data - sizeof(long)`).
///
/// Returns the reference's bool: true only for h264/hevc/av1; an unknown
/// codec replaces the packet with an identical copy and returns false
/// (Q-11: the reference's final `return avc || hevc || av1`).
fn process_metrics(
    output: *mut c_void,
    pkt: &mut encoder_packet,
    ept: &encoder_packet_time,
    track: &mut MetricsTrack,
) -> bool {
    if !update_metrics(output, pkt, ept, track) {
        // (unreachable via bpm_inject — kept for parity)
        blog_msg(
            LOG_DEBUG,
            &format!("update_metrics() for track index: {} failed", pkt.track_idx),
        );
        return false;
    }

    // SAFETY: the codec string is a live C string for the call.
    let codec = c_str(unsafe { obs_encoder_get_codec(pkt.encoder) });

    // The three payloads in the order TS, SM, ERM.
    let mut timestamps: Vec<(u8, &str)> = Vec::new();
    if track.cts.valid {
        timestamps.push((
            bpm::TS_EVENT_CTS,
            std::str::from_utf8(mt_string(&track.cts)).unwrap(),
        ));
    }
    if track.ferts.valid {
        timestamps.push((
            bpm::TS_EVENT_FER,
            std::str::from_utf8(mt_string(&track.ferts)).unwrap(),
        ));
    }
    if track.fercts.valid {
        timestamps.push((
            bpm::TS_EVENT_FERC,
            std::str::from_utf8(mt_string(&track.fercts)).unwrap(),
        ));
    }
    if track.pirts.valid {
        timestamps.push((
            bpm::TS_EVENT_PIR,
            std::str::from_utf8(mt_string(&track.pirts)).unwrap(),
        ));
    }
    let ts = bpm::ts_payload(&timestamps);
    let pir = std::str::from_utf8(mt_string(&track.pirts)).unwrap();
    let sm = bpm::sm_payload(
        pir,
        track.session_frames_rendered.diff,
        track.session_frames_lagged.diff,
        track.session_frames_dropped.diff,
        track.session_frames_output.diff,
    );
    let erm = bpm::erm_payload(
        pir,
        track.rendition_frames_input.diff,
        track.rendition_frames_skipped.diff,
        track.rendition_frames_output.diff,
    );

    // R56: a HEVC packet without an Annex-B start code is left unchanged
    // (the counters were already advanced above); frame_packet returns
    // None only in that case.
    let Some(new_data) = bpm::frame_packet(&codec, pkt.data_bytes(), [&ts[..], &sm[..], &erm[..]])
    else {
        blog_msg(
            LOG_DEBUG,
            "Annex-B start code not found, we may not generate a valid hevc nal unit header for our caption",
        );
        return false;
    };

    // `struct encoder_packet backup = *out; ... obs_encoder_packet_release(out);
    // *out = backup; out->data = out_data.array + sizeof(long); out->size = ...`
    // (only data/size change; the port keeps the 8-byte ref prefix layout
    // the conformance runner frees).
    // SAFETY: pkt points to the live packet; release is the (fake) no-op
    // in the runner and the real release in production.
    unsafe { obs_encoder_packet_release(pkt) };

    // SAFETY: a fresh bmem allocation of the exact size written; the
    // 8-byte prefix is the reference's `long ref = 1`.
    let buf = unsafe { bmalloc_c(new_data.len() + 8) };
    // SAFETY: buf is a valid allocation of new_data.len() + 8 bytes.
    unsafe {
        std::ptr::copy_nonoverlapping(1i64.to_ne_bytes().as_ptr(), buf, 8);
        std::ptr::copy_nonoverlapping(new_data.as_ptr(), buf.add(8), new_data.len());
        pkt.data = buf.add(8);
        pkt.size = new_data.len();
    }
    matches!(codec.as_str(), "h264" | "av1" | "hevc")
}

/// The packet data as a byte slice (empty when data is NULL or size is 0).
impl encoder_packet {
    fn data_bytes(&self) -> &[u8] {
        if self.data.is_null() || self.size == 0 {
            return &[];
        }
        // SAFETY: the runner/prod guarantees `size` valid bytes at data.
        unsafe { std::slice::from_raw_parts(self.data, self.size) }
    }
}

/// `bpm_inject` (bpm.h / S-024 §4.8): the packet callback registered with
/// each output needing BPM injection.
///
/// # Safety
/// `output` and `pkt` (and `pkt_time`, when non-NULL) must be live; the
/// packet is mutated in place (its data buffer is replaced) for processed
/// video keyframes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bpm_inject(
    output: *mut c_void,
    pkt: *mut encoder_packet,
    pkt_time: *mut encoder_packet_time,
    _param: *mut c_void,
) {
    let (output, pkt, pkt_time) = (
        output,
        // SAFETY: the caller guarantees a live packet (or NULL).
        unsafe { pkt.as_mut() },
        // SAFETY: the caller guarantees a live timing (or NULL).
        unsafe { pkt_time.as_mut() },
    );
    if output.is_null() || pkt.is_none() {
        blog_msg(
            LOG_DEBUG,
            "bpm_inject: Null pointer arguments supplied, returning",
        );
        return;
    }
    let pkt = pkt.unwrap();

    // Insert BPM on video frames and only when a keyframe is detected.
    if pkt.r#type == OBS_ENCODER_VIDEO && pkt.keyframe {
        if pkt_time.is_none() {
            blog_msg(
                LOG_DEBUG,
                &format!(
                    "bpm_inject: Packet timing missing for track {}, PTS {}",
                    pkt.track_idx, pkt.pts
                ),
            );
            return;
        }
        let ept = pkt_time.unwrap();

        // Get (or create) the metrics track for the output.
        let mut metrics = BPM_METRICS.lock().unwrap();
        let metrics = metrics.get_or_insert_with(Vec::new);
        // The reference indexes metrics_tracks[track] directly (track < 10
        // per the spec scope).
        let track_idx = pkt.track_idx.min(MAX_TRACKS - 1);
        let link_idx = match metrics.iter().position(|l| l.output == output as usize) {
            Some(i) => i,
            None => {
                metrics.push(OutputLink {
                    output: output as usize,
                    tracks: (0..MAX_TRACKS).map(|_| MetricsTrack::default()).collect(),
                });
                metrics.len() - 1
            }
        };
        let track = &mut metrics[link_idx].tracks[track_idx];

        // SAFETY: the counters/codec above only read the live packet.
        if !process_metrics(output, pkt, ept, track) {
            blog_msg(LOG_DEBUG, "bpm_inject: BPM injection processing failed");
        }
    }
}

/// `bpm_destroy` (bpm.h / S-024-R59): discard all tracks of the output.
///
/// # Safety
/// `output` must be a pointer value previously passed to `bpm_inject` (or
/// any pointer — a non-matching value discards nothing).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bpm_destroy(output: *mut c_void) {
    // SAFETY: the tracks own no C-allocated memory (payloads are freed
    // within process_metrics in the reference and built per call here).
    let mut metrics = BPM_METRICS.lock().unwrap();
    if let Some(metrics) = metrics.as_mut() {
        metrics.retain(|l| l.output != output as usize);
    }
}

// Test stand-ins for the C externs the MSVC backend keeps in the test
// binary (it does not dead-strip `#[no_mangle]` functions the way the ELF
// backends do, so on Windows every C service the bridge references must
// resolve here; on ELF the references are stripped and the conformance
// runner's fakes / the C shim provide them instead). All OBS-specific
// names — no system-symbol collisions on any platform.
#[cfg(test)]
mod test_standin {
    use super::*;

    #[unsafe(no_mangle)]
    pub extern "C" fn obs_output_get_total_frames(_output: *mut c_void) -> c_int {
        0
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn obs_output_get_frames_dropped(_output: *mut c_void) -> c_int {
        0
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn obs_get_total_frames() -> u32 {
        0
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn obs_get_lagged_frames() -> u32 {
        0
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn obs_encoder_video(_encoder: *mut c_void) -> *mut c_void {
        std::ptr::null_mut()
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn video_output_get_total_frames(_video: *mut c_void) -> u32 {
        0
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn video_output_get_skipped_frames(_video: *mut c_void) -> u32 {
        0
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn obs_encoder_get_encoded_frames(_encoder: *mut c_void) -> u32 {
        0
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn obs_encoder_get_name(_encoder: *mut c_void) -> *const c_char {
        std::ptr::null()
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn obs_encoder_get_codec(_encoder: *mut c_void) -> *const c_char {
        std::ptr::null()
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn obs_encoder_packet_release(_packet: *mut encoder_packet) {}

    #[unsafe(no_mangle)]
    pub extern "C" fn os_nstime_to_timespec(
        timestamp: u64,
        storage: *mut Timespec,
    ) -> *mut Timespec {
        // The identity model (the conformance runner's model): ns →
        // (sec, nsec). Test-only; never reached on the ELF test builds.
        // SAFETY: storage is a valid out pointer (the caller's local).
        unsafe {
            (*storage).tv_sec = timestamp as i64 / 1_000_000_000;
            (*storage).tv_nsec = (timestamp % 1_000_000_000) as c_long;
        }
        storage
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Layout pins against the C headers (the conformance runner builds
    /// packets with the real obs-encoder.h; the bpm_* vectors pin the
    /// round trip).
    #[test]
    fn layout_pins() {
        use std::mem::{offset_of, size_of};
        assert_eq!(offset_of!(encoder_packet, data), 0);
        assert_eq!(offset_of!(encoder_packet, size), 8);
        assert_eq!(offset_of!(encoder_packet, pts), 16);
        assert_eq!(offset_of!(encoder_packet, dts), 24);
        assert_eq!(offset_of!(encoder_packet, timebase_num), 32);
        assert_eq!(offset_of!(encoder_packet, timebase_den), 36);
        assert_eq!(offset_of!(encoder_packet, r#type), 40);
        assert_eq!(offset_of!(encoder_packet, keyframe), 44);
        assert_eq!(offset_of!(encoder_packet, dts_usec), 48);
        assert_eq!(offset_of!(encoder_packet, sys_dts_usec), 56);
        assert_eq!(offset_of!(encoder_packet, priority), 64);
        assert_eq!(offset_of!(encoder_packet, drop_priority), 68);
        assert_eq!(offset_of!(encoder_packet, track_idx), 72);
        assert_eq!(offset_of!(encoder_packet, encoder), 80);
        assert_eq!(size_of::<encoder_packet>(), 88);
        assert_eq!(size_of::<encoder_packet_time>(), 40);
        assert_eq!(offset_of!(encoder_packet_time, cts), 8);
        assert_eq!(offset_of!(encoder_packet_time, pir), 32);
    }
}
