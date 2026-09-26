//! Regression tests for recording identity, manual selection during lap
//! advancement, and applying Preferences to an already active follower.

use std::fmt::Write as _;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::sync::atomic::AtomicBool;
use std::time::{Duration, Instant};

use gpui_kit::component::Root;
use gpui_kit::test::TestWindowExt as _;
use gpui_kit::{App, AppContext as _, Entity, TestAppContext, Window, WindowHandle};
use mpv_player::PlaybackClock;
use omatrack_core::playback::ReferencePlayback;
use omatrack_core::session::IdentityState;
use omatrack_core::{LoadOptions, LoadedLap, Recording, load_lap};
use omatrack_library::Paths;

use super::Transport;
use crate::actions::Role;
use crate::preferences::PreferencesSection;
use crate::state::{AppState, Library, StateOptions};
use crate::sync::{IdentityStatus, VideoMap};
use crate::{AppOptions, Workspace};

/// Only identity hashing reads the companion bytes; no decoder is needed.
fn write_recording(path: &Path, hash: &str) {
    let header = serde_json::json!({
        "mtj": 1, "q": 20_000_000, "dur": 3_000_000_000_u64, "vo": 0,
        "vf": [{"n": "companion.bin", "i": 0, "fc": 30, "b3": hash, "po": 0}],
        "vpts": (0..30_u64).map(|i| i * 100_000_000).collect::<Vec<_>>()
    });
    let laps = serde_json::json!([
        [1, 0, 1_000_000_000, 1],
        [2, 1_000_000_000, 2_000_000_000, 1],
        [3, 2_000_000_000, 3_000_000_000_u64, 1]
    ]);
    let speed = serde_json::json!({"n": "Speed", "u": "km/h", "hz": 50, "v": vec![100.0; 150]});
    std::fs::write(path, format!("{header}\n{laps}\n{speed}\n")).unwrap();
}

fn loaded(recording: &Arc<Recording>, id: i32) -> LoadedLap {
    load_lap(
        recording.clone(),
        id,
        &LoadOptions::default(),
        &AtomicBool::new(false),
    )
    .unwrap()
}

struct Fixture {
    _dir: tempfile::TempDir,
    path: PathBuf,
    recording: Arc<Recording>,
    app: AppState,
    workspace: Entity<Workspace>,
    window: WindowHandle<Root>,
}

impl Fixture {
    fn new(cx: &mut TestAppContext) -> Self {
        let dir = tempfile::tempdir().unwrap();
        let folder = dir.path().join("recordings");
        std::fs::create_dir(&folder).unwrap();
        let companion = folder.join("companion.bin");
        std::fs::write(&companion, b"the recorded companion video").unwrap();
        let mut hash = String::new();
        for byte in omatrack_core::video_clock::blake3_file(&companion).unwrap() {
            write!(&mut hash, "{byte:02x}").unwrap();
        }
        let path = folder.join("Run1.telemetry.jsonl");
        write_recording(&path, &hash);
        let recording = Arc::new(Recording::open(&path).unwrap());
        let paths = Paths::with_roots(
            dir.path().join("config"),
            dir.path().join("cache"),
            dir.path().join("state"),
        );
        let options = StateOptions::isolated(paths).default_library(Some(folder));
        cx.update(|cx| {
            cx.set_reduce_motion(true);
            crate::init_with(AppOptions::isolated(options), cx);
        });
        let window = cx.update(crate::open_main_window).unwrap();
        let workspace = cx.update(|cx| {
            window
                .read(cx)
                .unwrap()
                .view()
                .clone()
                .downcast::<Workspace>()
                .unwrap()
        });
        let app = cx.update(|cx| AppState::global(cx).clone());
        cx.run_until_parked();
        Self {
            _dir: dir,
            path,
            recording,
            app,
            workspace,
            window,
        }
    }

    fn step(&self, cx: &mut TestAppContext, f: impl FnOnce(&mut Window, &mut App)) {
        cx.update_window(self.window.into(), |_, window, cx| {
            window.render_frame(cx);
            f(window, cx);
            window.render_frame(cx);
        })
        .unwrap();
        cx.run_until_parked();
    }

    /// Scan and load through the real Session, then supply a mock primary clock.
    fn load_session(&self, cx: &mut TestAppContext) -> gpui_kit::SharedString {
        cx.update(|cx| self.app.library.update(cx, Library::rescan));
        cx.run_until_parked();
        let session = cx.update(|cx| {
            self.app
                .library
                .read(cx)
                .snapshot()
                .session_for_path(&self.path)
                .unwrap()
                .id
                .clone()
        });
        cx.update(|cx| {
            self.app.session.update(cx, |session_state, cx| {
                session_state.set_primary(session.clone().into(), 2, cx);
            });
        });
        cx.run_until_parked();
        cx.update(|cx| {
            assert_eq!(
                self.app
                    .session
                    .read(cx)
                    .primary()
                    .unwrap()
                    .loaded()
                    .unwrap()
                    .lap_id(),
                2
            );
            self.app.video.update(cx, |video, cx| {
                video.attach_external_clock(PlaybackClock::new(), VideoMap::Offset(0.0), cx);
            });
        });
        session.into()
    }

    /// Run the primary past lap 2's end, triggering the ordinary countdown.
    fn countdown(&self, cx: &mut TestAppContext) {
        cx.update(|cx| {
            self.app.video.update(cx, |video, cx| {
                video.toggle_play(cx);
                let end = video
                    .timeline(Role::Primary)
                    .unwrap()
                    .video_at_fraction(1.0)
                    .unwrap();
                let now = Instant::now();
                let clock = video.clock(Role::Primary).unwrap();
                clock.set_speed(0.0, now);
                clock.seek_started(end + 0.1, now);
                clock.seek_finished(now);
                video.sync_frame(now, cx);
                assert_eq!(video.countdown(), Some(3));
            });
        });
        cx.run_until_parked();
    }

    /// Cross a lap boundary after prefetch; leave the Session load pending.
    fn adopt_next(&self, cx: &mut TestAppContext) -> PlaybackClock {
        cx.update(|cx| {
            self.app.video.update(cx, |video, cx| {
                video.toggle_continuous(cx);
                video.toggle_play(cx);
                let clock = video.clock(Role::Primary).unwrap();
                let now = Instant::now();
                clock.set_speed(0.0, now);
                clock.seek_started(1.8, now);
                clock.seek_finished(now);
                video.sync_frame(now, cx);
            });
        });
        cx.run_until_parked();
        cx.update(|cx| {
            self.app.video.update(cx, |video, cx| {
                assert!(video.prefetched.is_some());
                let clock = video.clock(Role::Primary).unwrap();
                let now = Instant::now();
                clock.seek_started(2.1, now);
                clock.seek_finished(now);
                video.sync_frame(now, cx);
                assert_eq!(video.adopting, Some(3));
                clock
            })
        })
    }
}

#[gpui_kit::test]
fn identity_finishes_after_a_lap_change_and_role_swap(cx: &mut TestAppContext) {
    let fixture = Fixture::new(cx);
    let first = loaded(&fixture.recording, 1);
    let second = loaded(&fixture.recording, 2);
    cx.update(|cx| {
        fixture.app.video.update(cx, |video, cx| {
            // The player is still starting when both lap selections arrive.
            video.primary.pending_open = Some((first.video().unwrap().path.clone(), 0.0));
            video.bind(Some(&first), cx);
            assert!(video.primary.identity_check.is_some());
            video.bind(Some(&second), cx);
            video.swap_decks(cx);
        });
    });
    cx.run_until_parked();
    cx.update(|cx| {
        let video = fixture.app.video.read(cx);
        assert_eq!(video.reference.lap.as_ref().unwrap().lap_id(), 2);
        assert_eq!(
            video.reference.identity,
            IdentityStatus::Trusted(IdentityState::VerifiedHash)
        );
        assert!(video.reference.identity_check.is_none());
    });
}

fn check_replacement_recording(cx: &mut TestAppContext, same_path: bool) {
    let fixture = Fixture::new(cx);
    let first = loaded(&fixture.recording, 1);
    let path = if same_path {
        fixture.path.clone()
    } else {
        fixture.path.with_file_name("Run2.telemetry.jsonl")
    };
    write_recording(&path, &"00".repeat(32));
    let replacement = Arc::new(Recording::open(path).unwrap());
    let second = loaded(&replacement, 1);
    cx.update(|cx| {
        fixture.app.video.update(cx, |video, cx| {
            video.primary.pending_open = Some((first.video().unwrap().path.clone(), 0.0));
            video.bind(Some(&first), cx);
            video.bind(Some(&second), cx);
        });
    });
    cx.run_until_parked();
    cx.update(|cx| {
        let video = fixture.app.video.read(cx);
        assert!(Arc::ptr_eq(
            video.primary.lap.as_ref().unwrap().recording(),
            &replacement
        ));
        assert!(matches!(
            video.primary.identity,
            IdentityStatus::Untrusted {
                state: IdentityState::Mismatch,
                ..
            }
        ));
        assert!(video.primary.identity_check.is_none());
    });
}

#[gpui_kit::test]
fn recordings_sharing_a_video_verify_their_own_identity(cx: &mut TestAppContext) {
    check_replacement_recording(cx, false);
}

#[gpui_kit::test]
fn a_reparsed_recording_does_not_reuse_the_previous_identity(cx: &mut TestAppContext) {
    check_replacement_recording(cx, true);
}

#[gpui_kit::test]
fn an_external_clock_cancels_a_pending_identity_check(cx: &mut TestAppContext) {
    let fixture = Fixture::new(cx);
    let lap = loaded(&fixture.recording, 1);
    cx.update(|cx| {
        fixture.app.video.update(cx, |video, cx| {
            video.primary.pending_open = Some((lap.video().unwrap().path.clone(), 0.0));
            video.bind(Some(&lap), cx);
            assert!(video.primary.identity_check.is_some());
            video.attach_external_clock(PlaybackClock::new(), VideoMap::Offset(0.0), cx);
        });
    });
    cx.run_until_parked();
    cx.update(|cx| {
        let video = fixture.app.video.read(cx);
        assert_eq!(video.primary.identity, IdentityStatus::External);
        assert!(video.primary.identity_check.is_none());
        assert!(video.is_synced());
    });
}

#[gpui_kit::test]
fn manual_selection_cancels_countdown_before_the_lap_loads(cx: &mut TestAppContext) {
    let fixture = Fixture::new(cx);
    let session_id = fixture.load_session(cx);
    fixture.countdown(cx);
    cx.update(|cx| {
        fixture.app.session.update(cx, |session, cx| {
            session.set_primary(session_id, 1, cx);
        });
    });
    cx.update(|cx| {
        assert!(fixture.app.session.read(cx).primary().unwrap().is_loading());
        let video = fixture.app.video.read(cx);
        assert_eq!(
            video.primary.lap.as_ref().unwrap().lap_id(),
            2,
            "the old picture stays while loading"
        );
        assert_eq!(
            video.advancing_to(),
            None,
            "the selection cancels autoplay immediately"
        );
    });
    cx.executor().advance_clock(Duration::from_secs(2));
    cx.run_until_parked();
    cx.update(|cx| {
        let video = fixture.app.video.read(cx);
        assert_eq!(video.primary.lap.as_ref().unwrap().lap_id(), 1);
        assert_eq!(video.advancing_to(), None);
        assert!(!video.is_playing());
    });
}

#[gpui_kit::test]
fn manually_selecting_the_pending_automatic_lap_cancels_its_resume(cx: &mut TestAppContext) {
    let fixture = Fixture::new(cx);
    let session_id = fixture.load_session(cx);
    fixture.countdown(cx);
    cx.update(|cx| {
        fixture.app.video.update(cx, |video, cx| {
            for _ in 0..3 {
                video.tick_countdown(cx);
            }
        });
    });
    cx.update(|cx| {
        assert_eq!(fixture.app.video.read(cx).advancing_to(), Some(3));
        assert!(fixture.app.session.read(cx).primary().unwrap().is_loading());
        fixture
            .app
            .session
            .update(cx, |session, cx| session.set_primary(session_id, 3, cx));
    });
    assert_eq!(
        cx.update(|cx| fixture.app.video.read(cx).advancing_to()),
        None
    );
    cx.run_until_parked();
    cx.update(|cx| {
        let video = fixture.app.video.read(cx);
        assert_eq!(video.primary.lap.as_ref().unwrap().lap_id(), 3);
        assert!(
            !video.is_playing(),
            "a manual selection must not inherit an automatic resume"
        );
    });
}

#[gpui_kit::test]
fn clicking_the_pending_lap_in_the_filmstrip_cancels_its_resume(cx: &mut TestAppContext) {
    let fixture = Fixture::new(cx);
    fixture.load_session(cx);
    fixture.countdown(cx);
    cx.update(|cx| {
        fixture.app.video.update(cx, |video, cx| {
            for _ in 0..3 {
                video.tick_countdown(cx);
            }
        });
    });
    cx.update_window(fixture.window.into(), |_, window, cx| {
        assert!(fixture.app.session.read(cx).primary().unwrap().is_loading());
        window.render_frame(cx);
        window.click(("lap-strip-cell", 3_u32), cx);
    })
    .unwrap();
    assert_eq!(
        cx.update(|cx| fixture.app.video.read(cx).advancing_to()),
        None
    );
    cx.run_until_parked();
    cx.update(|cx| {
        let video = fixture.app.video.read(cx);
        assert_eq!(video.primary.lap.as_ref().unwrap().lap_id(), 3);
        assert!(!video.is_playing());
        assert_eq!(fixture.app.cursor.read(cx).fraction(), Some(0.0));
    });
}

#[gpui_kit::test]
fn binding_another_lap_cancels_an_old_countdown(cx: &mut TestAppContext) {
    let fixture = Fixture::new(cx);
    fixture.load_session(cx);
    fixture.countdown(cx);
    let first = loaded(&fixture.recording, 1);
    cx.update(|cx| {
        fixture
            .app
            .video
            .update(cx, |video, cx| video.bind(Some(&first), cx));
    });
    assert_eq!(cx.update(|cx| fixture.app.video.read(cx).countdown()), None);
}

#[gpui_kit::test]
fn automatic_countdown_still_selects_and_resumes_the_next_lap(cx: &mut TestAppContext) {
    let fixture = Fixture::new(cx);
    fixture.load_session(cx);
    fixture.countdown(cx);
    for _ in 0..3 {
        cx.executor().advance_clock(Duration::from_millis(500));
        cx.run_until_parked();
    }
    cx.update(|cx| {
        let video = fixture.app.video.read(cx);
        assert_eq!(video.primary.lap.as_ref().unwrap().lap_id(), 3);
        assert_eq!(video.advancing_to(), None);
        assert!(video.is_playing());
    });
}

#[gpui_kit::test]
fn continuous_adoption_survives_the_sessions_automatic_selection(cx: &mut TestAppContext) {
    let fixture = Fixture::new(cx);
    fixture.load_session(cx);
    let clock = fixture.adopt_next(cx);
    let position = clock.sample().position;
    assert!(cx.update(|cx| fixture.app.session.read(cx).primary().unwrap().is_loading()));
    cx.run_until_parked();
    cx.update(|cx| {
        let video = fixture.app.video.read(cx);
        assert_eq!(video.primary.lap.as_ref().unwrap().lap_id(), 3);
        assert!(video.adopting.is_none());
        assert!(video.is_playing());
        assert!(
            (clock.sample().position - position).abs() < 1e-9,
            "adoption must not seek"
        );
    });
}

#[gpui_kit::test]
fn manual_selection_cancels_a_pending_continuous_adoption(cx: &mut TestAppContext) {
    let fixture = Fixture::new(cx);
    let session_id = fixture.load_session(cx);
    fixture.adopt_next(cx);
    cx.update(|cx| {
        fixture
            .app
            .session
            .update(cx, |session, cx| session.set_primary(session_id, 1, cx));
    });
    cx.update(|cx| {
        assert!(fixture.app.session.read(cx).primary().unwrap().is_loading());
        let video = fixture.app.video.read(cx);
        assert!(video.adopting.is_none());
        assert!(video.prefetch.is_none());
        assert!(video.prefetched.is_none());
    });
    cx.run_until_parked();
    cx.update(|cx| {
        assert_eq!(
            fixture
                .app
                .video
                .read(cx)
                .primary
                .lap
                .as_ref()
                .unwrap()
                .lap_id(),
            1
        );
        assert_eq!(
            fixture
                .app
                .session
                .read(cx)
                .primary()
                .unwrap()
                .lap_ref()
                .lap(),
            1
        );
    });
}

#[gpui_kit::test]
fn swapping_roles_cancels_a_countdown(cx: &mut TestAppContext) {
    let fixture = Fixture::new(cx);
    let session_id = fixture.load_session(cx);
    cx.update(|cx| {
        fixture
            .app
            .session
            .update(cx, |session, cx| session.set_reference(session_id, 1, cx));
    });
    cx.run_until_parked();
    fixture.countdown(cx);
    cx.update(|cx| fixture.app.session.update(cx, crate::state::Session::swap));
    cx.executor().advance_clock(Duration::from_secs(2));
    cx.run_until_parked();
    cx.update(|cx| {
        let video = fixture.app.video.read(cx);
        assert_eq!(video.advancing_to(), None);
        assert_eq!(video.primary.lap.as_ref().unwrap().lap_id(), 1);
        assert!(!video.is_playing());
    });
}

#[gpui_kit::test]
fn preferences_change_the_mode_of_an_already_active_pacer(cx: &mut TestAppContext) {
    let fixture = Fixture::new(cx);
    cx.update(|cx| {
        fixture.app.video.update(cx, |video, cx| {
            video.reference.transport = Some(Transport::External(PlaybackClock::new()));
            video.rebuild_pacer(cx);
            assert_eq!(
                video.pacer.as_ref().unwrap().mode(),
                ReferencePlayback::Corners
            );
        });
    });
    fixture.step(cx, |window, cx| {
        fixture
            .workspace
            .update(cx, |workspace, cx| workspace.open_preferences(window, cx));
    });
    fixture.step(cx, |window, cx| {
        fixture
            .workspace
            .read(cx)
            .preferences()
            .unwrap()
            .clone()
            .update(cx, |view, cx| {
                view.select_section(PreferencesSection::Video, window, cx);
            });
    });
    fixture.step(cx, |window, cx| {
        window.click("prefs-reference-playback", cx);
    });
    fixture.step(cx, |window, cx| {
        window.press("down", cx);
        window.press("down", cx);
        window.press("enter", cx);
    });
    cx.update(|cx| {
        assert_eq!(
            fixture.app.video.read(cx).reference_playback(cx),
            ReferencePlayback::Recording
        );
        assert_eq!(
            fixture.app.video.read(cx).pacer.as_ref().unwrap().mode(),
            ReferencePlayback::Recording
        );
    });
}
