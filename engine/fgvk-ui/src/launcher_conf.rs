//! Reads and writes the standalone launcher's `launcher.conf`, which holds the logging switches.
//!
//! The file is checked with the launcher's own rules: `version=1` is required, boolean keys take
//! only 0 or 1, a duplicate key or an unknown key makes the whole file invalid, and the retired
//! `disable_hdr_exposure` and `log_present` keys are accepted and ignored. A file that fails these rules is reported
//! and never written. A write changes only the switch being set: the file is rewritten atomically,
//! and comments, other keys and their values survive.

use std::ffi::OsString;
use std::fs;
use std::io;
use std::path::PathBuf;
use std::time::SystemTime;

use crate::config::{self, modified_time, write_atomically};

const APP_DIR: &str = "framegen-vulkan";
const FILE_NAME: &str = "launcher.conf";
const FORMAT_VERSION: &str = "1";
/// Keys the launcher no longer acts on. Each is checked like a boolean and then ignored.
const RETIRED_KEYS: [&str; 2] = ["disable_hdr_exposure", "log_present"];
const OVERLAY_KEY: &str = "disable_steam_overlay";
const ZINK_KEY: &str = "enable_zink";
const ALSA_KEY: &str = "force_alsa_audio";
const WHITESPACE: &[char] = &[' ', '\t', '\r', '\n'];

/// One logging switch. Each maps to one `launcher.conf` key, which the launcher turns into a
/// renderer diagnostic variable.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Switch {
    Pacing,
    Trace,
    Wayland,
    GpuCost,
}

impl Switch {
    /// The order the window shows the switches and the launcher writes them.
    pub const ALL: [Switch; 4] = [
        Switch::Pacing,
        Switch::Trace,
        Switch::Wayland,
        Switch::GpuCost,
    ];

    pub fn key(self) -> &'static str {
        match self {
            Switch::Pacing => "log_pacing",
            Switch::Trace => "log_trace",
            Switch::Wayland => "log_wayland",
            Switch::GpuCost => "log_gpu_cost",
        }
    }
}

/// The logging switches. A key the file leaves out is off.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Logging([bool; 4]);

impl Logging {
    pub fn get(self, switch: Switch) -> bool {
        self.0[switch as usize]
    }

    fn set(&mut self, switch: Switch, value: bool) {
        self.0[switch as usize] = value;
    }
}

/// Chooses `launcher.conf` the way the launcher does: `FGVK_LAUNCH_CONFIG`, then `$XDG_CONFIG_HOME`,
/// then `$HOME/.config`. Empty values are absent. The launcher's last fallback,
/// `/etc/framegen-vulkan/launcher.conf`, is not used: the window must not write to a system path.
pub fn resolve_path(lookup: impl Fn(&str) -> Option<OsString>) -> Result<PathBuf, String> {
    if let Some(path) = config::non_empty(lookup("FGVK_LAUNCH_CONFIG")) {
        return Ok(path);
    }
    if let Some(dir) = config::non_empty(lookup("XDG_CONFIG_HOME")) {
        return Ok(dir.join(APP_DIR).join(FILE_NAME));
    }
    match config::non_empty(lookup("HOME")) {
        Some(home) => Ok(home.join(".config").join(APP_DIR).join(FILE_NAME)),
        None => Err(
            "Set FGVK_LAUNCH_CONFIG, XDG_CONFIG_HOME or HOME to locate launcher.conf.".to_owned(),
        ),
    }
}

/// Checks the whole file the way the launcher does and returns its logging switches.
pub fn parse(text: &str) -> Result<Logging, String> {
    let mut version_seen = false;
    let mut seen: Vec<&str> = Vec::new();
    let mut logging = Logging::default();
    for line in text.lines() {
        let content = line.trim_matches(WHITESPACE);
        if content.is_empty() || content.starts_with('#') {
            continue;
        }
        let Some((key, value)) = content.split_once('=') else {
            return Err("invalid launcher configuration line".to_owned());
        };
        if value.contains('=') {
            return Err("invalid launcher configuration line".to_owned());
        }
        let key = key.trim_matches(WHITESPACE);
        let value = value.trim_matches(WHITESPACE);
        if key == "version" {
            if version_seen || value != FORMAT_VERSION {
                return Err("unsupported launcher configuration version".to_owned());
            }
            version_seen = true;
            continue;
        }
        let known = RETIRED_KEYS.contains(&key)
            || [OVERLAY_KEY, ZINK_KEY, ALSA_KEY].contains(&key)
            || Switch::ALL.iter().any(|switch| switch.key() == key);
        if !known {
            return Err(format!("unknown launcher setting: {key}"));
        }
        if seen.contains(&key) {
            return Err(format!("duplicate {key} launcher setting"));
        }
        let flag = match value {
            "0" => false,
            "1" => true,
            _ => return Err(format!("launcher setting {key} must be 0 or 1")),
        };
        seen.push(key);
        if let Some(switch) = Switch::ALL.into_iter().find(|switch| switch.key() == key) {
            logging.set(switch, flag);
        }
    }
    if !version_seen {
        return Err("launcher configuration version is missing".to_owned());
    }
    Ok(logging)
}

/// The file written when none exists: every key, in the launcher's order, with the launcher's
/// defaults for the compatibility keys.
fn canonical(logging: Logging) -> String {
    let mut text =
        format!("version={FORMAT_VERSION}\n{OVERLAY_KEY}=1\n{ZINK_KEY}=0\n{ALSA_KEY}=0\n");
    for switch in Switch::ALL {
        text.push_str(&format!(
            "{}={}\n",
            switch.key(),
            u8::from(logging.get(switch))
        ));
    }
    text
}

/// Returns `text` with one switch set. The switch's line is replaced in place, keeping its line
/// ending, or appended when the file has none. Every other line is copied unchanged. Callers
/// check `text` with `parse` first, so the switch appears at most once.
fn with_switch(text: &str, switch: Switch, value: bool) -> String {
    let key = switch.key();
    let new_line = format!("{key}={}", u8::from(value));
    let mut out = String::with_capacity(text.len() + new_line.len() + 1);
    let mut replaced = false;
    for segment in text.split_inclusive('\n') {
        let body = segment.trim_end_matches(['\r', '\n']);
        let content = body.trim_matches(WHITESPACE);
        let is_switch = !content.starts_with('#')
            && content
                .split_once('=')
                .is_some_and(|(name, _)| name.trim_matches(WHITESPACE) == key);
        if is_switch && !replaced {
            out.push_str(&new_line);
            out.push_str(&segment[body.len()..]);
            replaced = true;
        } else {
            out.push_str(segment);
        }
    }
    if !replaced {
        if !out.is_empty() && !out.ends_with('\n') {
            out.push('\n');
        }
        out.push_str(&new_line);
        out.push('\n');
    }
    out
}

/// The launcher's configuration file and its last-known state on disk.
pub struct LauncherConf {
    path: Result<PathBuf, String>,
    state: Result<Logging, String>,
    modified: Option<SystemTime>,
}

impl LauncherConf {
    /// Opens the file. An unresolved path (`Err`) is reported as an error.
    pub fn open(path: Result<PathBuf, String>) -> Self {
        let mut conf = Self {
            path,
            state: Ok(Logging::default()),
            modified: None,
        };
        conf.reload();
        conf
    }

    /// The switches the launcher will use. A missing file means all off. An invalid file is an
    /// error, and the launcher then exports none of the switches either.
    pub fn view(&self) -> Result<Logging, String> {
        self.state.clone()
    }

    /// Re-reads the file from disk.
    pub fn reload(&mut self) {
        let path = match &self.path {
            Ok(path) => path.clone(),
            Err(reason) => {
                self.state = Err(reason.clone());
                return;
            }
        };
        self.modified = modified_time(&path);
        self.state = match fs::read_to_string(&path) {
            Ok(text) => parse(&text).map_err(|message| format!("{}: {message}", path.display())),
            Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(Logging::default()),
            Err(error) => Err(format!("Cannot read {}: {error}", path.display())),
        };
    }

    /// True when the file's modification time differs from the last read or write.
    pub fn changed_on_disk(&self) -> bool {
        match &self.path {
            Ok(path) => modified_time(path) != self.modified,
            Err(_) => false,
        }
    }

    /// Sets one switch and reloads the state. A missing file is created with the launcher defaults.
    /// A file that fails the strict rules is refused and left unchanged.
    pub fn set(&mut self, switch: Switch, value: bool) -> Result<(), String> {
        let outcome = self.apply(switch, value);
        self.reload();
        outcome
    }

    fn apply(&self, switch: Switch, value: bool) -> Result<(), String> {
        let path = self.path.as_ref().map_err(Clone::clone)?;
        let text = match fs::read_to_string(path) {
            Ok(text) => {
                parse(&text).map_err(|message| {
                    format!("{}: {message}. The file was not changed.", path.display())
                })?;
                with_switch(&text, switch, value)
            }
            Err(error) if error.kind() == io::ErrorKind::NotFound => {
                if let Some(parent) = path.parent() {
                    fs::create_dir_all(parent)
                        .map_err(|error| format!("Cannot create {}: {error}", parent.display()))?;
                }
                let mut logging = Logging::default();
                logging.set(switch, value);
                canonical(logging)
            }
            Err(error) => return Err(format!("Cannot read {}: {error}", path.display())),
        };
        write_atomically(path, &text)
            .map_err(|error| format!("Cannot save {}: {error}", path.display()))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashMap;
    use std::process;
    use std::time::UNIX_EPOCH;

    fn env(pairs: &[(&str, &str)]) -> impl Fn(&str) -> Option<OsString> {
        let map: HashMap<String, OsString> = pairs
            .iter()
            .map(|(k, v)| ((*k).to_owned(), OsString::from(v)))
            .collect();
        move |key: &str| map.get(key).cloned()
    }

    fn temp_dir(tag: &str) -> PathBuf {
        let nanos = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map(|d| d.as_nanos())
            .unwrap_or(0);
        let dir =
            std::env::temp_dir().join(format!("fgvk-launcher-{tag}-{}-{nanos}", process::id()));
        fs::create_dir_all(&dir).expect("create test directory");
        dir
    }

    fn switch_states(logging: Logging) -> Vec<bool> {
        Switch::ALL
            .iter()
            .map(|switch| logging.get(*switch))
            .collect()
    }

    #[test]
    fn resolve_path_mirrors_the_launcher() {
        assert_eq!(
            resolve_path(env(&[("FGVK_LAUNCH_CONFIG", "/x/l.conf"), ("HOME", "/h")])),
            Ok(PathBuf::from("/x/l.conf"))
        );
        assert_eq!(
            resolve_path(env(&[
                ("FGVK_LAUNCH_CONFIG", ""),
                ("XDG_CONFIG_HOME", "/xdg"),
                ("HOME", "/h")
            ])),
            Ok(PathBuf::from("/xdg/framegen-vulkan/launcher.conf"))
        );
        assert_eq!(
            resolve_path(env(&[("XDG_CONFIG_HOME", ""), ("HOME", "/h")])),
            Ok(PathBuf::from("/h/.config/framegen-vulkan/launcher.conf"))
        );
        assert!(resolve_path(env(&[])).is_err());
    }

    #[test]
    fn parse_reads_the_switches_and_ignores_the_retired_key() {
        let logging = parse(
            "version=1\ndisable_hdr_exposure=1\ndisable_steam_overlay=0\nenable_zink=1\nforce_alsa_audio=0\nlog_trace=1\nlog_present=1\n",
        )
        .expect("valid file");
        assert_eq!(switch_states(logging), [false, true, false, false]);
    }

    #[test]
    fn parse_matches_the_launcher_spacing_and_line_endings() {
        let logging = parse("  version = 1  \n# comment\n\nlog_wayland = 1\r\nlog_present=0")
            .expect("valid file");
        assert_eq!(switch_states(logging), [false, false, true, false]);
    }

    #[test]
    fn parse_rejects_what_the_launcher_rejects() {
        let rejected = [
            "log_pacing=1\n",
            "version=2\n",
            "version=1\nversion=1\n",
            "version=1\nlog_pacing=1\nlog_pacing=0\n",
            "version=1\ndisable_hdr_exposure=1\ndisable_hdr_exposure=0\n",
            "version=1\nlog_pacing=true\n",
            "version=1\nlog_present=2\n",
            "version=1\nlog_present=1\nlog_present=0\n",
            "version=1\nunknown_setting=1\n",
            "version=1\nlog_trace=1=0\n",
            "version=1\nlog_trace\n",
            "version=1\n=1\n",
        ];
        for text in rejected {
            assert!(parse(text).is_err(), "must reject {text:?}");
        }
    }

    #[test]
    fn writing_a_missing_file_creates_the_launcher_defaults() {
        let dir = temp_dir("create");
        let path = dir.join("nested").join(FILE_NAME);
        let mut conf = LauncherConf::open(Ok(path.clone()));
        assert_eq!(conf.view(), Ok(Logging::default()));
        conf.set(Switch::Trace, true).expect("create succeeds");
        assert_eq!(
            fs::read_to_string(&path).expect("read back"),
            "version=1\ndisable_steam_overlay=1\nenable_zink=0\nforce_alsa_audio=0\n\
             log_pacing=0\nlog_trace=1\nlog_wayland=0\nlog_gpu_cost=0\n"
        );
        assert_eq!(
            switch_states(conf.view().expect("valid")),
            [false, true, false, false]
        );
        fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn writing_keeps_other_keys_comments_and_order() {
        let dir = temp_dir("preserve");
        let path = dir.join(FILE_NAME);
        let original = "version=1\n# keep me\ndisable_hdr_exposure=1\ndisable_steam_overlay=0\nenable_zink=1\nforce_alsa_audio=0\n";
        fs::write(&path, original).expect("write fixture");

        let mut conf = LauncherConf::open(Ok(path.clone()));
        conf.set(Switch::Pacing, true).expect("append succeeds");
        assert_eq!(
            fs::read_to_string(&path).expect("read back"),
            format!("{original}log_pacing=1\n")
        );
        assert!(conf.view().expect("still valid").get(Switch::Pacing));
        fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn writing_replaces_only_the_switch_line() {
        let dir = temp_dir("replace");
        let path = dir.join(FILE_NAME);
        fs::write(
            &path,
            "version=1\n# keep me\nlog_trace = 1\ndisable_hdr_exposure=1\n",
        )
        .expect("write fixture");

        let mut conf = LauncherConf::open(Ok(path.clone()));
        conf.set(Switch::Trace, false).expect("replace succeeds");
        assert_eq!(
            fs::read_to_string(&path).expect("read back"),
            "version=1\n# keep me\nlog_trace=0\ndisable_hdr_exposure=1\n"
        );
        assert!(
            !dir.join("launcher.conf.tmp").exists(),
            "no .tmp left behind"
        );
        fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn replacement_keeps_crlf_line_endings() {
        let dir = temp_dir("crlf");
        let path = dir.join(FILE_NAME);
        fs::write(&path, "version=1\r\nlog_trace=1\r\n").expect("write fixture");

        let mut conf = LauncherConf::open(Ok(path.clone()));
        conf.set(Switch::Trace, false).expect("replace succeeds");
        assert_eq!(
            fs::read_to_string(&path).expect("read back"),
            "version=1\r\nlog_trace=0\r\n"
        );
        fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn invalid_file_is_reported_and_never_overwritten() {
        let dir = temp_dir("invalid");
        let path = dir.join(FILE_NAME);
        let broken = "version=1\nlog_present=yes\n";
        fs::write(&path, broken).expect("write fixture");

        let mut conf = LauncherConf::open(Ok(path.clone()));
        assert!(conf.view().expect_err("invalid").contains("must be 0 or 1"));
        let refused = conf.set(Switch::Trace, true).expect_err("write refused");
        assert!(refused.contains("not changed"), "{refused}");
        assert_eq!(fs::read_to_string(&path).expect("read back"), broken);
        assert!(!dir.join("launcher.conf.tmp").exists());
        fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn unresolved_path_is_an_error_and_writes_nothing() {
        let mut conf = LauncherConf::open(Err("Set HOME.".to_owned()));
        assert_eq!(conf.view(), Err("Set HOME.".to_owned()));
        assert_eq!(conf.set(Switch::Wayland, true), Err("Set HOME.".to_owned()));
    }
}
