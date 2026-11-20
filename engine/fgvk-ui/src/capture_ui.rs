use std::path::PathBuf;
use std::sync::mpsc::{self, Receiver, TryRecvError};
use std::time::{Duration, SystemTime};

use egui::{Button, Context, Ui};

use crate::{analysis, live};

#[derive(Default)]
pub struct Captures {
    logs: Vec<(PathBuf, String)>,
    path: String,
    start: String,
    end: String,
    report: String,
    progress: Option<Receiver<String>>,
    loaded: bool,
}

impl Captures {
    fn discover(&mut self) {
        self.logs.clear();
        let Some(uid) = live::own_uid() else {
            return;
        };
        let mut directories = vec![live::log_directory(|key| std::env::var_os(key))];
        if let Some(saved) = live::saved_log_directory(|key| std::env::var_os(key)) {
            if !directories.contains(&saved) {
                directories.push(saved);
            }
        }
        let mut files: Vec<_> = directories
            .iter()
            .flat_map(|path| live::log_files(path, uid))
            .collect();
        // Wine helper processes can finish just after the main game. Prefer the largest capture
        // within the most recent group while preserving access to each individual file.
        let latest = files
            .iter()
            .filter_map(|(_, meta)| meta.modified().ok())
            .max();
        let recent = |meta: &std::fs::Metadata| {
            meta.modified()
                .ok()
                .zip(latest)
                .is_some_and(|(time, latest)| {
                    latest.duration_since(time).unwrap_or_default() < Duration::from_secs(10)
                })
        };
        files.sort_by_key(|(_, meta)| {
            std::cmp::Reverse((
                recent(meta),
                if recent(meta) { meta.len() } else { 0 },
                meta.modified().ok(),
            ))
        });
        let now = SystemTime::now();
        self.logs = files
            .into_iter()
            .map(|(path, meta)| {
                let name = path.file_name().unwrap_or_default().to_string_lossy();
                let mib = meta.len() as f64 / (1024.0 * 1024.0);
                let age = meta
                    .modified()
                    .ok()
                    .and_then(|time| now.duration_since(time).ok())
                    .unwrap_or_default()
                    .as_secs();
                let when = match age {
                    0..=59 => "just now".to_owned(),
                    60..=3599 => format!("{} min ago", age / 60),
                    3600..=86399 => format!("{} h ago", age / 3600),
                    _ => format!("{} d ago", age / 86400),
                };
                let label = format!("{when} · {mib:.1} MiB · {name}");
                (path, label)
            })
            .collect();
    }

    fn range(&self) -> Result<Option<(f64, f64)>, &'static str> {
        if self.start.trim().is_empty() && self.end.trim().is_empty() {
            return Ok(None);
        }
        let parse = |s: &str| {
            s.trim()
                .parse::<f64>()
                .ok()
                .filter(|v| v.is_finite() && *v >= 0.0)
        };
        let start = if self.start.trim().is_empty() {
            Some(0.0)
        } else {
            parse(&self.start)
        };
        match (start, parse(&self.end)) {
            (Some(start), Some(end)) if end > start => Ok(Some((start, end))),
            _ => Err("Enter a nonnegative start and a later end, in seconds."),
        }
    }

    fn start(&mut self, ctx: &Context) {
        let Some(uid) = live::own_uid() else {
            return;
        };
        let range = match self.range() {
            Ok(range) => range,
            Err(error) => {
                self.report = error.to_owned();
                return;
            }
        };
        let path = PathBuf::from(self.path.trim());
        if !path.is_absolute() {
            self.report = "Enter an absolute capture path.".to_owned();
            return;
        }
        let (send, receive) = mpsc::channel();
        let ctx = ctx.clone();
        let worker = std::thread::Builder::new()
            .name("fgvk-capture-analysis".to_owned())
            .spawn(move || {
                let report = match analysis::analyze(&path, uid, range) {
                    Ok(Some(report)) => report,
                    Ok(None) => "No complete pacing windows in this capture.".to_owned(),
                    Err(error) => format!("Could not read the capture. {error}"),
                };
                let _ = send.send(report);
                ctx.request_repaint();
            });
        if worker.is_ok() {
            self.progress = Some(receive);
            self.report.clear();
        } else {
            self.report = "Could not start capture analysis.".to_owned();
        }
    }

    pub fn show(&mut self, ui: &mut Ui) {
        egui::CollapsingHeader::new("Capture analyzer").show(ui, |ui| {
            if !self.loaded {
                self.loaded = true;
                self.discover();
                if let Some((path, _)) = self.logs.first() {
                    self.path = path.display().to_string();
                    self.start(ui.ctx());
                }
            }

            if let Some(progress) = &self.progress {
                let result = match progress.try_recv() {
                    Ok(report) => Some(report),
                    Err(TryRecvError::Disconnected) => {
                        Some("Capture analysis was interrupted.".to_owned())
                    }
                    Err(TryRecvError::Empty) => None,
                };
                if let Some(report) = result {
                    self.report = report;
                    self.progress = None;
                }
            }

            ui.add_enabled_ui(self.progress.is_none(), |ui| {
                let mut changed_capture = false;
                ui.horizontal(|ui| {
                    egui::ComboBox::from_id_salt("saved-captures")
                        .selected_text(if self.logs.is_empty() {
                            "No captures found"
                        } else {
                            "Choose a capture"
                        })
                        .show_ui(ui, |ui| {
                            for (path, label) in &self.logs {
                                let path = path.display().to_string();
                                if ui.selectable_label(self.path == path, label).clicked() {
                                    self.path = path;
                                    self.start.clear();
                                    self.end.clear();
                                    changed_capture = true;
                                }
                            }
                        });
                    if ui.button("Refresh list").clicked() {
                        self.discover();
                    }
                });
                if ui
                    .add(
                        egui::TextEdit::singleline(&mut self.path)
                            .hint_text("Absolute capture path")
                            .desired_width(f32::INFINITY),
                    )
                    .changed()
                {
                    self.report.clear();
                }
                let mut analyze = changed_capture;
                ui.horizontal_wrapped(|ui| {
                    ui.add(
                        egui::TextEdit::singleline(&mut self.start)
                            .hint_text("Start (s)")
                            .desired_width(65.0),
                    );
                    ui.add(
                        egui::TextEdit::singleline(&mut self.end)
                            .hint_text("End (s)")
                            .desired_width(65.0),
                    );
                    if ui
                        .add_enabled(!self.path.trim().is_empty(), Button::new("Analyze"))
                        .clicked()
                    {
                        analyze = true;
                    }
                });
                if analyze {
                    self.start(ui.ctx());
                }
            });

            if self.progress.is_some() {
                ui.horizontal(|ui| {
                    ui.spinner();
                    ui.label("Reading capture…");
                });
            } else if !self.report.is_empty() {
                if ui.button("Copy report").clicked() {
                    ui.ctx().copy_text(self.report.clone());
                }
                ui.add(egui::Label::new(&self.report).selectable(true));
            }
        });
    }
}
