//! Remembers the selected profile between launches. The name lives in its own small file, so
//! `conf.toml` is never touched for it.

use std::ffi::OsString;
use std::fs::{self, File, TryLockError};
use std::io::{self, Write};
use std::os::unix::fs::OpenOptionsExt;
use std::path::{Path, PathBuf};
use std::process;

use crate::config;

const APP_DIR: &str = "framegen-vulkan";
const FILE_NAME: &str = "fgvk-ui-state";
const WINDOW_LOCK: &str = "fgvk-ui.lock";

/// Hold the returned file open until the settings window closes. The kernel releases the
/// lock on exit, including after a crash. Keep the lock file so contenders share one inode.
pub fn lock_window(directory: &Path) -> io::Result<Option<File>> {
    fs::create_dir_all(directory)?;
    let lock = File::options()
        .write(true)
        .create(true)
        .truncate(false)
        .mode(0o600)
        .open(directory.join(WINDOW_LOCK))?;
    match lock.try_lock() {
        Ok(()) => Ok(Some(lock)),
        Err(TryLockError::WouldBlock) => Ok(None),
        Err(TryLockError::Error(error)) => Err(error),
    }
}

pub fn window_lock_directory(lookup: impl Fn(&str) -> Option<OsString>) -> Option<PathBuf> {
    config::non_empty(lookup("XDG_RUNTIME_DIR"))
        .filter(|dir| dir.is_absolute())
        .map(|dir| dir.join(APP_DIR))
        .or_else(|| resolve_path(lookup).and_then(|path| path.parent().map(Path::to_path_buf)))
}

/// Chooses the state file from an environment lookup.
///
/// `$XDG_STATE_HOME` wins when it is absolute, as the XDG specification requires. Otherwise the
/// file lives under `$HOME/.local/state`. Returns `None` when neither variable is usable.
pub fn resolve_path(lookup: impl Fn(&str) -> Option<OsString>) -> Option<PathBuf> {
    if let Some(dir) = config::non_empty(lookup("XDG_STATE_HOME")).filter(|dir| dir.is_absolute()) {
        return Some(dir.join(APP_DIR).join(FILE_NAME));
    }
    config::non_empty(lookup("HOME")).map(|home| {
        home.join(".local")
            .join("state")
            .join(APP_DIR)
            .join(FILE_NAME)
    })
}

/// Reads the remembered name from the first line. A missing or unreadable file, or an empty one,
/// gives `None`. The line break that ends the line is not part of the name.
pub fn load(path: &Path) -> Option<String> {
    let text = fs::read_to_string(path).ok()?;
    text.lines()
        .next()
        .filter(|name| !name.is_empty())
        .map(str::to_owned)
}

/// Writes the name and a trailing newline. The text goes to a temporary file beside the target,
/// which is then renamed over it, so a reader sees either the old name or the new one. Missing
/// parent directories are created.
pub fn save(path: &Path, name: &str) -> Result<(), String> {
    if name.contains(['\n', '\r']) {
        return Err("A profile name with a line break cannot be remembered.".to_owned());
    }
    let Some(dir) = path.parent() else {
        return Err(format!("{} has no directory.", path.display()));
    };
    fs::create_dir_all(dir).map_err(|error| format!("Cannot create {}: {error}", dir.display()))?;

    // The process id keeps two windows from writing the same temporary file.
    let tmp = path.with_file_name(format!("{FILE_NAME}.{}.tmp", process::id()));
    let outcome = write_and_rename(&tmp, path, name);
    if outcome.is_err() {
        let _ = fs::remove_file(&tmp);
    }
    outcome.map_err(|error| format!("Cannot write {}: {error}", path.display()))
}

fn write_and_rename(tmp: &Path, target: &Path, name: &str) -> io::Result<()> {
    {
        let mut file = File::create(tmp)?;
        writeln!(file, "{name}")?;
        file.sync_all()?;
    }
    fs::rename(tmp, target)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashMap;
    use std::time::{SystemTime, UNIX_EPOCH};

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
            std::env::temp_dir().join(format!("fgvk-state-test-{tag}-{}-{nanos}", process::id()));
        fs::create_dir_all(&dir).expect("create test directory");
        dir
    }

    #[test]
    fn resolve_path_uses_xdg_state_home_when_it_is_absolute() {
        assert_eq!(
            resolve_path(env(&[("XDG_STATE_HOME", "/xs"), ("HOME", "/h")])),
            Some(PathBuf::from("/xs/framegen-vulkan/fgvk-ui-state"))
        );
    }

    #[test]
    fn resolve_path_falls_back_to_home_local_state() {
        assert_eq!(
            resolve_path(env(&[("HOME", "/h")])),
            Some(PathBuf::from(
                "/h/.local/state/framegen-vulkan/fgvk-ui-state"
            ))
        );
        assert_eq!(
            resolve_path(env(&[("XDG_STATE_HOME", ""), ("HOME", "/h")])),
            Some(PathBuf::from(
                "/h/.local/state/framegen-vulkan/fgvk-ui-state"
            )),
            "an empty XDG_STATE_HOME is ignored"
        );
        assert_eq!(
            resolve_path(env(&[("XDG_STATE_HOME", "relative"), ("HOME", "/h")])),
            Some(PathBuf::from(
                "/h/.local/state/framegen-vulkan/fgvk-ui-state"
            )),
            "a relative XDG_STATE_HOME is ignored"
        );
    }

    #[test]
    fn resolve_path_is_none_when_neither_variable_is_set() {
        assert_eq!(resolve_path(env(&[])), None);
        assert_eq!(resolve_path(env(&[("HOME", "")])), None);
    }

    #[test]
    fn save_then_load_round_trips_through_a_missing_directory() {
        let dir = temp_dir("round-trip");
        let path = dir.join("nested").join(FILE_NAME);

        save(&path, "Gamma Rig").expect("save creates the parent directory");
        assert_eq!(
            fs::read_to_string(&path).expect("read back"),
            "Gamma Rig\n",
            "one line with a trailing newline"
        );
        assert_eq!(load(&path), Some("Gamma Rig".to_owned()));

        let entries: Vec<_> = fs::read_dir(dir.join("nested"))
            .expect("list directory")
            .map(|entry| entry.expect("entry").file_name())
            .collect();
        assert_eq!(
            entries,
            [OsString::from(FILE_NAME)],
            "no temporary file left"
        );

        fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn save_replaces_the_previous_name() {
        let dir = temp_dir("replace");
        let path = dir.join(FILE_NAME);
        save(&path, "First").expect("first save");
        save(&path, "Second").expect("second save");
        assert_eq!(load(&path), Some("Second".to_owned()));
        fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn missing_or_unreadable_state_loads_as_none() {
        let dir = temp_dir("missing");
        assert_eq!(load(&dir.join(FILE_NAME)), None, "missing file");
        assert_eq!(load(&dir), None, "a directory cannot be read as a file");
        fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn empty_state_loads_as_none() {
        let dir = temp_dir("empty");
        let path = dir.join(FILE_NAME);
        fs::write(&path, "").expect("write empty fixture");
        assert_eq!(load(&path), None);
        fs::write(&path, "\n").expect("write newline-only fixture");
        assert_eq!(load(&path), None);
        fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn load_keeps_the_name_exactly_apart_from_its_line_break() {
        let dir = temp_dir("exact");
        let path = dir.join(FILE_NAME);
        fs::write(&path, "Alpha\r\n").expect("write CRLF fixture");
        assert_eq!(load(&path), Some("Alpha".to_owned()));
        fs::write(&path, "Alpha ").expect("write fixture without newline");
        assert_eq!(load(&path), Some("Alpha ".to_owned()));
        fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn names_with_line_breaks_are_refused_without_writing() {
        let dir = temp_dir("breaks");
        let path = dir.join(FILE_NAME);
        assert!(save(&path, "Two\nLines").is_err());
        assert!(!path.exists());
        fs::remove_dir_all(&dir).ok();
    }
}
