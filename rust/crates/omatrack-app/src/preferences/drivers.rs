//! The driver name table: `driver_mappings` in `omatrack.yml` (logger
//! driver id -> name; `*` names every unmapped id).
//!
//! Rows are edited in place. Every change rebuilds the whole mapping from
//! the valid rows and hands it to [`crate::state::Preferences::update`], so
//! a half-typed id never lingers as a stale key; invalid rows show why and
//! are left out until fixed.

use std::collections::{BTreeMap, HashSet};

use gpui_kit::component::{
    ActiveTheme as _, IconName, Sizable as _,
    button::{Button, ButtonVariants as _},
    h_flex,
    input::{Input, InputEvent, InputState},
    v_flex,
};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::{
    AppContext as _, Context, ElementId, Entity, InteractiveElement as _, IntoElement,
    ParentElement as _, Render, SharedString, StatefulInteractiveElement as _, Styled as _,
    Subscription, TestSupportExt as _, Window, div, rems,
};
use omatrack_library::track_yml::normalized_driver_mapping_key;

use crate::state::AppState;
use omatrack_ui::TypeScale as _;

/// One table row. `id` is the row's identity for its whole life (element
/// ids, removal), independent of its position and of the text in it.
struct DriverRow {
    id: u64,
    key: Entity<InputState>,
    name: Entity<InputState>,
    error: Option<SharedString>,
    _subscriptions: [Subscription; 2],
}

/// Editor of `driver_mappings`.
pub struct DriverMappingsEditor {
    app: AppState,
    rows: Vec<DriverRow>,
    next_id: u64,
}

impl DriverMappingsEditor {
    pub fn new(app: AppState, window: &mut Window, cx: &mut Context<'_, Self>) -> Self {
        let mappings = app.preferences.read(cx).config().driver_mappings.clone();
        let mut editor = Self {
            app,
            rows: Vec::new(),
            next_id: 0,
        };
        for (key, name) in mappings {
            editor.push_row(&key, &name, window, cx);
        }
        editor
    }

    fn push_row(&mut self, key: &str, name: &str, window: &mut Window, cx: &mut Context<'_, Self>) {
        let id = self.next_id;
        self.next_id += 1;
        let key_input = cx.new(|cx| {
            InputState::new(window, cx)
                .placeholder("Id or *")
                .default_value(key.to_string())
        });
        let name_input = cx.new(|cx| {
            InputState::new(window, cx)
                .placeholder("Name")
                .default_value(name.to_string())
        });
        let on_change = |this: &mut Self,
                         _: Entity<InputState>,
                         event: &InputEvent,
                         cx: &mut Context<'_, Self>| {
            if matches!(event, InputEvent::Change) {
                this.commit(cx);
            }
        };
        let subscriptions = [
            cx.subscribe(&key_input, on_change),
            cx.subscribe(&name_input, on_change),
        ];
        self.rows.push(DriverRow {
            id,
            key: key_input,
            name: name_input,
            error: None,
            _subscriptions: subscriptions,
        });
    }

    /// Add an empty row and put the caret in its id field.
    pub fn add_row(&mut self, window: &mut Window, cx: &mut Context<'_, Self>) {
        self.push_row("", "", window, cx);
        if let Some(row) = self.rows.last() {
            row.key.update(cx, |input, cx| input.focus(window, cx));
        }
        cx.notify();
    }

    fn remove_row(&mut self, id: u64, cx: &mut Context<'_, Self>) {
        self.rows.retain(|row| row.id != id);
        self.commit(cx);
    }

    /// Validate every row and write the mapping the valid rows describe.
    fn commit(&mut self, cx: &mut Context<'_, Self>) {
        let mut mappings = BTreeMap::new();
        let mut seen = HashSet::new();
        for row in &mut self.rows {
            let key = row.key.read(cx).value().trim().to_string();
            let name = row.name.read(cx).value().trim().to_string();
            row.error = None;
            if key.is_empty() && name.is_empty() {
                continue;
            }
            let Some(normalized) = normalized_driver_mapping_key(&key) else {
                row.error = Some("Use a positive driver id, or * for every other id.".into());
                continue;
            };
            if name.is_empty() {
                row.error = Some("Enter the driver’s name.".into());
                continue;
            }
            if !seen.insert(normalized.clone()) {
                row.error = Some(format!("Driver id {normalized} is already named above.").into());
                continue;
            }
            mappings.insert(normalized, name);
        }
        self.app.preferences.update(cx, |preferences, cx| {
            preferences.update(cx, |config| config.driver_mappings = mappings);
        });
        cx.notify();
    }
}

fn key_input_id(row: u64) -> ElementId {
    ElementId::Name(format!("driver-key-{row}").into())
}

fn name_input_id(row: u64) -> ElementId {
    ElementId::Name(format!("driver-name-{row}").into())
}

impl Render for DriverMappingsEditor {
    fn render(&mut self, _: &mut Window, cx: &mut Context<'_, Self>) -> impl IntoElement {
        let theme = cx.theme();
        let header = h_flex()
            .gap_2()
            .text_label()
            .text_color(theme.muted_foreground)
            .child(div().w(rems(7.)).child("Driver id"))
            .child(div().flex_1().child("Name"));
        let rows = self.rows.iter().map(|row| {
            let id = row.id;
            v_flex()
                .gap_1()
                .child(
                    h_flex()
                        .gap_2()
                        .child(
                            div().w(rems(7.)).child(
                                Input::new(&row.key)
                                    .id(key_input_id(id))
                                    .aria_label("Driver id")
                                    .small()
                                    .when(row.error.is_some(), |input| {
                                        input.border_color(theme.danger)
                                    }),
                            ),
                        )
                        .child(
                            div().flex_1().min_w_0().child(
                                Input::new(&row.name)
                                    .id(name_input_id(id))
                                    .aria_label("Driver name")
                                    .small(),
                            ),
                        )
                        .child(
                            Button::new(ElementId::Name(format!("driver-remove-{id}").into()))
                                .ghost()
                                .small()
                                .icon(IconName::Minus)
                                .accessibility_label("Remove driver")
                                .tooltip("Remove driver")
                                .on_click(
                                    cx.listener(move |this, _, _, cx| this.remove_row(id, cx)),
                                ),
                        ),
                )
                .when_some(row.error.clone(), |this, error| {
                    this.child(
                        div()
                            .id(ElementId::Name(format!("driver-error-{id}").into()))
                            .test_support()
                            .aria_label(error.clone())
                            .text_label()
                            .text_color(theme.danger)
                            .child(error),
                    )
                })
        });
        v_flex()
            .id("driver-mappings")
            .test_support()
            .gap_2()
            .when(!self.rows.is_empty(), |this| this.child(header))
            .children(rows)
            .when(self.rows.is_empty(), |this| {
                this.child(
                    div()
                        .text_body()
                        .text_color(theme.muted_foreground)
                        .child("No driver names yet."),
                )
            })
            .child(
                h_flex().child(
                    Button::new("drivers-add")
                        .small()
                        .icon(IconName::Plus)
                        .label("Add driver")
                        .on_click(cx.listener(|this, _, window, cx| this.add_row(window, cx))),
                ),
            )
    }
}
