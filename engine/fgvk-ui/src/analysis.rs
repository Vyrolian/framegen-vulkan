//! Offline capture analysis. The report reads a bounded snapshot on a background thread.

use crate::live::{self, field, number, MAX_LINE, OPERATION, SKIP_KEYS};
use std::collections::{BTreeMap, HashMap};
use std::fs;
use std::io::{self, BufRead, BufReader, Read};
use std::path::Path;

const MS: u64 = 1_000_000;
const SECOND: u64 = 1000 * MS;
const MONOTONIC: u64 = 1;
const MARKER_TOLERANCE: u64 = 2 * MS;
const LEGACY_APP_LIMIT: u64 = 250 * MS;

type Source = (u64, u64, u64); // PID, swapchain context, source
type Feedback = (u64, u64, u64); // PID, surface, feedback
type OutputFeedback = (u64, u64, u64); // PID, context, feedback

#[derive(Clone, Copy)]
struct Window {
    stream: (u64, u64),
    end: u64,
    duration_ms: f64,
    sources: u64,
    skips: u64,
    errors: u64,
    multiplier: u8,
    cap: u64, // Thousandths of an FPS, so formatting changes do not split runs.
    configured_cap: u64,
    mode: u8,
    hold: bool,
    correction: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Output {
    key: Source,
    ordinal: u64,
    at: u64,
    finished: u64,
    phase: u8,
    success: bool,
}

#[derive(Clone, Copy, PartialEq, Eq)]
struct Request {
    key: Source,
    at: u64,
    generated: bool,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Outcome {
    Shown {
        at: u64,
        output: u64,
        sequence: u64,
        flags: u64,
    },
    Discarded,
}

#[derive(Default)]
enum Observation<T> {
    #[default]
    Missing,
    Once(T),
    Conflict,
}

impl<T: Copy + Eq> Observation<T> {
    fn add(&mut self, value: T) {
        *self = match *self {
            Self::Missing => Self::Once(value),
            Self::Once(previous) if previous == value => Self::Once(value),
            _ => Self::Conflict,
        };
    }

    fn value(&self) -> Option<T> {
        match *self {
            Self::Once(value) => Some(value),
            _ => None,
        }
    }
}

#[derive(Default)]
struct Marker {
    simulation: Observation<u64>,
    present: Observation<u64>,
}

#[derive(Default)]
struct Capture {
    windows: Vec<Window>,
    app: HashMap<Source, Observation<u64>>,
    legacy_app: HashMap<(u64, u64), Vec<u64>>,
    markers: HashMap<(u64, u64), Marker>,
    outputs: HashMap<OutputFeedback, Observation<Output>>,
    requests: HashMap<Feedback, Observation<Request>>,
    outcomes: HashMap<Feedback, Observation<Outcome>>,
    gpu: HashMap<Source, Observation<(u64, u64)>>,
    game: String,
    incomplete: bool,
}

impl Capture {
    fn read_record(&mut self, line: &str) -> Option<()> {
        let (_, body) = line.split_once(OPERATION)?;
        let (operation, fields) = body.split_once(' ')?;
        if matches!(
            operation,
            "pacing-profile-log-wrap"
                | "pacing-profile-capture-gap"
                | "pacing-profile-capture-limit"
                | "pacing-profile-wayland-gap"
        ) {
            self.incomplete = true;
            return Some(());
        }
        let pid = number(fields, "pid")?;
        let version = field(fields, "version");
        let stream = || Some((pid, number::<u64>(fields, "context")?));
        let source = || Some((pid, number(fields, "context")?, number(fields, "source")?));
        match (operation, version) {
            ("process-identity", _) => {
                if let Some(game) = field(fields, "wine_executable")
                    .filter(|value| !matches!(*value, "-" | "none" | "unknown" | ""))
                    .or_else(|| field(fields, "executable"))
                {
                    self.game = game.to_owned();
                }
            }
            ("pacing-profile", Some("2")) => {
                let duration_ms: f64 = number(fields, "window_ms")?;
                if !duration_ms.is_finite() || duration_ms <= 0.0 {
                    return None;
                }
                let multiplier = if field(fields, "frame_generation_enabled") == Some("0") {
                    1
                } else {
                    number(fields, "configured_multiplier")?
                };
                self.windows.push(Window {
                    stream: stream()?,
                    end: number::<u64>(fields, "sample_end_monotonic_ms")?.checked_mul(MS)?,
                    duration_ms,
                    sources: number(fields, "sources")?,
                    skips: SKIP_KEYS
                        .iter()
                        .filter_map(|key| number::<u64>(fields, key))
                        .fold(0, u64::saturating_add),
                    errors: number(fields, "output_errors").unwrap_or(0),
                    multiplier,
                    cap: (number::<f64>(fields, "effective_base_cap_fps").unwrap_or(0.0) * 1000.0)
                        .round() as u64,
                    configured_cap: number(fields, "configured_base_cap_fps").unwrap_or(0),
                    mode: number(fields, "present_mode").unwrap_or(u8::MAX),
                    hold: field(fields, "batch_hold") == Some("1"),
                    correction: field(fields, "real_offset_auto") == Some("1"),
                });
            }
            ("pacing-profile-app-frame", Some("1")) => {
                let source_id = number(fields, "source")?;
                let at = number::<u64>(fields, "enter_ns")?;
                if source_id == 0 || at == 0 {
                    return None;
                }
                if let Some(context) = number(fields, "context").filter(|&context| context != 0) {
                    self.app
                        .entry((pid, context, source_id))
                        .or_default()
                        .add(at);
                } else {
                    self.legacy_app
                        .entry((pid, source_id))
                        .or_default()
                        .push(at);
                }
            }
            ("pacing-profile-reflex-marker", Some("1")) => {
                let kind = field(fields, "marker")?;
                if !matches!(kind, "0" | "4") {
                    return None;
                }
                let id = number(fields, "present_id")?;
                let at = number::<u64>(fields, "ns")?;
                if id == 0 || at == 0 {
                    return None;
                }
                let marker = self.markers.entry((pid, id)).or_default();
                if kind == "0" {
                    marker.simulation.add(at);
                } else {
                    marker.present.add(at);
                }
            }
            ("pacing-profile-output", Some("1")) => {
                let key = source()?;
                let feedback = number(fields, "feedback")?;
                if feedback == 0 {
                    return None;
                }
                let phase = if field(fields, "generated") == Some("1") {
                    // The renderer reports fractions of a source interval for 2× through 5×.
                    (number::<f64>(fields, "phase")? * 60.0).round() as u8
                } else {
                    0
                };
                if field(fields, "generated") == Some("1") && !(1..60).contains(&phase) {
                    return None;
                }
                let at = number(fields, "started_ns")?;
                if at == 0 {
                    return None;
                }
                self.outputs
                    .entry((pid, key.1, feedback))
                    .or_default()
                    .add(Output {
                        key,
                        ordinal: number(fields, "ordinal")?,
                        at,
                        phase,
                        finished: number(fields, "finished_ns")?,
                        success: matches!(number(fields, "result")?, 0i32 | 1000001003),
                    });
            }
            ("pacing-profile-wayland", Some("2")) => {
                let event = field(fields, "event")?;
                let key = (
                    pid,
                    number(fields, "surface_token")?,
                    number(fields, "feedback")?,
                );
                match event {
                    "request" if number::<u64>(fields, "clock_id")? == MONOTONIC => {
                        self.requests.entry(key).or_default().add(Request {
                            key: source()?,
                            at: number(fields, "requested_ns")?,
                            generated: number::<u8>(fields, "generated")? == 1,
                        });
                    }
                    "presented" if number::<u64>(fields, "clock_id")? == MONOTONIC => {
                        self.outcomes.entry(key).or_default().add(Outcome::Shown {
                            at: number(fields, "presented_ns")?,
                            output: number(fields, "output_id")?,
                            sequence: number(fields, "sequence")?,
                            flags: number(fields, "flags")?,
                        });
                    }
                    "discarded" => self
                        .outcomes
                        .entry(key)
                        .or_default()
                        .add(Outcome::Discarded),
                    _ => {}
                }
            }
            ("pacing-profile-gpu-split", Some("1")) => {
                self.gpu.entry(source()?).or_default().add((
                    number(fields, "fg_start_ns")?,
                    number(fields, "generated_end_ns")?,
                ));
            }
            _ => {}
        }
        Some(())
    }
}

fn subtract(end: u64, start: u64) -> Option<u64> {
    (start != 0 && end > start).then(|| end - start)
}

fn percentiles(values: &mut [u64]) -> String {
    if values.is_empty() {
        return "Unavailable".to_owned();
    }
    values.sort_unstable();
    let at = |percent: usize| values[(values.len() * percent).div_ceil(100) - 1] as f64 / MS as f64;
    format!(
        "{:.2} / {:.2} / {:.2} ms  ({} frames)",
        at(50),
        at(95),
        at(99),
        values.len()
    )
}

#[derive(Default)]
struct Timing {
    sim_to_app: Vec<u64>,
    app_to_real: Vec<u64>,
    sim_to_real: Vec<u64>,
    sim_to_generated: BTreeMap<u8, Vec<u64>>,
    display_gap: Vec<u64>,
    gpu_cost: Vec<u64>,
    discarded: u64,
    shown: u64,
    vsync: u64,
    zero_copy: u64,
}

struct Segment {
    first: Window,
    last: Window,
    sources: u64,
    duration_ms: f64,
    skips: u64,
    errors: u64,
    timing: Timing,
}

impl Segment {
    fn include(&mut self, window: Window) {
        self.last = window;
        self.sources += window.sources;
        self.duration_ms += window.duration_ms;
        self.skips += window.skips;
        self.errors += window.errors;
    }

    fn same_settings(&self, window: Window) -> bool {
        self.first.stream == window.stream
            && self.first.multiplier == window.multiplier
            && self.first.cap == window.cap
            && self.first.configured_cap == window.configured_cap
            && self.first.mode == window.mode
            && self.first.hold == window.hold
            && self.first.correction == window.correction
            && window.end.saturating_sub(self.last.end)
                <= (window.duration_ms * MS as f64) as u64 + 250 * MS
    }

    fn start(&self) -> u64 {
        self.first
            .end
            .saturating_sub((self.first.duration_ms * MS as f64) as u64)
    }
}

struct BenchPoint {
    app: Option<u64>,
    sim: Option<u64>,
    segment: usize,
}

impl Capture {
    fn app_time(&self, key: Source, first_call: u64) -> Option<u64> {
        if let Some(exact) = self.app.get(&key) {
            return exact.value().filter(|&at| at <= first_call);
        }
        let mut candidates = self
            .legacy_app
            .get(&(key.0, key.2))?
            .iter()
            .copied()
            .filter(|&at| at <= first_call && first_call - at <= LEGACY_APP_LIMIT);
        let at = candidates.next()?;
        candidates.next().is_none().then_some(at)
    }

    fn simulation_by_app(&self) -> HashMap<(u64, u64), Observation<u64>> {
        let mut app_times: HashMap<u64, Vec<u64>> = HashMap::new();
        for (&(pid, _, _), app) in &self.app {
            app_times.entry(pid).or_default().extend(app.value());
        }
        for (&(pid, _), times) in &self.legacy_app {
            app_times.entry(pid).or_default().extend(times);
        }
        for times in app_times.values_mut() {
            times.sort_unstable();
            times.dedup();
        }
        let mut simulations: HashMap<_, Observation<u64>> = HashMap::new();
        for (&(pid, _), markers) in &self.markers {
            let (Some(sim), Some(present), Some(times)) = (
                markers.simulation.value(),
                markers.present.value(),
                app_times.get(&pid),
            ) else {
                continue;
            };
            if sim >= present {
                continue;
            }
            let begin = times.partition_point(|&at| at < present.saturating_sub(MARKER_TOLERANCE));
            let end = times.partition_point(|&at| at <= present.saturating_add(MARKER_TOLERANCE));
            if let [at] = times[begin..end] {
                if sim < at {
                    simulations.entry((pid, at)).or_default().add(sim);
                }
            }
        }
        simulations
    }

    fn report(mut self, range: Option<(f64, f64)>) -> Option<String> {
        self.windows.sort_by_key(|w| (w.stream, w.end));
        let origin = self
            .windows
            .iter()
            .map(|w| w.end.saturating_sub((w.duration_ms * MS as f64) as u64))
            .min()?;
        let last = self.windows.iter().map(|w| w.end).max()?;
        let (lo, hi) = range.map_or((origin, last), |(lo, hi)| {
            (
                origin.saturating_add((lo * SECOND as f64) as u64),
                origin.saturating_add((hi * SECOND as f64) as u64),
            )
        });
        let mut segments: Vec<Segment> = Vec::new();
        let mut segment_windows: BTreeMap<_, Vec<_>> = BTreeMap::new();
        for &window in &self.windows {
            let begin = window
                .end
                .saturating_sub((window.duration_ms * MS as f64) as u64);
            if begin < lo || window.end > hi {
                continue;
            }
            if !segments.last().is_some_and(|s| s.same_settings(window)) {
                segments.push(Segment {
                    first: window,
                    last: window,
                    sources: 0,
                    duration_ms: 0.0,
                    skips: 0,
                    errors: 0,
                    timing: Timing::default(),
                });
            }
            let index = segments.len() - 1;
            segments[index].include(window);
            segment_windows
                .entry(window.stream)
                .or_default()
                .push((begin, window.end, index));
        }

        let simulations = self.simulation_by_app();
        let mut source_calls = HashMap::<_, (u64, u64)>::new();
        for output in self.outputs.values().filter_map(Observation::value) {
            let (first, phases) = source_calls.entry(output.key).or_insert((output.at, 0));
            *first = (*first).min(output.at);
            *phases |= 1 << output.phase;
        }
        let mut app_matches = HashMap::new();
        let mut app_uses = HashMap::new();
        for (&key, &(first, _)) in &source_calls {
            if let Some(app) = self.app_time(key, first) {
                app_matches.insert(key, app);
                *app_uses.entry((key.0, app)).or_insert(0usize) += 1;
            }
        }
        let mut points = HashMap::new();
        for (key, (first, phases)) in source_calls {
            let Some(windows) = segment_windows.get(&(key.0, key.1)) else {
                continue;
            };
            let app = app_matches
                .get(&key)
                .copied()
                .filter(|&at| app_uses[&(key.0, at)] == 1);
            let t = app.unwrap_or(first);
            let slot = windows.partition_point(|&(_, end, _)| end <= t);
            let Some(&(begin, _, segment)) = windows.get(slot) else {
                continue;
            };
            if t < begin {
                continue;
            }
            let multiplier = segments[segment].first.multiplier;
            if !(1..=5).contains(&multiplier) {
                continue;
            }
            let mask = (0..multiplier).fold(0u64, |mask, ordinal| {
                mask | (1 << (ordinal * (60 / multiplier)))
            });
            // The summary's settings may have changed within this second. Keep only
            // batches whose observed phases agree with it, including incomplete batches.
            if phases & !mask != 0 || (multiplier > 1 && phases & (1 << (60 / multiplier)) == 0) {
                continue;
            }
            let sim = app.and_then(|at| simulations.get(&(key.0, at))?.value());
            points.insert(key, BenchPoint { app, sim, segment });
        }

        for (key, gpu) in &self.gpu {
            if let (Some(point), Some((start, end))) = (points.get(key), gpu.value()) {
                segments[point.segment]
                    .timing
                    .gpu_cost
                    .extend(subtract(end, start));
            }
        }

        let mut presentation = Vec::new();
        let mut used_outputs: HashMap<OutputFeedback, usize> = HashMap::new();
        let mut commits = HashMap::new();
        for (&key, request) in &self.requests {
            let Some(request) = request.value() else {
                continue;
            };
            let identity = (key.0, request.key.1, key.2);
            let Some(output) = self.outputs.get(&identity).and_then(Observation::value) else {
                continue;
            };
            if request.key != output.key
                || request.generated != (output.phase != 0)
                || !(output.at..=output.finished).contains(&request.at)
            {
                continue;
            }
            *used_outputs.entry(identity).or_default() += 1;
            let Some(outcome) = self.outcomes.get(&key).and_then(Observation::value) else {
                continue;
            };
            let commit = match outcome {
                Outcome::Shown {
                    at,
                    output: screen,
                    sequence,
                    ..
                } => Some((key.0, key.1, screen, at, sequence)),
                Outcome::Discarded => None,
            };
            if let Some(commit) = commit {
                *commits.entry(commit).or_insert(0usize) += 1;
            }
            if let Some(point) = points.get(&output.key) {
                presentation.push((key, point, output, outcome, commit));
            }
        }
        presentation.retain(|(key, _, output, _, commit)| {
            used_outputs[&(key.0, output.key.1, key.2)] == 1
                && commit.is_none_or(|identity| commits[&identity] == 1)
        });
        presentation.sort_unstable_by_key(|(key, point, output, _, _)| {
            (point.segment, key.1, output.ordinal)
        });
        let mut prior = None;
        for (key, point, output, outcome, _) in presentation {
            let timing = &mut segments[point.segment].timing;
            let Outcome::Shown {
                at,
                output: screen,
                flags,
                ..
            } = outcome
            else {
                timing.discarded += 1;
                continue;
            };
            if !output.success || at < output.at {
                continue;
            }
            timing.shown += 1;
            timing.vsync += u64::from(flags & 1 != 0);
            timing.zero_copy += u64::from(flags & 8 != 0);
            if output.phase == 0 {
                if let Some(app) = point.app {
                    timing.app_to_real.extend(subtract(at, app));
                    if let Some(sim) = point.sim {
                        timing.sim_to_app.extend(subtract(app, sim));
                        timing.sim_to_real.extend(subtract(at, sim));
                    }
                }
            } else if let Some(latency) = point.sim.and_then(|sim| subtract(at, sim)) {
                timing
                    .sim_to_generated
                    .entry(output.phase)
                    .or_default()
                    .push(latency);
            }
            if let Some((group, ordinal, previous)) = prior {
                if group == (point.segment, key.1, screen, flags)
                    && output.ordinal.checked_sub(1) == Some(ordinal)
                {
                    timing.display_gap.extend(subtract(at, previous));
                }
            }
            prior = Some(((point.segment, key.1, screen, flags), output.ordinal, at));
        }

        segments.sort_by_key(Segment::start);
        use std::fmt::Write;
        let mut report = String::new();
        if !self.game.is_empty() {
            let _ = writeln!(report, "{}", self.game);
        }
        let _ = writeln!(
            report,
            "Capture {:.1} s · {} segments",
            (last - origin) as f64 / SECOND as f64,
            segments.len()
        );
        let _ = writeln!(report, "Timing columns show median / p95 / p99.");
        let _ = writeln!(report, "Presentation times come from Wayland. Reflex timing matches the present-start marker to a nearby application present. The report uses complete timing windows.");
        if self.incomplete {
            let _ = writeln!(report, "Capture contains a gap, wrap, or recording limit.");
        }
        if segments.is_empty() {
            let _ = writeln!(report, "No complete timing windows in this range.");
        }
        for segment in &mut segments {
            let fps = segment.sources as f64 * 1000.0 / segment.duration_ms;
            let mode = match segment.first.mode {
                0 => "Immediate",
                1 => "Mailbox",
                2 => "FIFO",
                3 => "FIFO relaxed",
                _ => "Other mode",
            };
            let multiplier = if segment.first.multiplier == 1 {
                "FG off".to_owned()
            } else {
                format!("{}×", segment.first.multiplier)
            };
            let start = (segment.start() - origin) as f64 / SECOND as f64;
            let end = (segment.last.end - origin) as f64 / SECOND as f64;
            let _ = write!(
                report,
                "\n{start:.1}–{end:.1} s · {multiplier} · {fps:.1} base FPS · {mode}"
            );
            if segment.first.cap > 0 {
                let _ = write!(
                    report,
                    " · {:.1} FPS cap",
                    segment.first.cap as f64 / 1000.0
                );
            }
            if segment.first.configured_cap > 0 {
                let _ = write!(
                    report,
                    " · {} FPS configured limit",
                    segment.first.configured_cap
                );
            }
            let on = |enabled| if enabled { "on" } else { "off" };
            let _ = writeln!(
                report,
                "\nBatch Hold {} · Pacing Correction {}",
                on(segment.first.hold),
                on(segment.first.correction)
            );
            let timing = &mut segment.timing;
            let _ = writeln!(
                report,
                "{} generated skips · {} present errors",
                segment.skips, segment.errors
            );
            if timing.shown > 0 || timing.discarded > 0 {
                let _ = writeln!(
                    report,
                    "{} presented · {} discarded (matched compositor reports)",
                    timing.shown, timing.discarded
                );
            }
            if timing.shown > 0 {
                let share = |count| count as f64 * 100.0 / timing.shown as f64;
                let _ = writeln!(
                    report,
                    "Compositor flags   VSync {:.1}% · Zero copy {:.1}%",
                    share(timing.vsync),
                    share(timing.zero_copy),
                );
            }
            for (label, values) in [
                ("Simulation → real presentation", &mut timing.sim_to_real),
                (
                    "Application present → real presentation",
                    &mut timing.app_to_real,
                ),
                ("Simulation → application present", &mut timing.sim_to_app),
                (
                    "Gap between consecutive displayed outputs",
                    &mut timing.display_gap,
                ),
                ("Frame generation GPU time", &mut timing.gpu_cost),
            ] {
                let _ = writeln!(report, "{label}   {}", percentiles(values));
            }
            if segment.first.multiplier > 1 {
                for ordinal in 1..segment.first.multiplier {
                    let phase = ordinal * (60 / segment.first.multiplier);
                    let _ = writeln!(
                        report,
                        "Simulation → generated {ordinal}   {}",
                        percentiles(timing.sim_to_generated.entry(phase).or_default())
                    );
                }
            }
        }
        Some(report)
    }
}

pub fn analyze(path: &Path, uid: u32, range: Option<(f64, f64)>) -> io::Result<Option<String>> {
    let listed = fs::symlink_metadata(path)?;
    let (file, meta) = live::open_checked(path, uid, &listed)
        .ok_or_else(|| io::Error::other("Could not open the capture as an owned regular file"))?;
    let mut reader = BufReader::new((&file).take(meta.len()));
    let mut capture = Capture::default();
    let mut line = Vec::new();
    loop {
        line.clear();
        if reader
            .by_ref()
            .take(MAX_LINE as u64 + 1)
            .read_until(b'\n', &mut line)?
            == 0
        {
            break;
        }
        if line.len() > MAX_LINE {
            capture.incomplete = true;
            if !line.ends_with(b"\n") {
                reader.skip_until(b'\n')?;
            }
        } else if line.ends_with(b"\n") {
            if let Ok(text) = std::str::from_utf8(&line[..line.len() - 1]) {
                capture.read_record(text);
            }
        }
    }
    if file.metadata()?.len() < meta.len() {
        return Err(io::Error::other(
            "Capture wrapped during analysis. Run it again.",
        ));
    }
    Ok(capture.report(range))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn latency_joins_keep_recreated_swapchains_separate() {
        let mut capture = Capture::default();
        for (context, base, sim_latency) in [(11, SECOND, 45), (12, 4 * SECOND, 25)] {
            let source = (1, context, 1);
            let app = base + 400 * MS;
            let real = app + 20 * MS;
            capture.windows.push(Window {
                stream: (1, context),
                end: base + SECOND,
                duration_ms: 1000.0,
                sources: 1,
                skips: 0,
                errors: 0,
                multiplier: 1,
                cap: 0,
                configured_cap: 0,
                mode: 0,
                hold: false,
                correction: false,
            });
            capture.legacy_app.entry((1, 1)).or_default().push(app);
            let marker = capture.markers.entry((1, context)).or_default();
            marker.present.add(app - MS / 2);
            marker.simulation.add(real - sim_latency * MS);
            // Both swapchains reuse the source, ordinal, and feedback counters.
            let feedback = (1, context, 1);
            capture.outputs.entry(feedback).or_default().add(Output {
                key: source,
                ordinal: 1,
                at: app + MS,
                finished: app + 3 * MS,
                phase: 0,
                success: true,
            });
            capture.requests.entry(feedback).or_default().add(Request {
                key: source,
                at: app + 2 * MS,
                generated: false,
            });
            capture
                .outcomes
                .entry(feedback)
                .or_default()
                .add(Outcome::Shown {
                    at: real,
                    output: 1,
                    sequence: 1,
                    flags: 15,
                });
        }
        let report = capture.report(None).unwrap();
        assert!(report
            .contains("Simulation → real presentation   45.00 / 45.00 / 45.00 ms  (1 frames)"));
        assert!(report
            .contains("Simulation → real presentation   25.00 / 25.00 / 25.00 ms  (1 frames)"));
        assert_eq!(
            report
                .matches("Application present → real presentation   20.00")
                .count(),
            2
        );
    }

    #[test]
    fn close_markers_and_legacy_presents_with_multiple_candidates_stay_unmatched() {
        let mut capture = Capture::default();
        capture.legacy_app.insert((1, 1), vec![10 * MS, 11 * MS]);
        let marker = capture.markers.entry((1, 1)).or_default();
        marker.present.add(10 * MS);
        marker.simulation.add(MS);
        assert!(capture.simulation_by_app().is_empty());
        assert_eq!(capture.app_time((1, 22, 1), 12 * MS), None);

        capture.app.entry((1, 22, 1)).or_default().add(11 * MS);
        assert_eq!(capture.app_time((1, 22, 1), 12 * MS), Some(11 * MS));
    }

    #[test]
    fn ranks_and_capture_gap_without_a_pid() {
        let mut sample = [3, 1, 9, 2].map(|n| n * MS);
        assert_eq!(
            percentiles(&mut sample),
            "2.00 / 9.00 / 9.00 ms  (4 frames)"
        );
        let mut capture = Capture::default();
        capture.read_record(&format!(
            "{OPERATION}pacing-profile-capture-gap lost_records=2"
        ));
        assert!(capture.incomplete);
    }
}
