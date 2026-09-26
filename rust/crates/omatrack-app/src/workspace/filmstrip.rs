//! The lap filmstrip: the laps of the recordings being compared, full width
//! above the workspace (port of the Qt `LapFilmstrip.qml` docked slot).
//!
//! One row per recording that plays a role: the primary recording's row,
//! then the reference recording's. When both laps come from one recording
//! it is one row with both roles marked. Each row has a fixed gutter (role
//! swatch and marker, driver, session, the selected lap and its time in
//! the role colour) and then that recording's [`LapStrip`], proportional to
//! driving time (AGENTS.md 6.3). A swap button between the gutter and the
//! cells exchanges the roles (also `x` and the palette).
//!
//! Pointer contract (the Qt filmstrip's): a click selects that row's lap
//! for the row's role without touching cursor or viewport; clicking the
//! lap the role already holds jumps back to the lap start; a right click
//! (or Alt+click) sets the lap as the reference. Driver and lap details
//! live here, not in the title bar.
//!
//! State: [`Session`](crate::state::Session) owns the selection; this view
//! derives rows from it on every session change and keeps each
//! recording's cells while another lap of it loads, so the strip never
//! blanks. The playheads follow `CursorState`, quantized so playback only
//! re-renders this small view when a playhead moves visibly.
//!
//! Seam: the filmstrip is one [`Entity<Filmstrip>`] owned by the
//! [`Workspace`](super::Workspace) ([`Workspace::filmstrip`](super::Workspace::filmstrip)).
//! A surface that wants the same strip elsewhere (a bottom lane over
//! fullscreen video) renders that entity instead of the workspace's top
//! slot; it carries no layout assumptions beyond full width.

use std::collections::HashMap;
use std::sync::Arc;

use gpui_kit::AssetSource as _;
use gpui_kit::assets::IconName;
use gpui_kit::component::{
    ActiveTheme as _, Disableable as _, Icon, Sizable as _, StyledExt as _, Theme,
    button::{Button, ButtonVariants as _},
    h_flex, v_flex,
};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::{
    AnyElement, App, Context, ElementId, InteractiveElement as _, IntoElement, ParentElement as _,
    Render, Role as AccessRole, SharedString, StatefulInteractiveElement as _, Styled as _,
    Subscription, TestSupportExt as _, Window, div,
};
use omatrack_ui::{LapRole, LapSelect, LapStrip, LapStripItem, Swatch, TypeScale as _};

use crate::actions::{Role, SwapRoles};
use crate::keymap::WORKSPACE_CONTEXT;
use crate::state::{AppState, LapRef, RoleSlot, RoleState};

gpui_kit::assets::icon_assets!(FilmstripIconAssets, [ArrowDownUp]);

/// Playheads move in steps of this lap fraction: finer than a pixel of any
/// realistic cell, coarse enough that playback does not re-render the strip
/// every frame.
const PLAYHEAD_STEP: f64 = 1.0 / 2048.0;

/// One role's lap on a row.
#[derive(Debug, Clone, PartialEq)]
pub struct RowLap {
    role: LapRole,
    lap: i32,
    label: SharedString,
    time: SharedString,
    state: RowLapState,
}

#[derive(Debug, Clone, PartialEq)]
enum RowLapState {
    Loading,
    Loaded,
    Failed,
}

impl RowLap {
    pub fn role(&self) -> LapRole {
        self.role
    }

    pub fn lap(&self) -> i32 {
        self.lap
    }
}

/// One recording of the filmstrip.
#[derive(Debug, Clone)]
pub struct FilmstripRow {
    session: SharedString,
    driver: SharedString,
    detail: Option<SharedString>,
    laps: Vec<RowLap>,
    items: Arc<[LapStripItem]>,
}

impl FilmstripRow {
    /// The catalog session id of the recording.
    pub fn session(&self) -> &SharedString {
        &self.session
    }

    /// The roles this recording plays, primary first.
    pub fn roles(&self) -> Vec<LapRole> {
        self.laps.iter().map(|lap| lap.role).collect()
    }

    /// The lap `role` holds on this row.
    pub fn lap(&self, role: LapRole) -> Option<i32> {
        self.laps
            .iter()
            .find(|lap| lap.role == role)
            .map(|lap| lap.lap)
    }

    /// The row's cells.
    pub fn items(&self) -> &Arc<[LapStripItem]> {
        &self.items
    }

    /// The role a plain click on this row asks for.
    fn click_role(&self) -> LapRole {
        self.laps.first().map_or(LapRole::Primary, |lap| lap.role)
    }

    /// `Primary lap L10 1:16.091 · Driver 1 · CT1`, for the gutter's
    /// accessible name.
    fn spoken(&self) -> SharedString {
        let laps = self
            .laps
            .iter()
            .map(|lap| {
                let state = match lap.state {
                    RowLapState::Loading => " (loading)",
                    RowLapState::Failed => " (failed)",
                    RowLapState::Loaded => "",
                };
                format!("{} lap {} {}{state}", lap.role.label(), lap.label, lap.time)
                    .trim_end()
                    .to_string()
            })
            .collect::<Vec<_>>()
            .join(", ");
        let detail = self
            .detail
            .as_ref()
            .map(|detail| format!(" · {detail}"))
            .unwrap_or_default();
        format!("{laps} · {}{detail}", self.driver).into()
    }
}

/// The filmstrip view. See the module docs.
pub struct Filmstrip {
    app: AppState,
    rows: Vec<FilmstripRow>,
    /// The cells last loaded per recording, shown while another lap of it
    /// loads.
    cells: HashMap<SharedString, Arc<[LapStripItem]>>,
    playhead: Option<f64>,
    reference_playhead: Option<f64>,
    swap_icon: Icon,
    /// Shown as the lane over fullscreen video: a translucent black card
    /// instead of the full-width bar with its border.
    on_stage: bool,
    _subscriptions: Vec<Subscription>,
}

impl Filmstrip {
    pub fn new(app: AppState, cx: &mut Context<'_, Self>) -> Self {
        let subscriptions = vec![
            cx.observe(&app.session, |this, _, cx| {
                this.rebuild(cx);
                this.follow_cursor(cx);
                cx.notify();
            }),
            cx.observe(&app.cursor, |this, _, cx| {
                if this.follow_cursor(cx) {
                    cx.notify();
                }
            }),
            cx.observe_global::<Theme>(|_, cx| cx.notify()),
        ];
        let swap_icon = match FilmstripIconAssets.load(&IconName::ArrowDownUp.path()) {
            Ok(Some(bytes)) => Icon::default().data(&bytes),
            _ => Icon::empty(),
        };
        let mut filmstrip = Self {
            app,
            rows: Vec::new(),
            cells: HashMap::new(),
            playhead: None,
            reference_playhead: None,
            swap_icon,
            on_stage: false,
            _subscriptions: subscriptions,
        };
        filmstrip.rebuild(cx);
        filmstrip.follow_cursor(cx);
        filmstrip
    }

    /// Present the strip as the fullscreen lane (`true`) or the full-width
    /// bar below the title bar.
    pub fn set_on_stage(&mut self, on_stage: bool, cx: &mut Context<'_, Self>) {
        if self.on_stage != on_stage {
            self.on_stage = on_stage;
            cx.notify();
        }
    }

    pub fn is_on_stage(&self) -> bool {
        self.on_stage
    }

    /// The strip's height in rem: one 1.5 rem row per recording, 0.25 rem
    /// apart, 0.25 rem padding above and below; 0 without rows.
    #[expect(
        clippy::cast_precision_loss,
        reason = "UI geometry deliberately projects bounded counts and f64 telemetry coordinates into f32 pixels."
    )]
    pub fn height_rems(&self) -> f32 {
        match self.rows.len() {
            0 => 0.,
            rows => rows as f32 * 1.5 + (rows - 1) as f32 * 0.25 + 0.5,
        }
    }

    /// The rows on screen, primary recording first.
    pub fn rows(&self) -> &[FilmstripRow] {
        &self.rows
    }

    /// Derive the rows from the session's two roles.
    fn rebuild(&mut self, cx: &mut Context<'_, Self>) {
        let session = self.app.session.read(cx);
        let slots = [
            (LapRole::Primary, session.primary()),
            (LapRole::Reference, session.reference()),
        ];
        let mut rows: Vec<FilmstripRow> = Vec::with_capacity(2);
        for (role, slot) in slots {
            let Some(slot) = slot else { continue };
            let id = slot.lap_ref().session().clone();
            if let Some(lap) = slot.loaded() {
                let items: Arc<[LapStripItem]> =
                    lap.strip().iter().map(LapStripItem::from).collect();
                self.cells.insert(id.clone(), items);
            }
            let lap = row_lap(role, slot);
            if let Some(row) = rows.iter_mut().find(|row| row.session == id) {
                row.laps.push(lap);
            } else {
                let info = slot.info();
                rows.push(FilmstripRow {
                    items: self
                        .cells
                        .get(&id)
                        .cloned()
                        .unwrap_or_else(|| Arc::from(Vec::new())),
                    session: id,
                    driver: info.driver.clone().unwrap_or_else(|| info.title.clone()),
                    detail: info
                        .driver
                        .as_ref()
                        .and_then(|_| info.session_name.clone())
                        .filter(|name| !name.is_empty()),
                    laps: vec![lap],
                });
            }
        }
        // Only the recordings on screen keep cells.
        self.cells
            .retain(|id, _| rows.iter().any(|row| &row.session == id));
        self.rows = rows;
    }

    /// Follow the shared cursor into the playheads; true when one moved.
    fn follow_cursor(&mut self, cx: &mut Context<'_, Self>) -> bool {
        let fraction = self
            .app
            .cursor
            .read(cx)
            .fraction()
            .filter(|fraction| fraction.is_finite());
        let comparison = self
            .app
            .session
            .read(cx)
            .analysis()
            .and_then(|analysis| analysis.comparison().cloned());
        let quantize =
            |fraction: f64| (fraction.clamp(0.0, 1.0) / PLAYHEAD_STEP).round() * PLAYHEAD_STEP;
        let playhead = fraction.map(quantize);
        let reference = fraction.zip(comparison).map(|(fraction, comparison)| {
            quantize(comparison.compare_fraction_for_primary_fraction(fraction))
        });
        let changed = playhead != self.playhead || reference != self.reference_playhead;
        self.playhead = playhead;
        self.reference_playhead = reference;
        changed
    }

    fn render_gutter(row: &FilmstripRow, theme: &Theme) -> AnyElement {
        let two_roles = row.laps.len() > 1;
        let id = match row.click_role() {
            LapRole::Primary => "filmstrip-primary",
            LapRole::Reference => "filmstrip-reference",
        };
        h_flex()
            .id(id)
            .test_support()
            .aria_label(row.spoken())
            .h_6()
            .w_full()
            .min_w_0()
            .gap_1p5()
            .text_label()
            .whitespace_nowrap()
            .children(row.laps.iter().map(|lap| {
                h_flex()
                    .flex_shrink_0()
                    .gap_1()
                    .child(Swatch::new(lap.role.color(theme)))
                    .child(
                        div()
                            .font_medium()
                            .text_color(lap.role.color(theme))
                            .child(lap.role.marker()),
                    )
            }))
            .child(
                h_flex()
                    .flex_1()
                    .min_w_0()
                    .gap_1p5()
                    .overflow_hidden()
                    .child(
                        div()
                            .min_w_0()
                            .truncate()
                            .font_medium()
                            .text_color(theme.foreground)
                            .child(row.driver.clone()),
                    )
                    .when_some(row.detail.clone(), |el, detail| {
                        el.child(
                            div()
                                .flex_shrink_0()
                                .text_color(theme.muted_foreground)
                                .child(detail),
                        )
                    }),
            )
            .children(row.laps.iter().map(|lap| {
                let color = match lap.state {
                    RowLapState::Failed => theme.danger,
                    RowLapState::Loading => theme.muted_foreground,
                    RowLapState::Loaded => lap.role.color(theme),
                };
                h_flex()
                    .flex_shrink_0()
                    .gap_1()
                    .numeric()
                    .child(
                        div()
                            .text_color(theme.muted_foreground)
                            .child(lap.label.clone()),
                    )
                    // Two laps on one row: the cells carry the times.
                    .when(!two_roles && !lap.time.is_empty(), |el| {
                        el.child(div().text_color(color).child(lap.time.clone()))
                    })
            }))
            .into_any_element()
    }

    fn render_strip(&self, row: &FilmstripRow, theme: &Theme) -> AnyElement {
        let strip_id = ElementId::Name(format!("filmstrip-{}", row.session).into());
        if row.items.is_empty() {
            let loading = row.laps.iter().any(|lap| lap.state == RowLapState::Loading);
            return div()
                .id(strip_id)
                .h_6()
                .flex()
                .items_center()
                .px_2()
                .text_label()
                .text_color(theme.muted_foreground)
                .child(if loading {
                    "Loading laps…"
                } else {
                    "No laps"
                })
                .into_any_element();
        }
        let primary = row.lap(LapRole::Primary);
        let reference = row.lap(LapRole::Reference);
        let app = self.app.clone();
        let session = row.session.clone();
        let click_role = row.click_role();
        LapStrip::new(strip_id, row.items.clone())
            .role(click_role)
            .primary(primary)
            .reference(reference)
            .primary_playhead(primary.and(self.playhead))
            .reference_playhead(reference.and(self.reference_playhead))
            .on_select(move |select: &LapSelect, _, cx| {
                select_lap(&app, &session, *select, cx);
            })
            .into_any_element()
    }
}

/// Apply a strip request: select the lap for the role, or, when a plain
/// click hits the lap the role already holds, jump back to its start.
fn select_lap(app: &AppState, session: &SharedString, select: LapSelect, cx: &mut App) {
    let role = match select.role {
        LapRole::Primary => Role::Primary,
        LapRole::Reference => Role::Reference,
    };
    let lap_ref = LapRef::new(session.clone(), select.lap_id);
    let holds = app
        .session
        .read(cx)
        .slot(role)
        .is_some_and(|slot| slot.lap_ref() == &lap_ref);
    if holds {
        if !select.secondary {
            app.cursor
                .update(cx, |cursor, cx| cursor.set_fraction(Some(0.0), cx));
        }
        return;
    }
    app.session
        .update(cx, |session, cx| session.set_lap(role, lap_ref, cx));
}

fn row_lap(role: LapRole, slot: &RoleSlot) -> RowLap {
    let info = slot.info();
    RowLap {
        role,
        lap: slot.lap_ref().lap(),
        label: info.label.clone(),
        time: info.time.clone(),
        state: match slot.state() {
            RoleState::Loading => RowLapState::Loading,
            RoleState::Loaded(_) => RowLapState::Loaded,
            RoleState::Failed(_) => RowLapState::Failed,
        },
    }
}

impl Render for Filmstrip {
    fn render(&mut self, window: &mut Window, cx: &mut Context<'_, Self>) -> impl IntoElement {
        let _ = window;
        let theme = cx.theme();
        let root = div()
            .id("filmstrip")
            .role(AccessRole::Group)
            .aria_label("Laps")
            .test_support();
        if self.rows.is_empty() {
            return root;
        }
        let can_swap = self.app.session.read(cx).reference().is_some();
        let session = self.app.session.clone();
        root.w_full()
            .flex_shrink_0()
            .flex()
            .items_center()
            .gap_2()
            .px_2()
            .py_1()
            .map(|this| {
                if self.on_stage {
                    this.rounded(theme.radius_lg)
                        .bg(gpui_kit::black().opacity(0.72))
                } else {
                    this.border_b_1()
                        .border_color(theme.border)
                        .bg(theme.background)
                }
            })
            .child(
                v_flex()
                    .w_64()
                    .flex_shrink_0()
                    .gap_1()
                    .children(self.rows.iter().map(|row| Self::render_gutter(row, theme))),
            )
            .child(
                Button::new("filmstrip-swap")
                    .ghost()
                    .xsmall()
                    .icon(self.swap_icon.clone())
                    .disabled(!can_swap)
                    .accessibility_label("Swap primary and reference")
                    .tooltip_with_action(
                        "Swap primary and reference",
                        &SwapRoles,
                        Some(WORKSPACE_CONTEXT),
                    )
                    .on_click(move |_, _, cx| {
                        session.update(cx, super::super::state::session::Session::swap);
                    }),
            )
            .child(
                v_flex()
                    .flex_1()
                    .min_w_0()
                    .gap_1()
                    .children(self.rows.iter().map(|row| self.render_strip(row, theme))),
            )
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_swap_icon_is_embedded() {
        let bytes = FilmstripIconAssets
            .load(&IconName::ArrowDownUp.path())
            .unwrap()
            .unwrap();
        assert!(bytes.starts_with(b"<svg"));
    }
}
