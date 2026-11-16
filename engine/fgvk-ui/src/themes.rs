//! The themes the window can show: the built-in palettes from `theme.rs`, the `*.toml` files in the
//! themes folder, and the choice saved in `ui.toml`. This module reads the files. `theme.rs` builds
//! the palettes.
//!
//! A theme file has these keys. `name` is required and is the name the window lists. `base` names a
//! built-in theme to inherit from; it defaults to Catppuccin Mocha. `dark` defaults to the base's
//! setting. The colour keys take `"#rrggbb"` or `"#rrggbbaa"`: `background` (the palette's `base`),
//! `mantle`, `crust`, `surface0` to `surface2`, `overlay0`, `text`, `subtext0`, `error`, `success`,
//! `stripe` and `accent`. `stripe` defaults to `surface0` at half opacity. `accent` fixes the accent
//! colour. `catppuccin_accent` names one of the fourteen Catppuccin accents, for a Catppuccin base.
//!
//! A file that fails is skipped with a note naming the file and the line. An unknown key is ignored
//! with a note. Names are unique: a name already listed gets " (custom)" appended.

use std::ffi::OsString;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};

use egui::Color32;
use toml_edit::{Document, TomlError, Value};

use crate::config::{self, located};
use crate::theme::{self, Accent, AccentRule, Flavour, Palette, Theme};
use crate::ui_conf;

const APP_DIR: &str = "framegen-vulkan";
const DIR_NAME: &str = "themes";
const EXTENSION: &str = "toml";

/// A line for the Appearance section. `problem` marks a skipped file or an unreadable setting.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Note {
    pub text: String,
    pub problem: bool,
}

impl Note {
    fn info(text: String) -> Self {
        Self {
            text,
            problem: false,
        }
    }

    fn problem(text: String) -> Self {
        Self {
            text,
            problem: true,
        }
    }
}

/// A theme file that was read. `notes` lists the ignored keys, each with its line.
#[derive(Debug)]
pub struct Parsed {
    pub theme: Theme,
    pub notes: Vec<String>,
}

/// One top-level key of a theme file, with its value.
struct Entry<'a> {
    key: &'a str,
    value: &'a Value,
}

/// Chooses the themes folder from an environment lookup: `$XDG_CONFIG_HOME`, else `$HOME/.config`,
/// then `framegen-vulkan/themes`. Empty values are absent.
pub fn resolve_dir(lookup: impl Fn(&str) -> Option<OsString>) -> Result<PathBuf, String> {
    if let Some(dir) = config::non_empty(lookup("XDG_CONFIG_HOME")) {
        return Ok(dir.join(APP_DIR).join(DIR_NAME));
    }
    match config::non_empty(lookup("HOME")) {
        Some(home) => Ok(home.join(".config").join(APP_DIR).join(DIR_NAME)),
        None => Err("Set XDG_CONFIG_HOME or HOME to locate the themes folder.".to_owned()),
    }
}

/// Reads one theme file. An error names the first problem and its line.
pub fn parse(text: &str) -> Result<Parsed, String> {
    // The immutable document keeps each value's span, which the error lines come from.
    let document = Document::parse(text)
        .map_err(|error: TomlError| located(text, error.span(), error.message()))?;
    let mut entries = Vec::new();
    for (key, item) in document.as_table().iter() {
        match item.as_value() {
            Some(value) => entries.push(Entry { key, value }),
            None => {
                return Err(located(
                    text,
                    item.span(),
                    &format!("\"{key}\" must be a value, not a table."),
                ))
            }
        }
    }
    let find = |wanted: &str| entries.iter().find(|entry| entry.key == wanted);

    let name = match find("name") {
        Some(entry) => match entry.value.as_str().map(str::trim) {
            Some(name) if !name.is_empty() => name.to_owned(),
            _ => {
                return Err(located(
                    text,
                    entry.value.span(),
                    "name must be a non-empty string.",
                ))
            }
        },
        None => return Err("name is missing. Add name = \"Your theme\".".to_owned()),
    };

    let base = match find("base") {
        None => theme::catppuccin_theme(theme::MOCHA),
        Some(entry) => {
            let Some(base_name) = entry.value.as_str() else {
                return Err(located(
                    text,
                    entry.value.span(),
                    "base must be the quoted name of a built-in theme.",
                ));
            };
            match theme::builtin(base_name) {
                Some(base) => base,
                None => {
                    return Err(located(
                        text,
                        entry.value.span(),
                        &format!(
                            "\"{base_name}\" is not a built-in theme name. The background colour is the background key."
                        ),
                    ))
                }
            }
        }
    };

    let mut palette = base.palette(Accent::DEFAULT);
    let mut accent_set = false;
    let mut stripe_set = false;
    let mut pinned: Option<(Accent, &Entry)> = None;
    let mut notes = Vec::new();
    for entry in &entries {
        match entry.key {
            "name" | "base" => {}
            "dark" => {
                palette.dark = entry.value.as_bool().ok_or_else(|| {
                    located(text, entry.value.span(), "dark must be true or false.")
                })?;
            }
            "catppuccin_accent" => {
                let accent = entry
                    .value
                    .as_str()
                    .and_then(Accent::from_key)
                    .ok_or_else(|| {
                        located(
                            text,
                            entry.value.span(),
                            &format!(
                                "catppuccin_accent must be one of: {}.",
                                Accent::ALL.map(Accent::key).join(", ")
                            ),
                        )
                    })?;
                pinned = Some((accent, entry));
            }
            key => match role_slot(&mut palette, key) {
                Some(slot) => {
                    *slot = colour(text, entry)?;
                    accent_set |= key == "accent";
                    stripe_set |= key == "stripe";
                }
                None => notes.push(located(
                    text,
                    entry.value.span(),
                    &format!("unknown key \"{key}\" is ignored."),
                )),
            },
        }
    }

    let catppuccin = matches!(base.rule(), AccentRule::Catppuccin { .. });
    if let Some((_, entry)) = pinned {
        if !catppuccin {
            notes.push(located(
                text,
                entry.value.span(),
                "catppuccin_accent is ignored: base is not a Catppuccin flavour.",
            ));
        } else if accent_set {
            notes.push(located(
                text,
                entry.value.span(),
                "catppuccin_accent is ignored: accent is set.",
            ));
        }
    }
    let rule = match *base.rule() {
        AccentRule::Catppuccin { flavour, .. } if !accent_set => AccentRule::Catppuccin {
            flavour,
            pinned: pinned.map(|(accent, _)| accent),
        },
        _ => AccentRule::Own,
    };
    if !stripe_set {
        palette.stripe = theme::striped(palette.surface0);
    }
    Ok(Parsed {
        theme: Theme::new(name, palette, rule),
        notes,
    })
}

/// The palette field a colour key sets, or `None` for a key that is not a colour.
fn role_slot<'a>(palette: &'a mut Palette, key: &str) -> Option<&'a mut Color32> {
    Some(match key {
        "background" => &mut palette.base,
        "mantle" => &mut palette.mantle,
        "crust" => &mut palette.crust,
        "surface0" => &mut palette.surface0,
        "surface1" => &mut palette.surface1,
        "surface2" => &mut palette.surface2,
        "overlay0" => &mut palette.overlay0,
        "text" => &mut palette.text,
        "subtext0" => &mut palette.subtext0,
        "accent" => &mut palette.accent,
        "error" => &mut palette.error,
        "success" => &mut palette.success,
        "stripe" => &mut palette.stripe,
        _ => return None,
    })
}

fn colour(text: &str, entry: &Entry) -> Result<Color32, String> {
    entry.value.as_str().and_then(parse_colour).ok_or_else(|| {
        located(
            text,
            entry.value.span(),
            &format!("{} must be a colour such as \"#rrggbb\".", entry.key),
        )
    })
}

/// `#rrggbb`, or `#rrggbbaa` for a colour with transparency.
fn parse_colour(text: &str) -> Option<Color32> {
    let digits = text.strip_prefix('#')?;
    if !digits.bytes().all(|byte| byte.is_ascii_hexdigit()) {
        return None;
    }
    let value = u32::from_str_radix(digits, 16).ok()?;
    match digits.len() {
        6 => Some(theme::hex(value)),
        8 => Some(Color32::from_rgba_unmultiplied(
            (value >> 24) as u8,
            (value >> 16) as u8,
            (value >> 8) as u8,
            value as u8,
        )),
        _ => None,
    }
}

/// `name` when no listed theme has it, else `name (custom)`, then `name (custom 2)` and so on.
fn unique_name(name: &str, taken: &[String]) -> String {
    let free = |candidate: &str| taken.iter().all(|listed| listed != candidate);
    if free(name) {
        return name.to_owned();
    }
    let first = format!("{name} (custom)");
    if free(&first) {
        return first;
    }
    (2..)
        .map(|number| format!("{name} (custom {number})"))
        .find(|candidate| free(candidate.as_str()))
        .unwrap_or(first)
}

/// Reads every `*.toml` file in `folder`, in file-name order. A file that cannot be read or parsed is
/// skipped with a note. A missing folder is not an error: it simply holds no themes.
fn scan(folder: &Path, builtin: &[Theme]) -> (Vec<Theme>, Vec<Note>) {
    let mut themes = Vec::new();
    let mut notes = Vec::new();
    let entries = match fs::read_dir(folder) {
        Ok(entries) => entries,
        Err(error) if error.kind() == io::ErrorKind::NotFound => return (themes, notes),
        Err(error) => {
            notes.push(Note::problem(format!(
                "Cannot read {}: {error}",
                folder.display()
            )));
            return (themes, notes);
        }
    };
    let mut files: Vec<PathBuf> = entries
        .filter_map(Result::ok)
        .map(|entry| entry.path())
        .filter(|path| {
            path.extension()
                .is_some_and(|extension| extension == EXTENSION)
                && path.is_file()
        })
        .collect();
    files.sort();

    let mut taken: Vec<String> = builtin.iter().map(|theme| theme.name.clone()).collect();
    for path in files {
        let file = path.file_name().map_or_else(
            || path.display().to_string(),
            |name| name.to_string_lossy().into_owned(),
        );
        let text = match fs::read_to_string(&path) {
            Ok(text) => text,
            Err(error) => {
                notes.push(Note::problem(format!(
                    "{file}: cannot be read ({error}). The file is skipped."
                )));
                continue;
            }
        };
        match parse(&text) {
            Ok(parsed) => {
                let mut theme = parsed.theme;
                let name = unique_name(&theme.name, &taken);
                if name != theme.name {
                    notes.push(Note::info(format!(
                        "{file}: \"{}\" is already a theme name; it is listed as \"{name}\".",
                        theme.name
                    )));
                    theme.name = name;
                }
                for note in parsed.notes {
                    notes.push(Note::info(format!("{file}: {note}")));
                }
                taken.push(theme.name.clone());
                themes.push(theme);
            }
            Err(reason) => notes.push(Note::problem(format!(
                "{file}: {reason} The file is skipped."
            ))),
        }
    }
    (themes, notes)
}

/// Every theme the window can show, the one in use, and the Catppuccin accent.
pub struct Catalog {
    themes: Vec<Theme>,
    selected: usize,
    accent: Accent,
    notes: Vec<Note>,
    folder: Result<PathBuf, String>,
    ui_path: Result<PathBuf, String>,
}

impl Catalog {
    /// Lists the built-in themes and the files in `folder`, then selects the theme and accent saved in
    /// `ui_path`. A saved name that is not listed falls back to the default, with a note.
    pub fn open(folder: Result<PathBuf, String>, ui_path: Result<PathBuf, String>) -> Self {
        let mut catalog = Self {
            themes: Vec::new(),
            selected: 0,
            accent: Accent::DEFAULT,
            notes: Vec::new(),
            folder,
            ui_path,
        };
        catalog.list();

        let loaded = match &catalog.ui_path {
            Ok(path) => ui_conf::load(path),
            Err(_) => Ok(ui_conf::Choice::default()),
        };
        let saved = match loaded {
            Ok(choice) => choice,
            Err(message) => {
                catalog.notes.push(Note::problem(format!(
                    "{message} The default theme is used."
                )));
                ui_conf::Choice::default()
            }
        };
        catalog.apply_saved(&saved);
        catalog
    }

    /// Rebuilds the list from the built-in themes and the folder. The default is selected.
    fn list(&mut self) {
        let builtin = theme::builtin_themes();
        let (custom, notes) = match &self.folder {
            Ok(folder) => scan(folder, &builtin),
            Err(_) => (Vec::new(), Vec::new()),
        };
        self.themes = builtin;
        self.themes.extend(custom);
        self.notes = notes;
        self.selected = self.default_index();
    }

    fn apply_saved(&mut self, saved: &ui_conf::Choice) {
        if let Some(name) = &saved.theme {
            match self.themes.iter().position(|theme| &theme.name == name) {
                Some(index) => self.selected = index,
                None => self.notes.push(Note::info(format!(
                    "Saved theme \"{name}\" is not available; using {}.",
                    theme::DEFAULT_THEME
                ))),
            }
        }
        if let Some(key) = &saved.accent {
            match Accent::from_key(key) {
                Some(accent) => self.accent = accent,
                None => self.notes.push(Note::info(format!(
                    "Saved Catppuccin accent \"{key}\" is not one of the fourteen; using {}.",
                    Accent::DEFAULT.label()
                ))),
            }
        }
    }

    /// Rereads the themes folder. The theme in use stays selected when it is still listed.
    pub fn reload(&mut self) {
        let name = self.current().name.clone();
        self.list();
        match self.themes.iter().position(|theme| theme.name == name) {
            Some(index) => self.selected = index,
            None => self.notes.push(Note::info(format!(
                "\"{name}\" is no longer listed; using {}.",
                theme::DEFAULT_THEME
            ))),
        }
    }

    fn default_index(&self) -> usize {
        self.themes
            .iter()
            .position(|theme| theme.name == theme::DEFAULT_THEME)
            .unwrap_or(0)
    }

    pub fn themes(&self) -> &[Theme] {
        &self.themes
    }

    pub fn selected(&self) -> usize {
        self.selected
    }

    /// The theme in use. The selected index always names a listed theme.
    pub fn current(&self) -> &Theme {
        &self.themes[self.selected]
    }

    pub fn accent(&self) -> Accent {
        self.accent
    }

    /// The palette the window should draw with now.
    pub fn palette(&self) -> Palette {
        self.current().palette(self.accent)
    }

    /// The Catppuccin flavour behind the theme in use, when it has one.
    pub fn flavour(&self) -> Option<Flavour> {
        self.current().flavour()
    }

    pub fn select(&mut self, index: usize) {
        if index < self.themes.len() {
            self.selected = index;
        }
    }

    pub fn set_accent(&mut self, accent: Accent) {
        self.accent = accent;
    }

    pub fn notes(&self) -> &[Note] {
        &self.notes
    }

    pub fn folder(&self) -> &Result<PathBuf, String> {
        &self.folder
    }

    /// Saves the theme in use and the accent to `ui.toml`.
    pub fn save(&self) -> Result<(), String> {
        let path = self.ui_path.as_deref().map_err(Clone::clone)?;
        ui_conf::save(path, &self.current().name, self.accent.key())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashMap;
    use std::process;
    use std::time::{SystemTime, UNIX_EPOCH};

    /// The example file that ships with the package, read at build time so it stays valid.
    const EXAMPLE: &str = include_str!("../../dist/themes/example.toml");

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
        let dir = std::env::temp_dir().join(format!("fgvk-themes-{tag}-{}-{nanos}", process::id()));
        fs::create_dir_all(&dir).expect("create test directory");
        dir
    }

    /// The themes folder inside a test directory. `ui.toml` sits beside it, as in the real layout.
    fn themes_dir(dir: &Path) -> PathBuf {
        let folder = dir.join("themes");
        fs::create_dir_all(&folder).expect("create themes folder");
        folder
    }

    fn builtin_with(name: &str, accent: Accent) -> Palette {
        theme::builtin(name).expect("built-in").palette(accent)
    }

    fn builtin(name: &str) -> Palette {
        builtin_with(name, Accent::DEFAULT)
    }

    fn parse_ok(text: &str) -> Parsed {
        parse(text).unwrap_or_else(|error| panic!("expected success, got: {error}"))
    }

    #[test]
    fn a_name_alone_is_a_mocha_theme_with_a_setting_driven_accent() {
        let parsed = parse_ok("name = \"Mine\"\ntext = \"#abcdef\"\n");
        assert_eq!(parsed.theme.name, "Mine");
        assert!(parsed.notes.is_empty());
        let palette = parsed.theme.palette(Accent::Teal);
        assert_eq!(palette.text, theme::hex(0xabcdef));
        assert_eq!(
            palette.base,
            builtin("Catppuccin Mocha").base,
            "missing keys inherit"
        );
        assert_eq!(
            palette.accent,
            builtin_with("Catppuccin Mocha", Accent::Teal).accent
        );
        assert!(palette.dark);
        assert!(parsed.theme.follows_accent_setting());
    }

    #[test]
    fn base_names_a_builtin_whose_missing_keys_are_inherited() {
        let parsed = parse_ok("name = \"Nord mine\"\nbase = \"Nord\"\nbackground = \"#000000\"\n");
        let palette = parsed.theme.palette(Accent::DEFAULT);
        assert_eq!(
            palette.base,
            theme::hex(0x000000),
            "background sets the base role"
        );
        assert_eq!(palette.mantle, builtin("Nord").mantle, "mantle inherited");
        assert_eq!(
            palette.accent,
            builtin("Nord").accent,
            "fixed accent inherited"
        );
        assert!(!parsed.theme.follows_accent_setting());
    }

    #[test]
    fn dark_defaults_to_the_base_and_can_be_overridden() {
        assert!(
            !parse_ok("name = \"L\"\nbase = \"Catppuccin Latte\"\n")
                .theme
                .palette(Accent::DEFAULT)
                .dark
        );
        assert!(
            parse_ok("name = \"L\"\nbase = \"Catppuccin Latte\"\ndark = true\n")
                .theme
                .palette(Accent::DEFAULT)
                .dark
        );
        assert!(
            !parse_ok("name = \"D\"\ndark = false\n")
                .theme
                .palette(Accent::DEFAULT)
                .dark
        );
    }

    #[test]
    fn bad_colours_and_values_are_refused_with_the_line() {
        let cases = [
            (
                "name = \"X\"\ntext = \"#12345\"\n",
                "line 2:",
                "text must be a colour",
            ),
            (
                "name = \"X\"\n\ntext = \"red\"\n",
                "line 3:",
                "text must be a colour",
            ),
            (
                "name = \"X\"\ntext = 5\n",
                "line 2:",
                "text must be a colour",
            ),
            (
                "name = \"X\"\naccent = \"#gggggg\"\n",
                "line 2:",
                "accent must be a colour",
            ),
            (
                "name = \"X\"\nstripe = \"#12\"\n",
                "line 2:",
                "stripe must be a colour",
            ),
            (
                "name = \"X\"\nbackground = \"#abc\"\n",
                "line 2:",
                "background must be a colour",
            ),
            (
                "name = \"X\"\ndark = \"yes\"\n",
                "line 2:",
                "dark must be true or false",
            ),
            (
                "name = \"X\"\nbase = \"Nordd\"\n",
                "line 2:",
                "not a built-in theme name",
            ),
            (
                "name = \"X\"\nbase = \"#2e3440\"\n",
                "line 2:",
                "not a built-in theme name",
            ),
            (
                "name = \"X\"\nbase = 4\n",
                "line 2:",
                "base must be the quoted name",
            ),
            (
                "name = \"X\"\ncatppuccin_accent = \"Teal\"\n",
                "line 2:",
                "catppuccin_accent must be one of",
            ),
            (
                "name = \"\"\n",
                "line 1:",
                "name must be a non-empty string",
            ),
            ("text = \"#ffffff\"\n", "", "name is missing"),
            (
                "name = \"X\"\n[colours]\ntext = \"#ffffff\"\n",
                "line 2:",
                "must be a value, not a table",
            ),
            (
                "name = \"X\"\ntext = \"#ffffff\"\ntext = \"#000000\"\n",
                "line 3:",
                "",
            ),
            ("name = \"X\"\ntext =\n", "line 2:", ""),
        ];
        for (text, line, message) in cases {
            let error = parse(text).expect_err(text);
            assert!(error.contains(line), "{text:?}: {error}");
            assert!(error.contains(message), "{text:?}: {error}");
        }
    }

    #[test]
    fn unknown_keys_are_ignored_with_a_note_naming_the_line() {
        let parsed = parse_ok("name = \"X\"\n# comment\ncolour = \"#ffffff\"\n");
        assert_eq!(parsed.notes, ["line 3: unknown key \"colour\" is ignored."]);
    }

    #[test]
    fn stripe_defaults_to_half_opaque_surface0_and_accepts_alpha() {
        let derived = parse_ok("name = \"X\"\nsurface0 = \"#102030\"\n")
            .theme
            .palette(Accent::DEFAULT);
        assert_eq!(
            derived.stripe,
            theme::with_alpha(theme::hex(0x102030), 0x80)
        );

        let explicit = parse_ok("name = \"X\"\nstripe = \"#ffffff80\"\n")
            .theme
            .palette(Accent::DEFAULT);
        assert_eq!(
            explicit.stripe,
            Color32::from_rgba_unmultiplied(255, 255, 255, 128)
        );
    }

    #[test]
    fn catppuccin_accent_pins_the_accent_and_the_window_setting_applies_otherwise() {
        let pinned = parse_ok("name = \"P\"\ncatppuccin_accent = \"teal\"\n").theme;
        assert!(!pinned.follows_accent_setting());
        assert_eq!(
            pinned.palette(Accent::Red).accent,
            builtin_with("Catppuccin Mocha", Accent::Teal).accent
        );

        let unpinned = parse_ok("name = \"U\"\nbase = \"Catppuccin Frappé\"\n").theme;
        assert!(unpinned.follows_accent_setting());
        assert_eq!(
            unpinned.palette(Accent::Sky).accent,
            builtin_with("Catppuccin Frappé", Accent::Sky).accent
        );
    }

    #[test]
    fn explicit_accent_overrides_the_catppuccin_setting() {
        let parsed = parse_ok("name = \"E\"\naccent = \"#010203\"\n");
        assert!(!parsed.theme.follows_accent_setting());
        assert_eq!(
            parsed.theme.palette(Accent::Teal).accent,
            theme::hex(0x010203)
        );
    }

    #[test]
    fn catppuccin_accent_is_ignored_with_a_note_for_other_bases() {
        let parsed = parse_ok("name = \"N\"\nbase = \"Nord\"\ncatppuccin_accent = \"teal\"\n");
        assert_eq!(
            parsed.notes,
            ["line 3: catppuccin_accent is ignored: base is not a Catppuccin flavour."]
        );
        assert_eq!(
            parsed.theme.palette(Accent::Teal).accent,
            builtin("Nord").accent
        );
    }

    #[test]
    fn unique_names_take_a_suffix_then_a_number() {
        let taken = vec!["Nord".to_owned(), "Nord (custom)".to_owned()];
        assert_eq!(unique_name("Dracula", &taken), "Dracula");
        assert_eq!(unique_name("Nord", &taken), "Nord (custom 2)");
        assert_eq!(unique_name("Nord", &[]), "Nord");
    }

    #[test]
    fn scan_lists_good_files_in_name_order_and_skips_bad_ones() {
        let dir = temp_dir("scan");
        fs::write(dir.join("b-good.toml"), "name = \"Nord\"\n").expect("write");
        fs::write(dir.join("a-bad.toml"), "name = \"Bad\"\ntext = \"xyz\"\n").expect("write");
        fs::write(dir.join("notes.txt"), "not a theme").expect("write");
        let builtin = theme::builtin_themes();

        let (themes, notes) = scan(&dir, &builtin);
        assert_eq!(themes.len(), 1);
        assert_eq!(themes[0].name, "Nord (custom)");
        assert!(
            notes.contains(&Note::info(
                "b-good.toml: \"Nord\" is already a theme name; it is listed as \"Nord (custom)\"."
                    .to_owned()
            )),
            "{notes:?}"
        );
        assert!(
            notes.iter().any(|note| note.problem
                && note.text.starts_with("a-bad.toml: line 2:")
                && note.text.ends_with("The file is skipped.")),
            "{notes:?}"
        );
        assert_eq!(notes.len(), 2, "notes.txt is not a theme file: {notes:?}");
        fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn a_missing_folder_is_quiet_and_a_file_in_its_place_is_a_problem() {
        let dir = temp_dir("folder");
        let builtin = theme::builtin_themes();
        let (themes, notes) = scan(&dir.join("absent"), &builtin);
        assert!(themes.is_empty() && notes.is_empty());

        let file = dir.join("plain");
        fs::write(&file, "x").expect("write");
        let (_, notes) = scan(&file, &builtin);
        assert_eq!(notes.len(), 1);
        assert!(notes[0].problem);
        fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn the_catalog_selects_the_saved_theme_and_accent() {
        let dir = temp_dir("saved");
        let ui = dir.join("ui.toml");
        fs::write(&ui, "theme = \"Nord\"\ncatppuccin_accent = \"teal\"\n").expect("write");
        let catalog = Catalog::open(Ok(themes_dir(&dir)), Ok(ui));
        assert_eq!(catalog.current().name, "Nord");
        assert_eq!(catalog.accent(), Accent::Teal);
        assert!(catalog.notes().is_empty());
        fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn unknown_saved_names_fall_back_to_the_default_with_notes() {
        let dir = temp_dir("fallback");
        let ui = dir.join("ui.toml");
        fs::write(&ui, "theme = \"Gone\"\ncatppuccin_accent = \"violet\"\n").expect("write");
        let catalog = Catalog::open(Ok(themes_dir(&dir)), Ok(ui));
        assert_eq!(catalog.current().name, theme::DEFAULT_THEME);
        assert_eq!(catalog.accent(), Accent::Mauve);
        assert_eq!(catalog.notes().len(), 2, "{:?}", catalog.notes());
        fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn a_broken_ui_file_is_a_note_and_the_defaults_are_used() {
        let dir = temp_dir("broken-ui");
        let ui = dir.join("ui.toml");
        fs::write(&ui, "theme = = 1\n").expect("write");
        let catalog = Catalog::open(Ok(themes_dir(&dir)), Ok(ui.clone()));
        assert_eq!(catalog.current().name, theme::DEFAULT_THEME);
        assert!(catalog.notes()[0].problem);
        assert!(catalog.notes()[0].text.contains("line 1"));
        assert!(
            catalog.save().is_err(),
            "a broken file is never overwritten"
        );
        assert_eq!(fs::read_to_string(&ui).expect("read"), "theme = = 1\n");
        fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn saving_and_reloading_keep_the_choice_and_drop_a_removed_custom_theme() {
        let dir = temp_dir("reload");
        let ui = dir.join("ui.toml");
        fs::write(
            themes_dir(&dir).join("mine.toml"),
            "name = \"Mine\"\nbase = \"Nord\"\n",
        )
        .expect("write");

        let mut catalog = Catalog::open(Ok(themes_dir(&dir)), Ok(ui.clone()));
        let mine = catalog
            .themes()
            .iter()
            .position(|t| t.name == "Mine")
            .expect("listed");
        catalog.select(mine);
        catalog.set_accent(Accent::Sky);
        catalog.save().expect("save");
        assert_eq!(
            fs::read_to_string(&ui).expect("read"),
            "theme = \"Mine\"\ncatppuccin_accent = \"sky\"\n"
        );
        let reopened = Catalog::open(Ok(themes_dir(&dir)), Ok(ui.clone()));
        assert_eq!(reopened.current().name, "Mine");
        assert_eq!(reopened.accent(), Accent::Sky);

        catalog.reload();
        assert_eq!(catalog.current().name, "Mine", "still listed");
        fs::remove_file(themes_dir(&dir).join("mine.toml")).expect("remove");
        catalog.reload();
        assert_eq!(catalog.current().name, theme::DEFAULT_THEME);
        assert!(catalog
            .notes()
            .iter()
            .any(|note| note.text.contains("no longer listed")));
        fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn the_example_file_is_a_valid_theme_that_names_every_key() {
        let parsed = parse(EXAMPLE).expect("the shipped example parses");
        assert!(parsed.notes.is_empty(), "{:?}", parsed.notes);
        for key in [
            "name",
            "base",
            "dark",
            "background",
            "mantle",
            "crust",
            "surface0",
            "surface1",
            "surface2",
            "overlay0",
            "text",
            "subtext0",
            "accent",
            "error",
            "success",
            "stripe",
            "catppuccin_accent",
        ] {
            assert!(EXAMPLE.contains(key), "the example does not mention {key}");
        }
    }

    #[test]
    fn resolve_dir_uses_xdg_config_home_then_home() {
        assert_eq!(
            resolve_dir(env(&[("XDG_CONFIG_HOME", "/xdg"), ("HOME", "/h")])),
            Ok(PathBuf::from("/xdg/framegen-vulkan/themes"))
        );
        assert_eq!(
            resolve_dir(env(&[("XDG_CONFIG_HOME", ""), ("HOME", "/h")])),
            Ok(PathBuf::from("/h/.config/framegen-vulkan/themes"))
        );
        assert!(resolve_dir(env(&[])).is_err());
    }
}
