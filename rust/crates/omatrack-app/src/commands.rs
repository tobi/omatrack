//! The command registry and the command palette.
//!
//! Every palette entry is an [`Action`]: the palette shows the action's key
//! binding and confirming an entry dispatches it from the palette, so it
//! reaches the same handler as its keys, buttons and menu items. Static
//! commands live in the [`CommandRegistry`] global ([`register`] is the seam
//! later features use); laps, corners, panels and layouts are generated when
//! the palette opens.

use gpui_kit::component::{
    WindowExt as _,
    command::{Command, CommandGroup, CommandItem, CommandState},
    h_flex,
    kbd::Kbd,
    v_flex,
};
use gpui_kit::{
    Action, App, AppContext as _, Entity, FocusHandle, Global, InteractiveElement as _,
    ParentElement as _, SharedString, Styled as _, TestSupportExt as _, Window, div, rems,
};
use omatrack_core::format_lap_time;

use crate::actions::*;
use crate::state::AppState;

/// Where a registered command appears in the palette.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum CommandCategory {
    /// Application, library, trace and playback commands.
    Commands,
    /// Focus and show panels.
    Panels,
    /// Dock and video layouts.
    Layouts,
}

impl CommandCategory {
    fn label(self) -> &'static str {
        match self {
            Self::Commands => "Commands",
            Self::Panels => "Panels",
            Self::Layouts => "Layouts",
        }
    }
}

/// One registered command: a stable id, its interface title, its group and
/// the action it dispatches.
pub struct CommandSpec {
    id: SharedString,
    title: SharedString,
    category: CommandCategory,
    action: Box<dyn Action>,
    keywords: Vec<SharedString>,
}

impl Clone for CommandSpec {
    fn clone(&self) -> Self {
        Self {
            id: self.id.clone(),
            title: self.title.clone(),
            category: self.category,
            action: self.action.boxed_clone(),
            keywords: self.keywords.clone(),
        }
    }
}

impl CommandSpec {
    pub fn new(
        id: impl Into<SharedString>,
        title: impl Into<SharedString>,
        category: CommandCategory,
        action: impl Action,
    ) -> Self {
        Self {
            id: id.into(),
            title: title.into(),
            category,
            action: Box::new(action),
            keywords: Vec::new(),
        }
    }

    /// Extra search terms besides the title.
    pub fn keywords<I, S>(mut self, keywords: I) -> Self
    where
        I: IntoIterator<Item = S>,
        S: Into<SharedString>,
    {
        self.keywords.extend(keywords.into_iter().map(Into::into));
        self
    }

    pub fn id(&self) -> &SharedString {
        &self.id
    }
    pub fn title(&self) -> &SharedString {
        &self.title
    }
    pub fn category(&self) -> CommandCategory {
        self.category
    }
    pub fn action(&self) -> &dyn Action {
        self.action.as_ref()
    }

    fn item(&self, keys: &FocusHandle) -> CommandItem {
        palette_item(
            self.title.clone(),
            self.keywords.clone(),
            self.action.boxed_clone(),
            keys,
        )
    }
}

/// Every registered command, in registration order.
#[derive(Default)]
pub struct CommandRegistry {
    specs: Vec<CommandSpec>,
}

impl Global for CommandRegistry {}

impl CommandRegistry {
    pub fn specs(&self) -> &[CommandSpec] {
        &self.specs
    }

    pub fn get(&self, id: &str) -> Option<&CommandSpec> {
        self.specs.iter().find(|spec| spec.id == id)
    }
}

/// Add a command to the palette; a command with the same id is replaced.
pub fn register(cx: &mut App, spec: CommandSpec) {
    let registry = cx.default_global::<CommandRegistry>();
    match registry.specs.iter_mut().find(|known| known.id == spec.id) {
        Some(known) => *known = spec,
        None => registry.specs.push(spec),
    }
}

pub(crate) fn init(cx: &mut App) {
    use CommandCategory::{Commands, Layouts, Panels};
    let specs = [
        CommandSpec::new("open-folder", "Add folder…", Commands, OpenFolder)
            .keywords(["open", "library", "location"]),
        CommandSpec::new("rescan", "Rescan library", Commands, Rescan),
        CommandSpec::new("preferences", "Preferences…", Commands, OpenPreferences)
            .keywords(["settings"]),
        CommandSpec::new(
            "swap-roles",
            "Swap primary and reference",
            Commands,
            SwapRoles,
        ),
        CommandSpec::new("previous-lap", "Previous lap", Commands, PrevLap),
        CommandSpec::new("next-lap", "Next lap", Commands, NextLap),
        CommandSpec::new("previous-corner", "Previous corner", Commands, PrevCorner),
        CommandSpec::new("next-corner", "Next corner", Commands, NextCorner),
        CommandSpec::new("zoom-in", "Zoom in", Commands, ZoomIn),
        CommandSpec::new("zoom-out", "Zoom out", Commands, ZoomOut),
        CommandSpec::new("zoom-reset", "Reset zoom", Commands, ZoomReset),
        CommandSpec::new(
            "x-axis",
            "Toggle distance and time axis",
            Commands,
            ToggleXAxis,
        ),
        CommandSpec::new(
            "fit-lanes",
            "Fit lanes to the workspace",
            Commands,
            ToggleFit,
        ),
        CommandSpec::new(
            "trace-colors",
            "Toggle lap and channel colours",
            Commands,
            ToggleTraceColorMode,
        )
        .keywords([
            "traces",
            "colour",
            "color",
            "colors",
            "hue",
            "channel colours",
        ]),
        CommandSpec::new("resize-lanes", "Resize lanes…", Commands, ResizeLanes),
        CommandSpec::new("edit-corners", "Edit corners…", Commands, ToggleCornerEdit),
        CommandSpec::new("view-lap", "View: Lap", Commands, ViewLap).keywords([
            "traces",
            "whole lap",
            "mode",
        ]),
        CommandSpec::new("view-corners", "View: Corners", Commands, ViewCorners).keywords([
            "traces",
            "corner by corner",
            "mode",
        ]),
        CommandSpec::new(
            "view-consistency",
            "View: Consistency",
            Commands,
            ViewConsistency,
        )
        .keywords(["traces", "laps", "spread", "mode"]),
        CommandSpec::new("view-events", "View: Events", Commands, ViewEvents).keywords([
            "traces",
            "brake points",
            "shifts",
            "mode",
        ]),
        CommandSpec::new("play", "Play or pause", Commands, TogglePlay).keywords(["video"]),
        CommandSpec::new("seek-back", "Back 2 seconds", Commands, SeekBack),
        CommandSpec::new("seek-forward", "Forward 2 seconds", Commands, SeekForward),
        CommandSpec::new("mute", "Mute or unmute video", Commands, ToggleMute),
        CommandSpec::new("slow-motion", "Slow motion", Commands, ToggleSlowMotion),
        CommandSpec::new(
            "continuous",
            "Continuous playback",
            Commands,
            ToggleContinuous,
        ),
        CommandSpec::new(
            "fullscreen",
            "Video fullscreen",
            Commands,
            ToggleVideoFullscreen,
        ),
        CommandSpec::new(
            "exit-fullscreen",
            "Exit video fullscreen",
            Commands,
            ExitFullscreen,
        ),
        CommandSpec::new("quit", "Quit Omatrack", Commands, Quit),
        CommandSpec::new("toggle-library", "Toggle library", Panels, ToggleLibrary)
            .keywords(["sidebar", "dock"]),
        CommandSpec::new(
            "toggle-inspector",
            "Toggle inspector",
            Panels,
            ToggleInspector,
        )
        .keywords(["dock", "corners", "laps"]),
        CommandSpec::new("focus-laps", "Focus laps", Panels, FocusPanel1),
        CommandSpec::new("focus-traces", "Focus traces", Panels, FocusPanel2),
        CommandSpec::new("focus-video", "Focus video", Panels, FocusPanel3),
        CommandSpec::new("focus-corners", "Focus corners", Panels, FocusPanel4),
        CommandSpec::new(
            "focus-time-goes",
            "Where the time goes",
            Panels,
            FocusPanel5,
        )
        .keywords(["focus", "time lost", "heat", "map", "loss"]),
        CommandSpec::new("focus-library", "Browse library", Panels, FocusPanel6)
            .keywords(["focus", "library", "sessions", "events", "tree"]),
        CommandSpec::new("show-channels", "Show channels", Panels, ShowChannels)
            .keywords(["panel", "lanes", "traces"]),
        CommandSpec::new("show-map", "Show track map", Panels, ShowMap)
            .keywords(["panel", "gps", "circuit"]),
        CommandSpec::new("show-inspector", "Show inspector", Panels, ShowInspector)
            .keywords(["panel", "cursor", "values"]),
        CommandSpec::new("reset-layout", "Reset layout", Layouts, ResetLayout)
            .keywords(["dock", "panels"]),
        CommandSpec::new("video-split", "Video: split", Layouts, ComposeLayout1),
        CommandSpec::new(
            "video-primary-inset",
            "Video: primary with reference inset",
            Layouts,
            ComposeLayout2,
        ),
        CommandSpec::new(
            "video-reference-inset",
            "Video: reference with primary inset",
            Layouts,
            ComposeLayout3,
        ),
        CommandSpec::new(
            "video-primary",
            "Video: primary only",
            Layouts,
            ComposeLayout4,
        ),
        CommandSpec::new(
            "video-reference",
            "Video: reference only",
            Layouts,
            ComposeLayout5,
        ),
    ];
    for spec in specs {
        register(cx, spec);
    }
}

/// Lap entries from the library, corners from the current analysis.
fn dynamic_groups(keys: &FocusHandle, cx: &App) -> (Option<CommandGroup>, Option<CommandGroup>) {
    let Some(state) = AppState::try_global(cx) else {
        return (None, None);
    };
    let library = state.library.read(cx);
    let snapshot = library.snapshot();
    let mut laps = CommandGroup::new().label("Laps");
    let mut lap_count = 0;
    for track in snapshot.tracks() {
        for date in &track.dates {
            for session in &date.sessions {
                for lap in session.laps.iter().filter(|lap| lap.complete) {
                    let title = format!(
                        "{} {} · {}",
                        lap.label,
                        format_lap_time(lap.time_ms),
                        session.title
                    );
                    let keywords = [
                        track.name.clone(),
                        date.heading.clone(),
                        session.file_name(),
                    ];
                    for (role, suffix) in [
                        (Role::Primary, "as primary"),
                        (Role::Reference, "as reference"),
                    ] {
                        laps = laps.item(palette_item(
                            format!("{title} {suffix}").into(),
                            keywords.iter().cloned().map(Into::into).collect(),
                            Box::new(SelectLap {
                                session: session.id.clone().into(),
                                lap: lap.lap_id,
                                role,
                            }),
                            keys,
                        ));
                        lap_count += 1;
                    }
                }
            }
        }
    }
    let session = state.session.read(cx);
    let corners = session.analysis().map(|analysis| {
        CommandGroup::new()
            .label("Corners")
            .items(analysis.corners().iter().map(|zone| {
                palette_item(
                    format!("{} · Focus corner", zone.name).into(),
                    vec![zone.id.clone().into()],
                    Box::new(FocusCorner {
                        id: zone.id.clone().into(),
                    }),
                    keys,
                )
            }))
    });
    ((lap_count > 0).then_some(laps), corners)
}

fn groups(keys: &FocusHandle, cx: &App) -> Vec<CommandGroup> {
    let registry = cx.try_global::<CommandRegistry>();
    let group = |category: CommandCategory| {
        CommandGroup::new().label(category.label()).items(
            registry
                .into_iter()
                .flat_map(|registry| registry.specs.iter())
                .filter(|spec| spec.category == category)
                .map(|spec| spec.item(keys)),
        )
    };
    let (laps, corners) = dynamic_groups(keys, cx);
    let mut groups = vec![group(CommandCategory::Commands)];
    groups.extend(laps);
    groups.extend(corners);
    groups.push(group(CommandCategory::Panels));
    groups.push(group(CommandCategory::Layouts));
    groups
}

/// The palette's retained state while it is open.
#[derive(Default)]
pub(crate) struct Palette {
    state: Option<Entity<CommandState>>,
}

impl Palette {
    /// Whether the palette is the dialog on screen.
    pub(crate) fn is_open(&self, window: &mut Window, cx: &mut App) -> bool {
        window.has_active_dialog(cx)
            && self.state.as_ref().is_some_and(|state| {
                state
                    .read(cx)
                    .focus_handle_for_palette(cx)
                    .contains_focused(window, cx)
            })
    }

    /// Open the palette, or close it when it is open. Escape closes it and
    /// returns focus to where it was; confirming an entry dispatches its
    /// action and closes it.
    ///
    /// `keys` is the focus handle key bindings are shown for: the
    /// workspace's, so single-key commands show their keys even though
    /// the palette's own text field suppresses them.
    pub(crate) fn toggle(&mut self, keys: &FocusHandle, window: &mut Window, cx: &mut App) {
        if self.is_open(window, cx) {
            window.close_dialog(cx);
            self.state = None;
            return;
        }
        let state = cx.new(|cx| CommandState::new(window, cx));
        let groups = groups(keys, cx);
        let command_state = state.clone();
        window.open_dialog(cx, move |dialog, window, _| {
            let confirmed = command_state.clone();
            let mut command = Command::new(&command_state)
                .bordered(false)
                .placeholder("Search commands, laps and corners…")
                .max_h(rems(26.))
                .on_confirm(move |_, window, cx| close_after_confirm(&confirmed, window, cx));
            for group in &groups {
                command = command.group(group.clone());
            }
            dialog
                .close_button(false)
                // 600 px at the default 16 px base.
                .width(window.rem_size() * 37.5)
                .child(
                    v_flex()
                        .id("command-palette")
                        .test_support()
                        .w_full()
                        .child(command),
                )
        });
        let query = gpui_kit::Focusable::focus_handle(state.read(cx), cx);
        window.focus(&query, cx);
        self.state = Some(state);
    }
}

/// After an entry's action ran: close the palette. Focus returns to where
/// it was before the palette opened, unless the action moved focus itself
/// (a "Focus …" command), in which case it stays there.
fn close_after_confirm(state: &Entity<CommandState>, window: &mut Window, cx: &mut App) {
    let palette = state.read(cx).focus_handle_for_palette(cx);
    let moved = !palette.contains_focused(window, cx);
    let focused = window.focused(cx);
    window.close_dialog(cx);
    if moved && let Some(focused) = focused {
        window.focus(&focused, cx);
    }
}

trait PaletteFocus {
    fn focus_handle_for_palette(&self, cx: &App) -> FocusHandle;
}

impl PaletteFocus for CommandState {
    fn focus_handle_for_palette(&self, cx: &App) -> FocusHandle {
        gpui_kit::Focusable::focus_handle(self, cx)
    }
}

/// One palette row: the title, and the key binding of its action as the
/// workspace resolves it.
fn palette_item(
    title: SharedString,
    keywords: Vec<SharedString>,
    action: Box<dyn Action>,
    keys: &FocusHandle,
) -> CommandItem {
    let row_action = action.boxed_clone();
    let row_title = title.clone();
    let keys = keys.clone();
    CommandItem::new()
        .label(title)
        .keywords(keywords)
        .action(action)
        .child(move |window, _| {
            h_flex()
                .w_full()
                .gap_2()
                .child(div().flex_1().min_w_0().truncate().child(row_title.clone()))
                .children(Kbd::binding_for_action_in(
                    row_action.as_ref(),
                    &keys,
                    window,
                ))
        })
}
