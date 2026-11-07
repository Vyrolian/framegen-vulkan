//! The settings window. It renders typed snapshots from `config` and never touches TOML.

use std::ops::RangeInclusive;
use std::time::{Duration, Instant};

use egui::{Button, Key, RichText, Stroke, StrokeKind, Ui};

use crate::config::{self, Config, ConfigView, FpsMemory, Mode, Profile, ProfileKey, Settings};
use crate::theme;

const POLL_INTERVAL: Duration = Duration::from_secs(1);
const FPS_SLIDER_RANGE: RangeInclusive<i64> = 10..=360;
const MODES: [Mode; 6] = [
    Mode::Off,
    Mode::Fixed(2),
    Mode::Fixed(3),
    Mode::Fixed(4),
    Mode::Fixed(5),
    Mode::Adaptive,
];
const MAX_MULTIPLIERS: [i64; 4] = [2, 3, 4, 5];

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
    selected: usize,
    status: Status,
    name_edit: Option<NameEdit>,
    confirm_delete: bool,
    fps_memory: FpsMemory,
    /// Slider drafts exist only while a drag is in progress.
    fps_slider: Option<i64>,
    /// Number-box text exists only while the box has focus.
    fps_text: Option<String>,
    target_text: Option<String>,
    quality: Option<f64>,
    dll_text: Option<String>,
    new_game: String,
    last_poll: Instant,
}

impl App {
    pub fn new(path: Result<std::path::PathBuf, String>) -> Self {
        Self {
            config: Config::open(path),
            selected: 0,
            status: Status::Idle,
            name_edit: None,
            confirm_delete: false,
            fps_memory: FpsMemory::default(),
            fps_slider: None,
            fps_text: None,
            target_text: None,
            quality: None,
            dll_text: None,
            new_game: String::new(),
            last_poll: Instant::now(),
        }
    }

    /// True while any edit is unsaved. External reloads wait until it clears.
    fn has_pending(&self) -> bool {
        self.name_edit.is_some()
            || self.fps_slider.is_some()
            || self.fps_text.is_some()
            || self.target_text.is_some()
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
        self.target_text = None;
        self.quality = None;
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
            .show(ui, |ui| match self.config.view() {
                ConfigView::Missing(path) => self.missing(ui, &path),
                ConfigView::Invalid(message) => {
                    self.clear_profile_drafts();
                    invalid(ui, &message);
                }
                ConfigView::Ready(settings) => self.editor(ui, settings),
            });
    }

    fn missing(&mut self, ui: &mut Ui, path: &str) {
        ui.add_space(8.0);
        ui.label("No configuration file yet.");
        ui.label(RichText::new(path).small().color(theme::SUBTEXT0));
        ui.add_space(8.0);
        if ui.button("Create").clicked() {
            let result = self.config.create_default();
            self.report(result);
        }
    }

    fn editor(&mut self, ui: &mut Ui, settings: Settings) {
        let Settings { profiles, dll } = settings;
        self.selected = self.selected.min(profiles.len().saturating_sub(1));

        section(ui, "Profile");
        self.profile_row(ui, &profiles);
        self.name_editor(ui);
        self.delete_confirm(ui, &profiles);

        match profiles.get(self.selected) {
            Some(profile) => self.profile_fields(ui, profile),
            None => {
                self.clear_profile_drafts();
                hint(ui, "No profiles. Use New to add one.");
            }
        }

        section(ui, "Global");
        self.global(ui, dll.unwrap_or_default());
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
        let (delete, cancel) = ui
            .horizontal(|ui| {
                ui.label(format!("Delete \"{}\"?", key.name));
                let delete = ui
                    .button(RichText::new("Delete").color(theme::RED))
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

        ui.add_space(6.0);
        self.fps_limit(ui, profile);
    }

    fn games(&mut self, ui: &mut Ui, profile: &Profile) {
        let mut remove = None;
        for (index, entry) in profile.active_in.iter().enumerate() {
            ui.horizontal(|ui| {
                ui.label(entry.as_str());
                if ui.small_button("×").on_hover_text("Remove").clicked() {
                    remove = Some(index);
                }
            });
        }

        let add = ui
            .horizontal(|ui| {
                let response = ui.add(
                    egui::TextEdit::singleline(&mut self.new_game)
                        .desired_width(300.0)
                        .hint_text("Game.exe"),
                );
                let enter =
                    response.lost_focus() && ui.input(|input| input.key_pressed(Key::Enter));
                enter || ui.button("Add").clicked()
            })
            .inner;

        if add {
            self.add_game(profile);
        }
        if let Some(index) = remove {
            let mut entries = profile.active_in.clone();
            entries.remove(index);
            let result = self.config.set_active_in(&profile.key, &entries);
            self.report(result);
        }
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

        if profile.mode == Mode::Adaptive {
            ui.indent("adaptive", |ui| self.adaptive(ui, profile));
        } else {
            self.target_text = None;
        }
    }

    fn adaptive(&mut self, ui: &mut Ui, profile: &Profile) {
        let shown = profile.target_fps.to_string();
        let typed = ui
            .horizontal(|ui| {
                ui.label("Target FPS");
                text_commit(ui, &mut self.target_text, &shown, 64.0, "")
            })
            .inner;
        if let Some(text) = typed.filter(|text| text.trim() != shown) {
            let result = parse_in_range(&text, "Target FPS", &config::TARGET_FPS_RANGE)
                .and_then(|value| self.config.set_target_fps(&profile.key, value));
            self.report(result);
        }

        let chosen = ui
            .horizontal(|ui| {
                ui.label("Max multiplier");
                let mut max = profile.adaptive_max_multiplier;
                for option in MAX_MULTIPLIERS {
                    if option_button(
                        ui,
                        format!("{option}x"),
                        profile.adaptive_max_multiplier == option,
                    ) {
                        max = option;
                    }
                }
                max
            })
            .inner;
        if chosen != profile.adaptive_max_multiplier {
            let result = self
                .config
                .set_adaptive_max_multiplier(&profile.key, chosen);
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

    fn global(&mut self, ui: &mut Ui, dll: String) {
        ui.label("Lossless.dll");
        let typed = text_commit(ui, &mut self.dll_text, &dll, 360.0, "Path to Lossless.dll");
        if let Some(text) = typed.filter(|text| text.trim() != dll.trim()) {
            let result = self.config.set_global_dll(Some(text.trim()));
            self.report(result);
        }
        hint(
            ui,
            "Needed for LSFG and LS1 scaling. Leave empty to remove the setting.",
        );
    }
}

impl eframe::App for App {
    fn logic(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        ctx.request_repaint_after(POLL_INTERVAL);
        if self.last_poll.elapsed() < POLL_INTERVAL {
            return;
        }
        self.last_poll = Instant::now();
        if !self.has_pending() && self.config.changed_on_disk() {
            self.config.reload();
        }
    }

    fn ui(&mut self, ui: &mut Ui, _frame: &mut eframe::Frame) {
        footer(ui, &self.status);
        egui::CentralPanel::default().show(ui, |ui| self.content(ui));
    }
}

fn invalid(ui: &mut Ui, message: &str) {
    ui.add_space(8.0);
    ui.label(RichText::new("This configuration cannot be edited.").strong());
    ui.add_space(4.0);
    ui.label(RichText::new(message).color(theme::RED));
    ui.add_space(8.0);
    ui.label(
        RichText::new("The file has not been changed. fgvk loads it again once it is valid.")
            .small()
            .color(theme::SUBTEXT0),
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
    match status {
        // An empty label lays out the same text row as a message, so the height matches exactly.
        Status::Idle => {
            ui.label("");
        }
        Status::Saved => {
            ui.label(RichText::new("Saved").color(theme::GREEN));
        }
        Status::Error(message) => {
            ui.label(RichText::new(message.as_str()).color(theme::RED));
        }
    }
}

fn section(ui: &mut Ui, title: &str) {
    ui.add_space(6.0);
    ui.label(RichText::new(title).strong().color(theme::MAUVE));
}

fn hint(ui: &mut Ui, text: &str) {
    ui.label(RichText::new(text).small().color(theme::SUBTEXT0));
}

fn mode_label(mode: Mode) -> String {
    match mode {
        Mode::Off => "Off".to_owned(),
        Mode::Fixed(multiplier) => format!("{multiplier}x"),
        Mode::Adaptive => "Adaptive".to_owned(),
    }
}

/// Framed option button that returns true when clicked. The selected option gets a 1.5 px mauve
/// border painted over its frame. A stroke override on the button would change its layout size.
fn option_button(ui: &mut Ui, label: String, selected: bool) -> bool {
    let response = ui.add(Button::new(label).selected(selected));
    if selected {
        ui.painter().rect_stroke(
            response.rect,
            ui.visuals().widgets.inactive.corner_radius,
            Stroke::new(1.5, theme::MAUVE),
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
