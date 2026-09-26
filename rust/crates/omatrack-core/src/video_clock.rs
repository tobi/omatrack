//! Exact telemetry-to-player time mapping, copied from the recording so it
//! outlives the decoded source (port of `VideoClock` in `TelemetryEngine`).
//!
//! Telemetry time is integer nanoseconds relative to the file's first
//! sample; player time is MP4 presentation time; the only conversion is
//! `presentation = telemetry + signed per-video offset`. Frame lookup uses the
//! presentation-order timestamp table, never `seconds * nominal fps`.

use std::io::Read;
use std::path::Path;

/// One video file linked to a telemetry recording.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct VideoFileReference {
    pub filename: String,
    pub index: u32,
    pub blake3: Option<[u8; 32]>,
    pub frame_count: u64,
    pub presentation_offset_ns: Option<i64>,
}

/// Telemetry <-> presentation mapping for the linked video(s).
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct VideoClock {
    pub presentation_offset_ns: Option<i64>,
    pub presentation_times_ns: Vec<u64>,
    pub files: Vec<VideoFileReference>,
}

/// `time + offset` without wrapping; `None` on overflow/underflow.
#[expect(
    clippy::cast_sign_loss,
    reason = "The branch checks the signed offset is nonnegative before adding its magnitude."
)]
pub fn shifted_time(time: u64, offset: i64) -> Option<u64> {
    if offset >= 0 {
        time.checked_add(offset as u64)
    } else {
        time.checked_sub(offset.unsigned_abs())
    }
}

impl VideoClock {
    /// Linked-video metadata for an exact catalog filename.
    pub fn file_named(&self, filename: &str) -> Option<&VideoFileReference> {
        self.files.iter().find(|file| file.filename == filename)
    }

    /// True when frames and at least one offset are known.
    pub fn valid(&self) -> bool {
        !self.presentation_times_ns.is_empty()
            && (self.presentation_offset_ns.is_some()
                || self
                    .files
                    .iter()
                    .any(|file| file.presentation_offset_ns.is_some()))
    }

    fn offset_for_file(&self, file_index: Option<u32>) -> Option<i64> {
        if let Some(index) = file_index
            && let Some(file) = self.files.iter().find(|file| file.index == index)
            && file.presentation_offset_ns.is_some()
        {
            return file.presentation_offset_ns;
        }
        self.presentation_offset_ns
    }

    /// Player presentation time for file-relative telemetry nanoseconds.
    pub fn presentation_time_ns(&self, telemetry_ns: u64, file_index: Option<u32>) -> Option<u64> {
        shifted_time(telemetry_ns, self.offset_for_file(file_index)?)
    }

    /// File-relative telemetry nanoseconds for a player presentation time.
    pub fn telemetry_time_ns(&self, presentation_ns: u64, file_index: Option<u32>) -> Option<u64> {
        let offset = self.offset_for_file(file_index)?;
        if offset == i64::MIN {
            return None;
        }
        shifted_time(presentation_ns, -offset)
    }

    /// Presentation-order frame shown at telemetry time.
    pub fn frame_at(&self, telemetry_ns: u64) -> Option<u64> {
        if self.presentation_times_ns.is_empty() {
            return None;
        }
        let presentation = self.presentation_time_ns(telemetry_ns, None)?;
        let after = self
            .presentation_times_ns
            .partition_point(|t| *t <= presentation);
        Some(after.saturating_sub(1) as u64)
    }

    pub(crate) fn from_source(source: &dyn motorsport_telemetry_core::TelemetrySource) -> Self {
        let presentation_offset_ns = source
            .video_presentation_offset_ns()
            .and_then(|offset| i64::try_from(offset).ok());
        let files = source
            .video_files()
            .iter()
            .map(|video| VideoFileReference {
                filename: if video.filename.contains('\0') {
                    String::new()
                } else {
                    video.filename.clone()
                },
                index: video.index,
                blake3: video.blake3,
                frame_count: video.frame_count,
                presentation_offset_ns: video
                    .presentation_offset_ns
                    .and_then(|offset| i64::try_from(offset).ok()),
            })
            .collect();
        let presentation_times_ns = source
            .video_presentation_times_ns()
            .map(<[u64]>::to_vec)
            .unwrap_or_default();
        Self {
            presentation_offset_ns,
            presentation_times_ns,
            files,
        }
    }
}

/// Full BLAKE3-256 digest of a file (the linked-video identity).
pub fn blake3_file(path: impl AsRef<Path>) -> Option<[u8; 32]> {
    let mut file = std::fs::File::open(path).ok()?;
    let mut hasher = blake3::Hasher::new();
    let mut buffer = vec![0u8; 1 << 16];
    loop {
        let count = file.read(&mut buffer).ok()?;
        if count == 0 {
            break;
        }
        hasher.update(&buffer[..count]);
    }
    Some(*hasher.finalize().as_bytes())
}
