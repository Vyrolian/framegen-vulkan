//! Reads and writes `ui.toml`, the window's own choices: the theme and the Catppuccin accent.
//!
//! The file sits beside `conf.toml` but the layer never reads it, so it is not checked against the
//! layer's rules. A write changes the two choice keys only: comments, other keys and their order
//! survive, and the file is replaced atomically. A file that does not parse is reported and never
//! written.

use std::ffi::OsString;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};

use toml_edit::{value, Document, DocumentMut, TomlError};

use crate::config::{self, located, write_atomically};

const APP_DIR: &str = "framegen-vulkan";
const FILE_NAME: &str = "ui.toml";
const THEME_KEY: &str = "theme";
const ACCENT_KEY: &str = "catppuccin_accent";

/// The saved choices as text. The window decides what the names mean.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Choice {
    pub theme: Option<String>,
    pub accent: Option<String>,
}

/// Chooses `ui.toml` from an environment lookup: `$XDG_CONFIG_HOME`, else `$HOME/.config`, then
/// `framegen-vulkan/ui.toml`. Empty values are absent.
pub fn resolve_path(lookup: impl Fn(&str) -> Option<OsString>) -> Result<PathBuf, String> {
    if let Some(dir) = config::non_empty(lookup("XDG_CONFIG_HOME")) {
        return Ok(dir.join(APP_DIR).join(FILE_NAME));
    }
    match config::non_empty(lookup("HOME")) {
        Some(home) => Ok(home.join(".config").join(APP_DIR).join(FILE_NAME)),
        None => Err("Set XDG_CONFIG_HOME or HOME to locate ui.toml.".to_owned()),
    }
}

/// Reads the two choice keys. Each must be a quoted string when present.
pub fn parse(text: &str) -> Result<Choice, String> {
    let document = Document::parse(text)
        .map_err(|error: TomlError| located(text, error.span(), error.message()))?;
    Ok(Choice {
        theme: string_key(&document, text, THEME_KEY)?,
        accent: string_key(&document, text, ACCENT_KEY)?,
    })
}

fn string_key(document: &Document<&str>, text: &str, key: &str) -> Result<Option<String>, String> {
    let Some(item) = document.get(key) else {
        return Ok(None);
    };
    match item.as_str() {
        Some(name) => Ok(Some(name.to_owned())),
        None => Err(located(
            text,
            item.span(),
            &format!("{key} must be a quoted string."),
        )),
    }
}

/// Reads `ui.toml`. A missing file holds no choices. An error names the file and the line.
pub fn load(path: &Path) -> Result<Choice, String> {
    match fs::read_to_string(path) {
        Ok(text) => parse(&text).map_err(|message| format!("{}: {message}", path.display())),
        Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(Choice::default()),
        Err(error) => Err(format!("Cannot read {}: {error}", path.display())),
    }
}

/// Saves the theme name and the accent key. Other keys in an existing file are kept. A file that
/// does not parse is refused and left unchanged.
pub fn save(path: &Path, theme: &str, accent: &str) -> Result<(), String> {
    let mut document = match fs::read_to_string(path) {
        Ok(text) => text.parse::<DocumentMut>().map_err(|error| {
            format!(
                "{}: {}. The file was not changed.",
                path.display(),
                located(&text, error.span(), error.message())
            )
        })?,
        Err(error) if error.kind() == io::ErrorKind::NotFound => DocumentMut::new(),
        Err(error) => return Err(format!("Cannot read {}: {error}", path.display())),
    };
    document.insert(THEME_KEY, value(theme));
    document.insert(ACCENT_KEY, value(accent));

    if let Some(dir) = path.parent() {
        fs::create_dir_all(dir)
            .map_err(|error| format!("Cannot create {}: {error}", dir.display()))?;
    }
    write_atomically(path, &document.to_string())
        .map_err(|error| format!("Cannot save {}: {error}", path.display()))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashMap;
    use std::process;
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
            std::env::temp_dir().join(format!("fgvk-ui-conf-{tag}-{}-{nanos}", process::id()));
        fs::create_dir_all(&dir).expect("create test directory");
        dir
    }

    #[test]
    fn resolve_path_uses_xdg_config_home_then_home() {
        assert_eq!(
            resolve_path(env(&[("XDG_CONFIG_HOME", "/xdg"), ("HOME", "/h")])),
            Ok(PathBuf::from("/xdg/framegen-vulkan/ui.toml"))
        );
        assert_eq!(
            resolve_path(env(&[("XDG_CONFIG_HOME", ""), ("HOME", "/h")])),
            Ok(PathBuf::from("/h/.config/framegen-vulkan/ui.toml")),
            "an empty XDG_CONFIG_HOME is ignored"
        );
        assert!(resolve_path(env(&[])).is_err());
    }

    #[test]
    fn parse_reads_both_choices_and_ignores_other_keys() {
        let choice = parse("# mine\ntheme = \"Nord\"\ncatppuccin_accent = \"teal\"\nextra = 1\n")
            .expect("valid file");
        assert_eq!(choice.theme.as_deref(), Some("Nord"));
        assert_eq!(choice.accent.as_deref(), Some("teal"));
        assert_eq!(parse("").expect("empty file"), Choice::default());
    }

    #[test]
    fn parse_names_the_line_of_a_wrong_type_or_syntax_error() {
        let wrong_type = parse("theme = \"Nord\"\ncatppuccin_accent = 3\n").expect_err("number");
        assert!(wrong_type.starts_with("line 2:"), "{wrong_type}");
        assert!(wrong_type.contains("catppuccin_accent must be a quoted string"));

        let syntax = parse("theme = \"Nord\"\ntheme = = 2\n").expect_err("syntax");
        assert!(syntax.starts_with("line 2:"), "{syntax}");
    }

    #[test]
    fn missing_file_holds_no_choices() {
        let dir = temp_dir("missing");
        assert_eq!(load(&dir.join(FILE_NAME)), Ok(Choice::default()));
        fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn save_then_load_round_trips_theme_and_accent() {
        let dir = temp_dir("round-trip");
        let path = dir.join("nested").join(FILE_NAME);

        save(&path, "Catppuccin Frappé", "teal").expect("save creates the directory");
        assert_eq!(
            fs::read_to_string(&path).expect("read back"),
            "theme = \"Catppuccin Frappé\"\ncatppuccin_accent = \"teal\"\n"
        );
        assert_eq!(
            load(&path),
            Ok(Choice {
                theme: Some("Catppuccin Frappé".to_owned()),
                accent: Some("teal".to_owned()),
            })
        );

        save(&path, "Nord", "mauve").expect("second save replaces both keys");
        let choice = load(&path).expect("load");
        assert_eq!(choice.theme.as_deref(), Some("Nord"));
        assert_eq!(choice.accent.as_deref(), Some("mauve"));
        fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn save_keeps_other_keys_and_comments() {
        let dir = temp_dir("preserve");
        let path = dir.join(FILE_NAME);
        fs::write(
            &path,
            "# window choices\nscale = 1.5\ntheme = \"Dracula\"\n[section]\nkey = true\n",
        )
        .expect("write fixture");

        save(&path, "Nord", "rosewater").expect("save");
        assert_eq!(
            fs::read_to_string(&path).expect("read back"),
            "# window choices\nscale = 1.5\ntheme = \"Nord\"\ncatppuccin_accent = \"rosewater\"\n[section]\nkey = true\n"
        );
        fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn invalid_file_is_refused_and_never_overwritten() {
        let dir = temp_dir("invalid");
        let path = dir.join(FILE_NAME);
        let broken = "theme = = \"Nord\"\n";
        fs::write(&path, broken).expect("write fixture");

        let refused = save(&path, "Nord", "mauve").expect_err("refused");
        assert!(refused.contains("not changed"), "{refused}");
        assert!(refused.contains("line 1"), "{refused}");
        assert_eq!(fs::read_to_string(&path).expect("read back"), broken);
        assert!(!dir.join("ui.toml.tmp").exists());
        fs::remove_dir_all(&dir).ok();
    }
}
