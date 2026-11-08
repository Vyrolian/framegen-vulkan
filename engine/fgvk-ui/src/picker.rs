//! The "Pick a running program" window. It lists the programs the user is running and reports the
//! entries the user clicks. Scans run on a background thread, and nothing is signalled or changed.

use std::sync::mpsc::{self, Receiver, TryRecvError};
use std::thread;
use std::time::{Duration, Instant};

use egui::{
    Color32, Context, CornerRadius, CursorIcon, Frame, Label, Margin, RichText, ScrollArea, Sense,
    Shape, TextEdit, Ui, ViewportBuilder, ViewportId,
};

use crate::processes::{self, Group};
use crate::theme;

const TITLE: &str = "Pick a running program";
const WINDOW_SIZE: [f32; 2] = [520.0, 560.0];
const REFRESH_INTERVAL: Duration = Duration::from_secs(2);
/// How often a running scan is polled. The parent window also repaints at its own poll interval.
const SCAN_CHECK: Duration = Duration::from_millis(100);
const ROW_RADIUS: u8 = 6;
const BADGE_RADIUS: u8 = 4;

/// What one frame of the window produced.
#[derive(Default)]
pub struct Outcome {
    /// Entries clicked this frame, in click order.
    pub picked: Vec<String>,
    /// The user closed the window.
    pub closed: bool,
}

/// State of the window while it is open. The owner drops it when the window closes.
pub struct Picker {
    query: String,
    vulkan_only: bool,
    /// `None` until the first scan finishes.
    groups: Option<Vec<Group>>,
    scan: Option<Receiver<Vec<Group>>>,
    next_scan: Instant,
}

impl Picker {
    pub fn new() -> Self {
        Self {
            query: String::new(),
            vulkan_only: true,
            groups: None,
            scan: None,
            next_scan: Instant::now(),
        }
    }

    /// Shows the window for one frame. `listed` holds the profile's current entries, which
    /// disable their rows.
    pub fn show(&mut self, ctx: &Context, listed: &[String]) -> Outcome {
        self.poll(ctx);
        ctx.show_viewport_immediate(
            ViewportId::from_hash_of("fgvk-process-picker"),
            ViewportBuilder::default()
                .with_title(TITLE)
                .with_inner_size(WINDOW_SIZE)
                .with_resizable(true)
                .with_close_button(true),
            |ui, _class| {
                let mut outcome = Outcome::default();
                if ui.input(|input| input.viewport().close_requested()) {
                    outcome.closed = true;
                    return outcome;
                }
                Frame::central_panel(ui.style()).show(ui, |ui| {
                    self.body(ui, listed, &mut outcome.picked);
                });
                outcome
            },
        )
    }

    /// Collects a finished scan and starts the next one when it is due. Never waits for a scan.
    fn poll(&mut self, ctx: &Context) {
        if let Some(scan) = self.scan.take() {
            match scan.try_recv() {
                Ok(groups) => {
                    self.groups = Some(groups);
                    self.next_scan = Instant::now() + REFRESH_INTERVAL;
                }
                Err(TryRecvError::Empty) => self.scan = Some(scan),
                Err(TryRecvError::Disconnected) => {
                    // The scan thread ended without a result. Show an empty list and retry later.
                    self.groups.get_or_insert_with(Vec::new);
                    self.next_scan = Instant::now() + REFRESH_INTERVAL;
                }
            }
        }

        if self.scan.is_some() {
            ctx.request_repaint_after(SCAN_CHECK);
        } else if Instant::now() >= self.next_scan {
            self.start_scan();
            ctx.request_repaint_after(SCAN_CHECK);
        } else {
            ctx.request_repaint_after(self.next_scan.saturating_duration_since(Instant::now()));
        }
    }

    fn start_scan(&mut self) {
        let (sender, receiver) = mpsc::channel();
        let spawned = thread::Builder::new()
            .name("fgvk-process-scan".to_owned())
            .spawn(move || {
                // A failed send means the window closed first, so the result is not needed.
                let _ = sender.send(processes::group(&processes::scan_current_user()));
            });
        match spawned {
            Ok(_) => self.scan = Some(receiver),
            Err(_) => {
                self.groups.get_or_insert_with(Vec::new);
                self.next_scan = Instant::now() + REFRESH_INTERVAL;
            }
        }
    }

    fn body(&mut self, ui: &mut Ui, listed: &[String], picked: &mut Vec<String>) {
        let width = ui.available_width();
        ui.add(
            TextEdit::singleline(&mut self.query)
                .desired_width(width)
                .hint_text("Search name or path"),
        );
        ui.checkbox(&mut self.vulkan_only, "Only Vulkan apps");
        hint(
            ui,
            "Click a program to add it to this profile. Programs are only read.",
        );
        ui.add_space(4.0);

        match &self.groups {
            None => hint(ui, "Scanning…"),
            Some(groups) => {
                let rows = processes::visible(groups, &self.query, self.vulkan_only);
                if rows.is_empty() {
                    hint(ui, empty_message(groups, self.vulkan_only));
                }
                ScrollArea::vertical()
                    .auto_shrink([false, false])
                    .show(ui, |ui| {
                        for group in rows {
                            row(ui, group, listed, picked);
                        }
                    });
            }
        }
    }
}

/// One clickable row. Rows already in the profile are shown dimmed and do not respond to clicks.
fn row(ui: &mut Ui, group: &Group, listed: &[String], picked: &mut Vec<String>) {
    let added = processes::is_listed(&group.entry, listed);
    let (name_color, detail_color) = if added {
        (theme::OVERLAY0, theme::OVERLAY0)
    } else {
        (theme::TEXT, theme::SUBTEXT0)
    };

    // The background is painted after the content is laid out, so it can follow hover state.
    let background = ui.painter().add(Shape::Noop);
    let frame = Frame::new()
        .inner_margin(Margin::symmetric(10, 6))
        .show(ui, |ui| {
            ui.horizontal_wrapped(|ui| {
                ui.label(RichText::new(&group.entry).strong().color(name_color));
                ui.label(
                    RichText::new(process_count(group.processes))
                        .small()
                        .color(detail_color),
                );
                if group.vulkan {
                    badge(ui, "Vulkan", theme::GREEN);
                }
                if group.wine {
                    badge(ui, "Wine", theme::MAUVE);
                }
                if added {
                    badge(ui, "already added", theme::SUBTEXT0);
                }
                if group.launcher {
                    badge(ui, "launcher (ignored by layer)", theme::RED);
                }
            });
            ui.add(Label::new(RichText::new(&group.path).small().color(detail_color)).truncate());
        });

    let rect = frame.response.rect;
    let sense = if added {
        Sense::hover()
    } else {
        Sense::click()
    };
    let id = ui.scope_id().with(("picker-row", &group.entry));
    let response = ui.interact(rect, id, sense);
    let fill = if response.hovered() && !added {
        theme::SURFACE0
    } else {
        theme::MANTLE
    };
    ui.painter().set(
        background,
        Shape::rect_filled(rect, CornerRadius::same(ROW_RADIUS), fill),
    );

    if added {
        response.on_hover_text("Already in this profile.");
    } else {
        if response.clicked() {
            picked.push(group.entry.clone());
        }
        response.on_hover_cursor(CursorIcon::PointingHand);
    }
}

fn badge(ui: &mut Ui, text: &str, color: Color32) {
    Frame::new()
        .fill(theme::SURFACE0)
        .corner_radius(CornerRadius::same(BADGE_RADIUS))
        .inner_margin(Margin::symmetric(5, 0))
        .show(ui, |ui| {
            ui.label(RichText::new(text).small().color(color));
        });
}

fn hint(ui: &mut Ui, text: &str) {
    ui.label(RichText::new(text).small().color(theme::SUBTEXT0));
}

fn process_count(count: usize) -> String {
    if count == 1 {
        "1 process".to_owned()
    } else {
        format!("{count} processes")
    }
}

fn empty_message(groups: &[Group], vulkan_only: bool) -> &'static str {
    if groups.is_empty() {
        "No running programs found."
    } else if vulkan_only && !groups.iter().any(|group| group.vulkan) {
        "No running Vulkan apps. Untick Only Vulkan apps to list the rest."
    } else {
        "Nothing matches the search."
    }
}
