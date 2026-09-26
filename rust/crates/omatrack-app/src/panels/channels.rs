//! Channels: which channels the trace workspace shows, and how.
//!
//! Two lists share one [`DataTable`]: the plottable channels of the primary
//! lap (visibility switch, colour swatch), and the recording's raw vendor
//! channels from the [`SourceChannels`] catalog, which the user opts in to.
//! The selected channel's style is edited below the list: primary and
//! reference colour, stroke width, fill opacity, lane height, lane sharing
//! and Reset style.
//!
//! Every edit writes `channels.<key>.*` through
//! [`Preferences::update`](crate::state::Preferences::update); the traces
//! panel observes Preferences and restyles. This panel keeps no copy of the
//! configuration: controls are re-synchronised from it whenever it changes
//! (a focused text field is left alone while the user types in it).
//!
//! Palette commands: `Show channel …` / `Hide channel …` ([`ToggleChannel`])
//! and `Reset channel styles` ([`ResetChannelStyles`]), handled app-wide.

use gpui_kit::component::{
    ActiveTheme as _, Colorize as _, IconName, Sizable as _, Theme,
    button::{Button, ButtonVariants as _},
    checkbox::Checkbox,
    color_picker::{ColorPicker, ColorPickerEvent, ColorPickerState},
    h_flex,
    input::{Input, InputEvent, InputState, NumberInput},
    slider::{Slider, SliderEvent, SliderState},
    switch::Switch,
    tab::{Tab, TabBar},
    table::{Column, DataTable, TableDelegate, TableEvent, TableState},
    v_flex,
};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::{
    App, AppContext as _, Context, Div, ElementId, Entity, FocusHandle, Hsla,
    InteractiveElement as _, IntoElement, ParentElement as _, Pixels, Render, SharedString,
    Stateful, StatefulInteractiveElement as _, Styled as _, Subscription, TestSupportExt as _,
    Window, div, rems,
};
use omatrack_core::overlay::{ChannelProvider as _, STANDARD_CHANNELS, SourceChannels};
use omatrack_library::Config;
use omatrack_trace::LaneStyle;
use omatrack_trace::palette::TracePalette;
use omatrack_ui::Swatch;
use omatrack_ui::TypeScale as _;

use crate::commands::{self, CommandCategory, CommandSpec};
use crate::panels::{PanelKind, empty_state};
use crate::state::{AppState, Preferences};

/// Stroke width range, logical pixels (AGENTS.md: 0.5–4, default 1.25).
pub const STROKE_WIDTH: (f64, f64) = (0.5, 4.0);
/// Fill opacity range (0–1).
pub const FILL_OPACITY: (f64, f64) = (0.0, 1.0);
/// Lane height range, percent of the workspace (1–100).
pub const HEIGHT_PERCENT: (f64, f64) = (1.0, 100.0);

/// The gap lane's key and title (a lane, styled like a channel).
const DELTA_KEY: &str = "delta";
const DELTA_TITLE: &str = super::traces::DELTA_TITLE;

/// Show or hide one channel's lane (palette: `Show channel Speed`).
#[derive(Debug, Clone, PartialEq, Eq, gpui_kit::Action)]
#[action(namespace = omatrack, no_json)]
pub struct ToggleChannel {
    pub key: SharedString,
}

gpui_kit::actions!(
    omatrack,
    [
        /// Reset every channel's style (visibility is kept).
        ResetChannelStyles
    ]
);

impl Eq for ResetChannelStyles {}

/// Which list the table shows.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ChannelList {
    /// The plottable channels of the primary lap.
    Channels,
    /// The recording's raw vendor channels, opt in.
    Source,
}

/// One channel row (plain data).
#[derive(Debug, Clone, PartialEq)]
pub struct ChannelEntry {
    key: SharedString,
    title: SharedString,
    unit: SharedString,
    /// Native rate for a source channel, Hz.
    rate: Option<f64>,
}

impl ChannelEntry {
    fn new(key: impl Into<SharedString>, title: impl Into<SharedString>, unit: &str) -> Self {
        Self {
            key: key.into(),
            title: title.into(),
            unit: unit.to_string().into(),
            rate: None,
        }
    }

    pub fn key(&self) -> &SharedString {
        &self.key
    }

    pub fn title(&self) -> &SharedString {
        &self.title
    }

    fn matches(&self, query: &str) -> bool {
        query.is_empty()
            || self.title.to_lowercase().contains(query)
            || self.key.to_lowercase().contains(query)
    }
}

/// Whether `key`'s lane shows.
pub fn is_visible(config: &Config, key: &str) -> bool {
    config.channel_style(key).visible
}

/// Set `key`'s visibility (always explicit, so it never depends on a
/// default elsewhere).
pub fn set_visible(config: &mut Config, key: &str, visible: bool) {
    config.channels.entry(key.to_string()).or_default().visible = Some(visible);
}

/// Drop every style setting of `key`; its visibility and any unknown keys
/// are kept.
pub fn reset_style(config: &mut Config, key: &str) {
    let Some(channel) = config.channels.get_mut(key) else {
        return;
    };
    channel.color = None;
    channel.reference_color = None;
    channel.stroke_width = None;
    channel.fill_opacity = None;
    channel.height_percent = None;
    channel.weight = None;
    channel.combine_with_previous = None;
    if channel.visible.is_none() && channel.extra.is_empty() {
        config.channels.remove(key);
    }
}

/// Reset every channel's style ([`reset_style`]).
pub fn reset_all_styles(config: &mut Config) {
    let keys: Vec<String> = config.channels.keys().cloned().collect();
    for key in keys {
        reset_style(config, &key);
    }
}

/// Snap `value` to `step` and clamp it to `range`.
fn clamp_step(value: f64, (min, max): (f64, f64), step: f64) -> f64 {
    ((value / step).round() * step).clamp(min, max)
}

/// A colour the user set in `channels.<key>.color` (user data, parsed the
/// way the traces panel parses it).
fn user_color(text: Option<&str>) -> Option<Hsla> {
    gpui_kit::component::try_parse_color(text?.trim()).ok()
}

/// The primary and reference colours a channel's lane draws with.
fn lane_colors(config: &Config, key: &str, theme: &Theme) -> (Hsla, Hsla) {
    let style = config.channel_style(key);
    let lane = LaneStyle::default_for(key)
        .with_color(user_color(style.color.as_deref()))
        .with_reference_color(user_color(style.reference_color.as_deref()));
    TracePalette::from_theme(theme).channel_colors(key, !style.combine_with_previous, &lane)
}

/// The palette title of a channel's visibility command.
fn toggle_title(config: &Config, entry: &ChannelEntry) -> String {
    if is_visible(config, &entry.key) {
        format!("Hide channel {}", entry.title)
    } else {
        format!("Show channel {}", entry.title)
    }
}

/// The channels listed before any lap loads: the gap lane and every standard
/// channel.
fn default_entries() -> Vec<ChannelEntry> {
    std::iter::once(ChannelEntry::new(DELTA_KEY, DELTA_TITLE, "s"))
        .chain(
            STANDARD_CHANNELS
                .iter()
                .map(|(key, title, unit)| ChannelEntry::new(*key, *title, unit)),
        )
        .collect()
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Col {
    Visible,
    Channel,
    Unit,
    Rate,
}

impl Col {
    fn of(list: ChannelList) -> &'static [Self] {
        match list {
            ChannelList::Channels => &[Self::Visible, Self::Channel, Self::Unit],
            ChannelList::Source => &[Self::Visible, Self::Channel, Self::Unit, Self::Rate],
        }
    }

    fn title(self, list: ChannelList) -> &'static str {
        match (self, list) {
            (Self::Visible, ChannelList::Channels) => "Show",
            (Self::Visible, ChannelList::Source) => "Load",
            (Self::Channel, _) => "Channel",
            (Self::Unit, _) => "Unit",
            (Self::Rate, _) => "Rate Hz",
        }
    }

    fn width(self) -> f32 {
        match self {
            Self::Visible => 3.5,
            Self::Channel => 11.0,
            Self::Unit => 4.0,
            Self::Rate => 4.5,
        }
    }
}

/// The [`TableDelegate`] of the channel list.
pub struct ChannelTable {
    list: ChannelList,
    /// The rows after the search filter.
    entries: Vec<ChannelEntry>,
    /// The selected channel, by key (the Channels list only).
    selected: Option<SharedString>,
    preferences: Entity<Preferences>,
    /// The window's rem size when built (column widths are pixels).
    rem: Pixels,
}

impl ChannelTable {
    pub fn entries(&self) -> &[ChannelEntry] {
        &self.entries
    }

    pub fn list(&self) -> ChannelList {
        self.list
    }

    fn position(&self, key: &str) -> Option<usize> {
        self.entries.iter().position(|entry| entry.key == key)
    }
}

impl TableDelegate for ChannelTable {
    fn columns_count(&self, _: &App) -> usize {
        Col::of(self.list).len()
    }

    fn rows_count(&self, _: &App) -> usize {
        self.entries.len()
    }

    fn column(&self, col_ix: usize, _: &App) -> Column {
        let col = Col::of(self.list)[col_ix];
        let key = match col {
            Col::Visible => "visible",
            Col::Channel => "channel",
            Col::Unit => "unit",
            Col::Rate => "rate",
        };
        Column::new(key, col.title(self.list))
            .width(self.rem * col.width())
            .movable(false)
            .when(col == Col::Visible, |column| column.selectable(false))
            .when(col == Col::Rate, Column::text_right)
    }

    fn render_th(
        &mut self,
        col_ix: usize,
        _: &mut Window,
        cx: &mut Context<'_, TableState<Self>>,
    ) -> impl IntoElement {
        let col = Col::of(self.list)[col_ix];
        div()
            .size_full()
            .flex()
            .items_center()
            .when(col == Col::Rate, gpui_kit::Styled::justify_end)
            .text_color(cx.theme().muted_foreground)
            .child(col.title(self.list))
    }

    fn render_tr(
        &mut self,
        row_ix: usize,
        _: &mut Window,
        _: &mut Context<'_, TableState<Self>>,
    ) -> Stateful<Div> {
        let key = self
            .entries
            .get(row_ix)
            .map(|entry| entry.key.clone())
            .unwrap_or_default();
        div().id(ElementId::Name(format!("channel:{key}").into()))
    }

    fn render_td(
        &mut self,
        row_ix: usize,
        col_ix: usize,
        _: &mut Window,
        cx: &mut Context<'_, TableState<Self>>,
    ) -> impl IntoElement {
        let Some(entry) = self.entries.get(row_ix) else {
            return div().into_any_element();
        };
        let config = self.preferences.read(cx).config();
        let theme = cx.theme();
        match Col::of(self.list)[col_ix] {
            Col::Visible => {
                let visible = is_visible(config, &entry.key);
                let preferences = self.preferences.clone();
                let key = entry.key.clone();
                let verb = match self.list {
                    ChannelList::Channels => "Show",
                    ChannelList::Source => "Load",
                };
                Switch::new(ElementId::Name(
                    format!("channel-visible:{}", entry.key).into(),
                ))
                .small()
                .checked(visible)
                .accessibility_label(format!("{verb} {}", entry.title))
                .on_change(move |checked, _, cx| {
                    let (key, checked) = (key.clone(), *checked);
                    preferences.update(cx, |preferences, cx| {
                        preferences.update(cx, |config| set_visible(config, &key, checked));
                    });
                })
                .into_any_element()
            }
            Col::Channel => {
                let (primary, _) = lane_colors(config, &entry.key, theme);
                h_flex()
                    .w_full()
                    .gap_2()
                    .when(self.list == ChannelList::Channels, |this| {
                        this.child(Swatch::new(primary))
                    })
                    .child(
                        div()
                            .flex_1()
                            .min_w_0()
                            .truncate()
                            .child(entry.title.clone()),
                    )
                    .into_any_element()
            }
            Col::Unit => div()
                .text_color(theme.muted_foreground)
                .child(entry.unit.clone())
                .into_any_element(),
            Col::Rate => h_flex()
                .w_full()
                .justify_end()
                .numeric()
                .text_color(theme.muted_foreground)
                .child(match entry.rate {
                    Some(rate) if rate > 0.0 => format!("{rate:.0}"),
                    _ => omatrack_ui::MISSING_VALUE.to_string(),
                })
                .into_any_element(),
        }
    }

    fn render_empty(
        &mut self,
        _: &mut Window,
        _: &mut Context<'_, TableState<Self>>,
    ) -> impl IntoElement {
        match self.list {
            ChannelList::Channels => empty_state(
                IconName::Search,
                "No matching channels",
                "Change the search to see more channels.",
            ),
            ChannelList::Source => empty_state(
                IconName::Inbox,
                "No source channels",
                "Load a primary lap to list its recording’s raw channels.",
            ),
        }
    }

    fn cell_text(&self, row_ix: usize, col_ix: usize, cx: &App) -> String {
        let Some(entry) = self.entries.get(row_ix) else {
            return String::new();
        };
        match Col::of(self.list)[col_ix] {
            Col::Visible => {
                let visible = is_visible(self.preferences.read(cx).config(), &entry.key);
                if visible { "Yes" } else { "No" }.to_string()
            }
            Col::Channel => entry.title.to_string(),
            Col::Unit => entry.unit.to_string(),
            Col::Rate => entry.rate.map(|r| format!("{r:.0}")).unwrap_or_default(),
        }
    }
}

/// The style editor's controls (built on the first render: they need the
/// window).
struct StyleControls {
    search: Entity<InputState>,
    color: Entity<ColorPickerState>,
    reference_color: Entity<ColorPickerState>,
    stroke: Entity<SliderState>,
    stroke_input: Entity<InputState>,
    fill: Entity<SliderState>,
    fill_input: Entity<InputState>,
    height_input: Entity<InputState>,
}

/// A number the style editor writes.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum StyleNumber {
    StrokeWidth,
    FillOpacity,
    HeightPercent,
}

impl StyleNumber {
    fn range(self) -> (f64, f64) {
        match self {
            Self::StrokeWidth => STROKE_WIDTH,
            Self::FillOpacity => FILL_OPACITY,
            Self::HeightPercent => HEIGHT_PERCENT,
        }
    }

    /// The precision a value is stored with.
    fn step(self) -> f64 {
        match self {
            Self::StrokeWidth => 0.05,
            Self::FillOpacity => 0.01,
            Self::HeightPercent => 1.0,
        }
    }

    fn decimals(self) -> usize {
        match self {
            Self::StrokeWidth | Self::FillOpacity => 2,
            Self::HeightPercent => 0,
        }
    }

    fn write(self, config: &mut Config, key: &str, value: f64) {
        let value = clamp_step(value, self.range(), self.step());
        let channel = config.channels.entry(key.to_string()).or_default();
        match self {
            Self::StrokeWidth => channel.stroke_width = Some(value),
            Self::FillOpacity => channel.fill_opacity = Some(value),
            Self::HeightPercent => channel.height_percent = Some(value),
        }
    }

    fn read(self, config: &Config, key: &str) -> f64 {
        let style = config.channel_style(key);
        match self {
            Self::StrokeWidth => style.stroke_width,
            Self::FillOpacity => style.fill_opacity,
            Self::HeightPercent => style.height_percent,
        }
    }
}

pub struct ChannelsPanel {
    app: AppState,
    focus_handle: FocusHandle,
    table: Option<Entity<TableState<ChannelTable>>>,
    controls: Option<StyleControls>,
    list: ChannelList,
    /// Every channel of each list, before the search filter.
    channels: Vec<ChannelEntry>,
    sources: Vec<ChannelEntry>,
    /// The recording the source list was read from (identity).
    source_recording: Option<usize>,
    query: String,
    /// The controls show stale values (a selection or preference change):
    /// the next render re-synchronises them.
    controls_stale: bool,
    subscriptions: Vec<Subscription>,
}

impl ChannelsPanel {
    pub fn new(app: AppState, cx: &mut Context<'_, Self>) -> Self {
        let subscriptions = vec![
            cx.observe(&app.session, |this, _, cx| this.sync_channels(cx)),
            cx.observe(&app.preferences, |this, _, cx| this.preferences_changed(cx)),
            cx.observe_global::<Theme>(|this, cx| {
                this.controls_stale = true;
                cx.notify();
            }),
        ];
        let mut panel = Self {
            app,
            focus_handle: cx.focus_handle(),
            table: None,
            controls: None,
            list: ChannelList::Channels,
            channels: default_entries(),
            sources: Vec::new(),
            source_recording: None,
            query: String::new(),
            controls_stale: true,
            subscriptions,
        };
        panel.sync_channels(cx);
        panel
    }

    /// The table state, once the panel has rendered.
    pub fn table(&self) -> Option<&Entity<TableState<ChannelTable>>> {
        self.table.as_ref()
    }

    /// The list on screen.
    pub fn list(&self) -> ChannelList {
        self.list
    }

    /// The channel whose style is being edited.
    pub fn selected<'a>(&self, cx: &'a App) -> Option<&'a SharedString> {
        self.table.as_ref()?.read(cx).delegate().selected.as_ref()
    }

    /// Select a channel of the Channels list by key.
    pub fn select_channel(&mut self, key: &str, cx: &mut Context<'_, Self>) {
        if self.list != ChannelList::Channels {
            self.set_list(ChannelList::Channels, cx);
        }
        let Some(table) = self.table.clone() else {
            return;
        };
        table.update(cx, |table, cx| {
            if let Some(ix) = table.delegate().position(key) {
                table.delegate_mut().selected = Some(key.to_string().into());
                table.set_selected_row(ix, cx);
            }
        });
        self.controls_stale = true;
        cx.notify();
    }

    /// Rebuild the channel lists from the session's primary lap.
    fn sync_channels(&mut self, cx: &mut Context<'_, Self>) {
        let session = self.app.session.read(cx);
        let primary = session.primary().and_then(|slot| slot.loaded()).cloned();
        let recording = primary.as_ref().map(|lap| lap.recording().clone());
        let recording_id = recording
            .as_ref()
            .map(|r| std::sync::Arc::as_ptr(r) as usize);
        if recording_id != self.source_recording {
            self.source_recording = recording_id;
            self.sources = recording
                .as_ref()
                .map(|recording| {
                    SourceChannels
                        .catalog(recording)
                        .into_iter()
                        .map(|info| ChannelEntry {
                            key: info.key.into(),
                            title: info.title.into(),
                            unit: info.unit.into(),
                            rate: Some(info.sample_rate_hz),
                        })
                        .collect()
                })
                .unwrap_or_default();
        }
        let mut channels = match &primary {
            Some(lap) => std::iter::once(ChannelEntry::new(DELTA_KEY, DELTA_TITLE, "s"))
                .chain(lap.overlays().iter().flat_map(|group| {
                    group
                        .channels
                        .iter()
                        .map(|c| ChannelEntry::new(c.key.clone(), c.title.clone(), &c.unit))
                }))
                .collect::<Vec<_>>(),
            None => default_entries(),
        };
        // Opted-in source channels are styled like any other lane.
        let config = self.app.preferences.read(cx).config();
        for source in &self.sources {
            if is_visible(config, &source.key) && !channels.iter().any(|c| c.key == source.key) {
                channels.push(source.clone());
            }
        }
        if channels != self.channels {
            self.channels = channels;
            self.refresh_table(cx);
        }
        self.register_commands(cx);
        cx.notify();
    }

    fn preferences_changed(&mut self, cx: &mut Context<'_, Self>) {
        // A newly opted-in source channel joins the Channels list.
        self.sync_channels(cx);
        if let Some(table) = &self.table {
            table.update(cx, |_, cx| cx.notify());
        }
        self.controls_stale = true;
        cx.notify();
    }

    /// Palette entries for every listed channel, titled by what they do now.
    fn register_commands(&self, cx: &mut App) {
        let config = self.app.preferences.read(cx).config().clone();
        for entry in &self.channels {
            commands::register(
                cx,
                CommandSpec::new(
                    format!("channel-visibility:{}", entry.key),
                    toggle_title(&config, entry),
                    CommandCategory::Commands,
                    ToggleChannel {
                        key: entry.key.clone(),
                    },
                )
                .keywords(["channel", "lane", "show", "hide", "trace"]),
            );
        }
    }

    fn set_list(&mut self, list: ChannelList, cx: &mut Context<'_, Self>) {
        if self.list == list {
            return;
        }
        self.list = list;
        self.refresh_table(cx);
        self.controls_stale = true;
        cx.notify();
    }

    fn set_query(&mut self, query: &str, cx: &mut Context<'_, Self>) {
        let query = query.trim().to_lowercase();
        if query == self.query {
            return;
        }
        self.query = query;
        self.refresh_table(cx);
        cx.notify();
    }

    /// Push the current list (filtered) into the table, keeping the
    /// selected channel.
    fn refresh_table(&mut self, cx: &mut Context<'_, Self>) {
        let Some(table) = self.table.clone() else {
            return;
        };
        let source = match self.list {
            ChannelList::Channels => &self.channels,
            ChannelList::Source => &self.sources,
        };
        let entries: Vec<ChannelEntry> = source
            .iter()
            .filter(|entry| entry.matches(&self.query))
            .cloned()
            .collect();
        let list = self.list;
        table.update(cx, |table, cx| {
            let delegate = table.delegate_mut();
            delegate.list = list;
            delegate.entries = entries;
            let target = match list {
                ChannelList::Channels => delegate
                    .selected
                    .as_ref()
                    .and_then(|key| delegate.position(key))
                    .or_else(|| (!delegate.entries.is_empty()).then_some(0)),
                ChannelList::Source => None,
            };
            if list == ChannelList::Channels {
                delegate.selected = target.map(|ix| delegate.entries[ix].key.clone());
            }
            table.refresh(cx);
            match target {
                Some(ix) => table.set_selected_row(ix, cx),
                None => table.clear_selection(cx),
            }
            cx.notify();
        });
        self.controls_stale = true;
    }

    /// Move keyboard focus from the panel onto its table.
    fn focus_table(&mut self, window: &mut Window, cx: &mut Context<'_, Self>) {
        if let Some(table) = &self.table {
            let handle = gpui_kit::Focusable::focus_handle(table.read(cx), cx);
            window.focus(&handle, cx);
        }
    }

    #[expect(
        clippy::cast_possible_truncation,
        reason = "UI geometry deliberately projects bounded counts and f64 telemetry coordinates into f32 pixels."
    )]
    #[expect(
        clippy::too_many_lines,
        reason = "Keep this declarative layout or paint pass together so element order and geometry remain reviewable."
    )]
    fn ensure_controls(&mut self, window: &mut Window, cx: &mut Context<'_, Self>) {
        if self.controls.is_some() {
            return;
        }
        let rem = window.rem_size();
        let table = cx.new(|cx| {
            TableState::new(
                ChannelTable {
                    list: self.list,
                    entries: Vec::new(),
                    selected: None,
                    preferences: self.app.preferences.clone(),
                    rem,
                },
                window,
                cx,
            )
            .col_movable(false)
            .col_selectable(false)
            .sortable(false)
            .loop_selection(false)
        });
        let number = |window: &mut Window, cx: &mut Context<'_, Self>, number: StyleNumber| {
            let (min, max) = number.range();
            cx.new(|cx| {
                InputState::new(window, cx)
                    .step(match number {
                        StyleNumber::StrokeWidth => 0.25,
                        StyleNumber::FillOpacity => 0.05,
                        StyleNumber::HeightPercent => 1.0,
                    })
                    .min(min)
                    .max(max)
            })
        };
        let controls = StyleControls {
            search: cx.new(|cx| InputState::new(window, cx).placeholder("Search channels")),
            color: cx.new(|cx| ColorPickerState::new(window, cx)),
            reference_color: cx.new(|cx| ColorPickerState::new(window, cx)),
            stroke: cx.new(|_| {
                SliderState::new()
                    .min(STROKE_WIDTH.0 as f32)
                    .max(STROKE_WIDTH.1 as f32)
                    .step(0.05)
            }),
            stroke_input: number(window, cx, StyleNumber::StrokeWidth),
            fill: cx.new(|_| {
                SliderState::new()
                    .min(FILL_OPACITY.0 as f32)
                    .max(FILL_OPACITY.1 as f32)
                    .step(0.01)
            }),
            fill_input: number(window, cx, StyleNumber::FillOpacity),
            height_input: number(window, cx, StyleNumber::HeightPercent),
        };
        let mut subscriptions = vec![
            cx.subscribe_in(&table, window, Self::on_table_event),
            cx.subscribe_in(&controls.search, window, |this, state, event, _, cx| {
                if matches!(event, InputEvent::Change) {
                    let query = state.read(cx).value().to_string();
                    this.set_query(&query, cx);
                }
            }),
            cx.subscribe_in(&controls.color, window, |this, _, event, _, cx| {
                let ColorPickerEvent::Change(color) = event;
                this.write_color(false, *color, cx);
            }),
            cx.subscribe_in(
                &controls.reference_color,
                window,
                |this, _, event, _, cx| {
                    let ColorPickerEvent::Change(color) = event;
                    this.write_color(true, *color, cx);
                },
            ),
        ];
        for (slider, number) in [
            (&controls.stroke, StyleNumber::StrokeWidth),
            (&controls.fill, StyleNumber::FillOpacity),
        ] {
            subscriptions.push(
                cx.subscribe_in(slider, window, move |this, _, event, _, cx| {
                    let value = match event {
                        SliderEvent::Change(value) | SliderEvent::Release(value) => value.end(),
                    };
                    this.write_number(number, f64::from(value), cx);
                }),
            );
        }
        for (input, number) in [
            (&controls.stroke_input, StyleNumber::StrokeWidth),
            (&controls.fill_input, StyleNumber::FillOpacity),
            (&controls.height_input, StyleNumber::HeightPercent),
        ] {
            subscriptions.push(
                cx.subscribe_in(input, window, move |this, state, event, _, cx| {
                    match event {
                        InputEvent::Change => {
                            if let Ok(value) = state.read(cx).value().trim().parse::<f64>() {
                                this.write_number(number, value, cx);
                            }
                        }
                        // Show the stored (clamped) value once the field is left.
                        InputEvent::Blur | InputEvent::PressEnter { .. } => {
                            this.controls_stale = true;
                            cx.notify();
                        }
                        InputEvent::Focus => {}
                    }
                }),
            );
        }
        self.subscriptions.extend(subscriptions);
        self.table = Some(table);
        // Ctrl+N may have focused the panel before its table existed;
        // keyboard focus belongs on the rows.
        let own = self.focus_handle.clone();
        self.subscriptions
            .push(cx.on_focus(&own, window, |this, window, cx| {
                this.focus_table(window, cx);
            }));
        if own.is_focused(window) {
            cx.defer_in(window, Self::focus_table);
        }
        self.controls = Some(controls);
        self.refresh_table(cx);
    }

    fn on_table_event(
        &mut self,
        table: &Entity<TableState<ChannelTable>>,
        event: &TableEvent,
        _: &mut Window,
        cx: &mut Context<'_, Self>,
    ) {
        if let TableEvent::SelectRow(ix) = event {
            let ix = *ix;
            let changed = table.update(cx, |table, _| {
                let delegate = table.delegate_mut();
                if delegate.list != ChannelList::Channels {
                    return false;
                }
                let key = delegate.entries.get(ix).map(|entry| entry.key.clone());
                let changed = key.is_some() && key != delegate.selected;
                if changed {
                    delegate.selected = key;
                }
                changed
            });
            if changed {
                self.controls_stale = true;
                cx.notify();
            }
        }
    }

    fn selected_entry(&self, cx: &App) -> Option<ChannelEntry> {
        let key = self.selected(cx)?;
        self.channels
            .iter()
            .find(|entry| &entry.key == key)
            .cloned()
    }

    fn edit(&mut self, cx: &mut Context<'_, Self>, edit: impl FnOnce(&mut Config, &str)) {
        let Some(key) = self.selected(cx).cloned() else {
            return;
        };
        self.app.preferences.update(cx, |preferences, cx| {
            preferences.update(cx, |config| edit(config, &key));
        });
    }

    fn write_number(&mut self, number: StyleNumber, value: f64, cx: &mut Context<'_, Self>) {
        if value.is_finite() {
            self.edit(cx, |config, key| number.write(config, key, value));
        }
    }

    fn write_color(&mut self, reference: bool, color: Option<Hsla>, cx: &mut Context<'_, Self>) {
        let hex = color.map(|color| color.to_hex());
        self.edit(cx, |config, key| {
            let channel = config.channels.entry(key.to_string()).or_default();
            if reference {
                channel.reference_color = hex;
            } else {
                channel.color = hex;
            }
        });
    }

    /// Bring every control to the selected channel's stored style. A text
    /// field the user is typing in keeps its text.
    #[expect(
        clippy::cast_possible_truncation,
        reason = "UI geometry deliberately projects bounded counts and f64 telemetry coordinates into f32 pixels."
    )]
    fn sync_controls(&mut self, window: &mut Window, cx: &mut Context<'_, Self>) {
        if !std::mem::take(&mut self.controls_stale) {
            return;
        }
        let (Some(controls), Some(key)) = (&self.controls, self.selected(cx).cloned()) else {
            return;
        };
        let config = self.app.preferences.read(cx).config().clone();
        let (primary, reference) = lane_colors(&config, &key, cx.theme());
        controls
            .color
            .update(cx, |state, cx| state.set_value(primary, window, cx));
        controls
            .reference_color
            .update(cx, |state, cx| state.set_value(reference, window, cx));
        for (slider, number) in [
            (&controls.stroke, StyleNumber::StrokeWidth),
            (&controls.fill, StyleNumber::FillOpacity),
        ] {
            let value = number.read(&config, &key) as f32;
            slider.update(cx, |state, cx| state.set_value(value, window, cx));
        }
        for (input, number) in [
            (&controls.stroke_input, StyleNumber::StrokeWidth),
            (&controls.fill_input, StyleNumber::FillOpacity),
            (&controls.height_input, StyleNumber::HeightPercent),
        ] {
            if gpui_kit::Focusable::focus_handle(input.read(cx), cx).is_focused(window) {
                continue;
            }
            let text = format!("{:.*}", number.decimals(), number.read(&config, &key));
            input.update(cx, |state, cx| {
                if state.value() != text.as_str() {
                    state.set_value(text, window, cx);
                }
            });
        }
    }

    #[expect(
        clippy::too_many_lines,
        reason = "Keep this declarative layout or paint pass together so element order and geometry remain reviewable."
    )]
    fn render_editor(
        &self,
        entry: &ChannelEntry,
        cx: &mut Context<'_, Self>,
    ) -> gpui_kit::AnyElement {
        let Some(controls) = &self.controls else {
            return div().into_any_element();
        };
        let theme = cx.theme();
        let config = self.app.preferences.read(cx).config();
        let style = config.channel_style(&entry.key);
        let featured = vec![
            theme.primary,
            theme.warning,
            theme.success,
            theme.danger,
            theme.info,
            theme.chart_1,
            theme.chart_2,
            theme.chart_3,
            theme.chart_4,
            theme.chart_5,
        ];
        let row = |label: &'static str| {
            h_flex().gap_2().child(
                div()
                    .w(rems(5.5))
                    .flex_shrink_0()
                    .text_color(theme.muted_foreground)
                    .child(label),
            )
        };
        let key = entry.key.clone();
        v_flex()
            .id("channel-style")
            .test_support()
            .aria_label(SharedString::from(format!("{} style", entry.title)))
            .flex_shrink_0()
            .gap_2()
            .p_2()
            .border_t_1()
            .border_color(theme.border)
            .text_sm()
            .child(
                h_flex()
                    .justify_between()
                    .child(
                        div()
                            .font_weight(gpui_kit::FontWeight::SEMIBOLD)
                            .child(entry.title.clone()),
                    )
                    .child(
                        Button::new("channel-reset-style")
                            .ghost()
                            .small()
                            .icon(IconName::Undo2)
                            .label("Reset style")
                            .on_click(cx.listener(move |this, _, _, cx| {
                                let key = key.clone();
                                this.app.preferences.update(cx, |preferences, cx| {
                                    preferences.update(cx, |config| reset_style(config, &key));
                                });
                            })),
                    ),
            )
            .child(
                row("Colour")
                    .child(
                        ColorPicker::new(&controls.color)
                            .small()
                            .featured_colors(featured.clone())
                            .accessibility_label(format!("{} primary colour", entry.title)),
                    )
                    .child(div().text_color(theme.muted_foreground).child("Reference"))
                    .child(
                        ColorPicker::new(&controls.reference_color)
                            .small()
                            .featured_colors(featured)
                            .accessibility_label(format!("{} reference colour", entry.title)),
                    ),
            )
            .child(
                row("Width")
                    .child(
                        div()
                            .id("channel-stroke-width")
                            .test_support()
                            .flex_1()
                            .min_w_0()
                            .child(Slider::new(&controls.stroke)),
                    )
                    .child(
                        div()
                            .id("channel-stroke-width-input")
                            .test_support()
                            .w(rems(6.5))
                            .child(NumberInput::new(&controls.stroke_input).small()),
                    ),
            )
            .child(
                row("Fill")
                    .child(
                        div()
                            .id("channel-fill-opacity")
                            .test_support()
                            .flex_1()
                            .min_w_0()
                            .child(Slider::new(&controls.fill)),
                    )
                    .child(
                        div()
                            .w(rems(6.5))
                            .child(NumberInput::new(&controls.fill_input).small()),
                    ),
            )
            .child(
                row("Height %")
                    .child(
                        div()
                            .id("channel-height-input")
                            .test_support()
                            .w(rems(6.5))
                            .child(NumberInput::new(&controls.height_input).small()),
                    )
                    .child(
                        Checkbox::new("channel-combine")
                            .label("Share the previous lane")
                            .checked(style.combine_with_previous)
                            .on_change(cx.listener(|this, checked: &bool, _, cx| {
                                let checked = *checked;
                                this.edit(cx, |config, key| {
                                    config
                                        .channels
                                        .entry(key.to_string())
                                        .or_default()
                                        .combine_with_previous = Some(checked);
                                });
                            })),
                    ),
            )
            .into_any_element()
    }
}

impl gpui_kit::component::dock::BasePanel for ChannelsPanel {
    fn panel_name(&self) -> &'static str {
        PanelKind::Channels.name()
    }

    fn closable(&self, _: &App) -> bool {
        false
    }
}

impl gpui_kit::component::dock::Panel for ChannelsPanel {
    fn tab_name(&self, _: &App) -> Option<SharedString> {
        Some(PanelKind::Channels.title().into())
    }

    fn title(&mut self, _: &mut Window, _: &mut Context<'_, Self>) -> impl IntoElement {
        PanelKind::Channels.title()
    }
}

impl gpui_kit::EventEmitter<gpui_kit::component::dock::PanelEvent> for ChannelsPanel {}

impl gpui_kit::Focusable for ChannelsPanel {
    /// The list takes keyboard focus once it exists.
    fn focus_handle(&self, cx: &App) -> FocusHandle {
        match &self.table {
            Some(table) => gpui_kit::Focusable::focus_handle(table.read(cx), cx),
            None => self.focus_handle.clone(),
        }
    }
}

impl Render for ChannelsPanel {
    fn render(&mut self, window: &mut Window, cx: &mut Context<'_, Self>) -> impl IntoElement {
        self.ensure_controls(window, cx);
        self.sync_controls(window, cx);
        let root = div()
            .id("channels-panel")
            .test_support()
            .track_focus(&self.focus_handle)
            .size_full();
        let (Some(table), Some(controls)) = (self.table.clone(), &self.controls) else {
            return root.into_any_element();
        };
        let search = controls.search.clone();
        let list = self.list;
        let sources = self.sources.len();
        let editor = match list {
            ChannelList::Channels => self.selected_entry(cx),
            ChannelList::Source => None,
        };
        let editor = editor.map(|entry| self.render_editor(&entry, cx));
        let theme = cx.theme();
        root.child(
            v_flex()
                .size_full()
                .text_sm()
                .child(
                    v_flex()
                        .flex_shrink_0()
                        .gap_2()
                        .p_2()
                        .child(
                            TabBar::new("channel-lists")
                                .underline()
                                .small()
                                .selected_index(match list {
                                    ChannelList::Channels => 0,
                                    ChannelList::Source => 1,
                                })
                                .child(Tab::new().label("Channels"))
                                .child(Tab::new().label(if sources > 0 {
                                    format!("Source channels ({sources})")
                                } else {
                                    "Source channels".to_string()
                                }))
                                .on_click(cx.listener(|this, ix: &usize, _, cx| {
                                    let list = if *ix == 0 {
                                        ChannelList::Channels
                                    } else {
                                        ChannelList::Source
                                    };
                                    this.set_list(list, cx);
                                })),
                        )
                        .child(
                            Input::new(&search)
                                .small()
                                .cleanable(true)
                                .prefix(gpui_kit::component::Icon::new(IconName::Search).small()),
                        )
                        .when(list == ChannelList::Source, |this| {
                            this.child(div().text_xs().text_color(theme.muted_foreground).child(
                                "Raw vendor channels, resampled onto the lap when \
                                         loaded. Loaded ones join the Channels list.",
                            ))
                        }),
                )
                .child(
                    div()
                        .flex_1()
                        .min_h_0()
                        .child(DataTable::new(&table).small().bordered(false)),
                )
                .children(editor),
        )
        .into_any_element()
    }
}

/// This panel's app-wide setup: its dock registration, the channel
/// commands and their app-wide handlers. Called once from
/// [`crate::panels::init`].
pub fn init(cx: &mut App) {
    crate::panels::register(PanelKind::Channels, cx);
    let defaults = Config::default();
    for entry in default_entries() {
        commands::register(
            cx,
            CommandSpec::new(
                format!("channel-visibility:{}", entry.key),
                toggle_title(&defaults, &entry),
                CommandCategory::Commands,
                ToggleChannel {
                    key: entry.key.clone(),
                },
            )
            .keywords(["channel", "lane", "show", "hide", "trace"]),
        );
    }
    commands::register(
        cx,
        CommandSpec::new(
            "reset-channel-styles",
            "Reset channel styles",
            CommandCategory::Commands,
            ResetChannelStyles,
        )
        .keywords(["channel", "colour", "color", "width", "style"]),
    );
    cx.on_action(|action: &ToggleChannel, cx| {
        let Some(app) = AppState::try_global(cx).cloned() else {
            return;
        };
        let key = action.key.to_string();
        app.preferences.update(cx, |preferences, cx| {
            preferences.update(cx, |config| {
                let visible = is_visible(config, &key);
                set_visible(config, &key, !visible);
            });
        });
    });
    cx.on_action(|_: &ResetChannelStyles, cx| {
        let Some(app) = AppState::try_global(cx).cloned() else {
            return;
        };
        app.preferences.update(cx, |preferences, cx| {
            preferences.update(cx, reset_all_styles);
        });
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn numbers_are_stored_snapped_and_clamped() {
        let mut config = Config::default();
        StyleNumber::StrokeWidth.write(&mut config, "speed", 9.0);
        StyleNumber::FillOpacity.write(&mut config, "speed", -1.0);
        StyleNumber::HeightPercent.write(&mut config, "speed", 33.4);
        let speed = &config.channels["speed"];
        assert_eq!(speed.stroke_width, Some(4.0));
        assert_eq!(speed.fill_opacity, Some(0.0));
        assert_eq!(speed.height_percent, Some(33.0));
        StyleNumber::StrokeWidth.write(&mut config, "speed", 1.26);
        assert!((config.channels["speed"].stroke_width.unwrap() - 1.25).abs() < 1e-9);
    }

    #[test]
    fn reset_style_keeps_visibility() {
        let mut config = Config::default();
        set_visible(&mut config, "rpm", true);
        StyleNumber::StrokeWidth.write(&mut config, "rpm", 2.0);
        StyleNumber::StrokeWidth.write(&mut config, "speed", 2.0);
        reset_all_styles(&mut config);
        assert_eq!(config.channels["rpm"].visible, Some(true));
        assert_eq!(config.channels["rpm"].stroke_width, None);
        assert!(
            !config.channels.contains_key("speed"),
            "an empty entry goes"
        );
    }
}
