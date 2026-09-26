//! Inspector: every channel's primary, reference and delta values at the
//! cursor.
//!
//! The channel list (the primary's overlay channels, each paired with the
//! reference's channel of the same key) is a presentation snapshot rebuilt
//! only when the session's analysis changes identity. Readouts are computed
//! while rendering, from the cursor's readout fraction (hover, else the
//! cursor), for the visible rows of a virtualized list only.
//!
//! The panel observes [`CursorState`](omatrack_trace::CursorState) for its
//! readouts; nothing it does notifies the session or any other panel.

use std::sync::Arc;

use gpui_kit::component::{ActiveTheme as _, IconName, Sizable as _, h_flex, v_flex};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::{
    App, Context, FocusHandle, InteractiveElement as _, IntoElement, ParentElement as _, Render,
    SharedString, StatefulInteractiveElement as _, Styled as _, Subscription, TestSupportExt as _,
    UniformListScrollHandle, Window, div, rems, uniform_list,
};
use omatrack_core::session::Analysis;
use omatrack_trace::scale::value_at_fraction;
use omatrack_ui::{DeltaSense, DeltaText, LapRole, Swatch, format_value};

use crate::panels::{PanelKind, analysis_body, empty_state, panel_body};
use crate::state::AppState;

/// How a channel's delta reads.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum DeltaKind {
    /// Coloured by gain or loss (speed: higher is better).
    Scored(DeltaSense),
    /// A plain signed difference: neither direction is better.
    Neutral,
}

/// One channel of the inspector (plain data; the arrays are shared with the
/// analysis).
#[derive(Clone)]
pub struct InspectorChannel {
    key: SharedString,
    title: SharedString,
    unit: SharedString,
    primary: Arc<[f64]>,
    reference: Option<Arc<[f64]>>,
    decimals: usize,
    /// Display factor of the samples ([`omatrack_trace::scene::display_scale`]).
    scale: f64,
    delta: DeltaKind,
    /// Sampled at the nearest sample instead of interpolated (gear).
    stepped: bool,
    /// Whether the primary carries any finite sample; a channel without one
    /// is listed muted and marked "No data" instead of a row of dashes.
    has_data: bool,
}

impl InspectorChannel {
    pub fn key(&self) -> &SharedString {
        &self.key
    }

    pub fn title(&self) -> &SharedString {
        &self.title
    }

    /// Whether the primary lap has any value for this channel.
    pub fn has_data(&self) -> bool {
        self.has_data
    }

    fn value(&self, values: &[f64], fraction: f64) -> f64 {
        if self.stepped && values.len() > 1 {
            let ix = (fraction.clamp(0.0, 1.0) * (values.len() - 1) as f64).round() as usize;
            return values[ix] * self.scale;
        }
        value_at_fraction(values, fraction) * self.scale
    }
}

/// Decimals for a channel's readout, by unit.
fn decimals(key: &str, unit: &str) -> usize {
    match (key, unit) {
        ("gear", _) => 0,
        (_, "%" | "rpm" | "m") => 0,
        (_, "g") => 2,
        (_, "°") => 6,
        (_, "l") => 2,
        _ => 1,
    }
}

/// The inspector's channel list for an analysis.
fn channels(analysis: &Analysis) -> Vec<InspectorChannel> {
    let reference = analysis.reference();
    analysis
        .primary()
        .overlays()
        .iter()
        .flat_map(|group| {
            group.channels.iter().map(move |channel| {
                let paired = reference.and_then(|lap| {
                    lap.overlays()
                        .iter()
                        .find(|g| g.provider == group.provider)
                        .and_then(|g| g.channel(&channel.key))
                        .map(|c| c.values.clone())
                });
                let max = channel
                    .values
                    .iter()
                    .chain(paired.iter().flat_map(|values| values.iter()))
                    .copied()
                    .filter(|v| v.is_finite())
                    .fold(f64::NEG_INFINITY, f64::max);
                InspectorChannel {
                    scale: omatrack_trace::scene::display_scale(&channel.unit, max),
                    key: channel.key.clone().into(),
                    title: channel.title.clone().into(),
                    unit: channel.unit.clone().into(),
                    primary: channel.values.clone(),
                    reference: paired,
                    decimals: decimals(&channel.key, &channel.unit),
                    delta: if channel.key == "speed" {
                        DeltaKind::Scored(DeltaSense::HigherIsBetter)
                    } else {
                        DeltaKind::Neutral
                    },
                    stepped: channel.key == "gear",
                    has_data: channel.values.iter().any(|v| v.is_finite()),
                }
            })
        })
        .collect()
}

/// Where the readouts sample.
#[derive(Clone, Copy, Debug, PartialEq)]
struct Probe {
    primary: f64,
    reference: f64,
}

pub struct InspectorPanel {
    app: AppState,
    focus_handle: FocusHandle,
    shown: Option<Arc<Analysis>>,
    channels: Arc<[InspectorChannel]>,
    scroll: UniformListScrollHandle,
    renders: usize,
    _subscriptions: Vec<Subscription>,
}

impl InspectorPanel {
    pub fn new(app: AppState, cx: &mut Context<Self>) -> Self {
        let subscriptions = vec![
            cx.observe(&app.session, |this, _, cx| this.sync_analysis(cx)),
            cx.observe(&app.cursor, |_, _, cx| cx.notify()),
        ];
        let mut panel = Self {
            app,
            focus_handle: cx.focus_handle(),
            shown: None,
            channels: Arc::from(Vec::new()),
            scroll: UniformListScrollHandle::new(),
            renders: 0,
            _subscriptions: subscriptions,
        };
        panel.sync_analysis(cx);
        panel
    }

    /// The channels listed, in order.
    pub fn channels(&self) -> &[InspectorChannel] {
        &self.channels
    }

    /// Times this panel rendered.
    pub fn render_count(&self) -> usize {
        self.renders
    }

    fn sync_analysis(&mut self, cx: &mut Context<Self>) {
        let analysis = self.app.session.read(cx).analysis().cloned();
        let same = match (&analysis, &self.shown) {
            (Some(a), Some(b)) => Arc::ptr_eq(a, b),
            (None, None) => true,
            _ => false,
        };
        if !same {
            self.channels = analysis.as_deref().map(channels).unwrap_or_default().into();
            self.shown = analysis;
        }
        cx.notify();
    }

    fn probe(&self, cx: &App) -> Option<Probe> {
        let fraction = self.app.cursor.read(cx).readout_fraction()?;
        let analysis = self.shown.as_ref()?;
        let reference = analysis.comparison().map_or(fraction, |c| {
            c.compare_fraction_for_primary_fraction(fraction)
        });
        Some(Probe {
            primary: fraction,
            reference,
        })
    }

    fn render_header(
        &self,
        analysis: &Analysis,
        probe: Option<Probe>,
        cx: &App,
    ) -> impl IntoElement {
        let theme = cx.theme();
        // The status bar's cursor text, so the two never disagree.
        let position: SharedString = match probe {
            Some(probe) => crate::workspace::status::cursor_text(analysis, probe.primary),
            None => "No cursor".into(),
        };
        let delta = probe
            .filter(|_| !analysis.delta().is_empty())
            .map(|probe| value_at_fraction(analysis.delta(), probe.primary));
        let has_reference = analysis.reference().is_some();
        let approximate = analysis
            .comparison()
            .is_some_and(|c| crate::workspace::status::approximate(c.confidence()));
        v_flex()
            .flex_shrink_0()
            .gap_1()
            .px_2()
            .py_1p5()
            .border_b_1()
            .border_color(theme.border)
            .child(
                h_flex()
                    .justify_between()
                    .gap_2()
                    .child(
                        div()
                            .id("inspector-position")
                            .test_support()
                            .aria_label(position.clone())
                            .font_family(theme.mono_font_family.clone())
                            .child(position),
                    )
                    .when(has_reference, |this| {
                        this.child(
                            h_flex()
                                .gap_1()
                                .child(
                                    div()
                                        .text_xs()
                                        .text_color(theme.muted_foreground)
                                        .child(if approximate { "Δt≈" } else { "Δt" }),
                                )
                                .child(DeltaText::new(delta).unit("s")),
                        )
                    }),
            )
            .child(self.render_columns(has_reference, cx))
    }

    fn render_columns(&self, has_reference: bool, cx: &App) -> impl IntoElement {
        let theme = cx.theme();
        let role = |role: LapRole| {
            h_flex()
                .w(rems(VALUE_REMS))
                .flex_shrink_0()
                .justify_end()
                .gap_1()
                .child(Swatch::new(role.color(theme)).xsmall())
                .child(role.marker())
        };
        h_flex()
            .text_xs()
            .text_color(theme.muted_foreground)
            .gap_1()
            .child(div().flex_1().min_w_0().child("Channel"))
            .child(role(LapRole::Primary))
            .when(has_reference, |this| {
                this.child(role(LapRole::Reference)).child(
                    div()
                        .w(rems(VALUE_REMS))
                        .flex_shrink_0()
                        .text_right()
                        .child("Δ"),
                )
            })
    }
}

/// Width of one value column, rems.
const VALUE_REMS: f32 = 5.25;

impl gpui_kit::component::dock::BasePanel for InspectorPanel {
    fn panel_name(&self) -> &'static str {
        PanelKind::Inspector.name()
    }

    fn closable(&self, _: &App) -> bool {
        false
    }
}

impl gpui_kit::component::dock::Panel for InspectorPanel {
    fn tab_name(&self, _: &App) -> Option<SharedString> {
        Some(PanelKind::Inspector.title().into())
    }

    fn title(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        PanelKind::Inspector.title()
    }
}

impl gpui_kit::EventEmitter<gpui_kit::component::dock::PanelEvent> for InspectorPanel {}

impl gpui_kit::Focusable for InspectorPanel {
    fn focus_handle(&self, _: &App) -> FocusHandle {
        self.focus_handle.clone()
    }
}

impl Render for InspectorPanel {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        self.renders += 1;
        let root = div()
            .id("inspector-panel")
            .test_support()
            .track_focus(&self.focus_handle)
            .size_full();
        let Some(analysis) = self.shown.clone() else {
            return root
                .child(analysis_body("inspector-summary", &self.app, cx, |_| {
                    SharedString::default()
                }))
                .into_any_element();
        };
        if self.channels.is_empty() {
            return root
                .child(panel_body(
                    "inspector-summary",
                    "No channels for this lap.",
                    empty_state(
                        IconName::Inbox,
                        "No channels",
                        "This lap has no plottable channels.",
                    ),
                    cx,
                ))
                .into_any_element();
        }
        let probe = self.probe(cx);
        let has_reference = analysis.reference().is_some();
        let channels = self.channels.clone();
        let list = uniform_list("inspector-rows", channels.len(), move |range, _, cx| {
            let theme = cx.theme();
            range
                .map(|ix| {
                    let channel = &channels[ix];
                    let primary = probe.map(|p| channel.value(&channel.primary, p.primary));
                    let reference = probe.and_then(|p| {
                        channel
                            .reference
                            .as_ref()
                            .map(|values| channel.value(values, p.reference))
                    });
                    let delta = primary.zip(reference).map(|(p, r)| p - r);
                    // Units sit beside the channel name, so the value columns
                    // hold bare right-aligned numbers and a missing value is
                    // a lone muted dash.
                    let value = |value: Option<f64>| {
                        let finite = value.filter(|v| v.is_finite());
                        div()
                            .w(rems(VALUE_REMS))
                            .flex_shrink_0()
                            .text_right()
                            .whitespace_nowrap()
                            .font_family(theme.mono_font_family.clone())
                            .when(finite.is_none(), |this| {
                                this.text_color(theme.muted_foreground)
                            })
                            .child(format_value(finite, channel.decimals))
                    };
                    let spoken = SharedString::from(if channel.has_data {
                        format!(
                            "{}: {}",
                            channel.title,
                            format_value(primary, channel.decimals)
                        )
                    } else {
                        format!("{}: no data", channel.title)
                    });
                    h_flex()
                        .id(ElementIdFor::channel(&channel.key))
                        .test_support()
                        .aria_label(spoken)
                        .h_7()
                        .px_2()
                        .gap_1()
                        .border_b_1()
                        .border_color(theme.border.opacity(0.5))
                        .child(
                            h_flex()
                                .flex_1()
                                .min_w_0()
                                .gap_1()
                                .items_baseline()
                                .child(
                                    div()
                                        .min_w_0()
                                        .truncate()
                                        .text_color(if channel.has_data {
                                            theme.foreground
                                        } else {
                                            theme.muted_foreground
                                        })
                                        .child(channel.title.clone()),
                                )
                                .when(!channel.unit.is_empty(), |this| {
                                    this.child(
                                        div()
                                            .flex_shrink_0()
                                            .text_xs()
                                            .text_color(theme.muted_foreground)
                                            .child(channel.unit.clone()),
                                    )
                                })
                                .when(!channel.has_data, |this| {
                                    this.child(
                                        div()
                                            .flex_shrink_0()
                                            .text_xs()
                                            .italic()
                                            .text_color(theme.muted_foreground)
                                            .child("No data"),
                                    )
                                }),
                        )
                        .child(value(primary))
                        .when(has_reference, |this| {
                            let delta = match channel.delta {
                                DeltaKind::Scored(sense) => DeltaText::new(delta)
                                    .decimals(channel.decimals)
                                    .sense(sense)
                                    .into_any_element(),
                                DeltaKind::Neutral => div()
                                    .text_color(theme.muted_foreground)
                                    .child(
                                        omatrack_ui::format_delta(
                                            delta,
                                            channel.decimals,
                                            DeltaSense::LowerIsBetter,
                                        )
                                        .0,
                                    )
                                    .font_family(theme.mono_font_family.clone())
                                    .into_any_element(),
                            };
                            this.child(value(reference)).child(
                                h_flex()
                                    .w(rems(VALUE_REMS))
                                    .flex_shrink_0()
                                    .justify_end()
                                    .child(delta),
                            )
                        })
                })
                .collect()
        })
        .track_scroll(&self.scroll)
        .flex_1()
        .min_h_0();
        root.child(
            v_flex()
                .size_full()
                .text_sm()
                .child(self.render_header(&analysis, probe, cx))
                .child(list)
                .when(probe.is_none(), |this| {
                    this.child(
                        div()
                            .flex_shrink_0()
                            .p_2()
                            .text_xs()
                            .text_color(cx.theme().muted_foreground)
                            .child("Move the cursor over the traces to read values."),
                    )
                }),
        )
        .into_any_element()
    }
}

/// Stable element ids of inspector rows.
struct ElementIdFor;

impl ElementIdFor {
    fn channel(key: &str) -> gpui_kit::ElementId {
        gpui_kit::ElementId::Name(format!("inspector:{key}").into())
    }
}

/// This panel's app-wide setup: its dock registration (saved layouts
/// rebuild it by name). Called once from [`crate::panels::init`].
pub fn init(cx: &mut App) {
    crate::panels::register(PanelKind::Inspector, cx);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn fraction_pedals_read_as_percent_like_the_lane_legend() {
        let values: Arc<[f64]> = Arc::from(vec![0.0, 0.99, 1.0]);
        let channel = InspectorChannel {
            key: "throttle".into(),
            title: "Throttle".into(),
            unit: "%".into(),
            primary: values.clone(),
            reference: None,
            decimals: decimals("throttle", "%"),
            scale: omatrack_trace::scene::display_scale("%", 1.0),
            delta: DeltaKind::Neutral,
            stepped: false,
            has_data: true,
        };
        let value = channel.value(&values, 0.5);
        assert_eq!(format_value(Some(value), channel.decimals).as_ref(), "99");
        assert_eq!(omatrack_trace::scene::display_scale("km/h", 1.0), 1.0);
        assert_eq!(omatrack_trace::scene::display_scale("%", 100.0), 1.0);
    }
}
