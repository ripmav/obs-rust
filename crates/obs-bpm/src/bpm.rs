//! S-024 §4.8 (M8) — the pure BPM framing logic of `shared/bpm`.
//!
//! Counters, timestamp validity and the per-`(output, track)` state machine
//! live in the C-ABI bridge (`obs_ffi::abi_bpm`), which calls the builders
//! here. The wall-clock base is injected by the linked `os_nstime_to_timespec`
//! (the conformance runner links the identity model `wall(ns) = ns`;
//! production links libobs), so the RFC3339 rendering here is a pure
//! function of the converted `(sec, nsec)` pair (S-024 §4.8: "a port MUST
//! make this base injectable for tests").

/// `uuid_ts` — BPM timestamp SEI (S-024 §4.8).
pub const UUID_TS: [u8; 16] = [
    0x0a, 0xec, 0xff, 0xe7, 0x52, 0x72, 0x4e, 0x2f, 0xa6, 0x2f, 0xd1, 0x9c, 0xd6, 0x1a, 0x93, 0xb5,
];
/// `uuid_sm` — BPM session metrics SEI (S-024 §4.8).
pub const UUID_SM: [u8; 16] = [
    0xca, 0x60, 0xe7, 0x1c, 0x6a, 0x8b, 0x43, 0x88, 0xa3, 0x77, 0x15, 0x1d, 0xf7, 0xbf, 0x8a, 0xc2,
];
/// `uuid_erm` — BPM encoded rendition metrics SEI (S-024 §4.8).
pub const UUID_ERM: [u8; 16] = [
    0xf1, 0xfb, 0xc1, 0xd5, 0x10, 0x1e, 0x4f, 0xb5, 0xa6, 0x1e, 0xb8, 0xce, 0x3c, 0x07, 0xb8, 0xc0,
];

/// The SEI event tags (S-024 §4.8, bpm-internal.h `bpm_ts_event_tag`).
pub const TS_EVENT_CTS: u8 = 1;
pub const TS_EVENT_FER: u8 = 2;
pub const TS_EVENT_FERC: u8 = 3;
pub const TS_EVENT_PIR: u8 = 4;

/// The AV1 metadata type `METADATA_TYPE_USER_PRIVATE_6` (obs-av1.h).
pub const METADATA_TYPE_USER_PRIVATE_6: u8 = 6;

/// Emulation prevention of `sei_render` (S-024-R54): insert `03` before
/// every byte `<= 03` that is preceded by two consecutive `00` bytes; the
/// zero-run counter restarts after the inserted byte.
#[must_use]
pub fn sei_escape(payload: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(payload.len() + payload.len() / 3);
    let mut run = 0usize;
    for &b in payload {
        if run >= 2 && b <= 3 {
            out.push(3);
            run = 0;
        }
        out.push(b);
        run = if b == 0 { run + 1 } else { 0 };
    }
    out
}

/// `S(p)` of S-024 §4.8: the AVC user-data-unregistered SEI NAL
/// `06 | 05 | size(p) | escape(p) | 80`; `size(p)` is `ff` repeated
/// `len/255` times then `len%255` (`len = |p|` before escaping — BPM
/// payloads are < 255 bytes, so the extension is unreachable in practice).
#[must_use]
pub fn sei_nal(payload: &[u8]) -> Vec<u8> {
    let escaped = sei_escape(payload);
    let mut out = Vec::with_capacity(escaped.len() + 8);
    out.push(0x06);
    out.push(0x05);
    let mut len = payload.len();
    while 255 <= len {
        out.push(0xff);
        len -= 255;
    }
    out.push(len as u8);
    out.extend_from_slice(&escaped);
    out.push(0x80);
    out
}

/// The minimal unsigned LEB128 (S-012-R73: "no redundant groups") — the
/// `encode_uleb128` of obs-av1.c.
#[must_use]
pub fn uleb128(mut value: u64) -> Vec<u8> {
    let mut out = Vec::new();
    loop {
        let mut byte = (value & 0x7f) as u8;
        value >>= 7;
        if value != 0 {
            byte |= 0x80;
        }
        out.push(byte);
        if value == 0 {
            break;
        }
    }
    out
}

/// The AV1 metadata OBU of S-012 (inlined per the P1-33.1 scope):
/// `2a | leb128(len+2) | type | payload | 80` (obs-av1.c `metadata_obu`
/// with `METADATA_TYPE_USER_PRIVATE_6`).
#[must_use]
pub fn metadata_obu(payload: &[u8]) -> Vec<u8> {
    let size_field = 1 + payload.len() + 1; // metadata_type + payload + 0x80
    let leb = uleb128(size_field as u64);
    let mut out = Vec::with_capacity(1 + leb.len() + 1 + payload.len() + 1);
    out.push((5u8 << 3) | (1u8 << 1)); // OBU_METADATA | has_size_field
    out.extend_from_slice(&leb);
    out.push(METADATA_TYPE_USER_PRIVATE_6);
    out.extend_from_slice(payload);
    out.push(0x80);
    out
}

/// The RFC3339 rendering of `render_metrics_time` (S-024-R52):
/// `YYYY-MM-DDTHH:MM:SS.mmmZ` in UTC, `mmm` = nanoseconds/1,000,000
/// truncated (no rounding). The `(sec, nsec)` pair comes from
/// `os_nstime_to_timespec` (the injected wall-clock base).
///
/// shortcut: years < 1000 are zero-padded to 4 digits (glibc strftime
/// would print them shorter); the BPM vectors all live in year 1970+,
/// <upgrade if sub-1000-year wall clocks enter the test surface>
#[must_use]
pub fn rfc3339_utc(sec: i64, nsec: u64) -> String {
    let days = sec.div_euclid(86400);
    let time_of_day = sec.rem_euclid(86400) as u64;
    let (h, m, s) = (
        time_of_day / 3600,
        (time_of_day / 60) % 60,
        time_of_day % 60,
    );

    // Civil-from-days (Howard Hinnant's algorithm) for the proleptic
    // Gregorian calendar — what gmtime() yields.
    let z = days + 719468;
    let era = z.div_euclid(146097);
    let doe = z.rem_euclid(146097); // [0, 146096]
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; // [0, 399]
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100); // [0, 365]
    let mp = (5 * doy + 2) / 153; // [0, 11]
    let d = doy - (153 * mp + 2) / 5 + 1; // [1, 31]
    let mo = if mp < 10 { mp + 3 } else { mp - 9 }; // [1, 12]
    let y = if mo <= 2 { y + 1 } else { y };

    format!(
        "{y:04}-{:02}-{:02}T{h:02}:{m:02}:{s:02}.{:03}Z",
        mo,
        d,
        nsec / 1_000_000
    )
}

/// The TS payload (S-024 §4.8 / R53): `uuid_ts | (n-1) | { 01 | tag |
/// string 00 }*n` for the valid timestamps in the order CTS, FER, FERC,
/// PIR; the count byte holds `n-1` in its low 4 bits (high bits 0).
#[must_use]
pub fn ts_payload(timestamps: &[(u8, &str)]) -> Vec<u8> {
    let mut out = Vec::with_capacity(UUID_TS.len() + 1 + timestamps.len() * 27);
    out.extend_from_slice(&UUID_TS);
    out.push((timestamps.len() - 1) as u8 & 0x0f);
    for &(tag, string) in timestamps {
        out.push(1); // BPM_TS_RFC3339
        out.push(tag);
        out.extend_from_slice(string.as_bytes());
        out.push(0);
    }
    out
}

/// The SM payload (S-024 §4.8 / R53): `uuid_sm | 00 | 01 | 04 | PIR 00 |
/// 03 | 01 u32(rendered) | 02 u32(lagged) | 03 u32(dropped) |
/// 04 u32(session_output)` (counters big-endian).
#[must_use]
pub fn sm_payload(
    pir: &str,
    rendered: u32,
    lagged: u32,
    dropped: u32,
    session_output: u32,
) -> Vec<u8> {
    let mut out = Vec::with_capacity(UUID_SM.len() + 32);
    out.extend_from_slice(&UUID_SM);
    out.push(0); // one timestamp
    out.push(1); // BPM_TS_RFC3339
    out.push(TS_EVENT_PIR);
    out.extend_from_slice(pir.as_bytes());
    out.push(0);
    out.push(3); // four counters
    for (tag, value) in [
        (1u8, rendered),
        (2, lagged),
        (3, dropped),
        (4, session_output),
    ] {
        out.push(tag);
        out.extend_from_slice(&value.to_be_bytes());
    }
    out
}

/// The ERM payload (S-024 §4.8 / R53): `uuid_erm | 00 | 01 | 04 | PIR 00 |
/// 02 | 01 u32(rend_input) | 02 u32(rend_skipped) | 03 u32(rend_output)`.
#[must_use]
pub fn erm_payload(pir: &str, rend_input: u32, rend_skipped: u32, rend_output: u32) -> Vec<u8> {
    let mut out = Vec::with_capacity(UUID_ERM.len() + 24);
    out.extend_from_slice(&UUID_ERM);
    out.push(0); // one timestamp
    out.push(1); // BPM_TS_RFC3339
    out.push(TS_EVENT_PIR);
    out.extend_from_slice(pir.as_bytes());
    out.push(0);
    out.push(2); // three counters
    for (tag, value) in [(1u8, rend_input), (2, rend_skipped), (3, rend_output)] {
        out.push(tag);
        out.extend_from_slice(&value.to_be_bytes());
    }
    out
}

/// The codec framing of `process_metrics` (S-024 §4.8 / R55): for each of
/// the three payloads (in the order TS, SM, ERM) append
///
/// - `h264`: `00 00 00 01 | S(p)`;
/// - `hevc`: `00 00 01 | ((40<<1) | (h0 & 1)) | h1 | S(p)[1..]` where `h0,
///   h1` are the two bytes after the Annex-B start code (R56: a packet
///   without a `00 00 01` / `00 00 00 01` start code yields `None` — the
///   caller leaves the packet unchanged);
/// - `av1`: `metadata_obu(p, 0x06)` around the raw (un-escaped) payload;
/// - unknown codecs: the original bytes as an identical copy (Q-11).
///
/// Returns `None` only for the HEVC-without-start-code case (R56).
#[must_use]
pub fn frame_packet(codec: &str, original: &[u8], payloads: [&[u8]; 3]) -> Option<Vec<u8>> {
    match codec {
        "h264" => {
            let mut out = original.to_vec();
            for p in payloads {
                out.extend_from_slice(&[0, 0, 0, 1]);
                out.extend_from_slice(&sei_nal(p));
            }
            Some(out)
        }
        "hevc" => {
            // Skip past the Annex-B start code (3-byte first, then 4-byte,
            // as in the reference).
            let header_idx = if original.len() >= 3 && &original[..3] == b"\x00\x00\x01" {
                3
            } else if original.len() >= 4 && &original[..4] == b"\x00\x00\x00\x01" {
                4
            } else {
                return None;
            };
            // The 2-byte NAL unit header needs both bytes to be present.
            if original.len() < header_idx + 2 {
                return None;
            }
            let h0 = original[header_idx];
            let h1 = original[header_idx + 1];
            let mut out = original.to_vec();
            for p in payloads {
                let sei = sei_nal(p);
                out.extend_from_slice(&[0, 0, 1]);
                out.push((40u8 << 1) | (h0 & 1));
                out.push(h1);
                out.extend_from_slice(&sei[1..]);
            }
            Some(out)
        }
        "av1" => {
            let mut out = original.to_vec();
            for p in payloads {
                out.extend_from_slice(&metadata_obu(p));
            }
            Some(out)
        }
        _ => Some(original.to_vec()), // Q-11: identical copy
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// `bpm-avc-basic` vector bytes: the three SEI NALs (unescaped here).
    const CTS: &str = "1970-01-01T00:00:01.000Z";
    const FER: &str = "1970-01-01T00:00:01.010Z";
    const FERC: &str = "1970-01-01T00:00:01.020Z";
    const PIR: &str = "1970-01-01T00:00:01.030Z";

    #[test]
    fn rfc3339_vectors() {
        // runner time model: nstime N -> wall N ns after 1970-01-01T00:00:00Z
        assert_eq!(rfc3339_utc(1, 0), "1970-01-01T00:00:01.000Z");
        assert_eq!(rfc3339_utc(1, 10_000_000), "1970-01-01T00:00:01.010Z");
        assert_eq!(rfc3339_utc(1, 30_999_999), "1970-01-01T00:00:01.030Z"); // truncated
        assert_eq!(rfc3339_utc(86_399, 999_999_999), "1970-01-01T23:59:59.999Z");
        assert_eq!(rfc3339_utc(5_097_600, 0), "1970-03-01T00:00:00.000Z"); // day 59 = 31+28
        assert_eq!(rfc3339_utc(31_536_000, 0), "1971-01-01T00:00:00.000Z");
    }

    #[test]
    fn escape_vector_bytes() {
        // R54: "00 00 00 64" (the u32 counter 100) escapes to
        // "00 00 03 00 64" — the bpm-avc-basic SM SEI (4 escapes total,
        // escaped length 69, NAL length 73).
        let payload = sm_payload(PIR, 100, 3, 1, 90);
        let sei = sei_nal(&payload);
        assert_eq!(sei.len(), 2 + 1 + 69 + 1);
        assert!(sei.starts_with(&[6, 5, 65]));
        assert!(sei.ends_with(&[0x80]));
        assert!(sei.windows(6).any(|w| w == [0, 0, 3, 0, 0x64, 2]));
    }

    #[test]
    fn sm_payload_vector() {
        // the unescaped bpm-avc-basic SM payload (65 bytes)
        let expected = hex_to_bytes(concat!(
            "ca60e71c6a8b4388a377151df7bf8ac2",
            "000104",
            "313937302d30312d30315430303a30303a30312e3033305a",
            "00",
            "03",
            "0100000064",
            "0200000003",
            "0300000001",
            "040000005a",
        ));
        assert_eq!(sm_payload(PIR, 100, 3, 1, 90), expected);
        // the unescaped bpm-avc-basic ERM payload (60 bytes)
        let expected = hex_to_bytes(concat!(
            "f1fbc1d5101e4fb5a61eb8ce3c07b8c0",
            "000104",
            "313937302d30312d30315430303a30303a30312e3033305a",
            "00",
            "02",
            "0100000064",
            "0200000002",
            "0300000062",
        ));
        assert_eq!(erm_payload(PIR, 100, 2, 98), expected);
    }

    #[test]
    fn ts_payload_vector() {
        let payload = ts_payload(&[
            (TS_EVENT_CTS, CTS),
            (TS_EVENT_FER, FER),
            (TS_EVENT_FERC, FERC),
            (TS_EVENT_PIR, PIR),
        ]);
        // the bpm-avc-basic TS payload (125 bytes, unescaped)
        assert_eq!(payload.len(), 125);
        assert_eq!(
            &payload[..17],
            &[
                0x0a, 0xec, 0xff, 0xe7, 0x52, 0x72, 0x4e, 0x2f, 0xa6, 0x2f, 0xd1, 0x9c, 0xd6, 0x1a,
                0x93, 0xb5, 3
            ]
        );
    }

    #[test]
    fn av1_obu_vector() {
        // bpm-av1: 2a | 7f (=125+2) | 06 | payload | 80
        let payload = ts_payload(&[
            (TS_EVENT_CTS, CTS),
            (TS_EVENT_FER, FER),
            (TS_EVENT_FERC, FERC),
            (TS_EVENT_PIR, PIR),
        ]);
        let obu = metadata_obu(&payload);
        assert_eq!(&obu[..4], [0x2a, 0x7f, 0x06, 0x0a]);
        assert_eq!(&obu[obu.len() - 1..], [0x80]);
        assert_eq!(obu.len(), 1 + 1 + 1 + 125 + 1);
    }

    #[test]
    fn frame_packet_codecs() {
        let p = [
            ts_payload(&[(TS_EVENT_CTS, CTS)]),
            sm_payload(PIR, 1, 2, 3, 4),
            erm_payload(PIR, 5, 6, 7),
        ];
        let refs: [&[u8]; 3] = [&p[0], &p[1], &p[2]];

        let avc = frame_packet("h264", b"\x00\x00\x00\x01\x65\xaa\xbb\xcc", refs).unwrap();
        assert!(avc.starts_with(b"\x00\x00\x00\x01\x65\xaa\xbb\xcc\x00\x00\x00\x01\x06\x05"));

        // hevc: 3-byte start code, header bytes 41 01
        let hevc = frame_packet("hevc", b"\x00\x00\x01\x41\x01\x02", refs).unwrap();
        let appended = &hevc[6..];
        assert!(appended.starts_with(&[0, 0, 1, (40u8 << 1) | 1, 0x01, 5])); // S(p)[1..] starts with 05
        // hevc without a start code: unchanged (None)
        assert!(frame_packet("hevc", b"\x01\x02\x03\x04", refs).is_none());

        // av1: TS(CTS only) payload = 16 + 1 + 27 = 44 bytes; OBU size
        // field = 44 + 2 = 46
        let av1 = frame_packet("av1", b"\x12\x00", refs).unwrap();
        assert_eq!(&av1[..4], [0x12, 0x00, 0x2a, 46]);

        // Q-11: unknown codec -> identical copy
        let other = frame_packet("vp9", b"\xaa\xbb", refs).unwrap();
        assert_eq!(other, b"\xaa\xbb");
    }

    fn hex_to_bytes(hex: &str) -> Vec<u8> {
        (0..hex.len())
            .step_by(2)
            .map(|i| u8::from_str_radix(&hex[i..i + 2], 16).unwrap())
            .collect()
    }
}
