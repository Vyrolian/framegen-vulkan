//! Live readings for the settings window, taken from the pacing log that a game's layer writes.
//!
//! The window reads only the file the layer already writes. Once a second while the window is
//! open, `Follower::poll` picks the newest `fgvk-pacing-<uid>-*.log` in the temporary directory.
//! The file must be a regular file owned by this user, not a symbolic link, and modified in the
//! last three seconds. Only the bytes added since the previous poll are read, and the readings
//! keep only the window they show plus the records that finish it. No process, window or desktop
//! is queried.
//!
//! The layer writes records in order, so the records between two `pacing-profile` summaries
//! describe the second that the later summary reports. The section shows the newest summary's
//! second with the records written before that summary.

use std::collections::{BTreeMap, HashMap};
use std::ffi::OsString;
use std::fmt;
use std::fs::{self, File};
use std::io::{ErrorKind, Read, Seek, SeekFrom};
use std::os::unix::fs::{MetadataExt, OpenOptionsExt};
use std::path::{Path, PathBuf};
use std::str::FromStr;
use std::time::{Duration, SystemTime};

const FILE_PREFIX: &str = "fgvk-pacing-";
const FILE_SUFFIX: &str = ".log";
/// A log modified this recently belongs to a game that is running now.
const FRESH: Duration = Duration::from_secs(3);
/// A new log starts this many bytes before its end, and a skip lands here.
const TAIL_BYTES: u64 = 4 << 20;
/// More new bytes than this in one poll means skipping ahead to the tail.
const MAX_READ: u64 = 8 << 20;
/// A partial line longer than this is dropped. Every pacing record is far shorter.
const MAX_LINE: usize = 256 << 10;
const READ_CHUNK: usize = 32 << 10;
const OPERATION: &str = "framegen-vulkan: present diagnostics: operation=";
/// `O_NOFOLLOW`, checked on x86_64; aarch64 and riscv64 use the same asm-generic value. Elsewhere
/// the flag is 0 and the identity check in `open_checked` still refuses a symbolic link.
/// `libc` is not a dependency of this crate, so the value is written out here.
#[cfg(any(
    target_arch = "x86_64",
    target_arch = "aarch64",
    target_arch = "riscv64"
))]
const O_NOFOLLOW: i32 = 0o400000;
#[cfg(not(any(
    target_arch = "x86_64",
    target_arch = "aarch64",
    target_arch = "riscv64"
)))]
const O_NOFOLLOW: i32 = 0;
const SKIP_KEYS: [&str; 5] = [
    "generated_skip_readiness",
    "generated_skip_expedite",
    "generated_skip_give_up",
    "generated_skip_acquire",
    "generated_skip_other",
];
/// `wp_presentation` feedback flags: VSYNC and ZERO_COPY.
const VSYNC: u64 = 0x1;
const ZERO_COPY: u64 = 0x8;

/// What the settings window shows for the live section.
#[derive(Debug, PartialEq)]
pub enum View {
    /// No fresh pacing log exists: no game is logging right now.
    NoGame,
    Readings(Vec<Row>),
}

/// One line of the live section.
#[derive(Debug, PartialEq)]
pub enum Row {
    Text(String),
    /// A reading with no records in its window: it names the checkbox that writes them.
    Needs {
        reading: &'static str,
        checkbox: &'static str,
    },
}

impl fmt::Display for Row {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Row::Text(text) => f.write_str(text),
            Row::Needs { reading, checkbox } => write!(f, "{reading}: needs \"{checkbox}\""),
        }
    }
}

/// The followed log, the readings from it and whether the last poll found it fresh.
pub struct Follower {
    directory: PathBuf,
    uid: Option<u32>,
    tail: Option<Tail>,
    readings: Readings,
    fresh: bool,
}

impl Follower {
    /// `uid` is the owner the layer gives its logs. `None` (no uid could be read) follows nothing.
    pub fn new(directory: PathBuf, uid: Option<u32>) -> Self {
        Self {
            directory,
            uid,
            tail: None,
            readings: Readings::default(),
            fresh: false,
        }
    }

    /// Reads what was appended since the previous poll. Call once a second.
    pub fn poll(&mut self, now: SystemTime) {
        self.fresh = false;
        let Some(uid) = self.uid else { return };
        let Some((path, listed)) = newest_log(&self.directory, uid) else {
            return;
        };
        let Ok(modified) = listed.modified() else {
            return;
        };
        if !is_fresh(modified, now) {
            return;
        }
        let Some((mut file, meta)) = open_checked(&path, uid, &listed) else {
            return;
        };
        self.fresh = true;
        let identity = (meta.dev(), meta.ino());
        let len = meta.len();
        if !matches!(&self.tail, Some(tail) if tail.identity == identity) {
            // A log this follower has not read before (a new game): begin near its end.
            self.tail = Some(Tail::starting_at(identity, len.saturating_sub(TAIL_BYTES)));
            self.readings = Readings::default();
        }
        let Some(tail) = self.tail.as_mut() else {
            return;
        };
        if len < tail.offset {
            // The layer truncated the log when it wrapped: read it again from the start.
            *tail = Tail::starting_at(identity, 0);
            self.readings = Readings::default();
        } else if len - tail.offset > MAX_READ {
            // Too much was appended to read in one poll: skip to the tail, as on first attach.
            *tail = Tail::starting_at(identity, len - TAIL_BYTES);
            self.readings = Readings::default();
        }
        let wanted = len - tail.offset;
        if file.seek(SeekFrom::Start(tail.offset)).is_err() {
            return;
        }
        let mut input = file.take(wanted);
        let mut buffer = [0; READ_CHUNK];
        loop {
            match input.read(&mut buffer) {
                Ok(0) => break,
                Ok(count) => {
                    tail.offset += count as u64;
                    consume(tail, &mut self.readings, &buffer[..count]);
                }
                Err(error) if error.kind() == ErrorKind::Interrupted => continue,
                Err(_) => break,
            }
        }
    }

    pub fn view(&self) -> View {
        if self.fresh {
            View::Readings(self.readings.rows())
        } else {
            View::NoGame
        }
    }
}

/// `${TMPDIR:-/tmp}` as the layer chooses it: an empty or relative TMPDIR means `/tmp`.
pub fn log_directory(lookup: impl Fn(&str) -> Option<OsString>) -> PathBuf {
    lookup("TMPDIR")
        .filter(|value| !value.is_empty())
        .map(PathBuf::from)
        .filter(|path| path.is_absolute())
        .unwrap_or_else(|| PathBuf::from("/tmp"))
}

/// This process's effective uid, which the layer uses in its log names. `/proc/self` takes the
/// effective uid as its owner while the process is dumpable, which a settings window always is.
pub fn own_uid() -> Option<u32> {
    fs::metadata("/proc/self").ok().map(|meta| meta.uid())
}

/// A file being followed: its identity, the offset read up to, and the bytes after the last
/// newline. `skipping` is set after a skip, until the newline that ends the cut line.
struct Tail {
    identity: (u64, u64),
    offset: u64,
    partial: Vec<u8>,
    skipping: bool,
}

impl Tail {
    fn starting_at(identity: (u64, u64), offset: u64) -> Self {
        Self {
            identity,
            offset,
            partial: Vec::new(),
            skipping: offset > 0,
        }
    }
}

fn is_fresh(modified: SystemTime, now: SystemTime) -> bool {
    // A modification time after `now` (a clock step) counts as fresh.
    match now.duration_since(modified) {
        Ok(age) => age <= FRESH,
        Err(_) => true,
    }
}

/// The newest regular file named for `uid`, judged by its own metadata (not a followed link).
fn newest_log(directory: &Path, uid: u32) -> Option<(PathBuf, fs::Metadata)> {
    let prefix = format!("{FILE_PREFIX}{uid}-");
    let mut newest: Option<(SystemTime, PathBuf, fs::Metadata)> = None;
    for entry in fs::read_dir(directory).ok()?.flatten() {
        let name = entry.file_name();
        let Some(name) = name.to_str() else {
            continue;
        };
        if !(name.starts_with(&prefix) && name.ends_with(FILE_SUFFIX)) {
            continue;
        }
        let path = entry.path();
        // lstat: a symbolic link is never a log, whatever it points to.
        let Ok(meta) = fs::symlink_metadata(&path) else {
            continue;
        };
        if !meta.file_type().is_file() || meta.uid() != uid {
            continue;
        }
        let Ok(modified) = meta.modified() else {
            continue;
        };
        let newer = match &newest {
            Some((time, _, _)) => modified > *time,
            None => true,
        };
        if newer {
            newest = Some((modified, path, meta));
        }
    }
    newest.map(|(_, path, meta)| (path, meta))
}

/// Opens `path` without following a link, then checks that the open file is the one listed (same
/// device and inode), is regular and belongs to `uid`.
fn open_checked(path: &Path, uid: u32, listed: &fs::Metadata) -> Option<(File, fs::Metadata)> {
    let file = fs::OpenOptions::new()
        .read(true)
        .custom_flags(O_NOFOLLOW)
        .open(path)
        .ok()?;
    let meta = file.metadata().ok()?;
    let same_file = meta.dev() == listed.dev() && meta.ino() == listed.ino();
    (same_file && meta.file_type().is_file() && meta.uid() == uid).then_some((file, meta))
}

/// Parse complete lines directly from each read. Only a line split across reads or polls needs
/// storage. A cut or oversized line is skipped through its next newline.
fn consume(tail: &mut Tail, readings: &mut Readings, bytes: &[u8]) {
    for part in bytes.split_inclusive(|&byte| byte == b'\n') {
        let line = part.strip_suffix(b"\n");
        let fragment = line.unwrap_or(part);
        if tail.skipping || tail.partial.len() + fragment.len() > MAX_LINE {
            tail.partial.clear();
            tail.skipping = line.is_none();
            continue;
        }
        let Some(line) = line else {
            tail.partial.extend_from_slice(fragment);
            break;
        };
        let complete = if tail.partial.is_empty() {
            line
        } else {
            tail.partial.extend_from_slice(line);
            &tail.partial
        };
        if let Some(record) = std::str::from_utf8(complete).ok().and_then(parse_line) {
            readings.apply(record);
        }
        tail.partial.clear();
    }
}

/// The record kinds the live section reads. Other operations are skipped.
#[derive(Debug, PartialEq)]
enum Record {
    Summary(Summary),
    Request {
        key: WaylandKey,
        request: Request,
    },
    Presented {
        key: WaylandKey,
        presented: Presented,
    },
    /// The outcome of a request that was not presented.
    Ended {
        key: WaylandKey,
        discarded: bool,
    },
    Marker(SourceSample),
    Split(SplitSample),
}

#[derive(Debug, PartialEq)]
struct Summary {
    context: u64,
    window_ms: f64,
    sources: u64,
    output_attempts: u64,
    skipped: u64,
    internal_images: Option<bool>,
    configured_multiplier: Option<u64>,
    game_modes: String,
    real_modes: String,
    generated_modes: String,
}

#[derive(Debug, PartialEq, Eq, Hash, Clone, Copy)]
struct WaylandKey {
    pid: u64,
    surface: u64,
    feedback: u64,
}

#[derive(Debug, PartialEq)]
struct Request {
    context: u64,
    source: u64,
    generated: bool,
    requested_ns: u64,
    clock_id: u64,
}

#[derive(Debug, PartialEq)]
struct Presented {
    presented_ns: u64,
    clock_id: u64,
    refresh_ns: u64,
    flags: u64,
}

/// Identifies one output: the process, the swapchain context and the source frame it came from.
#[derive(Debug, PartialEq, Eq, Hash, PartialOrd, Ord, Clone, Copy)]
struct SourceKey {
    pid: u64,
    context: u64,
    source: u64,
}

#[derive(Debug, PartialEq)]
struct SourceSample {
    key: SourceKey,
    generated: bool,
    start_ns: u64,
}

#[derive(Debug, PartialEq)]
struct SplitSample {
    key: SourceKey,
    fg_start_ns: u64,
    prepass_end_ns: u64,
    generated_end_ns: u64,
}

/// The value of `key` among the space-separated `key=value` tokens of a record.
fn field<'a>(fields: &'a str, key: &str) -> Option<&'a str> {
    fields.split(' ').find_map(|token| {
        let (name, value) = token.split_once('=')?;
        (name == key).then_some(value)
    })
}

fn number<T: FromStr>(fields: &str, key: &str) -> Option<T> {
    field(fields, key)?.parse().ok()
}

fn flag(fields: &str, key: &str) -> Option<bool> {
    match field(fields, key)? {
        "0" => Some(false),
        "1" => Some(true),
        _ => None,
    }
}

/// Returns the record on one line, or `None` for any other line or a malformed record.
fn parse_line(line: &str) -> Option<Record> {
    let start = line.find(OPERATION)? + OPERATION.len();
    let body = &line[start..];
    let (operation, fields) = body.split_once(' ').unwrap_or((body, ""));
    match operation {
        "pacing-profile" => summary(fields).map(Record::Summary),
        "pacing-profile-wayland" => wayland(fields),
        "pacing-profile-present-marker-gpu" => marker(fields),
        "pacing-profile-gpu-split" => split(fields),
        _ => None,
    }
}

fn summary(fields: &str) -> Option<Summary> {
    let window_ms: f64 = number(fields, "window_ms")?;
    if !(window_ms.is_finite() && window_ms > 0.0) {
        return None;
    }
    let mut skipped = 0u64;
    for key in SKIP_KEYS {
        skipped = skipped.checked_add(number(fields, key)?)?;
    }
    Some(Summary {
        context: number(fields, "context")?,
        window_ms,
        sources: number(fields, "sources")?,
        output_attempts: number(fields, "output_attempts")?,
        skipped,
        internal_images: field(fields, "internal_images").and_then(|value| match value {
            "1" => Some(true),
            "0" => Some(false),
            _ => None,
        }),
        configured_multiplier: number(fields, "configured_multiplier"),
        game_modes: modes(fields, "application_present_modes"),
        real_modes: modes(fields, "real_present_modes"),
        generated_modes: modes(fields, "generated_present_modes"),
    })
}

/// The mode names in a `name:count,name:count` list, joined with `+`. `-` means none.
fn modes(fields: &str, key: &str) -> String {
    let Some(text) = field(fields, key) else {
        return "unknown".to_owned();
    };
    let names: Vec<&str> = text
        .split(',')
        .filter_map(|item| item.split(':').next())
        .filter(|name| !name.is_empty() && *name != "-")
        .collect();
    if names.is_empty() {
        "none".to_owned()
    } else {
        names.join("+")
    }
}

fn wayland(fields: &str) -> Option<Record> {
    if field(fields, "version")? != "2" {
        return None;
    }
    let key = WaylandKey {
        pid: number(fields, "pid")?,
        surface: number(fields, "surface_token")?,
        feedback: number(fields, "feedback")?,
    };
    match field(fields, "event")? {
        "request" => Some(Record::Request {
            key,
            request: Request {
                context: number(fields, "context")?,
                source: number(fields, "source")?,
                generated: flag(fields, "generated")?,
                requested_ns: number(fields, "requested_ns")?,
                clock_id: number(fields, "clock_id")?,
            },
        }),
        "presented" => {
            let presented_ns: u64 = number(fields, "presented_ns")?;
            if presented_ns == 0 {
                return None;
            }
            Some(Record::Presented {
                key,
                presented: Presented {
                    presented_ns,
                    clock_id: number(fields, "clock_id")?,
                    refresh_ns: number(fields, "refresh_ns")?,
                    flags: number(fields, "flags")?,
                },
            })
        }
        "discarded" => Some(Record::Ended {
            key,
            discarded: true,
        }),
        "abandoned" => Some(Record::Ended {
            key,
            discarded: false,
        }),
        _ => None,
    }
}

fn source_key(fields: &str) -> Option<SourceKey> {
    Some(SourceKey {
        pid: number(fields, "pid")?,
        context: number(fields, "context")?,
        source: number(fields, "source")?,
    })
}

fn marker(fields: &str) -> Option<Record> {
    if field(fields, "version")? != "1" || !flag(fields, "available")? {
        return None;
    }
    let start_ns: u64 = number(fields, "marker_start_ns")?;
    if start_ns == 0 {
        return None;
    }
    Some(Record::Marker(SourceSample {
        key: source_key(fields)?,
        generated: flag(fields, "generated")?,
        start_ns,
    }))
}

fn split(fields: &str) -> Option<Record> {
    if field(fields, "version")? != "1" {
        return None;
    }
    Some(Record::Split(SplitSample {
        key: source_key(fields)?,
        fg_start_ns: number(fields, "fg_start_ns")?,
        prepass_end_ns: number(fields, "prepass_end_ns")?,
        generated_end_ns: number(fields, "generated_end_ns")?,
    }))
}

/// A presented output, with the request that produced it.
struct Output {
    tag: u64,
    key: SourceKey,
    generated: bool,
    feedback: u64,
    requested_ns: u64,
    request_clock: u64,
    presented_ns: u64,
    presented_clock: u64,
    refresh_ns: u64,
    flags: u64,
}

/// A request waiting for its outcome.
struct Pending {
    tag: u64,
    request: Request,
}

/// A time on the device or compositor clock, ordered within its source.
#[derive(Clone, Copy)]
struct Timed {
    tag: u64,
    key: SourceKey,
    generated: bool,
    order: u64,
    at: u64,
}

struct Split {
    tag: u64,
    fg_start_ns: u64,
    prepass_end_ns: u64,
    generated_end_ns: u64,
}

/// The records the live section needs. Records carry the tag of the window they belong to: the
/// number of summaries read before them. The window of tag `t` is the one the summary that ends it
/// reports, so the shown window is tag `summaries - 1`.
#[derive(Default)]
struct Readings {
    /// The swapchain context the readings follow. A summary of another context starts over.
    context: Option<u64>,
    summaries: u64,
    latest: Option<Summary>,
    requests: HashMap<WaylandKey, Pending>,
    outputs: Vec<Output>,
    discards: Vec<u64>,
    markers: Vec<Timed>,
    splits: Vec<Split>,
    sequence: u64,
}

impl Readings {
    fn apply(&mut self, record: Record) {
        match record {
            Record::Summary(summary) => self.summary(summary),
            other => self.sample(other),
        }
    }

    fn summary(&mut self, summary: Summary) {
        if self.context != Some(summary.context) {
            *self = Readings {
                context: Some(summary.context),
                ..Readings::default()
            };
        }
        self.summaries += 1;
        // Keep the shown window (summaries - 1) and the one before it, for reals that open a pair.
        let keep = self.summaries.saturating_sub(2);
        self.requests.retain(|_, pending| pending.tag >= keep);
        self.outputs.retain(|output| output.tag >= keep);
        self.discards.retain(|&tag| tag >= keep);
        self.markers.retain(|marker| marker.tag >= keep);
        self.splits.retain(|split| split.tag >= keep);
        self.latest = Some(summary);
    }

    /// Records before the first summary of a context belong to no known window and are dropped.
    fn sample(&mut self, record: Record) {
        let Some(context) = self.context else {
            return;
        };
        if self.summaries == 0 {
            return;
        }
        let tag = self.summaries;
        self.sequence += 1;
        match record {
            Record::Request { key, request } if request.context == context => {
                self.requests.insert(key, Pending { tag, request });
            }
            Record::Presented { key, presented } => {
                if let Some(pending) = self.requests.remove(&key) {
                    let request = pending.request;
                    self.outputs.push(Output {
                        tag: pending.tag,
                        key: SourceKey {
                            pid: key.pid,
                            context,
                            source: request.source,
                        },
                        generated: request.generated,
                        feedback: key.feedback,
                        requested_ns: request.requested_ns,
                        request_clock: request.clock_id,
                        presented_ns: presented.presented_ns,
                        presented_clock: presented.clock_id,
                        refresh_ns: presented.refresh_ns,
                        flags: presented.flags,
                    });
                }
            }
            Record::Ended { key, discarded } => {
                if let Some(pending) = self.requests.remove(&key) {
                    if discarded {
                        self.discards.push(pending.tag);
                    }
                }
            }
            Record::Marker(sample) if sample.key.context == context => self.markers.push(Timed {
                tag,
                key: sample.key,
                generated: sample.generated,
                order: self.sequence,
                at: sample.start_ns,
            }),
            Record::Split(sample) if sample.key.context == context => self.splits.push(Split {
                tag,
                fg_start_ns: sample.fg_start_ns,
                prepass_end_ns: sample.prepass_end_ns,
                generated_end_ns: sample.generated_end_ns,
            }),
            _ => {}
        }
    }

    fn rows(&self) -> Vec<Row> {
        let mut rows = Vec::new();
        match &self.latest {
            Some(summary) => {
                rows.push(Row::Text(format!(
                    "Game: {} · Real: {} · Generated: {}",
                    summary.game_modes, summary.real_modes, summary.generated_modes
                )));
                if let Some(on) = summary.internal_images {
                    rows.push(Row::Text(format!(
                        "Internal images: {}",
                        if on { "on" } else { "off" }
                    )));
                }
                rows.push(Row::Text(format!(
                    "Real {:.0} fps · Output {:.0} fps · Skipped {}",
                    per_second(summary.sources, summary.window_ms),
                    per_second(summary.output_attempts, summary.window_ms),
                    summary.skipped
                )));
            }
            None => {
                rows.push(Row::Needs {
                    reading: "Present mode",
                    checkbox: "Pacing log",
                });
                rows.push(Row::Needs {
                    reading: "Rates",
                    checkbox: "Pacing log",
                });
            }
        }
        let multiplier = self
            .latest
            .as_ref()
            .and_then(|summary| summary.configured_multiplier);
        let shown = self.summaries.saturating_sub(1);
        self.on_screen_rows(shown, multiplier, &mut rows);
        self.gpu_row(shown, &mut rows);
        rows
    }

    fn on_screen_rows(&self, shown: u64, multiplier: Option<u64>, rows: &mut Vec<Row>) {
        // A presented time that another output shares is ambiguous, so it is left out of the
        // placement and the refresh interval (as the Python report leaves out ambiguous pairs).
        let mut counts: HashMap<u64, u32> = HashMap::new();
        for output in &self.outputs {
            *counts.entry(output.presented_ns).or_default() += 1;
        }
        let unique = |output: &&Output| counts.get(&output.presented_ns) == Some(&1);
        let shown_outputs: Vec<&Output> = self
            .outputs
            .iter()
            .filter(|output| output.tag == shown)
            .collect();
        let unique_outputs: Vec<Timed> = self
            .outputs
            .iter()
            .filter(unique)
            .map(|output| Timed {
                tag: output.tag,
                key: output.key,
                generated: output.generated,
                order: output.feedback,
                at: output.presented_ns,
            })
            .collect();
        match placement(&unique_outputs, shown, multiplier) {
            Some(placed) => rows.push(Row::Text(format!(
                "Generated frame: {}, on screen",
                placed.text()
            ))),
            None => match placement(&self.markers, shown, multiplier) {
                Some(placed) => rows.push(Row::Text(format!(
                    "Generated frame: {}, GPU clock",
                    placed.text()
                ))),
                None => rows.push(Row::Needs {
                    reading: "Generated frame",
                    checkbox: "On-screen times",
                }),
            },
        }
        let real_outputs: Vec<&Output> = shown_outputs
            .iter()
            .copied()
            .filter(|output| !output.generated && unique(output))
            .collect();
        if let Some(ratio) = refresh_ratio(&real_outputs) {
            rows.push(Row::Text(format!(
                "Real frames: {ratio:.1} refreshes apart"
            )));
        }

        let discarded = self.discards.iter().filter(|&&tag| tag == shown).count();
        if shown_outputs.is_empty() {
            rows.push(Row::Needs {
                reading: "Present to screen",
                checkbox: "On-screen times",
            });
            // Discards are records too: with only discards, the count is the reading.
            if discarded > 0 {
                rows.push(Row::Text(format!("Discarded: {discarded}")));
            } else {
                rows.push(Row::Needs {
                    reading: "Scanout",
                    checkbox: "On-screen times",
                });
            }
            return;
        }
        for generated in [false, true] {
            let delays: Vec<f64> = shown_outputs
                .iter()
                .filter(|output| output.generated == generated)
                .filter(|output| output.request_clock == output.presented_clock)
                .map(|output| (output.presented_ns as f64 - output.requested_ns as f64) / 1e6)
                .collect();
            let reading = if generated {
                "Present to screen, generated"
            } else {
                "Present to screen, real"
            };
            match summarize(&delays) {
                Some((mean, worst)) => rows.push(Row::Text(format!(
                    "{reading}: {mean:.1} ms (worst {worst:.1})"
                ))),
                None => rows.push(Row::Needs {
                    reading,
                    checkbox: "On-screen times",
                }),
            }
        }
        let count = shown_outputs.len() as f64;
        let share = |bit: u64| {
            let set = shown_outputs
                .iter()
                .filter(|output| output.flags & bit != 0)
                .count();
            100.0 * set as f64 / count
        };
        rows.push(Row::Text(format!(
            "Direct scanout: {:.0}%",
            share(ZERO_COPY)
        )));
        rows.push(Row::Text(format!(
            "Compositor sync flag: {:.0}%",
            share(VSYNC)
        )));
        rows.push(Row::Text(format!("Discarded: {discarded}")));
    }

    fn gpu_row(&self, shown: u64, rows: &mut Vec<Row>) {
        let mut cost = Vec::new();
        let mut prepass = Vec::new();
        for split in self.splits.iter().filter(|split| split.tag == shown) {
            if split.fg_start_ns == 0 {
                continue;
            }
            if split.generated_end_ns >= split.fg_start_ns {
                cost.push((split.generated_end_ns - split.fg_start_ns) as f64 / 1e6);
            }
            if split.prepass_end_ns >= split.fg_start_ns {
                prepass.push((split.prepass_end_ns - split.fg_start_ns) as f64 / 1e6);
            }
        }
        match summarize(&cost) {
            Some((mean, worst)) => {
                let text = match summarize(&prepass) {
                    Some((prepass_mean, _)) => format!(
                        "Generated frame GPU: {mean:.1} ms (worst {worst:.1}), prepass {prepass_mean:.1}"
                    ),
                    None => format!("Generated frame GPU: {mean:.1} ms (worst {worst:.1})"),
                };
                rows.push(Row::Text(text));
            }
            None => rows.push(Row::Needs {
                reading: "Generated frame GPU",
                checkbox: "GPU cost",
            }),
        }
    }
}

fn per_second(count: u64, window_ms: f64) -> f64 {
    count as f64 * 1000.0 / window_ms
}

/// The mean and the largest value of a list of milliseconds, or `None` when it is empty.
fn summarize(values: &[f64]) -> Option<(f64, f64)> {
    if values.is_empty() {
        return None;
    }
    let mean = values.iter().sum::<f64>() / values.len() as f64;
    let worst = values.iter().copied().fold(f64::MIN, f64::max);
    Some((mean, worst))
}

/// Mean and worst absolute placement error of the generated outputs of one window, in ms.
struct Placement {
    mean_ms: f64,
    worst_ms: f64,
}

impl Placement {
    fn text(&self) -> String {
        format!("{} (worst {:.2})", signed_ms(self.mean_ms), self.worst_ms)
    }
}

/// `+0.21 ms late` or `0.21 ms early`; a value that rounds to zero is `0.00 ms`.
fn signed_ms(mean_ms: f64) -> String {
    let rounded = (mean_ms * 100.0).round() / 100.0;
    if rounded > 0.0 {
        format!("+{rounded:.2} ms late")
    } else if rounded < 0.0 {
        format!("{:.2} ms early", -rounded)
    } else {
        "0.00 ms".to_owned()
    }
}

/// Placement of the generated outputs of window `shown` between their reals. With `k = m - 1`
/// generated outputs between two reals of consecutive sources, the i-th (1-based, in request
/// order) has the target `before + i/(k+1) * (after - before)`; for m = 2 that is the midpoint.
/// A group with fewer or more outputs than `k` (a skip) is left out, since its slots are unknown.
fn placement(items: &[Timed], shown: u64, multiplier: Option<u64>) -> Option<Placement> {
    let mut reals: HashMap<SourceKey, u64> = HashMap::new();
    for item in items.iter().filter(|item| !item.generated) {
        reals.insert(item.key, item.at);
    }
    let mut groups: BTreeMap<SourceKey, Vec<Timed>> = BTreeMap::new();
    for item in items
        .iter()
        .filter(|item| item.generated && item.tag == shown)
    {
        groups.entry(item.key).or_default().push(*item);
    }
    let mut errors = Vec::new();
    for (key, mut members) in groups {
        let Some(previous) = key.source.checked_sub(1) else {
            continue;
        };
        let before_key = SourceKey {
            source: previous,
            ..key
        };
        let (Some(&before), Some(&after)) = (reals.get(&before_key), reals.get(&key)) else {
            continue;
        };
        if after <= before {
            continue;
        }
        let slots = match multiplier {
            Some(multiplier) => multiplier.saturating_sub(1),
            None => members.len() as u64,
        };
        if slots == 0 || members.len() as u64 != slots {
            continue;
        }
        members.sort_by_key(|member| member.order);
        let span = (after - before) as f64;
        for (index, member) in members.iter().enumerate() {
            let fraction = (index as f64 + 1.0) / (slots as f64 + 1.0);
            let target = before as f64 + fraction * span;
            errors.push((member.at as f64 - target) / 1e6);
        }
    }
    if errors.is_empty() {
        return None;
    }
    let count = errors.len() as f64;
    Some(Placement {
        mean_ms: errors.iter().sum::<f64>() / count,
        worst_ms: errors
            .iter()
            .fold(0.0, |worst, error| worst.max(error.abs())),
    })
}

/// The mean on-screen interval between consecutive reals (by source), in refresh intervals, using
/// the refresh of the presented records. `None` when no pair or no refresh is known.
fn refresh_ratio(reals: &[&Output]) -> Option<f64> {
    let mut at: HashMap<u64, u64> = HashMap::new();
    let mut refreshes = Vec::new();
    for output in reals {
        at.insert(output.key.source, output.presented_ns);
        if output.refresh_ns > 0 {
            refreshes.push(output.refresh_ns);
        }
    }
    let mut gaps = Vec::new();
    for (&source, &start) in &at {
        if let Some(&end) = at.get(&(source + 1)) {
            if end > start {
                gaps.push((end - start) as f64);
            }
        }
    }
    if gaps.is_empty() || refreshes.is_empty() {
        return None;
    }
    refreshes.sort_unstable();
    let refresh = refreshes[refreshes.len() / 2] as f64;
    Some(gaps.iter().sum::<f64>() / gaps.len() as f64 / refresh)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;
    use std::process;
    use std::time::UNIX_EPOCH;

    /// The layer's own fixture lines, which the golden record test also reads.
    const GOLDEN: &str = include_str!("../../fgvk-render/tests/fixtures/pacing-record-golden.txt");

    /// The first golden line for `operation`, with the placeholder pid replaced by a number.
    fn golden(operation: &str) -> String {
        let needle = format!("operation={operation} ");
        GOLDEN
            .lines()
            .find(|line| line.contains(&needle))
            .expect("golden line")
            .replace("pid=N", "pid=10")
    }

    /// A scratch directory under the test temporary directory, removed on drop.
    struct Scratch(PathBuf);

    impl Scratch {
        fn new(tag: &str) -> Self {
            let nanos = SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .map(|elapsed| elapsed.as_nanos())
                .unwrap_or(0);
            let path =
                std::env::temp_dir().join(format!("fgvk-live-{tag}-{}-{nanos}", process::id()));
            fs::create_dir_all(&path).expect("create scratch directory");
            Self(path)
        }

        fn path(&self) -> &Path {
            &self.0
        }
    }

    impl Drop for Scratch {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }

    fn own() -> u32 {
        own_uid().expect("own uid")
    }

    fn log_name(uid: u32) -> String {
        format!("{FILE_PREFIX}{uid}-4242-1.log")
    }

    fn write_lines(path: &Path, lines: &[String]) {
        let mut text = lines.join("\n");
        text.push('\n');
        fs::write(path, text).expect("write log");
    }

    /// A time one second after the file's modification, so the file is fresh.
    fn fresh_time(path: &Path) -> SystemTime {
        fs::metadata(path)
            .expect("metadata")
            .modified()
            .expect("mtime")
            + Duration::from_secs(1)
    }

    fn texts(view: &View) -> Vec<String> {
        match view {
            View::NoGame => vec!["NoGame".to_owned()],
            View::Readings(rows) => rows.iter().map(ToString::to_string).collect(),
        }
    }

    fn summary_line(context: u64, sources: u64, multiplier: u64, modes: &str) -> String {
        format!(
            "framegen-vulkan: present diagnostics: operation=pacing-profile version=2 context={context} pid=10 sample_end_monotonic_ms=1000 window_ms=1000 present_mode=1 sources={sources} output_attempts={} wsi=wayland generated_skip_readiness=0 generated_skip_expedite=0 generated_skip_give_up=0 generated_skip_acquire=0 generated_skip_other=0 internal_images=1 configured_multiplier={multiplier} application_present_modes={modes} real_present_modes=immediate:2 generated_present_modes=immediate:2 swapchain_present_mode=1",
            sources * 2
        )
    }

    fn request(feedback: u64, source: u64, generated: u8, requested_ns: u64) -> String {
        format!(
            "framegen-vulkan: present diagnostics: operation=pacing-profile-wayland version=2 pid=10 surface_token=12288 event=request feedback={feedback} context=17 source={source} image_index=0 generated={generated} requested_ns={requested_ns} clock_id=1 presentation_now_ns={requested_ns} calibration_span_ns=10"
        )
    }

    fn presented(feedback: u64, presented_ns: u64, flags: u64) -> String {
        format!(
            "framegen-vulkan: present diagnostics: operation=pacing-profile-wayland version=2 pid=10 surface_token=12288 event=presented feedback={feedback} received_ns={presented_ns} clock_id=1 presented_ns={presented_ns} sequence=0 refresh_ns=5000000 flags={flags} output_id=26"
        )
    }

    fn discarded(feedback: u64) -> String {
        format!(
            "framegen-vulkan: present diagnostics: operation=pacing-profile-wayland version=2 pid=10 surface_token=12288 event=discarded feedback={feedback} received_ns=1"
        )
    }

    fn marker(source: u64, generated: u8, start_ns: u64) -> String {
        format!(
            "framegen-vulkan: present diagnostics: operation=pacing-profile-present-marker-gpu version=1 pid=10 context=17 source={source} feedback=2 generated={generated} available=1 marker_start_ns={start_ns} marker_end_ns={start_ns}"
        )
    }

    fn split(source: u64, fg_start_ns: u64, prepass_end_ns: u64, generated_end_ns: u64) -> String {
        format!(
            "framegen-vulkan: present diagnostics: operation=pacing-profile-gpu-split version=1 pid=10 context=17 source={source} backend_frame=0 game_done_ns=1 source_copy_end_ns=2 fg_start_ns={fg_start_ns} prepass_end_ns={prepass_end_ns} generated_end_ns={generated_end_ns} batch_end_ns=9"
        )
    }

    /// Two summaries around one window: the shown window is the records between them.
    fn window(records: &[String], multiplier: u64) -> Vec<String> {
        let mut lines = vec![summary_line(17, 2, multiplier, "immediate:2")];
        lines.extend(records.iter().cloned());
        lines.push(summary_line(17, 2, multiplier, "immediate:2"));
        lines
    }

    fn follow(directory: &Path, uid: u32) -> Follower {
        Follower::new(directory.to_path_buf(), Some(uid))
    }

    fn poll_path(follower: &mut Follower, path: &Path) {
        follower.poll(fresh_time(path));
    }

    fn assert_row(texts: &[String], expected: &str) {
        assert!(
            texts.iter().any(|text| text == expected),
            "missing {expected:?} in {texts:#?}"
        );
    }

    #[test]
    fn parses_the_golden_summary_and_counts_its_skips() {
        let Some(Record::Summary(summary)) = parse_line(&golden("pacing-profile")) else {
            panic!("golden summary parses");
        };
        assert_eq!(summary.context, 17);
        assert_eq!(summary.window_ms, 1000.0);
        assert_eq!(summary.sources, 2);
        assert_eq!(summary.output_attempts, 2);
        assert_eq!(summary.skipped, 2, "readiness and acquire skips");
        assert_eq!(summary.internal_images, Some(true));
        assert_eq!(summary.configured_multiplier, Some(2));
        assert_eq!(summary.game_modes, "none");
        assert_eq!(summary.generated_modes, "none");
    }

    #[test]
    fn parses_the_golden_marker_and_gpu_split() {
        let Some(Record::Marker(marker)) = parse_line(&golden("pacing-profile-present-marker-gpu"))
        else {
            panic!("golden marker parses");
        };
        assert_eq!(
            marker.key,
            SourceKey {
                pid: 10,
                context: 17,
                source: 4
            }
        );
        assert!(marker.generated);
        assert_eq!(marker.start_ns, 10_008_000);

        let Some(Record::Split(split)) = parse_line(&golden("pacing-profile-gpu-split")) else {
            panic!("golden split parses");
        };
        assert_eq!(split.key.source, 1);
        assert_eq!(split.fg_start_ns, 3000);
        assert_eq!(split.prepass_end_ns, 4000);
        assert_eq!(split.generated_end_ns, 5000);
    }

    #[test]
    fn parses_wayland_request_presented_and_discarded_lines() {
        let Some(Record::Request { key, request }) = parse_line(&request(7, 3, 1, 500)) else {
            panic!("request parses");
        };
        assert_eq!(
            key,
            WaylandKey {
                pid: 10,
                surface: 12288,
                feedback: 7
            }
        );
        assert_eq!(request.context, 17);
        assert_eq!(request.source, 3);
        assert!(request.generated);
        assert_eq!(request.requested_ns, 500);
        assert_eq!(request.clock_id, 1);

        let Some(Record::Presented { presented, .. }) = parse_line(&presented(7, 900, 15)) else {
            panic!("presented parses");
        };
        assert_eq!(presented.presented_ns, 900);
        assert_eq!(presented.refresh_ns, 5_000_000);
        assert_eq!(presented.flags, 15);

        assert_eq!(
            parse_line(&discarded(8)),
            Some(Record::Ended {
                key: WaylandKey {
                    pid: 10,
                    surface: 12288,
                    feedback: 8
                },
                discarded: true,
            })
        );
    }

    #[test]
    fn skips_other_operations_and_unusable_records() {
        assert!(parse_line(&golden("pacing-profile-output")).is_none());
        assert!(parse_line(&golden("pacing-profile-skip")).is_none());
        assert!(parse_line("framegen-vulkan: present diagnostics: operation=pacing-profile-wayland version=2 pid=10 surface_token=1 event=unavailable feedback=1 context=2 source=1 image_index=0 generated=1 requested_ns=3 clock_id=1 reason=3").is_none());
        assert!(
            parse_line(&presented(9, 0, 1)).is_none(),
            "zero on-screen time is not a result"
        );
        assert!(parse_line("no diagnostics here").is_none());
    }

    #[test]
    fn midpoint_error_has_a_known_answer_at_multiplier_two() {
        let dir = Scratch::new("midpoint");
        let name = log_name(own());
        let path = dir.path().join(&name);
        // Reals on screen at 10 and 20 ms; the generated frame at 16 ms is 1 ms after the midpoint 15.
        let records = [
            request(1, 1, 0, 9_000_000),
            presented(1, 10_000_000, 15),
            request(2, 2, 1, 15_500_000),
            presented(2, 16_000_000, 15),
            request(3, 2, 0, 19_000_000),
            presented(3, 20_000_000, 14),
        ];
        write_lines(&path, &window(&records, 2));
        let mut follower = follow(dir.path(), own());
        poll_path(&mut follower, &path);
        let texts = texts(&follower.view());
        assert_row(
            &texts,
            "Generated frame: +1.00 ms late (worst 1.00), on screen",
        );
        assert_row(&texts, "Real frames: 2.0 refreshes apart");
        assert_row(&texts, "Present to screen, real: 1.0 ms (worst 1.0)");
        assert_row(&texts, "Present to screen, generated: 0.5 ms (worst 0.5)");
    }

    #[test]
    fn multiplier_three_uses_evenly_spaced_targets() {
        let dir = Scratch::new("multiplier-three");
        let name = log_name(own());
        let path = dir.path().join(&name);
        // Reals at 100 and 130 ms; two generated frames between them with targets 110 and 120 ms.
        let records = [
            request(1, 1, 0, 99_000_000),
            presented(1, 100_000_000, 15),
            request(10, 2, 1, 110_000_000),
            presented(10, 111_000_000, 15),
            request(11, 2, 1, 116_000_000),
            presented(11, 117_000_000, 15),
            request(12, 2, 0, 129_000_000),
            presented(12, 130_000_000, 15),
        ];
        write_lines(&path, &window(&records, 3));
        let mut follower = follow(dir.path(), own());
        poll_path(&mut follower, &path);
        assert_row(
            &texts(&follower.view()),
            "Generated frame: 1.00 ms early (worst 3.00), on screen",
        );
    }

    #[test]
    fn flag_shares_and_discards_come_from_presented_records() {
        let dir = Scratch::new("flags");
        let name = log_name(own());
        let path = dir.path().join(&name);
        // Flags 15 and 14 both carry ZERO_COPY (0x8); only 15 carries VSYNC (0x1).
        let records = [
            request(1, 1, 0, 9_000_000),
            presented(1, 10_000_000, 15),
            request(2, 2, 0, 19_000_000),
            presented(2, 20_000_000, 14),
            request(3, 3, 0, 29_000_000),
            discarded(3),
        ];
        write_lines(&path, &window(&records, 2));
        let mut follower = follow(dir.path(), own());
        poll_path(&mut follower, &path);
        let texts = texts(&follower.view());
        assert_row(&texts, "Direct scanout: 100%");
        assert_row(&texts, "Compositor sync flag: 50%");
        assert_row(&texts, "Discarded: 1");
        assert!(
            texts.iter().all(|text| !text.contains("tearing")),
            "the flag never proves tearing: {texts:#?}"
        );
    }

    #[test]
    fn discards_alone_are_counted_without_a_scanout_reading() {
        let dir = Scratch::new("discards-only");
        let name = log_name(own());
        let path = dir.path().join(&name);
        let records = [request(4, 4, 0, 1_000_000), discarded(4)];
        write_lines(&path, &window(&records, 2));
        let mut follower = follow(dir.path(), own());
        poll_path(&mut follower, &path);
        let texts = texts(&follower.view());
        assert_row(&texts, "Present to screen: needs \"On-screen times\"");
        assert_row(&texts, "Discarded: 1");
        assert!(texts.iter().all(|text| !text.starts_with("Scanout")));
    }

    #[test]
    fn marker_placement_is_labelled_as_gpu_clock() {
        let dir = Scratch::new("markers");
        let name = log_name(own());
        let path = dir.path().join(&name);
        let records = [
            marker(1, 0, 10_000_000),
            marker(2, 1, 16_000_000),
            marker(2, 0, 20_000_000),
        ];
        write_lines(&path, &window(&records, 2));
        let mut follower = follow(dir.path(), own());
        poll_path(&mut follower, &path);
        assert_row(
            &texts(&follower.view()),
            "Generated frame: +1.00 ms late (worst 1.00), GPU clock",
        );
    }

    #[test]
    fn gpu_cost_is_generated_end_minus_fg_start() {
        let dir = Scratch::new("gpu");
        let name = log_name(own());
        let path = dir.path().join(&name);
        write_lines(
            &path,
            &window(&[split(2, 10_000_000, 11_500_000, 12_500_000)], 2),
        );
        let mut follower = follow(dir.path(), own());
        poll_path(&mut follower, &path);
        assert_row(
            &texts(&follower.view()),
            "Generated frame GPU: 2.5 ms (worst 2.5), prepass 1.5",
        );
    }

    #[test]
    fn missing_records_show_the_checkbox_that_writes_them() {
        let dir = Scratch::new("needs");
        let name = log_name(own());
        let path = dir.path().join(&name);
        write_lines(&path, &window(&[], 2));
        let mut follower = follow(dir.path(), own());
        poll_path(&mut follower, &path);
        let texts = texts(&follower.view());
        assert_row(&texts, "Generated frame: needs \"On-screen times\"");
        assert_row(&texts, "Present to screen: needs \"On-screen times\"");
        assert_row(&texts, "Scanout: needs \"On-screen times\"");
        assert_row(&texts, "Generated frame GPU: needs \"GPU cost\"");
        assert!(texts.iter().all(|text| !text.contains("Real frames")));
    }

    #[test]
    fn a_log_with_no_summary_needs_the_pacing_log() {
        let dir = Scratch::new("no-summary");
        let name = log_name(own());
        let path = dir.path().join(&name);
        write_lines(&path, &[request(1, 1, 0, 9_000_000)]);
        let mut follower = follow(dir.path(), own());
        poll_path(&mut follower, &path);
        let texts = texts(&follower.view());
        assert_row(&texts, "Present mode: needs \"Pacing log\"");
        assert_row(&texts, "Rates: needs \"Pacing log\"");
    }

    #[test]
    fn summary_rates_and_modes_come_from_the_newest_summary() {
        let dir = Scratch::new("rates");
        let name = log_name(own());
        let path = dir.path().join(&name);
        write_lines(
            &path,
            &[
                summary_line(17, 75, 2, "immediate:75"),
                summary_line(17, 75, 2, "immediate:75,fifo:1"),
            ],
        );
        let mut follower = follow(dir.path(), own());
        poll_path(&mut follower, &path);
        let texts = texts(&follower.view());
        assert_row(
            &texts,
            "Game: immediate+fifo · Real: immediate · Generated: immediate",
        );
        assert_row(&texts, "Internal images: on");
        assert_row(&texts, "Real 75 fps · Output 150 fps · Skipped 0");
    }

    #[test]
    fn partial_last_line_waits_for_its_newline() {
        let dir = Scratch::new("partial");
        let name = log_name(own());
        let path = dir.path().join(&name);
        let line = summary_line(17, 40, 2, "immediate:40");
        let (head, tail) = line.split_at(line.len() / 2);
        fs::write(&path, head).expect("write head");
        let mut follower = follow(dir.path(), own());
        poll_path(&mut follower, &path);
        assert_row(
            &texts(&follower.view()),
            "Present mode: needs \"Pacing log\"",
        );

        let mut file = fs::OpenOptions::new()
            .append(true)
            .open(&path)
            .expect("append");
        file.write_all(tail.as_bytes()).expect("write tail");
        file.write_all(b"\n").expect("write newline");
        drop(file);
        poll_path(&mut follower, &path);
        assert_row(
            &texts(&follower.view()),
            "Real 40 fps · Output 80 fps · Skipped 0",
        );
    }

    #[test]
    fn a_summary_crossing_the_read_boundary_survives_an_oversized_line() {
        let dir = Scratch::new("chunk-boundary");
        let path = dir.path().join(log_name(own()));
        let prefix = "x".repeat(MAX_LINE + READ_CHUNK - 9);
        write_lines(&path, &[prefix, summary_line(17, 40, 2, "immediate:40")]);
        let mut follower = follow(dir.path(), own());
        poll_path(&mut follower, &path);
        assert_row(
            &texts(&follower.view()),
            "Real 40 fps · Output 80 fps · Skipped 0",
        );
    }

    #[test]
    fn a_shrunken_file_restarts_at_zero() {
        let dir = Scratch::new("wrap");
        let name = log_name(own());
        let path = dir.path().join(&name);
        write_lines(
            &path,
            &[
                summary_line(17, 2, 2, "immediate:2"),
                summary_line(17, 5, 2, "immediate:5"),
                "x".repeat(200),
            ],
        );
        let mut follower = follow(dir.path(), own());
        poll_path(&mut follower, &path);
        assert_row(
            &texts(&follower.view()),
            "Real 5 fps · Output 10 fps · Skipped 0",
        );

        // The layer's wrap truncates the file and writes from the beginning again.
        write_lines(&path, &[summary_line(17, 9, 2, "immediate:9")]);
        poll_path(&mut follower, &path);
        assert_row(
            &texts(&follower.view()),
            "Real 9 fps · Output 18 fps · Skipped 0",
        );
    }

    #[test]
    fn a_large_append_skips_to_the_tail_and_drops_the_cut_line() {
        let dir = Scratch::new("skip");
        let name = log_name(own());
        let path = dir.path().join(&name);
        write_lines(&path, &[summary_line(17, 2, 2, "immediate:2")]);
        let mut follower = follow(dir.path(), own());
        poll_path(&mut follower, &path);

        let filler = "f".repeat(1023);
        let mut file = fs::OpenOptions::new()
            .append(true)
            .open(&path)
            .expect("append");
        for _ in 0..(9 << 10) {
            file.write_all(filler.as_bytes()).expect("filler");
            file.write_all(b"\n").expect("newline");
        }
        file.write_all(summary_line(17, 7, 2, "immediate:7").as_bytes())
            .expect("summary");
        file.write_all(b"\n").expect("newline");
        drop(file);
        poll_path(&mut follower, &path);
        assert_row(
            &texts(&follower.view()),
            "Real 7 fps · Output 14 fps · Skipped 0",
        );
    }

    #[test]
    fn a_fresh_log_is_read_from_its_end_on_first_attach() {
        let dir = Scratch::new("attach");
        let name = log_name(own());
        let path = dir.path().join(&name);
        let filler = "f".repeat(1023);
        let mut text = String::new();
        for _ in 0..(6 << 10) {
            text.push_str(&filler);
            text.push('\n');
        }
        text.push_str(&summary_line(17, 3, 2, "immediate:3"));
        text.push('\n');
        fs::write(&path, text).expect("write large log");
        let mut follower = follow(dir.path(), own());
        poll_path(&mut follower, &path);
        assert_row(
            &texts(&follower.view()),
            "Real 3 fps · Output 6 fps · Skipped 0",
        );
    }

    #[test]
    fn a_stale_log_gives_no_game() {
        let dir = Scratch::new("stale");
        let name = log_name(own());
        let path = dir.path().join(&name);
        write_lines(&path, &window(&[], 2));
        let modified = fs::metadata(&path)
            .expect("metadata")
            .modified()
            .expect("mtime");
        let mut follower = follow(dir.path(), own());
        follower.poll(modified + Duration::from_secs(10));
        assert_eq!(follower.view(), View::NoGame);
        follower.poll(modified + Duration::from_secs(2));
        assert_ne!(follower.view(), View::NoGame, "two seconds old is fresh");
    }

    #[test]
    fn nothing_is_shown_before_the_first_poll() {
        let dir = Scratch::new("first");
        let follower = follow(dir.path(), own());
        assert_eq!(follower.view(), View::NoGame);
    }

    #[test]
    fn a_symlink_is_ignored_even_when_its_target_is_fresh() {
        let dir = Scratch::new("symlink");
        let target = dir.path().join("target.txt");
        write_lines(&target, &window(&[], 2));
        let link = dir.path().join(log_name(own()));
        std::os::unix::fs::symlink(&target, &link).expect("symlink");
        let mut follower = follow(dir.path(), own());
        follower.poll(fresh_time(&target));
        assert_eq!(follower.view(), View::NoGame);
    }

    #[test]
    fn a_log_named_for_another_uid_is_ignored() {
        let dir = Scratch::new("other-uid");
        let path = dir.path().join(log_name(own().wrapping_add(1)));
        write_lines(&path, &window(&[], 2));
        let mut follower = follow(dir.path(), own());
        follower.poll(fresh_time(&path));
        assert_eq!(follower.view(), View::NoGame);
    }

    #[test]
    fn a_log_owned_by_another_uid_is_ignored() {
        // Only the file owner can be checked without root: follow a uid this user does not own.
        let dir = Scratch::new("owner");
        let other = own().wrapping_add(1);
        let path = dir.path().join(log_name(other));
        write_lines(&path, &window(&[], 2));
        let mut follower = Follower::new(dir.path().to_path_buf(), Some(other));
        follower.poll(fresh_time(&path));
        assert_eq!(follower.view(), View::NoGame);
    }

    #[test]
    fn a_missing_uid_follows_nothing() {
        let dir = Scratch::new("no-uid");
        let path = dir.path().join(log_name(own()));
        write_lines(&path, &window(&[], 2));
        let mut follower = Follower::new(dir.path().to_path_buf(), None);
        follower.poll(fresh_time(&path));
        assert_eq!(follower.view(), View::NoGame);
    }

    #[test]
    fn own_uid_is_the_owner_of_files_this_process_creates() {
        let dir = Scratch::new("uid");
        let path = dir.path().join("probe");
        fs::write(&path, "x").expect("probe");
        let owner = fs::metadata(&path).expect("metadata").uid();
        assert_eq!(own_uid(), Some(owner));
    }

    #[test]
    fn log_directory_follows_the_layer_rules() {
        let with =
            |value: &'static str| move |key: &str| (key == "TMPDIR").then(|| OsString::from(value));
        assert_eq!(
            log_directory(with("/run/user/1000")),
            PathBuf::from("/run/user/1000")
        );
        assert_eq!(log_directory(with("relative")), PathBuf::from("/tmp"));
        assert_eq!(log_directory(with("")), PathBuf::from("/tmp"));
        assert_eq!(log_directory(|_| None), PathBuf::from("/tmp"));
    }
}
