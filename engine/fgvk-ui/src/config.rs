//! Reads and writes the framegen-vulkan `conf.toml` file.
//!
//! Edits go through `toml_edit::DocumentMut`, so comments, unknown keys and key
//! order survive. Each change re-reads the file, applies one edit to that copy,
//! and replaces the file atomically. A file that does not parse is reported and
//! never written.

use std::collections::HashMap;
use std::ffi::OsString;
use std::fs::{self, File};
use std::io::{self, Write};
use std::ops::RangeInclusive;
use std::path::{Path, PathBuf};
use std::time::SystemTime;

use toml_edit::{value, Array, ArrayOfTables, DocumentMut, Item, Table, TableLike, Value};

const APP_DIR: &str = "framegen-vulkan";
const FILE_NAME: &str = "conf.toml";
const FORMAT_VERSION: i64 = 2;
const DEFAULT_PROFILE_NAME: &str = "Default";
const UNNAMED_PROFILE: &str = "unnamed";
const STALE_PROFILE: &str = "The profile list changed on disk. Nothing was written; try again.";
const MISSING_PROFILES: &str = "The file has no profile tables.";

/// Turning the FPS limit on without a remembered value uses this number.
pub const DEFAULT_FPS_LIMIT: i64 = 60;

pub const MULTIPLIER_RANGE: RangeInclusive<i64> = 2..=5;
pub const ADAPTIVE_MAX_MULTIPLIER_RANGE: RangeInclusive<i64> = 2..=5;
pub const TARGET_FPS_RANGE: RangeInclusive<i64> = 10..=1000;
pub const FPS_LIMIT_RANGE: RangeInclusive<i64> = 1..=1000;
pub const BASE_FPS_CAP_RANGE: RangeInclusive<i64> = 0..=1000;
pub const FLOW_SCALE_RANGE: RangeInclusive<f64> = 0.25..=1.0;

/// Values shown for keys the file leaves out. They match the Renderer defaults.
const DEFAULT_MULTIPLIER: i64 = 2;
const DEFAULT_TARGET_FPS: i64 = 120;
const DEFAULT_ADAPTIVE_MAX_MULTIPLIER: i64 = 3;
const DEFAULT_FLOW_SCALE: f64 = 0.8;
const DEFAULT_ALLOW_FP16: bool = true;
const DEFAULT_PERFORMANCE_MODE: bool = false;

/// Chooses the configuration path from an environment lookup.
///
/// `FGVK_CONFIG` wins, then `$XDG_CONFIG_HOME` (only when absolute, as the XDG
/// specification requires), then `$HOME/.config`.
pub fn resolve_path(lookup: impl Fn(&str) -> Option<OsString>) -> Result<PathBuf, String> {
    if let Some(path) = non_empty(lookup("FGVK_CONFIG")) {
        return Ok(path);
    }
    if let Some(dir) = non_empty(lookup("XDG_CONFIG_HOME")).filter(|dir| dir.is_absolute()) {
        return Ok(dir.join(APP_DIR).join(FILE_NAME));
    }
    match non_empty(lookup("HOME")) {
        Some(home) => Ok(home.join(".config").join(APP_DIR).join(FILE_NAME)),
        None => Err("Set FGVK_CONFIG or HOME to locate conf.toml.".to_owned()),
    }
}

/// Treats an unset or empty environment value as absent.
pub(crate) fn non_empty(value: Option<OsString>) -> Option<PathBuf> {
    value.filter(|value| !value.is_empty()).map(PathBuf::from)
}

/// One position of the frame-generation selector.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Mode {
    Off,
    /// Fixed total output multiplier.
    Fixed(i64),
    Adaptive,
}

impl Mode {
    /// Derives the position in the order disabled, adaptive, then multiplier.
    pub fn from_settings(enabled: bool, adaptive: bool, multiplier: i64) -> Self {
        if !enabled {
            Self::Off
        } else if adaptive {
            Self::Adaptive
        } else {
            Self::Fixed(multiplier)
        }
    }

    /// Writes the keys for this position. `Off` leaves `multiplier` and `adaptive` alone.
    fn write(self, table: &mut dyn TableLike) {
        match self {
            Self::Off => set_value(table, "frame_generation_enabled", Value::from(false)),
            Self::Fixed(multiplier) => {
                set_value(table, "frame_generation_enabled", Value::from(true));
                set_value(table, "adaptive", Value::from(false));
                set_value(table, "multiplier", Value::from(multiplier));
            }
            Self::Adaptive => {
                set_value(table, "frame_generation_enabled", Value::from(true));
                set_value(table, "adaptive", Value::from(true));
            }
        }
    }
}

/// Identifies a profile by its position and name. Edits refuse to run when the
/// file changed underneath the window and the profile at that position differs.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ProfileKey {
    pub index: usize,
    pub name: String,
}

/// Typed snapshot of one `[[profile]]` table.
#[derive(Clone, Debug, PartialEq)]
pub struct Profile {
    pub key: ProfileKey,
    pub active_in: Vec<String>,
    pub mode: Mode,
    pub target_fps: i64,
    pub adaptive_max_multiplier: i64,
    pub flow_scale: f64,
    pub base_fps_cap: i64,
    pub performance_mode: bool,
}

/// Typed snapshot of the parts of the file the window edits.
#[derive(Clone, Debug, PartialEq)]
pub struct Settings {
    pub profiles: Vec<Profile>,
    pub dll: Option<String>,
    pub allow_fp16: bool,
}

/// What the window should show for the file right now.
#[derive(Clone, Debug)]
pub enum ConfigView {
    /// The file does not exist yet. Carries the path for display.
    Missing(String),
    /// The file cannot be used. Carries the error for display.
    Invalid(String),
    Ready(Settings),
}

/// Remembers the last non-zero FPS limit for each profile during this session.
#[derive(Debug, Default)]
pub struct FpsMemory {
    last: HashMap<String, i64>,
}

impl FpsMemory {
    pub fn record(&mut self, profile: &str, value: i64) {
        if !FPS_LIMIT_RANGE.contains(&value) {
            return;
        }
        if let Some(slot) = self.last.get_mut(profile) {
            *slot = value;
        } else {
            self.last.insert(profile.to_owned(), value);
        }
    }

    /// Value to write when the FPS limit is switched on.
    pub fn enable_value(&self, profile: &str) -> i64 {
        self.last.get(profile).copied().unwrap_or(DEFAULT_FPS_LIMIT)
    }

    pub fn rename(&mut self, from: &str, to: &str) {
        if let Some(value) = self.last.remove(from) {
            self.last.insert(to.to_owned(), value);
        }
    }

    pub fn forget(&mut self, profile: &str) {
        self.last.remove(profile);
    }
}

enum Document {
    Missing,
    Invalid(String),
    Ready(DocumentMut),
}

/// The configuration file and its last-known state on disk.
pub struct Config {
    path: Result<PathBuf, String>,
    document: Document,
    modified: Option<SystemTime>,
}

impl Config {
    /// Opens the file. An unresolved path (`Err`) is reported as an invalid configuration.
    pub fn open(path: Result<PathBuf, String>) -> Self {
        let mut config = Self {
            path,
            document: Document::Missing,
            modified: None,
        };
        config.reload();
        config
    }

    pub fn view(&self) -> ConfigView {
        match &self.document {
            Document::Missing => ConfigView::Missing(self.location()),
            Document::Invalid(message) => ConfigView::Invalid(message.clone()),
            Document::Ready(doc) => ConfigView::Ready(Settings {
                profiles: read_profiles(doc),
                dll: read_global_dll(doc),
                allow_fp16: read_global_allow_fp16(doc),
            }),
        }
    }

    /// Re-reads the file from disk.
    pub fn reload(&mut self) {
        let path = match &self.path {
            Ok(path) => path.clone(),
            Err(reason) => {
                self.document = Document::Invalid(reason.clone());
                return;
            }
        };
        self.modified = modified_time(&path);
        self.document = match fs::read_to_string(&path) {
            Ok(text) => match parse_document(&text) {
                Ok(doc) => Document::Ready(doc),
                Err(message) => Document::Invalid(format!("{}: {message}", path.display())),
            },
            Err(error) if error.kind() == io::ErrorKind::NotFound => Document::Missing,
            Err(error) => Document::Invalid(format!("Cannot read {}: {error}", path.display())),
        };
    }

    /// True when the file's modification time differs from the last read or write.
    pub fn changed_on_disk(&self) -> bool {
        match &self.path {
            Ok(path) => modified_time(path) != self.modified,
            Err(_) => false,
        }
    }

    /// Creates the file with one `Default` profile. Never overwrites an existing file.
    pub fn create_default(&mut self) -> Result<(), String> {
        let path = self.path.clone()?;
        if fs::symlink_metadata(&path).is_ok() {
            self.reload();
            return Err(format!(
                "{} already exists. It was not overwritten.",
                path.display()
            ));
        }
        let outcome = create_file(&path);
        self.reload();
        outcome
    }

    pub fn add_profile(&mut self, name: &str) -> Result<ProfileKey, String> {
        let name = clean_name(name)?.to_owned();
        let index = self.edit(|doc| append_profile(doc, &name))?;
        Ok(ProfileKey { index, name })
    }

    pub fn rename_profile(&mut self, key: &ProfileKey, name: &str) -> Result<(), String> {
        let name = clean_name(name)?;
        self.edit(|doc| rename_profile_in(doc, key, name))
    }

    pub fn delete_profile(&mut self, key: &ProfileKey) -> Result<(), String> {
        self.edit(|doc| remove_profile_in(doc, key))
    }

    pub fn set_active_in(&mut self, key: &ProfileKey, entries: &[String]) -> Result<(), String> {
        self.edit_profile(key, |table| write_active_in(table, entries))
    }

    pub fn set_mode(&mut self, key: &ProfileKey, mode: Mode) -> Result<(), String> {
        if let Mode::Fixed(multiplier) = mode {
            check_i64("Frame generation multiplier", multiplier, &MULTIPLIER_RANGE)?;
        }
        self.edit_profile(key, |table| mode.write(table))
    }

    pub fn set_target_fps(&mut self, key: &ProfileKey, value: i64) -> Result<(), String> {
        check_i64("Target FPS", value, &TARGET_FPS_RANGE)?;
        self.edit_profile(key, |table| {
            set_value(table, "target_fps", Value::from(value))
        })
    }

    pub fn set_adaptive_max_multiplier(
        &mut self,
        key: &ProfileKey,
        value: i64,
    ) -> Result<(), String> {
        check_i64("Max multiplier", value, &ADAPTIVE_MAX_MULTIPLIER_RANGE)?;
        self.edit_profile(key, |table| {
            set_value(table, "adaptive_max_multiplier", Value::from(value));
        })
    }

    /// Stores the flow scale rounded to hundredths, so the file holds `0.85` rather than a float artifact.
    pub fn set_flow_scale(&mut self, key: &ProfileKey, value: f64) -> Result<(), String> {
        let value = (value * 100.0).round() / 100.0;
        if !FLOW_SCALE_RANGE.contains(&value) {
            return Err("Quality must be from 25% to 100%.".to_owned());
        }
        self.edit_profile(key, |table| {
            set_value(table, "flow_scale", Value::from(value))
        })
    }

    /// Zero disables the FPS limit.
    pub fn set_base_fps_cap(&mut self, key: &ProfileKey, value: i64) -> Result<(), String> {
        check_i64("FPS limit", value, &BASE_FPS_CAP_RANGE)?;
        self.edit_profile(key, |table| {
            set_value(table, "base_fps_cap", Value::from(value))
        })
    }

    pub fn set_performance_mode(&mut self, key: &ProfileKey, value: bool) -> Result<(), String> {
        self.edit_profile(key, |table| {
            set_value(table, "performance_mode", Value::from(value));
        })
    }

    /// `None` removes the `dll` key. The `[global]` table is created only when a path is set.
    pub fn set_global_dll(&mut self, dll: Option<&str>) -> Result<(), String> {
        let dll = dll.map(str::trim).filter(|dll| !dll.is_empty());
        self.edit(|doc| {
            write_global_dll(doc, dll);
            Ok(())
        })
    }

    /// Writes `[global] allow_fp16`. The `[global]` table is created when the file has none.
    pub fn set_global_allow_fp16(&mut self, value: bool) -> Result<(), String> {
        self.edit(|doc| {
            if let Some(global) = global_mut(doc) {
                set_value(global, "allow_fp16", Value::from(value));
            }
            Ok(())
        })
    }

    fn location(&self) -> String {
        match &self.path {
            Ok(path) => path.display().to_string(),
            Err(_) => FILE_NAME.to_owned(),
        }
    }

    fn edit_profile<R>(
        &mut self,
        key: &ProfileKey,
        change: impl FnOnce(&mut Table) -> R,
    ) -> Result<R, String> {
        self.edit(|doc| Ok(change(profile_mut(doc, key)?)))
    }

    /// Applies one change to a fresh read of the file, then reloads the window state.
    fn edit<R>(
        &mut self,
        change: impl FnOnce(&mut DocumentMut) -> Result<R, String>,
    ) -> Result<R, String> {
        let outcome = self.apply(change);
        self.reload();
        outcome
    }

    fn apply<R>(
        &self,
        change: impl FnOnce(&mut DocumentMut) -> Result<R, String>,
    ) -> Result<R, String> {
        let path = self.path.as_ref().map_err(|reason| reason.clone())?;
        let text = match fs::read_to_string(path) {
            Ok(text) => text,
            Err(error) if error.kind() == io::ErrorKind::NotFound => {
                return Err(format!("{} is missing. Create it first.", path.display()));
            }
            Err(error) => return Err(format!("Cannot read {}: {error}", path.display())),
        };
        let mut doc = parse_document(&text).map_err(|message| {
            format!("{}: {message}. The file was not changed.", path.display())
        })?;
        let result = change(&mut doc)?;
        write_atomically(path, &doc.to_string())
            .map_err(|error| format!("Cannot save {}: {error}", path.display()))?;
        Ok(result)
    }
}

fn modified_time(path: &Path) -> Option<SystemTime> {
    fs::metadata(path).and_then(|meta| meta.modified()).ok()
}

fn create_file(path: &Path) -> Result<(), String> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)
            .map_err(|error| format!("Cannot create {}: {error}", parent.display()))?;
    }
    write_atomically(path, &default_document().to_string())
        .map_err(|error| format!("Cannot write {}: {error}", path.display()))
}

/// Parses and checks the root keys the window depends on. Unknown keys are kept.
fn parse_document(text: &str) -> Result<DocumentMut, String> {
    let doc: DocumentMut = text
        .parse()
        .map_err(|error: toml_edit::TomlError| error.to_string())?;
    if doc.get("version").and_then(Item::as_integer) != Some(FORMAT_VERSION) {
        return Err(format!(
            "unsupported configuration version; expected version = {FORMAT_VERSION}"
        ));
    }
    if doc
        .get("profile")
        .is_some_and(|item| !item.is_array_of_tables())
    {
        return Err("profile entries must be written as [[profile]] tables".to_owned());
    }
    if doc
        .get("global")
        .is_some_and(|item| item.as_table_like().is_none())
    {
        return Err("global must be a table".to_owned());
    }
    Ok(doc)
}

/// The document written when no file exists yet.
pub fn default_document() -> DocumentMut {
    let mut doc = DocumentMut::new();
    doc.insert("version", value(FORMAT_VERSION));
    let mut profiles = ArrayOfTables::new();
    profiles.push(new_profile(DEFAULT_PROFILE_NAME));
    doc.insert("profile", Item::ArrayOfTables(profiles));
    doc
}

fn new_profile(name: &str) -> Table {
    let mut table = Table::new();
    table.insert("name", value(name));
    table.insert("active_in", value(Array::new()));
    table.insert("frame_generation_enabled", value(true));
    table.insert("multiplier", value(DEFAULT_MULTIPLIER));
    table.insert("adaptive", value(false));
    table.insert("flow_scale", value(DEFAULT_FLOW_SCALE));
    table.insert("base_fps_cap", value(0_i64));
    table
}

fn read_profiles(doc: &DocumentMut) -> Vec<Profile> {
    profiles_ref(doc)
        .map(|profiles| {
            profiles
                .iter()
                .enumerate()
                .map(|(index, table)| read_profile(index, table))
                .collect()
        })
        .unwrap_or_default()
}

fn read_profile(index: usize, table: &Table) -> Profile {
    let enabled = table
        .get("frame_generation_enabled")
        .and_then(Item::as_bool)
        .unwrap_or(true);
    let adaptive = table
        .get("adaptive")
        .and_then(Item::as_bool)
        .unwrap_or(false);
    Profile {
        key: ProfileKey {
            index,
            name: profile_name(table).to_owned(),
        },
        active_in: read_active_in(table.get("active_in")),
        mode: Mode::from_settings(
            enabled,
            adaptive,
            int_or(table, "multiplier", DEFAULT_MULTIPLIER),
        ),
        target_fps: int_or(table, "target_fps", DEFAULT_TARGET_FPS),
        adaptive_max_multiplier: int_or(
            table,
            "adaptive_max_multiplier",
            DEFAULT_ADAPTIVE_MAX_MULTIPLIER,
        ),
        flow_scale: float_or(table, "flow_scale", DEFAULT_FLOW_SCALE),
        base_fps_cap: int_or(table, "base_fps_cap", 0),
        performance_mode: table
            .get("performance_mode")
            .and_then(Item::as_bool)
            .unwrap_or(DEFAULT_PERFORMANCE_MODE),
    }
}

/// Accepts either a single string or an array of strings. Other entries are ignored.
fn read_active_in(item: Option<&Item>) -> Vec<String> {
    match item {
        Some(item) => match (item.as_str(), item.as_array()) {
            (Some(entry), _) => vec![entry.to_owned()],
            (None, Some(array)) => array
                .iter()
                .filter_map(Value::as_str)
                .map(str::to_owned)
                .collect(),
            _ => Vec::new(),
        },
        None => Vec::new(),
    }
}

fn read_global_dll(doc: &DocumentMut) -> Option<String> {
    doc.get("global")?
        .as_table_like()?
        .get("dll")?
        .as_str()
        .map(str::to_owned)
}

/// A missing table, a missing key and a non-boolean value all read as the renderer default.
fn read_global_allow_fp16(doc: &DocumentMut) -> bool {
    doc.get("global")
        .and_then(Item::as_table_like)
        .and_then(|global| global.get("allow_fp16"))
        .and_then(Item::as_bool)
        .unwrap_or(DEFAULT_ALLOW_FP16)
}

fn int_or(table: &Table, key: &str, default: i64) -> i64 {
    table.get(key).and_then(Item::as_integer).unwrap_or(default)
}

fn float_or(table: &Table, key: &str, default: f64) -> f64 {
    table
        .get(key)
        .and_then(|item| {
            item.as_float()
                .or_else(|| item.as_integer().map(|v| v as f64))
        })
        .unwrap_or(default)
}

fn profile_name(table: &Table) -> &str {
    table
        .get("name")
        .and_then(Item::as_str)
        .unwrap_or(UNNAMED_PROFILE)
}

fn profiles_ref(doc: &DocumentMut) -> Option<&ArrayOfTables> {
    doc.get("profile").and_then(Item::as_array_of_tables)
}

fn profiles_mut(doc: &mut DocumentMut) -> Result<&mut ArrayOfTables, String> {
    doc.get_mut("profile")
        .and_then(Item::as_array_of_tables_mut)
        .ok_or_else(|| MISSING_PROFILES.to_owned())
}

fn ensure_profiles(doc: &mut DocumentMut) -> Result<&mut ArrayOfTables, String> {
    if doc.get("profile").is_none() {
        doc.insert("profile", Item::ArrayOfTables(ArrayOfTables::new()));
    }
    profiles_mut(doc)
}

/// Appends a profile with the specified defaults and returns its index.
fn append_profile(doc: &mut DocumentMut, name: &str) -> Result<usize, String> {
    let profiles = ensure_profiles(doc)?;
    if name_taken(profiles, name, None) {
        return Err(taken(name));
    }
    profiles.push(new_profile(name));
    Ok(profiles.len() - 1)
}

fn rename_profile_in(doc: &mut DocumentMut, key: &ProfileKey, name: &str) -> Result<(), String> {
    let index = verified_index(doc, key)?;
    let profiles = profiles_mut(doc)?;
    if name_taken(profiles, name, Some(index)) {
        return Err(taken(name));
    }
    let table = profiles
        .get_mut(index)
        .ok_or_else(|| STALE_PROFILE.to_owned())?;
    set_value(table, "name", Value::from(name));
    Ok(())
}

fn remove_profile_in(doc: &mut DocumentMut, key: &ProfileKey) -> Result<(), String> {
    let index = verified_index(doc, key)?;
    let profiles = profiles_mut(doc)?;
    if profiles.len() <= 1 {
        return Err("The last profile cannot be deleted.".to_owned());
    }
    profiles.remove(index);
    Ok(())
}

fn verified_index(doc: &DocumentMut, key: &ProfileKey) -> Result<usize, String> {
    let profiles = profiles_ref(doc).ok_or_else(|| STALE_PROFILE.to_owned())?;
    match profiles.get(key.index) {
        Some(table) if profile_name(table) == key.name => Ok(key.index),
        _ => Err(STALE_PROFILE.to_owned()),
    }
}

fn profile_mut<'a>(doc: &'a mut DocumentMut, key: &ProfileKey) -> Result<&'a mut Table, String> {
    let index = verified_index(doc, key)?;
    profiles_mut(doc)?
        .get_mut(index)
        .ok_or_else(|| STALE_PROFILE.to_owned())
}

fn name_taken(profiles: &ArrayOfTables, name: &str, except: Option<usize>) -> bool {
    profiles
        .iter()
        .enumerate()
        .any(|(index, table)| Some(index) != except && profile_name(table) == name)
}

fn taken(name: &str) -> String {
    format!("A profile named \"{name}\" already exists.")
}

fn clean_name(name: &str) -> Result<&str, String> {
    let name = name.trim();
    if name.is_empty() {
        Err("Profile names cannot be empty.".to_owned())
    } else {
        Ok(name)
    }
}

fn check_i64(label: &str, value: i64, range: &RangeInclusive<i64>) -> Result<(), String> {
    if range.contains(&value) {
        Ok(())
    } else {
        Err(format!(
            "{label} must be a whole number from {} to {}.",
            range.start(),
            range.end()
        ))
    }
}

/// Replaces a value and keeps the formatting around it, including any trailing comment.
fn set_value(table: &mut dyn TableLike, key: &str, mut new: Value) {
    match table.get_mut(key) {
        Some(item) => {
            if let Some(old) = item.as_value() {
                *new.decor_mut() = old.decor().clone();
            }
            *item = Item::Value(new);
        }
        None => {
            table.insert(key, Item::Value(new));
        }
    }
}

fn write_active_in(table: &mut dyn TableLike, entries: &[String]) {
    let mut array = Array::new();
    for entry in entries {
        array.push(entry.as_str());
    }
    set_value(table, "active_in", Value::Array(array));
}

/// The `[global]` table, created empty when the file has none.
fn global_mut(doc: &mut DocumentMut) -> Option<&mut dyn TableLike> {
    if doc.get("global").is_none() {
        doc.insert("global", Item::Table(Table::new()));
    }
    doc.get_mut("global")?.as_table_like_mut()
}

fn write_global_dll(doc: &mut DocumentMut, dll: Option<&str>) {
    match dll {
        Some(path) => {
            if let Some(global) = global_mut(doc) {
                set_value(global, "dll", Value::from(path));
            }
        }
        None => {
            if let Some(global) = doc
                .get_mut("global")
                .and_then(|item| item.as_table_like_mut())
            {
                global.remove("dll");
            }
        }
    }
}

/// Writes `conf.toml.tmp` beside the target, syncs it, then renames it over the target.
///
/// Symbolic links are followed so the link itself survives. The original file's
/// permissions carry over to the replacement.
fn write_atomically(path: &Path, text: &str) -> io::Result<()> {
    let target = fs::canonicalize(path).unwrap_or_else(|_| path.to_path_buf());
    let Some(file_name) = target.file_name() else {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "configuration path has no file name",
        ));
    };
    let mut tmp_name = file_name.to_os_string();
    tmp_name.push(".tmp");
    let tmp = target.with_file_name(tmp_name);
    let permissions = fs::metadata(&target).ok().map(|meta| meta.permissions());
    let outcome = write_and_replace(&tmp, &target, text, permissions);
    if outcome.is_err() {
        let _ = fs::remove_file(&tmp);
    }
    outcome
}

fn write_and_replace(
    tmp: &Path,
    target: &Path,
    text: &str,
    permissions: Option<fs::Permissions>,
) -> io::Result<()> {
    {
        let mut file = File::create(tmp)?;
        file.write_all(text.as_bytes())?;
        file.sync_all()?;
    }
    if let Some(permissions) = permissions {
        fs::set_permissions(tmp, permissions)?;
    }
    fs::rename(tmp, target)?;
    if let Some(handle) = target.parent().and_then(|dir| File::open(dir).ok()) {
        let _ = handle.sync_all();
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::process;
    use std::time::{SystemTime, UNIX_EPOCH};

    const BASE: &str = "version = 2\n\n[global]\nallow_fp16 = true\n\n[[profile]]\nname = \"Alpha\"\nactive_in = [\"Game.exe\"]\nframe_generation_enabled = true\nmultiplier = 3\nadaptive = false\nflow_scale = 0.85\n";

    fn parse(text: &str) -> DocumentMut {
        text.parse().expect("test document parses")
    }

    fn profiles(doc: &DocumentMut) -> Vec<Profile> {
        read_profiles(doc)
    }

    fn key(index: usize, name: &str) -> ProfileKey {
        ProfileKey {
            index,
            name: name.to_owned(),
        }
    }

    fn temp_dir(tag: &str) -> PathBuf {
        let nanos = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map(|d| d.as_nanos())
            .unwrap_or(0);
        let dir = std::env::temp_dir().join(format!("fgvk-test-{tag}-{}-{nanos}", process::id()));
        fs::create_dir_all(&dir).expect("create test directory");
        dir
    }

    const POSITIONS: [Mode; 6] = [
        Mode::Off,
        Mode::Fixed(2),
        Mode::Fixed(3),
        Mode::Fixed(4),
        Mode::Fixed(5),
        Mode::Adaptive,
    ];

    #[test]
    fn mode_round_trips_all_six_positions() {
        for mode in POSITIONS {
            let mut doc = parse(BASE);
            let profile = profile_mut(&mut doc, &key(0, "Alpha")).expect("profile");
            mode.write(profile);
            let read = profiles(&doc).remove(0);
            assert_eq!(read.mode, mode, "round trip for {mode:?}");
        }
    }

    #[test]
    fn mode_writes_the_documented_keys() {
        let mut doc = parse(BASE);
        let profile = profile_mut(&mut doc, &key(0, "Alpha")).expect("profile");
        Mode::Fixed(4).write(profile);
        assert_eq!(profile["frame_generation_enabled"].as_bool(), Some(true));
        assert_eq!(profile["adaptive"].as_bool(), Some(false));
        assert_eq!(profile["multiplier"].as_integer(), Some(4));

        Mode::Adaptive.write(profile);
        assert_eq!(profile["adaptive"].as_bool(), Some(true));
        assert_eq!(profile["multiplier"].as_integer(), Some(4));

        Mode::Off.write(profile);
        assert_eq!(profile["frame_generation_enabled"].as_bool(), Some(false));
        assert_eq!(profile["adaptive"].as_bool(), Some(true));
        assert_eq!(profile["multiplier"].as_integer(), Some(4));
    }

    #[test]
    fn disabled_wins_over_adaptive_when_deriving_the_position() {
        assert_eq!(Mode::from_settings(false, true, 3), Mode::Off);
        assert_eq!(Mode::from_settings(true, true, 3), Mode::Adaptive);
        assert_eq!(Mode::from_settings(true, false, 5), Mode::Fixed(5));
    }

    #[test]
    fn missing_keys_use_renderer_defaults() {
        let doc = parse("version = 2\n\n[[profile]]\nname = \"Bare\"\n");
        let profile = profiles(&doc).remove(0);
        assert_eq!(profile.mode, Mode::Fixed(2));
        assert_eq!(profile.target_fps, 120);
        assert_eq!(profile.adaptive_max_multiplier, 3);
        assert!((profile.flow_scale - 0.8).abs() < f64::EPSILON);
        assert_eq!(profile.base_fps_cap, 0);
    }

    #[test]
    fn fps_toggle_restores_the_remembered_value() {
        let mut doc = parse(BASE);
        let mut memory = FpsMemory::default();
        assert_eq!(memory.enable_value("Alpha"), 60);

        let profile = profile_mut(&mut doc, &key(0, "Alpha")).expect("profile");
        set_value(profile, "base_fps_cap", Value::from(144_i64));
        memory.record("Alpha", profiles_cap(&doc));

        let profile = profile_mut(&mut doc, &key(0, "Alpha")).expect("profile");
        set_value(profile, "base_fps_cap", Value::from(0_i64));
        assert_eq!(profiles_cap(&doc), 0, "toggle off writes zero");
        assert_eq!(
            memory.enable_value("Alpha"),
            144,
            "toggle on reuses the last value"
        );

        memory.rename("Alpha", "Beta");
        assert_eq!(memory.enable_value("Beta"), 144);
        assert_eq!(memory.enable_value("Alpha"), 60);
    }

    fn profiles_cap(doc: &DocumentMut) -> i64 {
        profiles(doc).remove(0).base_fps_cap
    }

    #[test]
    fn fps_memory_ignores_zero_and_out_of_range_values() {
        let mut memory = FpsMemory::default();
        memory.record("A", 0);
        memory.record("A", 5000);
        assert_eq!(memory.enable_value("A"), 60);
        memory.record("A", 90);
        memory.record("A", 0);
        assert_eq!(memory.enable_value("A"), 90);
    }

    #[test]
    fn active_in_reads_a_string_and_an_array() {
        let doc = parse(
            "version = 2\n\n[[profile]]\nname = \"One\"\nactive_in = \"Single.exe\"\n\n[[profile]]\nname = \"Two\"\nactive_in = [\"A.exe\", 7, \"B.exe\"]\n",
        );
        let list = profiles(&doc);
        assert_eq!(list[0].active_in, vec!["Single.exe".to_owned()]);
        assert_eq!(
            list[1].active_in,
            vec!["A.exe".to_owned(), "B.exe".to_owned()]
        );
    }

    #[test]
    fn active_in_is_always_written_as_an_array() {
        let mut doc = parse(BASE);
        let profile = profile_mut(&mut doc, &key(0, "Alpha")).expect("profile");
        write_active_in(profile, &["Game.exe".to_owned(), "Other.exe".to_owned()]);
        assert!(doc
            .to_string()
            .contains("active_in = [\"Game.exe\", \"Other.exe\"]"));
    }

    #[test]
    fn edits_keep_comments_unknown_keys_and_order() {
        let text = "# top comment\nversion = 2\nfuture_root = \"keep\"\n\n[global]\n# dll note\nallow_fp16 = true\n\n[[profile]]\n# profile note\nname = \"Alpha\"\nunknown_profile_key = 42\nmultiplier = 3 # trailing note\nactive_in = [\"Game.exe\"]\n";
        let mut doc = parse(text);
        let profile = profile_mut(&mut doc, &key(0, "Alpha")).expect("profile");
        Mode::Fixed(4).write(profile);
        write_active_in(profile, &["Game.exe".to_owned(), "New.exe".to_owned()]);
        write_global_dll(&mut doc, Some("C:/games/Lossless.dll"));
        let out = doc.to_string();

        assert!(out.contains("# top comment"));
        assert!(out.contains("future_root = \"keep\""));
        assert!(out.contains("# dll note"));
        assert!(out.contains("allow_fp16 = true"));
        assert!(out.contains("# profile note"));
        assert!(out.contains("unknown_profile_key = 42"));
        assert!(out.contains("multiplier = 4 # trailing note"), "{out}");
        assert!(out.contains("active_in = [\"Game.exe\", \"New.exe\"]"));
        assert!(out.find("name = \"Alpha\"") < out.find("multiplier = 4"));
        assert!(out.contains("dll = \"C:/games/Lossless.dll\""));
    }

    #[test]
    fn global_table_is_created_only_when_the_dll_path_is_set() {
        let mut doc = parse("version = 2\n\n[[profile]]\nname = \"Alpha\"\n");
        write_global_dll(&mut doc, None);
        assert!(
            doc.get("global").is_none(),
            "clearing without a table creates nothing"
        );

        write_global_dll(&mut doc, Some("C:/Lossless.dll"));
        assert_eq!(
            doc["global"]["dll"].as_str(),
            Some("C:/Lossless.dll"),
            "setting a path creates the table"
        );

        write_global_dll(&mut doc, None);
        assert!(doc["global"]
            .as_table_like()
            .expect("table")
            .get("dll")
            .is_none());
    }

    fn ready(config: &Config) -> Settings {
        match config.view() {
            ConfigView::Ready(settings) => settings,
            other => panic!("expected ready view, got {other:?}"),
        }
    }

    #[test]
    fn missing_fp16_and_performance_keys_read_as_defaults() {
        let doc = parse("version = 2\n\n[[profile]]\nname = \"Bare\"\n");
        assert!(
            read_global_allow_fp16(&doc),
            "missing [global] reads as allowed"
        );
        assert!(
            !profiles(&doc).remove(0).performance_mode,
            "missing key reads as off"
        );

        let doc = parse(
            "version = 2\n\n[global]\nallow_fp16 = \"no\"\n\n[[profile]]\nname = \"Bare\"\nperformance_mode = 1\n",
        );
        assert!(read_global_allow_fp16(&doc), "non-boolean reads as allowed");
        assert!(
            !profiles(&doc).remove(0).performance_mode,
            "non-boolean reads as off"
        );
    }

    #[test]
    fn fp16_and_performance_mode_setters_round_trip() {
        let dir = temp_dir("toggles");
        let path = dir.join(FILE_NAME);
        fs::write(&path, BASE).expect("write fixture");
        let mut config = Config::open(Ok(path.clone()));
        let alpha = key(0, "Alpha");

        let settings = ready(&config);
        assert!(settings.allow_fp16, "BASE writes allow_fp16 = true");
        assert!(!settings.profiles[0].performance_mode);

        config.set_global_allow_fp16(false).expect("fp16 off saves");
        config
            .set_performance_mode(&alpha, true)
            .expect("performance mode on saves");
        let settings = ready(&config);
        assert!(!settings.allow_fp16);
        assert!(settings.profiles[0].performance_mode);

        config.set_global_allow_fp16(true).expect("fp16 on saves");
        config
            .set_performance_mode(&alpha, false)
            .expect("performance mode off saves");
        let settings = ready(&config);
        assert!(settings.allow_fp16);
        assert!(!settings.profiles[0].performance_mode);
        fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn fp16_setter_creates_global_when_the_file_has_none() {
        let dir = temp_dir("fp16-create");
        let path = dir.join(FILE_NAME);
        fs::write(&path, "version = 2\n\n[[profile]]\nname = \"Alpha\"\n").expect("write fixture");
        let mut config = Config::open(Ok(path.clone()));
        assert!(ready(&config).allow_fp16, "missing table reads as allowed");

        config.set_global_allow_fp16(false).expect("fp16 off saves");
        let saved = fs::read_to_string(&path).expect("read back");
        assert!(saved.contains("[global]\nallow_fp16 = false"), "{saved}");
        let settings = ready(&config);
        assert!(!settings.allow_fp16);
        assert_eq!(settings.profiles[0].key.name, "Alpha", "profile is kept");
        fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn new_edits_keep_comments_unknown_keys_and_order() {
        let text = "# top comment\nversion = 2\nfuture_root = \"keep\"\n\n[global]\n# fp16 note\nallow_fp16 = true # trailing fp16 note\nfuture_global = 7\n\n[[profile]]\n# profile note\nname = \"Alpha\"\nunknown_profile_key = 42\nperformance_mode = false # trailing performance note\nmultiplier = 3 # trailing note\nactive_in = [\"Game.exe\"]\n";
        let dir = temp_dir("keep-comments");
        let path = dir.join(FILE_NAME);
        fs::write(&path, text).expect("write fixture");
        let mut config = Config::open(Ok(path.clone()));

        config.set_global_allow_fp16(false).expect("fp16 off saves");
        config
            .set_performance_mode(&key(0, "Alpha"), true)
            .expect("performance mode on saves");
        let out = fs::read_to_string(&path).expect("read back");

        assert!(out.contains("# top comment"));
        assert!(out.contains("future_root = \"keep\""));
        assert!(out.contains("# fp16 note"));
        assert!(
            out.contains("allow_fp16 = false # trailing fp16 note"),
            "{out}"
        );
        assert!(out.contains("future_global = 7"));
        assert!(out.contains("# profile note"));
        assert!(out.contains("unknown_profile_key = 42"));
        assert!(
            out.contains("performance_mode = true # trailing performance note"),
            "{out}"
        );
        assert!(out.contains("multiplier = 3 # trailing note"));
        assert!(out.find("[global]") < out.find("[[profile]]"));
        assert!(out.find("name = \"Alpha\"") < out.find("performance_mode = true"));
        fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn new_profile_has_the_specified_defaults() {
        let mut doc = default_document();
        let profiles_mut = ensure_profiles(&mut doc).expect("profiles");
        profiles_mut.push(new_profile("Fresh"));
        let fresh = read_profiles(&doc).remove(1);
        assert_eq!(fresh.key.name, "Fresh");
        assert!(fresh.active_in.is_empty());
        assert_eq!(fresh.mode, Mode::Fixed(2));
        assert_eq!(fresh.base_fps_cap, 0);
        assert!((fresh.flow_scale - 0.8).abs() < f64::EPSILON);
        let text = doc.to_string();
        let order = [
            "name",
            "active_in",
            "frame_generation_enabled",
            "multiplier",
            "adaptive",
            "flow_scale",
            "base_fps_cap",
        ];
        let positions: Vec<usize> = order
            .iter()
            .map(|k| text.find(&format!("{k} =")).expect("key written"))
            .collect();
        assert!(positions.windows(2).all(|pair| pair[0] < pair[1]), "{text}");
    }

    #[test]
    fn default_document_is_version_two_with_one_default_profile() {
        let text = default_document().to_string();
        let doc = parse_document(&text).expect("default document is valid");
        let list = read_profiles(&doc);
        assert_eq!(list.len(), 1);
        assert_eq!(list[0].key.name, "Default");
        assert!(text.starts_with("version = 2"));
    }

    #[test]
    fn last_profile_cannot_be_deleted() {
        let mut doc = parse(BASE);
        assert_eq!(
            remove_profile_in(&mut doc, &key(0, "Alpha")),
            Err("The last profile cannot be deleted.".to_owned())
        );
        assert_eq!(profiles(&doc).len(), 1);

        let mut doc =
            parse("version = 2\n\n[[profile]]\nname = \"One\"\n\n[[profile]]\nname = \"Two\"\n");
        remove_profile_in(&mut doc, &key(0, "One")).expect("second profile can be removed");
        let remaining = profiles(&doc);
        assert_eq!(remaining.len(), 1);
        assert_eq!(remaining[0].key.name, "Two");
    }

    #[test]
    fn stale_profile_keys_are_refused_before_any_change() {
        let mut doc = parse(BASE);
        let stale = key(0, "Wrong");
        assert_eq!(verified_index(&doc, &stale), Err(STALE_PROFILE.to_owned()));
        assert!(rename_profile_in(&mut doc, &stale, "Renamed").is_err());
        assert!(remove_profile_in(&mut doc, &stale).is_err());
        assert_eq!(profiles(&doc)[0].key.name, "Alpha", "document is unchanged");
    }

    #[test]
    fn rename_and_add_refuse_duplicate_names() {
        let mut doc =
            parse("version = 2\n\n[[profile]]\nname = \"One\"\n\n[[profile]]\nname = \"Two\"\n");
        assert_eq!(
            rename_profile_in(&mut doc, &key(0, "One"), "Two"),
            Err(taken("Two"))
        );
        rename_profile_in(&mut doc, &key(0, "One"), "One")
            .expect("keeping the same name is allowed");
        rename_profile_in(&mut doc, &key(1, "Two"), "Three").expect("unique rename succeeds");
        assert_eq!(append_profile(&mut doc, "One"), Err(taken("One")));
        assert_eq!(append_profile(&mut doc, "Four"), Ok(2));
        let names: Vec<String> = profiles(&doc).into_iter().map(|p| p.key.name).collect();
        assert_eq!(names, ["One", "Three", "Four"]);
    }

    #[test]
    fn unsupported_or_malformed_roots_are_rejected() {
        assert!(parse_document("version = 1\n").is_err());
        assert!(parse_document("[[profile]]\nname = \"x\"\n").is_err());
        assert!(parse_document("version = 2\nprofile = [1, 2]\n").is_err());
        assert!(parse_document("version = 2\nglobal = 5\n").is_err());
        assert!(parse_document("version = 2\n").is_ok());
    }

    #[test]
    fn flow_scale_reads_integers_and_floats() {
        let doc = parse("version = 2\n\n[[profile]]\nname = \"Whole\"\nflow_scale = 1\n");
        assert!((profiles(&doc).remove(0).flow_scale - 1.0).abs() < f64::EPSILON);
    }

    #[test]
    fn resolve_path_prefers_the_override_then_xdg_then_home() {
        let env = |pairs: &[(&str, &str)]| {
            let map: HashMap<String, OsString> = pairs
                .iter()
                .map(|(k, v)| ((*k).to_owned(), OsString::from(v)))
                .collect();
            move |key: &str| map.get(key).cloned()
        };
        assert_eq!(
            resolve_path(env(&[("FGVK_CONFIG", "/x/y.toml"), ("HOME", "/h")])),
            Ok(PathBuf::from("/x/y.toml"))
        );
        assert_eq!(
            resolve_path(env(&[("XDG_CONFIG_HOME", "/xdg"), ("HOME", "/h")])),
            Ok(PathBuf::from("/xdg/framegen-vulkan/conf.toml"))
        );
        assert_eq!(
            resolve_path(env(&[("XDG_CONFIG_HOME", "relative"), ("HOME", "/h")])),
            Ok(PathBuf::from("/h/.config/framegen-vulkan/conf.toml"))
        );
        assert!(resolve_path(env(&[])).is_err());
    }

    #[test]
    fn atomic_save_leaves_no_tmp_file_behind() {
        let dir = temp_dir("atomic");
        let path = dir.join(FILE_NAME);
        fs::write(&path, BASE).expect("write fixture");

        let mut config = Config::open(Ok(path.clone()));
        config
            .set_mode(&key(0, "Alpha"), Mode::Fixed(5))
            .expect("save succeeds");

        let saved = fs::read_to_string(&path).expect("read back");
        assert!(saved.contains("multiplier = 5"));
        assert!(!dir.join("conf.toml.tmp").exists(), "no .tmp left behind");
        fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn unparseable_file_is_never_overwritten() {
        let dir = temp_dir("invalid");
        let path = dir.join(FILE_NAME);
        let broken = "version = 2\n[[profile]\nname = \"oops\"\n";
        fs::write(&path, broken).expect("write fixture");

        let mut config = Config::open(Ok(path.clone()));
        assert!(matches!(config.view(), ConfigView::Invalid(_)));
        assert!(config.set_mode(&key(0, "oops"), Mode::Adaptive).is_err());
        assert!(
            config.create_default().is_err(),
            "create refuses to replace a file"
        );
        assert!(config.set_global_dll(Some("C:/x.dll")).is_err());
        assert!(config.set_global_allow_fp16(false).is_err());
        assert!(config.set_performance_mode(&key(0, "oops"), true).is_err());

        assert_eq!(fs::read_to_string(&path).expect("read back"), broken);
        assert!(!dir.join("conf.toml.tmp").exists());
        fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn missing_file_is_created_with_the_default_profile() {
        let dir = temp_dir("create");
        let path = dir.join("nested").join(FILE_NAME);
        let mut config = Config::open(Ok(path.clone()));
        assert!(matches!(config.view(), ConfigView::Missing(_)));
        config.create_default().expect("create succeeds");
        match config.view() {
            ConfigView::Ready(settings) => assert_eq!(settings.profiles[0].key.name, "Default"),
            other => panic!("expected ready view, got {other:?}"),
        }
        fs::remove_dir_all(&dir).ok();
    }
}
