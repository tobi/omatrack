//! Whether a bound video is provably the one its telemetry describes.
//!
//! Sync applies a video's clock only to a trusted video: the recording
//! itself (an AiM MP4) or a companion whose BLAKE3 matches the catalog.
//! Anything else keeps playing but does not drive or follow the cursor, and
//! the panel says why — never a silent seek on a clock that may not apply.
//! Hashing reads the whole file, so it runs on a worker ([`check`]).

use std::path::Path;

use gpui_kit::SharedString;
use omatrack_core::recording::Recording;
use omatrack_core::session::{IdentityState, VideoBinding, verify_video_identity};

/// What is known about a bound video's identity.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum IdentityStatus {
    /// Nothing is bound.
    None,
    /// The BLAKE3 check is running.
    Checking,
    /// The video is the recording itself or its hash matches.
    Trusted(IdentityState),
    /// An external clock drives the timeline; its owner vouches for it.
    External,
    /// Missing or mismatched identity: sync is off for this video.
    Untrusted {
        state: IdentityState,
        message: SharedString,
    },
}

impl IdentityStatus {
    /// Whether the video's clock may drive or follow the cursor.
    pub fn is_trusted(&self) -> bool {
        matches!(self, Self::Trusted(_) | Self::External)
    }

    pub fn is_checking(&self) -> bool {
        matches!(self, Self::Checking)
    }

    /// Why the video is not trusted, when it is not.
    pub fn warning(&self) -> Option<&SharedString> {
        match self {
            Self::Untrusted { message, .. } => Some(message),
            _ => None,
        }
    }

    /// The status of a freshly loaded binding. `NotChecked` needs the
    /// worker ([`IdentityStatus::Checking`]).
    pub fn of_binding(binding: &VideoBinding) -> Self {
        Self::from_state(binding.identity, binding.warning.as_deref())
    }

    fn from_state(state: IdentityState, warning: Option<&str>) -> Self {
        match state {
            IdentityState::ExactSource | IdentityState::VerifiedHash => Self::Trusted(state),
            IdentityState::NotChecked => Self::Checking,
            IdentityState::Unverified | IdentityState::Mismatch => Self::Untrusted {
                state,
                message: warning
                    .unwrap_or(match state {
                        IdentityState::Mismatch => "The video doesn’t match the telemetry.",
                        _ => "The video identity couldn’t be verified.",
                    })
                    .to_string()
                    .into(),
            },
        }
    }

    /// One line for a notification after an explicit check.
    pub fn summary(&self) -> SharedString {
        match self {
            Self::None => "No video is bound.".into(),
            Self::Checking => "Checking the video identity…".into(),
            Self::External => "The video clock is supplied externally.".into(),
            Self::Trusted(IdentityState::ExactSource) => {
                "The video is the recording itself; its timing is exact.".into()
            }
            Self::Trusted(_) => "The video matches the telemetry (BLAKE3).".into(),
            Self::Untrusted { message, .. } => {
                format!("{message} Video sync is off for it.").into()
            }
        }
    }
}

/// Verify `video` against `recording`'s catalog identity, hashing a
/// companion file. Blocking: call from a background task.
pub fn check(recording: &Recording, video: &Path) -> IdentityStatus {
    let (state, _, warning) = verify_video_identity(recording, video, true);
    IdentityStatus::from_state(state, warning.as_deref())
}
