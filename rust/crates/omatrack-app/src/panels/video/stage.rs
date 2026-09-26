//! Geometry of the fullscreen video stage (port of the Qt fullscreen
//! compositor and `FilmstripLayout::reservedHeight`), pure so it is tested
//! as arithmetic.
//!
//! The stage is the whole window on black. Every picture is aspect-fit and
//! vertically centred in its box:
//!
//! - **Split** (1): two halves `(w - 2) / 2` wide with a 2 px seam, the
//!   pictures meeting at the seam;
//! - **Primary + inset** (2) / **Reference + inset** (3): the large picture
//!   in a box inset by 7% of the short side (flush left in 2, flush right in
//!   3), the inset 30% of the width, 16 px from the bottom-right (2) or
//!   bottom-left (3) corner;
//! - **Primary** (4) / **Reference** (5): one picture on the whole stage.
//!
//! The filmstrip lane and the transport controls sit at the bottom. When the
//! letterbox below the pictures is too short to hold them, the video area
//! gives up that height ([`reserved_height`]); otherwise they float over the
//! letterbox and the pictures stay centred on the full stage.

use crate::actions::Role;
use crate::state::ComposeLayout;

/// The seam between split pictures, px.
pub const SPLIT_GAP: f32 = 2.;
/// The inset picture's share of the stage width.
pub const INSET_WIDTH_SHARE: f32 = 0.3;
/// The inset picture's distance from the stage corner, px.
pub const INSET_MARGIN: f32 = 16.;
/// The large picture's inset in layouts 2 and 3, share of the short side.
pub const MAIN_INSET_SHARE: f32 = 0.07;
/// Space around the filmstrip lane, px (the Qt `+ 16`).
pub const LANE_MARGIN: f32 = 8.;

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
}

/// How a picture sits in a box wider than itself.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Align {
    Start,
    Center,
    End,
}

/// One picture on the stage.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Pane {
    pub role: Role,
    pub rect: Rect,
    /// The small picture-in-picture.
    pub inset: bool,
}

/// `aspect` (width / height) fit inside `outer`, vertically centred and
/// horizontally aligned per `align`.
pub fn fit(outer: Rect, aspect: f32, align: Align) -> Rect {
    let aspect = if aspect.is_finite() && aspect > 0. {
        aspect
    } else {
        16. / 9.
    };
    let (mut w, mut h) = (outer.w.max(0.), outer.w.max(0.) / aspect);
    if h > outer.h {
        h = outer.h.max(0.);
        w = h * aspect;
    }
    let x = match align {
        Align::Start => outer.x,
        Align::Center => outer.x + (outer.w - w) * 0.5,
        Align::End => outer.right() - w,
    };
    Rect::new(x, outer.y + (outer.h - h) * 0.5, w, h)
}

/// The pictures of `layout` on a `width` x `height` video area, large
/// picture first. `aspect(role)` is each video's width / height.
pub fn compose(
    layout: ComposeLayout,
    width: f32,
    height: f32,
    aspect: impl Fn(Role) -> f32,
) -> Vec<Pane> {
    let (w, h) = (width.max(0.), height.max(0.));
    let main = |role: Role, rect: Rect, align: Align| Pane {
        role,
        rect: fit(rect, aspect(role), align),
        inset: false,
    };
    match layout {
        ComposeLayout::Split => {
            let half = ((w - SPLIT_GAP) * 0.5).max(0.);
            vec![
                main(Role::Primary, Rect::new(0., 0., half, h), Align::End),
                main(
                    Role::Reference,
                    Rect::new(w - half, 0., half, h),
                    Align::Start,
                ),
            ]
        }
        ComposeLayout::PrimaryWithReferenceInset | ComposeLayout::ReferenceWithPrimaryInset => {
            let (large, small, left) = match layout {
                ComposeLayout::PrimaryWithReferenceInset => (Role::Primary, Role::Reference, true),
                _ => (Role::Reference, Role::Primary, false),
            };
            let inset = (w.min(h) * MAIN_INSET_SHARE).round();
            let (main_w, main_h) = ((w - 2. * inset).max(0.), (h - 2. * inset).max(0.));
            let main_x = if left { 0. } else { w - main_w };
            let pip_w = (w * INSET_WIDTH_SHARE).round();
            let pip_h = pip_w / sane_aspect(aspect(small));
            let pip_x = if left {
                w - pip_w - INSET_MARGIN
            } else {
                INSET_MARGIN
            };
            vec![
                main(
                    large,
                    Rect::new(main_x, inset, main_w, main_h),
                    Align::Center,
                ),
                Pane {
                    role: small,
                    rect: Rect::new(pip_x, h - pip_h - INSET_MARGIN, pip_w, pip_h),
                    inset: true,
                },
            ]
        }
        ComposeLayout::PrimaryOnly => {
            vec![main(Role::Primary, Rect::new(0., 0., w, h), Align::Center)]
        }
        ComposeLayout::ReferenceOnly => {
            vec![main(
                Role::Reference,
                Rect::new(0., 0., w, h),
                Align::Center,
            )]
        }
    }
}

fn sane_aspect(aspect: f32) -> f32 {
    if aspect.is_finite() && aspect > 0. {
        aspect
    } else {
        16. / 9.
    }
}

/// The height the video area gives up at the bottom for the filmstrip lane
/// (`strip` tall) and the controls (`controls` tall): none when the
/// letterbox below the pictures already holds them, else all of it (the
/// Qt `filmstripReservedHeight`). No strip reserves nothing.
pub fn reserved_height(
    layout: ComposeLayout,
    width: f32,
    height: f32,
    aspect: impl Fn(Role) -> f32,
    strip: f32,
    controls: f32,
) -> f32 {
    if strip <= 0. || width <= 0. || height <= 0. {
        return 0.;
    }
    let needed = height.min(strip + controls + 2. * LANE_MARGIN);
    let letterbox = |pane_width: f32, role: Role| {
        (height - height.min(pane_width / sane_aspect(aspect(role)))) * 0.5
    };
    let space = match layout {
        ComposeLayout::Split => {
            let half = (width - SPLIT_GAP) * 0.5;
            letterbox(half, Role::Primary).min(letterbox(half, Role::Reference))
        }
        ComposeLayout::PrimaryOnly => letterbox(width, Role::Primary),
        ComposeLayout::ReferenceOnly => letterbox(width, Role::Reference),
        ComposeLayout::PrimaryWithReferenceInset | ComposeLayout::ReferenceWithPrimaryInset => 0.,
    };
    if space >= needed { 0. } else { needed }
}

/// The telemetry band's top-left corner: at the normalized `position` of
/// the space it can reach (the stage above `bottom_inset`, minus the band),
/// else centred horizontally with its centre at 90% of the stage height
/// (clamped into that space).
pub fn hud_origin(
    stage: (f32, f32),
    hud: (f32, f32),
    bottom_inset: f32,
    position: Option<(f32, f32)>,
) -> (f32, f32) {
    let (available_x, available_y) = hud_room(stage, hud, bottom_inset);
    match position {
        Some((x, y)) => (x.clamp(0., 1.) * available_x, y.clamp(0., 1.) * available_y),
        None => (
            available_x * 0.5,
            (stage.1 * 0.9 - hud.1 * 0.5).clamp(0., available_y),
        ),
    }
}

/// How far the band's top-left corner can travel each way.
pub fn hud_room(stage: (f32, f32), hud: (f32, f32), bottom_inset: f32) -> (f32, f32) {
    (
        (stage.0 - hud.0).max(0.),
        (stage.1 - bottom_inset - hud.1).max(0.),
    )
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

    fn close(a: Rect, b: Rect) {
        let eq = |x: f32, y: f32| (x - y).abs() < 0.01;
        assert!(
            eq(a.x, b.x) && eq(a.y, b.y) && eq(a.w, b.w) && eq(a.h, b.h),
            "{a:?} != {b:?}"
        );
    }

    #[test]
    fn a_picture_fits_its_box_centred() {
        // Pillarbox: full height, centred.
        close(
            fit(Rect::new(0., 0., 400., 100.), 2., Align::Center),
            Rect::new(100., 0., 200., 100.),
        );
        // Letterbox: full width, vertically centred whatever the align.
        close(
            fit(Rect::new(10., 0., 200., 400.), 2., Align::End),
            Rect::new(10., 150., 200., 100.),
        );
        close(
            fit(Rect::new(0., 0., 400., 100.), 2., Align::End),
            Rect::new(200., 0., 200., 100.),
        );
    }

    #[test]
    fn split_pictures_meet_at_the_seam_and_centre_vertically() {
        let panes = compose(ComposeLayout::Split, 1920., 1080., |_| WIDE);
        assert_eq!(panes.len(), 2);
        let half = (1920. - SPLIT_GAP) / 2.;
        let h = half / WIDE;
        close(panes[0].rect, Rect::new(0., (1080. - h) / 2., half, h));
        close(
            panes[1].rect,
            Rect::new(1920. - half, (1080. - h) / 2., half, h),
        );
        assert!((panes[1].rect.x - panes[0].rect.right() - SPLIT_GAP).abs() < 0.01);
        assert_eq!(panes[0].role, Role::Primary);
    }

    #[test]
    fn inset_layouts_follow_the_qt_geometry() {
        let (w, h) = (1920., 1080.);
        let inset = (1080. * MAIN_INSET_SHARE).round();
        let panes = compose(ComposeLayout::PrimaryWithReferenceInset, w, h, |_| WIDE);
        let main_box = Rect::new(0., inset, w - 2. * inset, h - 2. * inset);
        close(panes[0].rect, fit(main_box, WIDE, Align::Center));
        assert_eq!(panes[1].role, Role::Reference);
        assert!(panes[1].inset);
        let pip_w = (w * 0.3).round();
        close(
            panes[1].rect,
            Rect::new(w - pip_w - 16., h - pip_w / WIDE - 16., pip_w, pip_w / WIDE),
        );
        // Layout 3 mirrors it: the large reference's box flush right, the
        // primary inset bottom-left.
        let panes = compose(ComposeLayout::ReferenceWithPrimaryInset, w, h, |_| WIDE);
        assert_eq!(panes[0].role, Role::Reference);
        let mirrored = Rect::new(2. * inset, inset, w - 2. * inset, h - 2. * inset);
        close(panes[0].rect, fit(mirrored, WIDE, Align::Center));
        assert_eq!(panes[1].rect.x, 16.);
    }

    #[test]
    fn single_pictures_fill_the_stage_centred() {
        let panes = compose(ComposeLayout::ReferenceOnly, 2000., 1000., |_| WIDE);
        assert_eq!(panes.len(), 1);
        assert_eq!(panes[0].role, Role::Reference);
        close(
            panes[0].rect,
            Rect::new((2000. - 1000. * WIDE) / 2., 0., 1000. * WIDE, 1000.),
        );
    }

    #[test]
    fn the_lane_reserves_height_only_when_the_letterbox_is_too_short() {
        // Split halves of a 16:9 stage leave a tall letterbox: nothing is
        // reserved.
        assert_eq!(
            reserved_height(ComposeLayout::Split, 1920., 1080., |_| WIDE, 60., 40.),
            0.
        );
        // One 16:9 picture on a 16:9 stage has no letterbox: all of it.
        assert_eq!(
            reserved_height(ComposeLayout::PrimaryOnly, 1920., 1080., |_| WIDE, 60., 40.),
            60. + 40. + 16.
        );
        // Insets never count their letterbox.
        assert_eq!(
            reserved_height(
                ComposeLayout::PrimaryWithReferenceInset,
                1920.,
                2000.,
                |_| WIDE,
                60.,
                40.
            ),
            116.
        );
        // Never more than the stage, nothing without a strip.
        assert_eq!(
            reserved_height(ComposeLayout::PrimaryOnly, 1920., 50., |_| WIDE, 60., 40.),
            50.
        );
        assert_eq!(
            reserved_height(ComposeLayout::PrimaryOnly, 1920., 1080., |_| WIDE, 0., 40.),
            0.
        );
    }

    #[test]
    fn the_band_defaults_low_centre_and_round_trips_its_position() {
        let stage = (1920., 1080.);
        let hud = (650., 136.5);
        let (x, y) = hud_origin(stage, hud, 116., None);
        assert!((x - (1920. - 650.) / 2.).abs() < 1e-3);
        // 90% down, clamped above the lane.
        assert!((y - (1080. - 116. - 136.5)).abs() < 1e-3, "{y}");
        let (x, y) = hud_origin(stage, hud, 0., None);
        assert!((y - (972. - 68.25)).abs() < 1e-3, "{y} {x}");
        let origin = hud_origin(stage, hud, 116., Some((0.25, 0.5)));
        let back = hud_position(stage, hud, 116., origin);
        assert!((back.0 - 0.25).abs() < 1e-6 && (back.1 - 0.5).abs() < 1e-6);
        // Dragged past the edges clamps.
        assert_eq!(hud_position(stage, hud, 116., (-50., 5000.)), (0., 1.));
    }
}
