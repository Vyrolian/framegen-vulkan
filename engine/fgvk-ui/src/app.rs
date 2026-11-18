//! The settings window. It renders typed snapshots from `config` and never touches TOML.

use std::ops::RangeInclusive;
use std::path::PathBuf;
use std::time::{Duration, Instant, SystemTime};

use egui::text::{LayoutJob, TextWrapping};
use egui::{
    pos2, vec2, Align, Button, Color32, CornerRadius, Key, Label, Layout, Margin, Rect, RichText,
    Sense, Stroke, StrokeKind, TextStyle, Ui, UiBuilder,
};

use crate::config::{self, Config, ConfigView, FpsMemory, Mode, Profile, ProfileKey, Settings};
use crate::launcher_conf::{LauncherConf, Logging, Switch};
use crate::live::{self, Row, View};
use crate::picker::Picker;
use crate::processes;
use crate::state;
use crate::theme::{self, Accent, Flavour};
use crate::themes::{Catalog, Note};

const POLL_INTERVAL: Duration = Duration::from_secs(1);
// egui sizes the glyph atlas to the reported texture limit, up to 16,384 pixels
// wide. Its portable 2,048-pixel default is enough for this text-only window.
const MAX_TEXTURE_SIDE: usize = 2048;
const FPS_SLIDER_RANGE: RangeInclusive<i64> = 10..=360;
const ROW_HEIGHT: f32 = 26.0;
/// Entry text starts this far inside its row.
const ROW_TEXT_INSET: f32 = 8.0;
/// Outer width of the games box. A narrower space shrinks the box to fit.
const GAMES_BOX_WIDTH: f32 = 380.0;
/// Space between an entry's name and its Remove button.
const REMOVE_GAP: f32 = 2.0;
const NO_GAMES: &str = "No games yet. Add an executable name or pick a running program.";
const LOW_LATENCY_TIP: &str =
    "starts the game's next frame just in time at the cap; off = up to three frames queued";
const BATCH_HOLD_HINT: &str =
    "Waits about 2 ms after frames are ready so GPU timing wobble does not show as uneven frames. \
    Off: up to about 2 ms less latency, slightly less even.";
const MODES: [Mode; 5] = [
    Mode::Off,
    Mode::Fixed(2),
    Mode::Fixed(3),
    Mode::Fixed(4),
    Mode::Fixed(5),
];
/// The Logging section, in launcher order: the switch, its checkbox label and the hint under it.
const LOGGING: [(Switch, &str, &str); 4] = [
    (Switch::Pacing, "Pacing log", "Light timing summaries."),
    (
        Switch::Trace,
        "Detailed trace",
        "Every present, acquire and wait. Larger file.",
    ),
    (
        Switch::Wayland,
        "On-screen times",
        "Compositor presentation feedback.",
    ),
    (
        Switch::GpuCost,
        "GPU cost",
        "Frame-generation GPU time. Does not change pacing.",
    ),
];

enum Status {
    Idle,
    Saved,
    Error(String),
}

enum NameTarget {
    New,
    Rename(ProfileKey),
}

struct NameEdit {
    target: NameTarget,
    text: String,
    focus: bool,
}

pub struct App {
    config: Config,
    /// The standalone launcher's `launcher.conf`, which holds the Logging switches.
    launcher: LauncherConf,
    selected: usize,
    status: Status,
    name_edit: Option<NameEdit>,
    confirm_delete: bool,
    fps_memory: FpsMemory,
    /// Slider drafts exist only while a drag is in progress.
    fps_slider: Option<i64>,
    /// Number-box text exists only while the box has focus.
    fps_text: Option<String>,
    quality: Option<f64>,
    dll_text: Option<String>,
    new_game: String,
    /// Exists only while the "Pick a running program" window is open for the selected profile.
    picker: Option<Picker>,
    last_poll: Instant,
    /// The Live section's reader of the newest fresh pacing log. Polled with `last_poll`.
    live: live::Follower,
    /// Where the selected profile's name is remembered. `None` when no state directory is known.
    state_path: Option<PathBuf>,
    /// The profile name last selected, or loaded from the state file at start.
    remembered: Option<String>,
    /// Set once the first configuration view with profiles has applied the remembered selection.
    restored: bool,
    /// The theme list, the theme in use and the Catppuccin accent. Its palette is applied to egui.
    themes: Catalog,
}

impl App {
    pub fn new(
        path: Result<PathBuf, String>,
        state_path: Option<PathBuf>,
        launcher_path: Result<PathBuf, String>,
        themes: Catalog,
    ) -> Self {
        let remembered = state_path.as_deref().and_then(state::load);
        Self {
            config: Config::open(path),
            launcher: LauncherConf::open(launcher_path),
            selected: 0,
            status: Status::Idle,
            name_edit: None,
            confirm_delete: false,
            fps_memory: FpsMemory::default(),
            fps_slider: None,
            fps_text: None,
            quality: None,
            dll_text: None,
            new_game: String::new(),
            picker: None,
            last_poll: Instant::now(),
            live: live::Follower::new(
                live::log_directory(|key| std::env::var_os(key)),
                live::own_uid(),
            ),
            state_path,
            remembered,
            restored: false,
            themes,
        }
    }

    /// True while any edit is unsaved. External reloads wait until it clears.
    fn has_pending(&self) -> bool {
        self.name_edit.is_some()
            || self.fps_slider.is_some()
            || self.fps_text.is_some()
            || self.quality.is_some()
            || self.dll_text.is_some()
    }

    fn select(&mut self, index: usize) {
        self.selected = index;
        self.confirm_delete = false;
        self.name_edit = None;
        self.clear_profile_drafts();
    }

    fn clear_profile_drafts(&mut self) {
        self.fps_slider = None;
        self.fps_text = None;
        self.quality = None;
        // The picker adds to the selected profile, so it closes when that profile goes away.
        self.picker = None;
    }

    fn report(&mut self, result: Result<(), String>) {
        self.status = match result {
            Ok(()) => Status::Saved,
            Err(message) => Status::Error(message),
        };
    }

    fn open_name_editor(&mut self, target: NameTarget, text: String) {
        self.name_edit = Some(NameEdit {
            target,
            text,
            focus: true,
        });
        self.confirm_delete = false;
    }

    fn content(&mut self, ui: &mut Ui) {
        egui::ScrollArea::vertical()
            .auto_shrink([false, false])
            .show(ui, |ui| {
                match self.config.view() {
                    ConfigView::Missing(path) => self.missing(ui, &path),
                    ConfigView::Invalid(message) => {
                        self.clear_profile_drafts();
                        invalid(ui, &message);
                    }
                    ConfigView::Ready(settings) => self.editor(ui, settings),
                }
                // The launcher settings are separate from conf.toml, so this section shows
                // whatever state the profile file is in.
                self.live_readings(ui);
                self.logging(ui);
                self.appearance(ui);
            });
    }

    fn missing(&mut self, ui: &mut Ui, path: &str) {
        let palette = theme::current(ui.ctx());
        ui.add_space(8.0);
        ui.label("No configuration file yet.");
        ui.label(RichText::new(path).small().color(palette.subtext0));
        ui.add_space(8.0);
        if ui.button("Create").clicked() {
            let result = self.config.create_default();
            self.report(result);
        }
    }

    fn editor(&mut self, ui: &mut Ui, settings: Settings) {
        let Settings {
            profiles,
            default_profile,
            dll,
            allow_fp16,
            pacing_correction,
        } = settings;
        if !self.restored {
            self.select(initial_index(&profiles, self.remembered.as_deref()));
            self.restored = true;
        }
        self.selected = self.selected.min(profiles.len().saturating_sub(1));

        section(ui, "Profile");
        self.profile_row(ui, &profiles);
        self.name_editor(ui);
        self.delete_confirm(ui, &profiles);

        match profiles.get(self.selected) {
            Some(profile) => {
                let mut is_default = default_profile.as_deref() == Some(profile.key.name.as_str());
                if ui
                    .checkbox(&mut is_default, "Use as default for fgvk launches")
                    .on_hover_text("Uses this profile when no per-game profile matches.")
                    .changed()
                {
                    let result = self.config.set_default_profile(&profile.key, is_default);
                    self.report(result);
                }
                self.profile_fields(ui, profile);
            }
            None => {
                self.clear_profile_drafts();
                hint(ui, "No profiles. Use New to add one.");
            }
        }

        section(ui, "Global");
        self.global(ui, dll.unwrap_or_default(), allow_fp16, pacing_correction);
    }

    fn profile_row(&mut self, ui: &mut Ui, profiles: &[Profile]) {
        let current = profiles
            .get(self.selected)
            .map_or("No profiles", |profile| profile.key.name.as_str());
        let rename_target = profiles
            .get(self.selected)
            .map(|profile| profile.key.clone());
        let mut selected = self.selected;

        let (new_clicked, rename_clicked, delete_clicked) = ui
            .horizontal(|ui| {
                egui::ComboBox::from_id_salt("profile")
                    .width(170.0)
                    .selected_text(current)
                    .show_ui(ui, |ui| {
                        for (index, profile) in profiles.iter().enumerate() {
                            ui.selectable_value(&mut selected, index, profile.key.name.as_str());
                        }
                    });
                let new_clicked = ui.button("New").clicked();
                let rename_clicked = ui
                    .add_enabled(rename_target.is_some(), Button::new("Rename"))
                    .clicked();
                let delete_clicked = ui
                    .add_enabled(profiles.len() > 1, Button::new("Delete"))
                    .on_disabled_hover_text("The last profile cannot be deleted.")
                    .clicked();
                (new_clicked, rename_clicked, delete_clicked)
            })
            .inner;

        if selected != self.selected {
            self.select(selected);
        }
        if new_clicked {
            let name = default_name(profiles);
            self.open_name_editor(NameTarget::New, name);
        }
        if let (true, Some(key)) = (rename_clicked, rename_target) {
            let name = key.name.clone();
            self.open_name_editor(NameTarget::Rename(key), name);
        }
        if delete_clicked {
            self.confirm_delete = true;
        }
    }

    fn name_editor(&mut self, ui: &mut Ui) {
        let Some(mut edit) = self.name_edit.take() else {
            return;
        };
        let (confirm, cancel) = ui
            .horizontal(|ui| {
                let response = ui.add(
                    egui::TextEdit::singleline(&mut edit.text)
                        .desired_width(200.0)
                        .hint_text("Profile name"),
                );
                if edit.focus {
                    response.request_focus();
                    edit.focus = false;
                }
                let enter =
                    response.lost_focus() && ui.input(|input| input.key_pressed(Key::Enter));
                let confirm = enter || ui.button("OK").clicked();
                let cancel = ui.button("Cancel").clicked();
                (confirm, cancel)
            })
            .inner;
        if cancel {
            return;
        }
        if !confirm {
            self.name_edit = Some(edit);
            return;
        }
        match self.submit_name(&edit) {
            Ok(()) => self.report(Ok(())),
            Err(message) => {
                self.report(Err(message));
                self.name_edit = Some(edit);
            }
        }
    }

    fn submit_name(&mut self, edit: &NameEdit) -> Result<(), String> {
        match &edit.target {
            NameTarget::New => {
                let key = self.config.add_profile(&edit.text)?;
                self.select(key.index);
                Ok(())
            }
            NameTarget::Rename(key) => {
                let name = edit.text.trim();
                self.config.rename_profile(key, name)?;
                self.fps_memory.rename(&key.name, name);
                Ok(())
            }
        }
    }

    fn delete_confirm(&mut self, ui: &mut Ui, profiles: &[Profile]) {
        if !self.confirm_delete {
            return;
        }
        let Some(profile) = profiles.get(self.selected) else {
            self.confirm_delete = false;
            return;
        };
        let key = profile.key.clone();
        let palette = theme::current(ui.ctx());
        let (delete, cancel) = ui
            .horizontal(|ui| {
                ui.label(format!("Delete \"{}\"?", key.name));
                let delete = ui
                    .button(RichText::new("Delete").color(palette.error))
                    .clicked();
                let cancel = ui.button("Cancel").clicked();
                (delete, cancel)
            })
            .inner;
        if delete {
            let result = self.config.delete_profile(&key);
            if result.is_ok() {
                self.fps_memory.forget(&key.name);
                self.select(key.index.saturating_sub(1));
            }
            self.report(result);
        } else if cancel {
            self.confirm_delete = false;
        }
    }

    fn profile_fields(&mut self, ui: &mut Ui, profile: &Profile) {
        self.fps_memory
            .record(&profile.key.name, profile.base_fps_cap);

        section(ui, "Games");
        hint(
            ui,
            "Matches executable names, process names or path suffixes.",
        );
        self.games(ui, profile);

        section(ui, "Frame generation");
        self.frame_generation(ui, profile);

        section(ui, "Quality (flow scale)");
        self.quality(ui, profile);
        hint(ui, "Higher = better motion quality, more GPU");
        self.performance_mode(ui, profile);
        hint(ui, "Lighter model: more FPS, lower quality. Applies live.");

        ui.add_space(6.0);
        self.fps_limit(ui, profile);
        self.low_latency(ui, profile);
        self.batch_hold(ui, profile);
    }

    fn games(&mut self, ui: &mut Ui, profile: &Profile) {
        let remove = games_box(ui, &profile.active_in);
        ui.add_space(4.0);

        let (add, pick) = ui
            .horizontal_wrapped(|ui| {
                let response = ui.add(
                    egui::TextEdit::singleline(&mut self.new_game)
                        .desired_width(220.0)
                        .hint_text("Game.exe"),
                );
                let enter =
                    response.lost_focus() && ui.input(|input| input.key_pressed(Key::Enter));
                let add = enter || ui.button("Add").clicked();
                let pick = ui.button("Pick running…").clicked();
                (add, pick)
            })
            .inner;

        if add {
            self.add_game(profile);
        }
        if pick && self.picker.is_none() {
            self.picker = Some(Picker::new());
        }
        if let Some(index) = remove {
            let mut entries = profile.active_in.clone();
            entries.remove(index);
            let result = self.config.set_active_in(&profile.key, &entries);
            self.report(result);
        }
        self.picker_window(ui.ctx(), profile);
    }

    /// Saves the selected profile's name when it differs from the remembered one. This runs after
    /// the frame's edits, so a New or Rename is saved in the same frame. A failed save is logged,
    /// and the name counts as remembered anyway, so the write is not retried every frame.
    fn remember_selection(&mut self) {
        if !self.restored {
            return;
        }
        let ConfigView::Ready(settings) = self.config.view() else {
            return;
        };
        let Some(profile) = settings.profiles.get(self.selected) else {
            return;
        };
        let name = &profile.key.name;
        if self.remembered.as_ref() == Some(name) {
            return;
        }
        if let Some(path) = &self.state_path {
            if let Err(message) = state::save(path, name) {
                eprintln!("framegen-vulkan: {message}");
            }
        }
        self.remembered = Some(name.clone());
    }

    /// Shows the picker for the selected profile and adds the entries the user clicked.
    fn picker_window(&mut self, ctx: &egui::Context, profile: &Profile) {
        let Some(picker) = self.picker.as_mut() else {
            return;
        };
        let outcome = picker.show(ctx, &profile.active_in);
        if outcome.closed {
            self.picker = None;
        }
        self.add_picked(profile, &outcome.picked);
    }

    /// Adds picked entries in one write, skipping any already in the profile. Nothing is written
    /// when every picked entry is already there.
    fn add_picked(&mut self, profile: &Profile, picked: &[String]) {
        let entries = processes::with_added(&profile.active_in, picked);
        if entries.len() == profile.active_in.len() {
            return;
        }
        let result = self.config.set_active_in(&profile.key, &entries);
        self.report(result);
    }

    fn add_game(&mut self, profile: &Profile) {
        let entry = self.new_game.trim().to_owned();
        if entry.is_empty() {
            return;
        }
        if profile.active_in.contains(&entry) {
            self.new_game.clear();
            return;
        }
        let mut entries = profile.active_in.clone();
        entries.push(entry);
        let result = self.config.set_active_in(&profile.key, &entries);
        if result.is_ok() {
            self.new_game.clear();
        }
        self.report(result);
    }

    fn frame_generation(&mut self, ui: &mut Ui, profile: &Profile) {
        let mut mode = profile.mode;
        ui.horizontal(|ui| {
            for option in MODES {
                if option_button(ui, mode_label(option), profile.mode == option) {
                    mode = option;
                }
            }
        });
        if mode != profile.mode {
            let result = self.config.set_mode(&profile.key, mode);
            self.report(result);
        }
    }

    fn quality(&mut self, ui: &mut Ui, profile: &Profile) {
        let committed = ui
            .horizontal(|ui| {
                ui.spacing_mut().slider_width = 260.0;
                let mut value = self.quality.unwrap_or(profile.flow_scale);
                let response = ui.add(
                    egui::Slider::new(&mut value, config::FLOW_SCALE_RANGE)
                        .step_by(0.05)
                        .custom_formatter(|fraction, _| format!("{:.0}%", fraction * 100.0)),
                );
                slider_commit(&mut self.quality, value, &response)
            })
            .inner;
        if let Some(value) = committed {
            let rounded = (value * 100.0).round() / 100.0;
            if (rounded - profile.flow_scale).abs() > 1e-9 {
                let result = self.config.set_flow_scale(&profile.key, rounded);
                self.report(result);
            }
        }
    }

    fn performance_mode(&mut self, ui: &mut Ui, profile: &Profile) {
        let mut on = profile.performance_mode;
        if ui.checkbox(&mut on, "Performance mode").changed() {
            let result = self.config.set_performance_mode(&profile.key, on);
            self.report(result);
        }
    }

    /// Starts each capped frame just in time instead of queueing frames ahead. Applies live.
    fn low_latency(&mut self, ui: &mut Ui, profile: &Profile) {
        let mut on = profile.low_latency;
        let response = ui
            .checkbox(&mut on, "Low latency")
            .on_hover_text(LOW_LATENCY_TIP);
        if response.changed() {
            let result = self.config.set_low_latency(&profile.key, on);
            self.report(result);
        }
    }

    /// Holds each output batch about 2 ms after its frames are ready, with Low latency on or off.
    /// The explanation stays visible under it.
    fn batch_hold(&mut self, ui: &mut Ui, profile: &Profile) {
        let mut on = profile.batch_hold;
        if ui.checkbox(&mut on, "Batch hold").changed() {
            let result = self.config.set_batch_hold(&profile.key, on);
            self.report(result);
        }
        hint(ui, BATCH_HOLD_HINT);
    }

    fn fps_limit(&mut self, ui: &mut Ui, profile: &Profile) {
        let shown_on = profile.base_fps_cap > 0;
        let mut on = shown_on;
        if ui.checkbox(&mut on, "FPS limit").changed() {
            let value = if on {
                self.fps_memory.enable_value(&profile.key.name)
            } else {
                0
            };
            let result = self.config.set_base_fps_cap(&profile.key, value);
            self.report(result);
        }
        if !shown_on {
            self.fps_text = None;
            self.fps_slider = None;
            return;
        }

        let (typed, dragged) = ui
            .horizontal(|ui| {
                ui.spacing_mut().slider_width = 180.0;
                let current = self.fps_slider.unwrap_or(profile.base_fps_cap);
                let typed = text_commit(ui, &mut self.fps_text, &current.to_string(), 56.0, "");

                let mut value = current.clamp(*FPS_SLIDER_RANGE.start(), *FPS_SLIDER_RANGE.end());
                let response =
                    ui.add(egui::Slider::new(&mut value, FPS_SLIDER_RANGE).show_value(false));
                let dragged = slider_commit(&mut self.fps_slider, value, &response);
                (typed, dragged)
            })
            .inner;

        let current = profile.base_fps_cap.to_string();
        if let Some(text) = typed.filter(|text| text.trim() != current) {
            let result = parse_in_range(&text, "FPS limit", &config::FPS_LIMIT_RANGE)
                .and_then(|value| self.config.set_base_fps_cap(&profile.key, value));
            self.report(result);
        }
        if let Some(value) = dragged.filter(|value| *value != profile.base_fps_cap) {
            let result = self.config.set_base_fps_cap(&profile.key, value);
            self.report(result);
        }
    }

    fn global(&mut self, ui: &mut Ui, dll: String, allow_fp16: bool, pacing_correction: bool) {
        let mut fp16 = allow_fp16;
        if ui.checkbox(&mut fp16, "FP16 (faster)").changed() {
            let result = self.config.set_global_allow_fp16(fp16);
            self.report(result);
        }
        hint(
            ui,
            "Half precision for frame generation. Restart the game to apply.",
        );

        let mut correction = pacing_correction;
        if ui.checkbox(&mut correction, "Pacing correction").changed() {
            let result = self.config.set_global_pacing_correction(correction);
            self.report(result);
        }
        hint(
            ui,
            "Centres generated frames between real frames from measured GPU timing. Restart the game to apply.",
        );

        ui.label("Lossless.dll");
        let typed = text_commit(ui, &mut self.dll_text, &dll, 360.0, "Path to Lossless.dll");
        if let Some(text) = typed.filter(|text| text.trim() != dll.trim()) {
            let result = self.config.set_global_dll(Some(text.trim()));
            self.report(result);
        }
        hint(
            ui,
            "Needed for frame generation. Leave empty to remove the setting.",
        );
    }

    /// Readings from the pacing log of the game that is running, refreshed by the poll in `logic`.
    /// Nothing is read from processes or windows: the log file is the only input.
    fn live_readings(&mut self, ui: &mut Ui) {
        let palette = theme::current(ui.ctx());
        section(ui, "Live");
        match self.live.view() {
            View::NoGame => hint(ui, "No game is logging right now"),
            View::Readings(rows) => {
                for row in rows {
                    match row {
                        Row::Text(text) => {
                            ui.label(text);
                        }
                        Row::Needs { reading, checkbox } => {
                            ui.horizontal(|ui| {
                                ui.label(format!("{reading}:"));
                                ui.label(
                                    RichText::new(format!("needs \"{checkbox}\""))
                                        .small()
                                        .color(palette.subtext0),
                                );
                            });
                        }
                    }
                }
            }
        }
    }

    /// Logging switches in `launcher.conf`. The launcher exports them as renderer variables when it
    /// starts a game. A file that fails the strict rules is shown as an error and left unchanged.
    fn logging(&mut self, ui: &mut Ui) {
        let palette = theme::current(ui.ctx());
        section(ui, "Logging");
        let logging = match self.launcher.view() {
            Ok(logging) => logging,
            Err(message) => {
                ui.label(RichText::new(message).color(palette.error));
                Logging::default()
            }
        };
        for (switch, label, text) in LOGGING {
            let mut on = logging.get(switch);
            if ui.checkbox(&mut on, label).changed() {
                let result = self.launcher.set(switch, on);
                self.report(result);
            }
            hint(ui, text);
        }
        hint(
            ui,
            "Applies the next time a game starts. Logs: /tmp/fgvk-pacing-*.log",
        );
    }

    /// Theme and Catppuccin accent, saved in `ui.toml`. A change applies at once.
    fn appearance(&mut self, ui: &mut Ui) {
        let palette = theme::current(ui.ctx());
        section(ui, "Appearance");

        let mut index = self.themes.selected();
        let mut accent = self.themes.accent();
        let mut reload = false;
        ui.horizontal(|ui| {
            egui::ComboBox::from_id_salt("theme")
                .width(200.0)
                .selected_text(self.themes.current().name.as_str())
                .show_ui(ui, |ui| {
                    for (position, listed) in self.themes.themes().iter().enumerate() {
                        ui.selectable_value(&mut index, position, listed.name.as_str());
                    }
                });
            reload = ui.button("Reload themes").clicked();
        });
        if index != self.themes.selected() {
            self.themes.select(index);
            self.theme_changed(ui.ctx());
        }

        let flavour: Option<Flavour> = self.themes.flavour();
        if let (Some(flavour), true) = (flavour, self.themes.current().follows_accent_setting()) {
            ui.horizontal(|ui| {
                egui::ComboBox::from_id_salt("accent")
                    .width(160.0)
                    .selected_text(accent.label())
                    .show_ui(ui, |ui| {
                        for choice in Accent::ALL {
                            ui.horizontal(|ui| {
                                swatch(ui, flavour.accent(choice));
                                ui.selectable_value(&mut accent, choice, choice.label());
                            });
                        }
                    });
            });
            if accent != self.themes.accent() {
                self.themes.set_accent(accent);
                self.theme_changed(ui.ctx());
            }
        }

        ui.horizontal_wrapped(|ui| {
            hint(ui, "Themes folder:");
            match self.themes.folder() {
                Ok(folder) => {
                    ui.add(
                        Label::new(
                            RichText::new(folder.display().to_string())
                                .small()
                                .color(palette.subtext0),
                        )
                        .selectable(true),
                    );
                }
                Err(message) => {
                    ui.label(RichText::new(message.as_str()).small().color(palette.error));
                }
            }
        });
        hint(
            ui,
            "Add a .toml file to this folder, then press Reload themes. The example is in share/doc/framegen-vulkan/themes.",
        );
        for note in self.themes.notes() {
            note_line(ui, note);
        }
        if reload {
            self.themes.reload();
            self.sync_theme(ui.ctx());
        }
    }

    /// Applies the catalogue's palette when the frame does not use it yet. The frame is then drawn
    /// again, so the change shows at once.
    fn sync_theme(&self, ctx: &egui::Context) {
        let palette = self.themes.palette();
        if theme::current(ctx) != palette {
            theme::apply(ctx, &palette);
            ctx.request_discard("theme changed");
        }
    }

    /// Applies a changed theme and saves the choice. A failed save shows in the footer.
    fn theme_changed(&mut self, ctx: &egui::Context) {
        self.sync_theme(ctx);
        let result = self.themes.save();
        self.report(result);
    }
}

impl eframe::App for App {
    fn raw_input_hook(&mut self, _ctx: &egui::Context, input: &mut egui::RawInput) {
        input.max_texture_side = input.max_texture_side.map(|side| side.min(MAX_TEXTURE_SIDE));
    }

    fn logic(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        ctx.request_repaint_after(POLL_INTERVAL);
        if self.last_poll.elapsed() < POLL_INTERVAL {
            return;
        }
        self.last_poll = Instant::now();
        self.live.poll(SystemTime::now());
        if !self.has_pending() {
            if self.config.changed_on_disk() {
                self.config.reload();
            }
            if self.launcher.changed_on_disk() {
                self.launcher.reload();
            }
        }
    }

    fn ui(&mut self, ui: &mut Ui, _frame: &mut eframe::Frame) {
        self.sync_theme(ui.ctx());
        footer(ui, &self.status);
        egui::CentralPanel::default().show(ui, |ui| self.content(ui));
        self.remember_selection();
    }
}

/// The boxed list of executable names. Returns the index of the entry whose Remove button was
/// clicked.
fn games_box(ui: &mut Ui, entries: &[String]) -> Option<usize> {
    let palette = theme::current(ui.ctx());
    let mut remove = None;
    let frame = egui::Frame::NONE
        .fill(palette.mantle)
        .stroke(Stroke::new(1.0, palette.surface0))
        .corner_radius(CornerRadius::same(6))
        .inner_margin(Margin::same(4));
    // The box is GAMES_BOX_WIDTH wide, or as wide as the space when that is smaller. Its rows get
    // the width left inside the padding and the stroke.
    let outer_width = ui.available_width().min(GAMES_BOX_WIDTH);
    let inner_width = (outer_width - frame.total_margin().sum().x).max(0.0);
    frame.show(ui, |ui| {
        ui.set_width(inner_width);
        if entries.is_empty() {
            ui.horizontal_wrapped(|ui| {
                ui.add_space(ROW_TEXT_INSET);
                ui.label(RichText::new(NO_GAMES).small().color(palette.subtext0));
            });
            return;
        }
        // Rows touch each other, so the box's own item spacing does not apply between them.
        ui.spacing_mut().item_spacing.y = 0.0;
        for (index, entry) in entries.iter().enumerate() {
            if game_row(ui, index, entry) {
                remove = Some(index);
            }
        }
    });
    remove
}

/// One entry row: the name on the left and a Remove button on the right, shown on every row. The
/// name gets only the space the button leaves, so it never runs under the button. Returns true when
/// the button is clicked.
fn game_row(ui: &mut Ui, index: usize, entry: &str) -> bool {
    let palette = theme::current(ui.ctx());
    let (row, _) = ui.allocate_exact_size(vec2(ui.available_width(), ROW_HEIGHT), Sense::hover());
    let hovered = ui.rect_contains_pointer(row);

    let fill = if hovered {
        Some(palette.surface1)
    } else if index % 2 == 1 {
        Some(palette.stripe)
    } else {
        None
    };
    if let Some(fill) = fill {
        ui.painter().rect_filled(row, CornerRadius::same(4), fill);
    }

    // The button lays out right to left inside the row, so it keeps its own measured size, sits at
    // the right edge and is centred vertically. The row is already allocated, so this child adds no
    // space of its own.
    let mut right = ui.new_child(
        UiBuilder::new()
            .max_rect(row)
            .layout(Layout::right_to_left(Align::Center)),
    );
    let remove = right
        .small_button("Remove")
        .on_hover_text("Remove from this profile");

    let text_left = row.left() + ROW_TEXT_INSET;
    let text_rect = Rect::from_min_max(
        pos2(text_left, row.top()),
        pos2(
            (remove.rect.left() - REMOVE_GAP).max(text_left),
            row.bottom(),
        ),
    );
    name_text(ui, text_rect, index, entry);
    remove.clicked()
}

/// Paints an entry name inside `rect`, cut with an ellipsis when it does not fit. A cut name shows
/// its full text on hover.
fn name_text(ui: &mut Ui, rect: Rect, index: usize, entry: &str) {
    let palette = theme::current(ui.ctx());
    let font = TextStyle::Body.resolve(ui.style());
    let mut job = LayoutJob::simple_singleline(entry.to_owned(), font, palette.text);
    job.wrap = TextWrapping {
        max_width: rect.width(),
        max_rows: 1,
        break_anywhere: true,
        overflow_character: Some('…'),
    };
    let galley = ui.painter().layout_job(job);
    let elided = galley.elided;
    let pos = pos2(rect.left(), rect.center().y - galley.size().y / 2.0);
    ui.painter_at(rect).galley(pos, galley, palette.text);

    if elided {
        ui.interact(
            rect,
            ui.scope_id().with(("game-name", index)),
            Sense::hover(),
        )
        .on_hover_text(entry);
    }
}

fn invalid(ui: &mut Ui, message: &str) {
    let palette = theme::current(ui.ctx());
    ui.add_space(8.0);
    ui.label(RichText::new("This configuration cannot be edited.").strong());
    ui.add_space(4.0);
    ui.label(RichText::new(message).color(palette.error));
    ui.add_space(8.0);
    ui.label(
        RichText::new("The file has not been changed. fgvk loads it again once it is valid.")
            .small()
            .color(palette.subtext0),
    );
}

/// Bottom panel with the separator and status line. Its height follows the message, so a wrapped
/// error stays visible. Idle keeps one text row, so "Saved" does not move the layout.
fn footer(ui: &mut Ui, status: &Status) {
    let mut frame = egui::Frame::side_top_panel(ui.style());
    frame.inner_margin.bottom = 10;
    let response = egui::Panel::bottom("footer")
        .frame(frame)
        .show(ui, |ui| status_line(ui, status))
        .response;

    // A panel is sized from the previous pass. When the footer height changes, request another
    // pass so the new height is used in this frame instead of one frame later.
    let id = ui.scope_id().with("footer_height");
    let height = response.rect.height();
    if ui.data(|data| data.get_temp::<f32>(id)) != Some(height) {
        ui.data_mut(|data| data.insert_temp(id, height));
        ui.ctx().request_discard("footer height changed");
    }
}

fn status_line(ui: &mut Ui, status: &Status) {
    let palette = theme::current(ui.ctx());
    match status {
        // An empty label lays out the same text row as a message, so the height matches exactly.
        Status::Idle => {
            ui.label("");
        }
        Status::Saved => {
            ui.label(RichText::new("Saved").color(palette.success));
        }
        Status::Error(message) => {
            ui.label(RichText::new(message.as_str()).color(palette.error));
        }
    }
}

fn section(ui: &mut Ui, title: &str) {
    let palette = theme::current(ui.ctx());
    ui.add_space(6.0);
    ui.label(RichText::new(title).strong().color(palette.accent));
}

fn hint(ui: &mut Ui, text: &str) {
    let palette = theme::current(ui.ctx());
    ui.label(RichText::new(text).small().color(palette.subtext0));
}

/// One note from the theme catalogue: problems in the error colour, the rest in the hint colour.
fn note_line(ui: &mut Ui, note: &Note) {
    let palette = theme::current(ui.ctx());
    let color = if note.problem {
        palette.error
    } else {
        palette.subtext0
    };
    ui.label(RichText::new(note.text.as_str()).small().color(color));
}

/// A small filled square in `color`, drawn beside a name in a list.
fn swatch(ui: &mut Ui, color: Color32) {
    let (rect, _) = ui.allocate_exact_size(vec2(12.0, 12.0), Sense::hover());
    ui.painter().rect_filled(rect, CornerRadius::same(3), color);
}

fn mode_label(mode: Mode) -> String {
    match mode {
        Mode::Off => "Off".to_owned(),
        Mode::Fixed(multiplier) => format!("{multiplier}x"),
    }
}

/// Framed option button that returns true when clicked. The selected option gets a 1.5 px mauve
/// border painted over its frame. A stroke override on the button would change its layout size.
fn option_button(ui: &mut Ui, label: String, selected: bool) -> bool {
    let palette = theme::current(ui.ctx());
    let response = ui.add(Button::new(label).selected(selected));
    if selected {
        ui.painter().rect_stroke(
            response.rect,
            ui.visuals().widgets.inactive.corner_radius,
            Stroke::new(1.5, palette.accent),
            StrokeKind::Inside,
        );
    }
    response.clicked()
}

fn default_name(profiles: &[Profile]) -> String {
    let taken = |name: &str| profiles.iter().any(|profile| profile.key.name == name);
    if !taken("New profile") {
        return "New profile".to_owned();
    }
    (2..)
        .map(|number| format!("New profile {number}"))
        .find(|name| !taken(name.as_str()))
        .unwrap_or_default()
}

/// The profile to select first: the first one named like the remembered name, else the first one.
fn initial_index(profiles: &[Profile], remembered: Option<&str>) -> usize {
    remembered
        .and_then(|name| profiles.iter().position(|profile| profile.key.name == name))
        .unwrap_or(0)
}

fn parse_in_range(text: &str, label: &str, range: &RangeInclusive<i64>) -> Result<i64, String> {
    text.trim()
        .parse::<i64>()
        .ok()
        .filter(|value| range.contains(value))
        .ok_or_else(|| {
            format!(
                "{label} must be a whole number from {} to {}.",
                range.start(),
                range.end()
            )
        })
}

/// Text box that commits on Enter or focus loss. Returns the text at commit time.
fn text_commit(
    ui: &mut Ui,
    buffer: &mut Option<String>,
    shown: &str,
    width: f32,
    hint_text: &str,
) -> Option<String> {
    let mut text = buffer.take().unwrap_or_else(|| shown.to_owned());
    let response = ui.add(
        egui::TextEdit::singleline(&mut text)
            .desired_width(width)
            .hint_text(hint_text),
    );
    if response.lost_focus() {
        return Some(text);
    }
    if response.has_focus() {
        *buffer = Some(text);
    }
    None
}

/// Keeps a draft while a slider is dragged and returns the value once the interaction ends.
fn slider_commit<T>(draft: &mut Option<T>, value: T, response: &egui::Response) -> Option<T> {
    if response.dragged() {
        *draft = Some(value);
        None
    } else if response.drag_stopped() || response.changed() {
        *draft = None;
        Some(value)
    } else {
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::config::ProfileKey;

    fn profile(name: &str) -> Profile {
        Profile {
            key: ProfileKey {
                index: 0,
                name: name.to_owned(),
            },
            active_in: Vec::new(),
            mode: Mode::Fixed(2),
            flow_scale: 0.8,
            base_fps_cap: 0,
            performance_mode: false,
            low_latency: true,
            batch_hold: true,
        }
    }

    #[test]
    fn initial_index_selects_the_remembered_profile() {
        let profiles = [profile("Alpha"), profile("Beta"), profile("Gamma")];
        assert_eq!(initial_index(&profiles, Some("Beta")), 1);
        assert_eq!(initial_index(&profiles, Some("Gamma")), 2);
    }

    #[test]
    fn initial_index_falls_back_to_the_first_profile() {
        let profiles = [profile("Alpha"), profile("Beta")];
        assert_eq!(initial_index(&profiles, Some("Missing")), 0, "no match");
        assert_eq!(initial_index(&profiles, Some("beta")), 0, "match is exact");
        assert_eq!(initial_index(&profiles, None), 0, "nothing remembered");
        assert_eq!(initial_index(&[], Some("Alpha")), 0, "empty list");
        assert_eq!(
            initial_index(&[], None),
            0,
            "empty list, nothing remembered"
        );
    }

    #[test]
    fn initial_index_takes_the_first_of_duplicate_names() {
        let profiles = [profile("Same"), profile("Other"), profile("Same")];
        assert_eq!(initial_index(&profiles, Some("Same")), 0);
    }
}
