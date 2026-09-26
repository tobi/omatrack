//! UI integration tests for the domain components in a headless window.

#![cfg(test)]

use gpui_kit::component::{Root, Selectable as _};
use gpui_kit::prelude::*;
use gpui_kit::test::{TestSupportExt as _, TestWindowExt as _};
use gpui_kit::{Context, Entity, TestAppContext, Window, div, px, size};
use omatrack_ui::{DeltaText, LapRole, Readout, RoleChip, theme};

struct Comparison {
    primary_clicks: usize,
    selected: bool,
}

impl Render for Comparison {
    fn render(&mut self, _: &mut Window, cx: &mut Context<'_, Self>) -> impl IntoElement {
        let this = cx.entity().downgrade();
        div()
            .size_full()
            .flex()
            .flex_col()
            .gap_2()
            .child(
                RoleChip::new("role-chip-primary", LapRole::Primary, "L8")
                    .driver("Tobi")
                    .time("1:13.644")
                    .selected(self.selected)
                    .on_click(move |_, _, cx| {
                        _ = this.update(cx, |this, cx| {
                            this.primary_clicks += 1;
                            this.selected = !this.selected;
                            cx.notify();
                        });
                    }),
            )
            .child(
                div()
                    .id("reference-chip")
                    .test_support()
                    .child(RoleChip::new(
                        "role-chip-reference",
                        LapRole::Reference,
                        "L3",
                    )),
            )
            .child(
                div()
                    .id("delta")
                    .test_support()
                    .child(DeltaText::new(Some(-0.123)).unit("s")),
            )
            .child(
                div()
                    .id("speed")
                    .test_support()
                    .child(Readout::number(Some(187.46), 1).unit("km/h")),
            )
    }
}

fn open(cx: &mut TestAppContext) -> (gpui_kit::AnyWindowHandle, Entity<Comparison>) {
    cx.update(|cx| {
        gpui_kit::init(cx);
        theme::install(theme::ThemeSource::none(), cx);
    });
    let mut view = None;
    let handle = cx.open_window(size(px(640.), px(320.)), |window, cx| {
        let comparison = cx.new(|_| Comparison {
            primary_clicks: 0,
            selected: false,
        });
        view = Some(comparison.clone());
        Root::new(comparison, window, cx)
    });
    (handle.into(), view.unwrap())
}

#[gpui_kit::test]
fn a_clickable_role_chip_is_a_button_that_reports_clicks(cx: &mut TestAppContext) {
    let (handle, view) = open(cx);
    cx.update_window(handle, |_, window, cx| {
        window.render_frame(cx);
        let chip = window.find("role-chip-primary");
        assert!(chip.visible());
        assert_eq!(chip.label(), Some("Primary lap L8"));
        window.click("role-chip-primary", cx);
    })
    .unwrap();
    // gpui-component's Button paints `selected` but does not report it to
    // accessibility, so the owner's state is the assertion.
    cx.update(|cx| assert!(view.read(cx).selected));
    cx.update_window(handle, |_, window, cx| {
        window.render_frame(cx);
        window.click("role-chip-primary", cx);
    })
    .unwrap();
    cx.update(|cx| {
        let view = view.read(cx);
        assert_eq!(view.primary_clicks, 2);
        assert!(!view.selected);
    });
}

#[gpui_kit::test]
fn readouts_and_chips_render_in_their_regions(cx: &mut TestAppContext) {
    let (handle, _) = open(cx);
    cx.update_window(handle, |_, window, cx| {
        window.render_frame(cx);
        let reference = window.find("reference-chip");
        assert!(reference.visible());
        assert!(reference.bounds().size.width > px(0.));
        // A chip without a handler is not a control.
        assert!(
            window
                .try_find("role-chip-reference")
                .is_none_or(|chip| chip.label().is_none())
        );
        for id in ["delta", "speed"] {
            let region = window.find(id);
            assert!(region.visible(), "{id}");
            assert!(region.bounds().size.width > px(0.), "{id}");
        }
    })
    .unwrap();
}
