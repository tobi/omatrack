//! Geometry of the fullscreen video stage, pure so it is tested as
//! arithmetic.
//!
//! The stage is the whole window on black, laid out as one centred column
//! on a grid ([`plan`]):
//!
//! ```text
//!   filmstrip lane        (the laps, above the pictures as in the dock)
//!   delta lane            P driver · lap ·······  Δ  ······· lap · driver R
//!   pictures              touching the delta lane
//!   telemetry band        just below the pictures, 65% of their width
//!   (controls float at the bottom and hide when idle)
//! ```
//!
//! The pictures take the largest size at which the whole column fits, so
//! the letterbox carries the lanes instead of staying empty, and nothing
//! is drawn over a picture's burned-in overlays. Layouts:
//!
//! - **Split** (1): primary and reference side by side with a 2 px seam;
//! - **Primary + inset** (2) / **Reference + inset** (3): the large
//!   picture with the other beside it at [`INSET_SCALE`] of its height, top
//!   aligned (right of it in 2, left in 3), never over it;
//! - **Primary** (4) / **Reference** (5): one picture.
//!
//! The band can be dragged anywhere on the stage (`video.hud_position`);
//! without a stored place it sits in its lane.

use crate::actions::Role;
use crate::state::ComposeLayout;

/// The seam between split pictures, px.
pub const SPLIT_GAP: f32 = 2.;
/// The inset picture's height as a share of the large one's.
pub const INSET_SCALE: f32 = 0.42;
/// Space between the lanes and around the stage edge, px.
pub const LANE_MARGIN: f32 = 8.;
/// The telemetry band's width as a share of the pictures' combined width.
pub const BAND_SHARE: f32 = 0.65;
/// The band's height / width (the Qt 1000:210).
pub const BAND_ASPECT: f32 = 0.21;
/// The band never grows beyond this width, px.
pub const BAND_MAX_WIDTH: f32 = 1400.;

/// An axis-aligned rectangle in logical pixels, stage-relative.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Rect {
    pub x: f32,
    pub y: f32,
    pub w: f32,
    pub h: f32,
}

impl Rect {
    pub const fn new(x: f32, y: f32, w: f32, h: f32) -> Self {
        Self { x, y, w, h }
    }

    pub fn right(&self) -> f32 {
        self.x + self.w
    }

    pub fn bottom(&self) -> f32 {
        self.y + self.h
    }

    pub fn center_x(&self) -> f32 {
        self.x + self.w * 0.5
    }

    #[cfg(test)]
    fn overlaps(&self, other: &Rect) -> bool {
        self.x < other.right()
            && other.x < self.right()
            && self.y < other.bottom()
            && other.y < self.bottom()
    }
}

/// One picture on the stage.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Pane {
    pub role: Role,
    pub rect: Rect,
    /// The smaller picture of layouts 2 and 3.
    pub inset: bool,
}

/// The heights of the stage's lanes, px (0 hides a lane).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Lanes {
    /// The filmstrip.
    pub strip: f32,
    /// The delta lane over the pictures.
    pub delta: f32,
    /// The floating controls at the bottom.
    pub controls: f32,
    /// Whether the telemetry band is shown.
    pub band: bool,
}

/// Where everything sits on the stage.
#[derive(Clone, Debug, PartialEq)]
pub struct StagePlan {
    /// The pictures, large first.
    pub panes: Vec<Pane>,
    /// The pictures' bounding box.
    pub pictures: Rect,
    /// The filmstrip lane (zero height without a strip).
    pub strip: Rect,
    /// The delta lane, directly on top of the pictures.
    pub delta: Rect,
    /// The band's lane below the pictures (zero size when hidden).
    pub band: Rect,
}

impl StagePlan {
    /// The rectangle of `role`'s picture.
    pub fn pane(&self, role: Role) -> Option<Rect> {
        self.panes
            .iter()
            .find(|pane| pane.role == role)
            .map(|pane| pane.rect)
    }
}

fn sane_aspect(aspect: f32) -> f32 {
    if aspect.is_finite() && aspect > 0. {
        aspect
    } else {
        16. / 9.
    }
}

/// Lay out `layout` on a `width` x `height` stage with `lanes`.
/// `aspect(role)` is each video's width / height.
pub fn plan(
    layout: ComposeLayout,
    width: f32,
    height: f32,
    aspect: impl Fn(Role) -> f32,
    lanes: Lanes,
) -> StagePlan {
    let (w, h) = (width.max(0.), height.max(0.));
    let m = LANE_MARGIN;
    // The pictures' block is `c * ph + g` wide for pictures `ph` tall.
    let (c, g, max_w) = match layout {
        ComposeLayout::Split => (
            sane_aspect(aspect(Role::Primary)) + sane_aspect(aspect(Role::Reference)),
            SPLIT_GAP,
            w,
        ),
        ComposeLayout::PrimaryWithReferenceInset | ComposeLayout::ReferenceWithPrimaryInset => {
            let (large, small) = large_small(layout);
            (
                sane_aspect(aspect(large)) + INSET_SCALE * sane_aspect(aspect(small)),
                m,
                (w - 2. * m).max(0.),
            )
        }
        ComposeLayout::PrimaryOnly => (sane_aspect(aspect(Role::Primary)), 0., w),
        ComposeLayout::ReferenceOnly => (sane_aspect(aspect(Role::Reference)), 0., w),
    };
    let top = if lanes.strip > 0. {
        m + lanes.strip + m
    } else {
        m
    } + lanes.delta;
    let bottom = m + lanes.controls;
    let k = if lanes.band {
        BAND_SHARE * BAND_ASPECT
    } else {
        0.
    };
    let band_gap = if lanes.band { m } else { 0. };
    let room = (h - top - bottom - band_gap).max(0.);
    // Tallest pictures whose block fits the width and whose column (lanes,
    // pictures, band) fits the height; the band's width caps at
    // BAND_MAX_WIDTH.
    let by_width = ((max_w - g) / c).max(0.);
    let by_height = ((room - k * g) / (1. + k * c)).max(0.);
    let by_band_cap = if lanes.band {
        (room - BAND_MAX_WIDTH * BAND_ASPECT).max(0.)
    } else {
        0.
    };
    let ph = by_width.min(by_height.max(by_band_cap)).floor();
    let block_w = (c * ph + g).min(max_w);
    let band_w = if lanes.band {
        (block_w * BAND_SHARE).min(BAND_MAX_WIDTH).round()
    } else {
        0.
    };
    let band_h = (band_w * BAND_ASPECT).round();
    let column = top + ph + band_gap + band_h + bottom;
    let y0 = ((h - column) * 0.5).max(0.).round();
    let pictures = Rect::new(((w - block_w) * 0.5).round(), y0 + top, block_w, ph);
    let panes = match layout {
        ComposeLayout::Split => {
            let pw = |role: Role| (sane_aspect(aspect(role)) * ph).round();
            let (left_w, right_w) = (pw(Role::Primary), pw(Role::Reference));
            let seam = (pictures.x + left_w).min(w);
            vec![
                Pane {
                    role: Role::Primary,
                    rect: Rect::new(pictures.x, pictures.y, left_w, ph),
                    inset: false,
                },
                Pane {
                    role: Role::Reference,
                    rect: Rect::new(seam + SPLIT_GAP, pictures.y, right_w, ph),
                    inset: false,
                },
            ]
        }
        ComposeLayout::PrimaryWithReferenceInset | ComposeLayout::ReferenceWithPrimaryInset => {
            let (large, small) = large_small(layout);
            let large_w = (sane_aspect(aspect(large)) * ph).round();
            let small_h = (ph * INSET_SCALE).round();
            let small_w = (sane_aspect(aspect(small)) * small_h).round();
            let (large_x, small_x) = if large == Role::Primary {
                (pictures.x, pictures.x + large_w + m)
            } else {
                (pictures.right() - large_w, pictures.x)
            };
            vec![
                Pane {
                    role: large,
                    rect: Rect::new(large_x, pictures.y, large_w, ph),
                    inset: false,
                },
                Pane {
                    role: small,
                    rect: Rect::new(small_x, pictures.y, small_w, small_h),
                    inset: true,
                },
            ]
        }
        ComposeLayout::PrimaryOnly | ComposeLayout::ReferenceOnly => {
            let role = if layout == ComposeLayout::PrimaryOnly {
                Role::Primary
            } else {
                Role::Reference
            };
            vec![Pane {
                role,
                rect: pictures,
                inset: false,
            }]
        }
    };
    let lane_x = pictures.x.max(m);
    let lane_w = (pictures.right().min(w - m) - lane_x).max(0.);
    let strip = Rect::new(lane_x, y0 + m, lane_w, lanes.strip.max(0.));
    let delta = Rect::new(
        pictures.x,
        pictures.y - lanes.delta,
        pictures.w,
        lanes.delta,
    );
    let band = Rect::new(
        (pictures.center_x() - band_w * 0.5).round(),
        pictures.bottom() + band_gap,
        band_w,
        band_h,
    );
    StagePlan {
        panes,
        pictures,
        strip,
        delta,
        band,
    }
}

fn large_small(layout: ComposeLayout) -> (Role, Role) {
    match layout {
        ComposeLayout::ReferenceWithPrimaryInset => (Role::Reference, Role::Primary),
        _ => (Role::Primary, Role::Reference),
    }
}

/// How far the band's top-left corner can travel each way on a stage whose
/// bottom `bottom_inset` px are the controls.
pub fn hud_room(stage: (f32, f32), hud: (f32, f32), bottom_inset: f32) -> (f32, f32) {
    (
        (stage.0 - hud.0).max(0.),
        (stage.1 - bottom_inset - hud.1).max(0.),
    )
}

/// The band's top-left corner: at the stored normalized `position` of the
/// space it can reach, else at `default` (its lane).
pub fn hud_origin(
    stage: (f32, f32),
    hud: (f32, f32),
    bottom_inset: f32,
    position: Option<(f32, f32)>,
    default: (f32, f32),
) -> (f32, f32) {
    let (available_x, available_y) = hud_room(stage, hud, bottom_inset);
    match position {
        Some((x, y)) => (x.clamp(0., 1.) * available_x, y.clamp(0., 1.) * available_y),
        None => (
            default.0.clamp(0., available_x),
            default.1.clamp(0., available_y),
        ),
    }
}

/// The normalized position of a band whose top-left corner is at `origin`.
pub fn hud_position(
    stage: (f32, f32),
    hud: (f32, f32),
    bottom_inset: f32,
    origin: (f32, f32),
) -> (f32, f32) {
    let (available_x, available_y) = hud_room(stage, hud, bottom_inset);
    let share = |value: f32, room: f32| {
        if room > 0. {
            (value / room).clamp(0., 1.)
        } else {
            0.5
        }
    };
    (share(origin.0, available_x), share(origin.1, available_y))
}

#[cfg(test)]
mod tests {
    use super::*;

    const WIDE: f32 = 16. / 9.;
    const LANES: Lanes = Lanes {
        strip: 60.,
        delta: 64.,
        controls: 44.,
        band: true,
    };

    fn stacked(plan: &StagePlan, height: f32) {
        // strip above delta, delta touching the pictures, band below them,
        // all above the controls.
        assert!(plan.strip.bottom() <= plan.delta.y, "{plan:?}");
        assert!((plan.delta.bottom() - plan.pictures.y).abs() < 0.5);
        assert!(plan.band.y >= plan.pictures.bottom());
        assert!(plan.band.bottom() <= height - LANES.controls, "{plan:?}");
        for pane in &plan.panes {
            assert!(pane.rect.y >= plan.pictures.y - 0.5);
            assert!(pane.rect.bottom() <= plan.pictures.bottom() + 0.5);
            assert!(!pane.rect.overlaps(&plan.band));
        }
    }

    #[test]
    fn split_pictures_meet_at_the_seam_under_the_delta_lane() {
        for (w, h) in [(1920., 1080.), (1280., 800.), (3840., 2160.)] {
            let plan = plan(ComposeLayout::Split, w, h, |_| WIDE, LANES);
            stacked(&plan, h);
            let (p, r) = (plan.panes[0].rect, plan.panes[1].rect);
            assert_eq!(plan.panes[0].role, Role::Primary);
            assert!((r.x - p.right() - SPLIT_GAP).abs() < 0.5);
            assert!(r.right() <= w + 0.5);
            // Width-limited on 16:9: the pictures span the stage.
            assert!(plan.pictures.w >= w - 4., "{w}: {plan:?}");
            // The band is 65% of the pictures, 1000:210.
            let band = plan.band;
            assert!((band.w - (plan.pictures.w * BAND_SHARE).min(BAND_MAX_WIDTH)).abs() < 1.);
            assert!((band.h / band.w - BAND_ASPECT).abs() < 0.01);
            assert!((band.center_x() - w / 2.).abs() < 1.5);
        }
    }

    #[test]
    fn single_pictures_shrink_so_the_column_fits() {
        let plan = plan(ComposeLayout::PrimaryOnly, 1920., 1080., |_| WIDE, LANES);
        stacked(&plan, 1080.);
        assert_eq!(plan.panes.len(), 1);
        let rect = plan.panes[0].rect;
        assert!((rect.w / rect.h - WIDE).abs() < 0.01);
        assert!(rect.w < 1920. && rect.h > 600., "{rect:?}");
        // Without the band and strip the picture grows.
        let bare = super::plan(
            ComposeLayout::PrimaryOnly,
            1920.,
            1080.,
            |_| WIDE,
            Lanes {
                band: false,
                strip: 0.,
                ..LANES
            },
        );
        assert!(bare.panes[0].rect.h > rect.h);
        assert_eq!(bare.band.w, 0.);
    }

    #[test]
    fn insets_sit_beside_the_large_picture_never_over_it() {
        for layout in [
            ComposeLayout::PrimaryWithReferenceInset,
            ComposeLayout::ReferenceWithPrimaryInset,
        ] {
            let plan = plan(layout, 1920., 1080., |_| WIDE, LANES);
            stacked(&plan, 1080.);
            let (large, small) = (plan.panes[0], plan.panes[1]);
            assert!(!large.inset && small.inset);
            assert!(!large.rect.overlaps(&small.rect));
            assert!((small.rect.h - large.rect.h * INSET_SCALE).abs() < 1.);
            assert_eq!(small.rect.y, large.rect.y, "top aligned");
            if layout == ComposeLayout::PrimaryWithReferenceInset {
                assert_eq!(large.role, Role::Primary);
                assert!(small.rect.x > large.rect.right());
            } else {
                assert_eq!(large.role, Role::Reference);
                assert!(small.rect.right() < large.rect.x);
            }
            assert!(plan.pictures.x >= LANE_MARGIN - 0.5);
            assert!(plan.pictures.right() <= 1920. - LANE_MARGIN + 0.5);
        }
    }

    #[test]
    fn a_tiny_stage_degrades_to_nothing_without_panicking() {
        let plan = plan(ComposeLayout::Split, 10., 10., |_| f32::NAN, LANES);
        assert!(plan.pictures.h >= 0. && plan.band.w >= 0.);
    }

    #[test]
    fn the_band_defaults_to_its_lane_and_round_trips_its_position() {
        let stage = (1920., 1080.);
        let hud = (1248., 262.);
        let lane = (336., 760.);
        assert_eq!(hud_origin(stage, hud, 44., None, lane), lane);
        let origin = hud_origin(stage, hud, 44., Some((0.25, 0.5)), lane);
        let back = hud_position(stage, hud, 44., origin);
        assert!((back.0 - 0.25).abs() < 1e-6 && (back.1 - 0.5).abs() < 1e-6);
        // Dragged past the edges clamps.
        assert_eq!(hud_position(stage, hud, 44., (-50., 5000.)), (0., 1.));
    }
}
