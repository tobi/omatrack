//! Where the time goes: the lap's losses at a glance.
//!
//! One scrolling column, headings as content rather than chrome:
//!
//! 1. the track map in heat mode ([`TrackMapData::with_heat`]): the primary
//!    lap coloured by time lost per metre (the analysis' loss rate, from the
//!    one cached delta) on the "less — more" loss ramp, with corner labels
//!    and the cursor ring;
//! 2. the corners sorted by time lost, largest first: name, a loss bar
//!    (width and intensity on the same ramp), Δt in seconds, and the entry
//!    speed of the primary against the reference;
//! 3. the corners-versus-straights split of the final delta
//!    ([`Analysis::time_split`]);
//! 4. a card for the selected corner: its Δt, entry and minimum speed, the
//!    entry/exit split, and the corner checks' notes
//!    ([`omatrack_core::corners::checks`]) as sentences, with a link that
//!    focuses the corner in the traces and opens the Corners table.
//!
//! Ownership: the [`Session`](crate::state::Session) owns the analysis; the
//! panel keeps a presentation snapshot ([`LossLine`]s) rebuilt only when the
//! analysis changes identity. The selected corner is derived, never stored
//! apart from its source: the focused corner (a row click, `h`/`j`, the
//! ruler, the palette), else the corner under the cursor, else the largest
//! loss. The panel observes the cursor but notifies only when that derived
//! selection changes.
//!
//! Every number comes from the analysis; nothing here measures a lap.
//! Under a LOW-confidence alignment the figures are marked `≈`. When the
//! map does not place time loss ([`Analysis::time_loss_placed`]: a lap-time
//! base, whose delta is the lap-time gap spread evenly), the panel says so
//! instead of ranking corners: no heat, no loss table, no split, and the
//! card keeps only the primary's speeds and the notes (principle 9).
//!
//! Only the [`TABLE_ROWS`] largest losses are listed (plus the selected
//! corner when it ranks lower), so the map, the table, the split and the
//! card fit a 900 px window; "Show all" opens the full Corners table.

use std::sync::Arc;

use gpui_kit::component::{
    ActiveTheme as _, Icon, IconName, Sizable as _, StyledExt as _,
    button::{Button, ButtonVariants as _},
    h_flex,
    scroll::ScrollableElement as _,
    v_flex,
};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::{
    AnyElement, App, AppContext as _, Context, Entity, FocusHandle, Hsla, InteractiveElement as _,
    IntoElement, ParentElement as _, Render, Role, SharedString, StatefulInteractiveElement as _,
    Styled as _, Subscription, TestSupportExt as _, Window, div, relative, rems,
};
use omatrack_core::corners::checks::NoteSeverity;
use omatrack_core::session::{Analysis, CornerRow};
use omatrack_trace::palette::TracePalette;
use omatrack_trace::{Selection, TrackMap, TrackMapData, TrackMapEvent};
use omatrack_ui::{DeltaSense, TypeScale as _, format_delta, format_value};

use crate::actions::{FocusCorner, FocusPanel4};
use crate::panels::{PanelKind, analysis_body, empty_state, map, panel_body};
use crate::state::AppState;

/// Height of the heat map, in rems (320 px at the default base)...
const MAP_REMS: f32 = 20.0;
/// ...at most this share of the window height, so the table and the card
/// stay in view on a short window.
const MAP_MAX_WINDOW_SHARE: f32 = 0.32;
/// Corners listed by time lost before "Show all" (the Corners table).
pub const TABLE_ROWS: usize = 8;
/// Width of the corner-name column, in rems.
const NAME_REMS: f32 = 3.25;
/// Width of each numeric column, in rems.
const NUMBER_REMS: f32 = 3.0;

/// One corner as this panel shows it (plain data).
#[derive(Debug, Clone, PartialEq)]
pub struct LossLine {
    id: SharedString,
    name: SharedString,
    /// `T10A` for `Turn 10A`.
    short: SharedString,
    zone: (f64, f64),
    dt: f64,
    entry_dt: f64,
    exit_dt: f64,
    /// Primary entry and minimum speed, km/h.
    entry: f64,
    minimum: f64,
    /// Primary minus reference entry speed, km/h (NaN without).
    entry_delta: f64,
    /// The corner checks' notes, in registry order.
    notes: Vec<(NoteSeverity, SharedString)>,
}

impl LossLine {
    fn new(row: &CornerRow) -> Self {
        let name: SharedString = row.zone.name.clone().into();
        let short =
            omatrack_trace::corner_ruler::short_label(&name).unwrap_or_else(|| name.clone());
        Self {
            id: row.zone.id.clone().into(),
            short,
            name,
            zone: (row.zone.start, row.zone.end),
            dt: row.dt,
            entry_dt: row.entry_dt,
            exit_dt: row.exit_dt,
            entry: row.speeds.entry,
            minimum: row.speeds.apex,
            entry_delta: row
                .reference_speeds
                .map_or(f64::NAN, |reference| row.speeds.entry - reference.entry),
            notes: row
                .notes
                .iter()
                // The card states the entry speed with its numbers already.
                .filter(|note| note.id != "entry_speed")
                .map(|note| (note.severity, note.sentence().into()))
                .collect(),
        }
    }

    /// The zone id (`T5`, an atlas range id).
    pub fn id(&self) -> &SharedString {
        &self.id
    }

    pub fn name(&self) -> &SharedString {
        &self.name
    }

    /// Time lost (+) through the corner, seconds.
    pub fn dt(&self) -> f64 {
        self.dt
    }

    /// The notes as the card prints them.
    pub fn notes(&self) -> impl Iterator<Item = &SharedString> {
        self.notes.iter().map(|(_, text)| text)
    }

    fn contains(&self, fraction: f64) -> bool {
        (self.zone.0..self.zone.1).contains(&fraction)
    }

    fn is_zone(&self, selection: Selection) -> bool {
        (self.zone.0 - selection.start).abs() < 1e-9 && (self.zone.1 - selection.end).abs() < 1e-9
    }
}

/// The analysis' corners, largest loss first (unknown Δt last, then lap
/// order); in lap order when the map does not place time loss.
pub fn loss_lines(analysis: &Analysis) -> Vec<LossLine> {
    let mut lines: Vec<LossLine> = analysis.rows().iter().map(LossLine::new).collect();
    if !analysis.time_loss_placed() {
        return lines;
    }
    lines.sort_by(|a, b| match (a.dt.is_finite(), b.dt.is_finite()) {
        (true, true) => b.dt.total_cmp(&a.dt),
        (true, false) => std::cmp::Ordering::Less,
        (false, true) => std::cmp::Ordering::Greater,
        (false, false) => std::cmp::Ordering::Equal,
    });
    lines
}

/// `≈1.55 s` (the split is always approximate: it sums over zones).
fn seconds(value: f64, approximate: bool) -> String {
    let (text, _) = format_delta(Some(value), 2, DeltaSense::LowerIsBetter);
    let text = text.trim_start_matches('+').to_string();
    if approximate {
        format!("≈{text} s")
    } else {
        format!("{text} s")
    }
}

/// `Corners ≈1.55 s, straights ≈0.90 s.`
pub fn split_text(corners: f64, straights: f64) -> String {
    format!(
        "Corners {}, straights {}.",
        seconds(corners, true),
        seconds(straights, true)
    )
}

pub struct TimeGoesPanel {
    app: AppState,
    focus_handle: FocusHandle,
    map: Entity<TrackMap>,
    shown: Option<Arc<Analysis>>,
    lines: Arc<[LossLine]>,
    selected: Option<SharedString>,
    followed_focus: Option<Selection>,
    _subscriptions: Vec<Subscription>,
}

impl TimeGoesPanel {
    pub fn new(app: AppState, cx: &mut Context<'_, Self>) -> Self {
        let map = cx.new(|cx| TrackMap::new(app.cursor.clone(), cx));
        let subscriptions = vec![
            cx.observe(&app.session, |this, _, cx| this.sync_analysis(cx)),
            cx.subscribe(&map, |this, _, event, cx| this.on_map_event(event, cx)),
            // The map repaints its own overlay on cursor moves; this panel
            // only follows the selected corner.
            cx.observe(&app.cursor, |this, _, cx| this.follow_cursor(cx)),
        ];
        let mut panel = Self {
            app,
            focus_handle: cx.focus_handle(),
            map,
            shown: None,
            lines: Arc::from([]),
            selected: None,
            followed_focus: None,
            _subscriptions: subscriptions,
        };
        panel.sync_analysis(cx);
        panel
    }

    /// The heat map view.
    pub fn map(&self) -> &Entity<TrackMap> {
        &self.map
    }

    /// The corners as listed, largest loss first.
    pub fn lines(&self) -> &[LossLine] {
        &self.lines
    }

    /// The corner the card describes.
    pub fn selected(&self) -> Option<&LossLine> {
        let id = self.selected.as_ref()?;
        self.lines.iter().find(|line| &line.id == id)
    }

    fn sync_analysis(&mut self, cx: &mut Context<'_, Self>) {
        let analysis = self.app.session.read(cx).analysis().cloned();
        let same = match (&analysis, &self.shown) {
            (Some(a), Some(b)) => Arc::ptr_eq(a, b),
            (None, None) => true,
            _ => false,
        };
        if !same {
            let data = analysis
                .as_deref()
                .map_or_else(TrackMapData::new, |analysis| {
                    let rate = analysis.loss_rate();
                    map::map_layers(analysis)
                        .with_heat((!rate.is_empty()).then(|| Arc::<[f64]>::from(rate)))
                });
            self.map.update(cx, |map, cx| {
                map.set_data(Arc::new(data), cx);
                map.set_focused_corner(None, cx);
            });
            self.lines = analysis
                .as_deref()
                .map(loss_lines)
                .unwrap_or_default()
                .into();
            self.shown = analysis;
            self.followed_focus = None;
            self.selected = None;
            self.follow_cursor(cx);
        }
        cx.notify();
    }

    fn on_map_event(&mut self, event: &TrackMapEvent, cx: &mut Context<'_, Self>) {
        match event {
            TrackMapEvent::MapHover(fraction) => {
                let fraction = *fraction;
                self.app
                    .cursor
                    .update(cx, |cursor, cx| cursor.set_hover(fraction, cx));
            }
            TrackMapEvent::MapClicked(fraction) => {
                let fraction = *fraction;
                self.app
                    .cursor
                    .update(cx, |cursor, cx| cursor.set_fraction(Some(fraction), cx));
            }
            _ => {}
        }
    }

    /// Derive the selected corner from the cursor (see the module docs);
    /// notify only when it changes.
    fn follow_cursor(&mut self, cx: &mut Context<'_, Self>) {
        let cursor = self.app.cursor.read(cx);
        let (focus, fraction) = (cursor.focus(), cursor.fraction());
        let selected = focus
            .and_then(|focus| self.lines.iter().find(|line| line.is_zone(focus)))
            .or_else(|| fraction.and_then(|f| self.lines.iter().find(|line| line.contains(f))))
            .or_else(|| self.lines.first())
            .map(|line| line.id.clone());
        if focus != self.followed_focus {
            self.followed_focus = focus;
            let id = focus.and_then(|focus| {
                self.shown
                    .as_ref()?
                    .corners()
                    .iter()
                    .position(|zone| {
                        (zone.start - focus.start).abs() < 1e-9
                            && (zone.end - focus.end).abs() < 1e-9
                    })
                    .map(map::corner_id)
            });
            self.map
                .update(cx, |map, cx| map.set_focused_corner(id, cx));
        }
        if selected != self.selected {
            self.selected = selected;
            cx.notify();
        }
    }

    /// Whether the analysis places time loss (see the module docs).
    fn placed(&self) -> bool {
        self.shown
            .as_deref()
            .is_some_and(Analysis::time_loss_placed)
    }

    fn approximate(&self) -> bool {
        self.shown
            .as_deref()
            .is_some_and(crate::workspace::status::analysis_approximate)
    }

    fn render_header(palette: &TracePalette, heat: bool, cx: &App) -> impl IntoElement {
        let theme = cx.theme();
        let key = |color: Hsla, label: &'static str| {
            h_flex()
                .gap_1()
                .child(div().w(rems(0.75)).h(rems(0.1875)).rounded_full().bg(color))
                .child(label)
        };
        h_flex()
            .justify_between()
            .gap_2()
            .child(
                div()
                    .text_heading()
                    .font_semibold()
                    .text_color(theme.foreground)
                    .child("Where the time goes"),
            )
            .when(heat, |this| {
                this.child(
                    h_flex()
                        .id("time-goes-legend")
                        .test_support()
                        .aria_label("Colour: time lost per metre, less to more")
                        .gap_2()
                        .text_caption()
                        .text_color(theme.muted_foreground)
                        .child(key(palette.heat(0.0), "less"))
                        .child(key(palette.heat(1.0), "more")),
                )
            })
    }

    #[expect(
        clippy::cast_possible_truncation,
        reason = "UI geometry deliberately projects bounded counts and f64 telemetry coordinates into f32 pixels."
    )]
    #[expect(
        clippy::too_many_lines,
        reason = "Keep this declarative layout or paint pass together so element order and geometry remain reviewable."
    )]
    fn render_table(&self, palette: &TracePalette, cx: &mut Context<'_, Self>) -> impl IntoElement {
        let theme = cx.theme();
        let approximate = self.approximate();
        let worst = self
            .lines
            .iter()
            .map(|line| line.dt)
            .filter(|dt| dt.is_finite())
            .fold(0.0_f64, f64::max);
        let number = |width: f32| {
            div()
                .w(rems(width))
                .flex_shrink_0()
                .text_right()
                .numeric()
                .whitespace_nowrap()
        };
        let head = h_flex()
            .px_2()
            .gap_2()
            .text_caption()
            .text_color(theme.muted_foreground)
            .child(div().w(rems(NAME_REMS)).flex_shrink_0().child("Corner"))
            .child(
                div()
                    .flex_1()
                    .min_w_0()
                    .truncate()
                    .child("Time lost, largest first"),
            )
            .child(number(NUMBER_REMS).child(if approximate { "≈ s" } else { "s" }))
            .child(number(NUMBER_REMS).child("Entry"));
        // The largest losses, plus the selected corner when it ranks lower.
        let listed = self
            .lines
            .iter()
            .enumerate()
            .filter(|(ix, line)| *ix < TABLE_ROWS || self.selected.as_ref() == Some(&line.id));
        let hidden = self.lines.len().saturating_sub(TABLE_ROWS);
        let rows = listed.map(|(_, line)| {
            let selected = self.selected.as_ref() == Some(&line.id);
            let share = if worst > 0.0 && line.dt.is_finite() {
                (line.dt / worst).clamp(0.0, 1.0)
            } else {
                0.0
            };
            let (dt_text, _) = format_delta(Some(line.dt), 2, DeltaSense::LowerIsBetter);
            let (entry_text, _) =
                format_delta(Some(line.entry_delta), 1, DeltaSense::HigherIsBetter);
            let spoken = format!(
                "{}: {} s lost, entry {} km/h against reference",
                line.name, dt_text, entry_text
            );
            let id = line.id.clone();
            h_flex()
                .id(SharedString::from(format!("time-goes-row-{}", line.id)))
                .test_support()
                .role(Role::ListItem)
                .aria_label(SharedString::from(spoken))
                .aria_selected(selected)
                .h(rems(1.4375))
                .px_2()
                .gap_2()
                .rounded(theme.radius)
                .cursor_pointer()
                .text_body()
                .when(selected, |row| row.bg(theme.list_active))
                .when(!selected, |row| row.hover(|row| row.bg(theme.list_hover)))
                .on_click(move |_, window, cx| {
                    window.dispatch_action(Box::new(FocusCorner { id: id.clone() }), cx);
                })
                .child(
                    div()
                        .w(rems(NAME_REMS))
                        .flex_shrink_0()
                        .font_medium()
                        .text_color(theme.foreground)
                        .truncate()
                        .child(line.short.clone()),
                )
                .child(
                    div().flex_1().min_w_0().h(rems(0.5)).child(
                        div()
                            .h_full()
                            .w(relative(share as f32))
                            .rounded_full()
                            .bg(palette.heat(0.25 + 0.75 * share as f32)),
                    ),
                )
                .child(
                    number(NUMBER_REMS)
                        .text_color(if line.dt < 0.0 && !approximate {
                            theme.success
                        } else {
                            theme.foreground
                        })
                        .child(dt_text),
                )
                .child(
                    number(NUMBER_REMS)
                        .text_color(theme.muted_foreground)
                        .child(entry_text),
                )
        });
        v_flex()
            .id("time-goes-table")
            .test_support()
            .role(Role::List)
            .aria_label(SharedString::from(format!(
                "{} corners by time lost, largest first",
                self.lines.len()
            )))
            .gap_px()
            .child(head)
            .children(rows)
            .when(hidden > 0, |this| {
                this.child(
                    h_flex().child(
                        Button::new("time-goes-show-all")
                            .ghost()
                            .small()
                            .label(SharedString::from(format!(
                                "Show all {} corners",
                                self.lines.len()
                            )))
                            .tooltip("Open the Corners table (Ctrl+4)")
                            .on_click(|_, window, cx| {
                                window.dispatch_action(Box::new(FocusPanel4), cx);
                            }),
                    ),
                )
            })
    }

    /// Why nothing is ranked: the map does not place time loss.
    fn render_unplaced(analysis: &Analysis, cx: &App) -> impl IntoElement {
        let theme = cx.theme();
        let gap = analysis
            .lap_time_delta()
            .map(|dt| {
                format!(
                    " ({} s)",
                    format_delta(Some(dt), 3, DeltaSense::LowerIsBetter).0
                )
            })
            .unwrap_or_default();
        let title = "Time loss can\u{2019}t be placed on the lap";
        let body = format!(
            "Without GPS or logger distance on both laps, they align by lap time, \
             which spreads the lap-time gap{gap} evenly. Corner notes are approximate."
        );
        v_flex()
            .id("time-goes-unplaced")
            .test_support()
            .role(Role::Group)
            .aria_label(SharedString::from(format!("{title}. {body}")))
            .gap_1()
            .px_2()
            .child(
                h_flex()
                    .gap_2()
                    .text_body()
                    .font_medium()
                    .text_color(theme.foreground)
                    .child(
                        Icon::new(IconName::TriangleAlert)
                            .small()
                            .text_color(theme.warning),
                    )
                    .child(title),
            )
            .child(
                div()
                    .text_label()
                    .numeric()
                    .text_color(theme.muted_foreground)
                    .child(body),
            )
    }

    fn render_summary(&self, cx: &App) -> Option<impl IntoElement> {
        let split = self.shown.as_ref()?.time_split()?;
        let text = format!(
            "{} Entry is primary speed against reference, km/h.",
            split_text(split.corners, split.straights)
        );
        Some(
            div()
                .id("time-goes-split")
                .test_support()
                .aria_label(SharedString::from(text.clone()))
                .px_2()
                .text_label()
                .text_color(cx.theme().muted_foreground)
                .child(text),
        )
    }

    #[expect(
        clippy::too_many_lines,
        reason = "Keep this declarative layout or paint pass together so element order and geometry remain reviewable."
    )]
    fn render_card(&self, cx: &App) -> Option<impl IntoElement> {
        let line = self.selected()?.clone();
        let theme = cx.theme();
        let approximate = self.approximate();
        let placed = self.placed();
        let signed = |value: f64, decimals: usize, sense: DeltaSense| {
            let (text, _) = format_delta(Some(value), decimals, sense);
            if approximate || decimals == 2 {
                format!("≈{text}")
            } else {
                text.to_string()
            }
        };
        // Without a placing map the entry points of the two laps are not
        // the same place: the primary's own speeds only.
        let speed = if placed {
            format!(
                "Entry {} km/h ({} vs R), minimum {} km/h.",
                format_value(Some(line.entry), 0),
                format_delta(Some(line.entry_delta), 1, DeltaSense::HigherIsBetter).0,
                format_value(Some(line.minimum), 0),
            )
        } else {
            format!(
                "Entry {} km/h, minimum {} km/h.",
                format_value(Some(line.entry), 0),
                format_value(Some(line.minimum), 0),
            )
        };
        let split = format!(
            "Entry {} s, exit {} s.",
            signed(line.entry_dt, 2, DeltaSense::LowerIsBetter),
            signed(line.exit_dt, 2, DeltaSense::LowerIsBetter),
        );
        // The notes continue the entry/exit sentence as one paragraph.
        let paragraph = placed
            .then_some(split)
            .into_iter()
            .chain(line.notes().map(ToString::to_string))
            .collect::<Vec<_>>()
            .join(" ");
        let loss_color = if line.dt > 0.0 {
            theme.danger
        } else if line.dt < 0.0 {
            theme.success
        } else {
            theme.muted_foreground
        };
        let id = line.id.clone();
        let label: SharedString = format!("Open {} in detail", line.name).into();
        let headline =
            placed.then(|| format!("{} s", signed(line.dt, 2, DeltaSense::LowerIsBetter)));
        let spoken = match &headline {
            Some(headline) => format!("{}, {headline}. {speed} {paragraph}", line.name),
            None => format!("{}. {speed} {paragraph}", line.name),
        };
        Some(
            v_flex()
                .id("time-goes-card")
                .test_support()
                .role(Role::Group)
                .aria_label(SharedString::from(spoken))
                .gap_1p5()
                .p_3()
                .rounded(theme.radius_lg)
                .border_1()
                .border_color(theme.border)
                .bg(theme.secondary)
                .text_body()
                .child(
                    h_flex()
                        .justify_between()
                        .gap_2()
                        .child(
                            div()
                                .text_heading()
                                .font_semibold()
                                .text_color(theme.foreground)
                                .child(line.name),
                        )
                        .children(headline.map(|headline| {
                            div().numeric().text_color(loss_color).child(headline)
                        })),
                )
                .child(div().numeric().text_color(theme.foreground).child(speed))
                .when(!paragraph.is_empty(), |this| {
                    this.child(
                        div()
                            .numeric()
                            .text_color(theme.foreground)
                            .child(paragraph),
                    )
                })
                .child(
                    h_flex().child(
                        Button::new("time-goes-open-corner")
                            .ghost()
                            .small()
                            .label(label)
                            .tooltip("Focus it in the traces and open the Corners table")
                            .on_click(move |_, window, cx| {
                                window
                                    .dispatch_action(Box::new(FocusCorner { id: id.clone() }), cx);
                                window.dispatch_action(Box::new(FocusPanel4), cx);
                            }),
                    ),
                ),
        )
    }

    fn render_ready(
        &self,
        analysis: &Analysis,
        window: &Window,
        cx: &mut Context<'_, Self>,
    ) -> AnyElement {
        // Measured runtime geometry: the window's height caps the map.
        let map_height = (window.rem_size() * MAP_REMS)
            .min(window.viewport_size().height * MAP_MAX_WINDOW_SHARE);
        let theme = cx.theme();
        let palette = TracePalette::from_theme(theme);
        let map_data = self.map.read(cx).data().clone();
        let has_map = !map_data.is_empty();
        let heat = map_data.is_heat();
        let outline_note = (has_map && map_data.is_on_outline()).then(|| {
            div()
                .id("time-goes-outline-note")
                .test_support()
                .text_caption()
                .text_color(cx.theme().muted_foreground)
                .child(map::OUTLINE_NOTE)
        });
        let body = if analysis.reference().is_none() {
            let description = "Pick a reference lap to see where this lap loses time.";
            panel_body(
                "time-goes-empty",
                description,
                empty_state(IconName::ChartPie, "No reference lap", description),
                cx,
            )
            .into_any_element()
        } else if self.lines.is_empty() {
            let description = "No corner zones were found for this lap.";
            panel_body(
                "time-goes-empty",
                description,
                empty_state(IconName::Map, "No corners", description),
                cx,
            )
            .into_any_element()
        } else if !analysis.time_loss_placed() {
            v_flex()
                .gap_3()
                .child(Self::render_unplaced(analysis, cx))
                .children(self.render_card(cx))
                .into_any_element()
        } else {
            v_flex()
                .gap_3()
                .child(self.render_table(&palette, cx))
                .children(self.render_summary(cx))
                .children(self.render_card(cx))
                .into_any_element()
        };
        v_flex()
            .id("time-goes-scroll")
            .size_full()
            .overflow_y_scrollbar()
            .px_3()
            .py_2()
            .gap_3()
            .child(Self::render_header(&palette, heat, cx))
            .when(has_map, |this| {
                this.child(div().flex_shrink_0().h(map_height).child(self.map.clone()))
            })
            .children(outline_note)
            .child(body)
            .into_any_element()
    }
}

impl gpui_kit::component::dock::BasePanel for TimeGoesPanel {
    fn panel_name(&self) -> &'static str {
        PanelKind::TimeGoes.name()
    }

    fn closable(&self, _: &App) -> bool {
        false
    }
}

impl gpui_kit::component::dock::Panel for TimeGoesPanel {
    fn tab_name(&self, _: &App) -> Option<SharedString> {
        Some(PanelKind::TimeGoes.title().into())
    }

    fn title(&mut self, _: &mut Window, _: &mut Context<'_, Self>) -> impl IntoElement {
        PanelKind::TimeGoes.title()
    }

    /// The heading is content ("Where the time goes"), not chrome.
    fn title_bar(&self, _: &App) -> bool {
        false
    }
}

impl gpui_kit::EventEmitter<gpui_kit::component::dock::PanelEvent> for TimeGoesPanel {}

impl gpui_kit::Focusable for TimeGoesPanel {
    fn focus_handle(&self, _: &App) -> FocusHandle {
        self.focus_handle.clone()
    }
}

impl Render for TimeGoesPanel {
    fn render(&mut self, window: &mut Window, cx: &mut Context<'_, Self>) -> impl IntoElement {
        let root = div()
            .id("time-goes-panel")
            .test_support()
            .track_focus(&self.focus_handle)
            .size_full();
        match self.shown.clone() {
            Some(analysis) => root.child(self.render_ready(&analysis, window, cx)),
            None => root.child(analysis_body("time-goes-summary", &self.app, cx, |_| {
                SharedString::default()
            })),
        }
    }
}

/// This panel's app-wide setup: its dock registration. Called once from
/// [`crate::panels::init`].
pub fn init(cx: &mut App) {
    crate::panels::register(PanelKind::TimeGoes, cx);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_split_reads_in_seconds() {
        assert_eq!(
            split_text(1.554, 0.9),
            "Corners ≈1.55 s, straights ≈0.90 s."
        );
    }
}
