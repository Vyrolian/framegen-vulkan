//! Colour palettes for the settings window and the egui visuals built from them.
//!
//! The window draws only with a `Palette`. `apply` stores the active palette in the egui context,
//! and `current` reads it back, so every widget uses the same colours.
//!
//! Built-in themes are the four Catppuccin flavours, whose accent is chosen in the window, and six
//! fixed palettes. The values are written from each project's public colour specification. Where a
//! specification does not name a value for a role, the value is a step darker or lighter picked by
//! hand, and the comment above that palette says which values those are.
//!
//! Role use: `base` is the window background, `mantle` the picker rows, `crust` the text field
//! background, `surface0` to `surface2` the widget fills, `overlay0` borders and disabled text,
//! `text` the body text, `subtext0` hints, `accent` headings, links, selection and the Wine badge,
//! `error` and `success` status text, and `stripe` every second game entry.

use egui::{
    style::WidgetVisuals, Color32, Context, CornerRadius, FontFamily, FontId, Id, Stroke, Style,
    TextStyle, ThemePreference, Visuals,
};

/// Temp data key under which `apply` keeps the palette the window draws with.
const ACTIVE_PALETTE: &str = "framegen-vulkan active palette";

/// The theme a fresh window starts with.
pub const DEFAULT_THEME: &str = "Catppuccin Mocha";

/// Colours the window draws with. Field names follow Catppuccin's role names.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Palette {
    /// Dark or light. It picks the egui visuals and the preference egui follows.
    pub dark: bool,
    pub base: Color32,
    pub mantle: Color32,
    pub crust: Color32,
    pub surface0: Color32,
    pub surface1: Color32,
    pub surface2: Color32,
    pub overlay0: Color32,
    pub text: Color32,
    pub subtext0: Color32,
    pub accent: Color32,
    pub error: Color32,
    pub success: Color32,
    pub stripe: Color32,
}

/// Packed `0xRRGGBB`, written the way the colour specifications print their values.
pub const fn hex(value: u32) -> Color32 {
    Color32::from_rgb((value >> 16) as u8, (value >> 8) as u8, value as u8)
}

/// Returns `color` with the given alpha, usable in constants.
pub const fn with_alpha(color: Color32, alpha: u8) -> Color32 {
    Color32::from_rgba_unmultiplied_const(color.r(), color.g(), color.b(), alpha)
}

/// The alternating-row fill for a `surface0` colour: about half opacity.
pub const fn striped(surface0: Color32) -> Color32 {
    with_alpha(surface0, 0x80)
}

/// The fourteen Catppuccin accents, in the order the Accent combo lists them.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Accent {
    Rosewater,
    Flamingo,
    Pink,
    Mauve,
    Red,
    Maroon,
    Peach,
    Yellow,
    Green,
    Teal,
    Sky,
    Sapphire,
    Blue,
    Lavender,
}

/// Lowercase key and label for each accent, indexed by the enum's discriminant.
const ACCENT_NAMES: [(&str, &str); 14] = [
    ("rosewater", "Rosewater"),
    ("flamingo", "Flamingo"),
    ("pink", "Pink"),
    ("mauve", "Mauve"),
    ("red", "Red"),
    ("maroon", "Maroon"),
    ("peach", "Peach"),
    ("yellow", "Yellow"),
    ("green", "Green"),
    ("teal", "Teal"),
    ("sky", "Sky"),
    ("sapphire", "Sapphire"),
    ("blue", "Blue"),
    ("lavender", "Lavender"),
];

impl Accent {
    pub const ALL: [Accent; 14] = [
        Accent::Rosewater,
        Accent::Flamingo,
        Accent::Pink,
        Accent::Mauve,
        Accent::Red,
        Accent::Maroon,
        Accent::Peach,
        Accent::Yellow,
        Accent::Green,
        Accent::Teal,
        Accent::Sky,
        Accent::Sapphire,
        Accent::Blue,
        Accent::Lavender,
    ];

    /// Mauve, so the default look is the one the window always had.
    pub const DEFAULT: Accent = Accent::Mauve;

    /// The lowercase name written to `ui.toml` and theme files.
    pub fn key(self) -> &'static str {
        ACCENT_NAMES[self as usize].0
    }

    /// The name shown in the window.
    pub fn label(self) -> &'static str {
        ACCENT_NAMES[self as usize].1
    }

    /// The accent with this lowercase key, if there is one.
    pub fn from_key(key: &str) -> Option<Accent> {
        Accent::ALL.into_iter().find(|accent| accent.key() == key)
    }
}

/// One Catppuccin flavour, from its official palette.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Flavour {
    pub name: &'static str,
    pub dark: bool,
    pub base: Color32,
    pub mantle: Color32,
    pub crust: Color32,
    pub surface0: Color32,
    pub surface1: Color32,
    pub surface2: Color32,
    pub overlay0: Color32,
    pub text: Color32,
    pub subtext0: Color32,
    /// The fourteen accents in `Accent::ALL` order.
    accents: [Color32; 14],
}

impl Flavour {
    /// The flavour's colour for one accent name.
    pub fn accent(&self, accent: Accent) -> Color32 {
        self.accents[accent as usize]
    }

    /// The palette with `accent` as its accent role. Error and success stay the flavour's red and
    /// green, whatever accent is chosen.
    pub fn palette(&self, accent: Accent) -> Palette {
        Palette {
            dark: self.dark,
            base: self.base,
            mantle: self.mantle,
            crust: self.crust,
            surface0: self.surface0,
            surface1: self.surface1,
            surface2: self.surface2,
            overlay0: self.overlay0,
            text: self.text,
            subtext0: self.subtext0,
            accent: self.accent(accent),
            error: self.accent(Accent::Red),
            success: self.accent(Accent::Green),
            stripe: striped(self.surface0),
        }
    }
}

/// Catppuccin Latte, the light flavour.
pub const LATTE: Flavour = Flavour {
    name: "Catppuccin Latte",
    dark: false,
    base: hex(0xeff1f5),
    mantle: hex(0xe6e9ef),
    crust: hex(0xdce0e8),
    surface0: hex(0xccd0da),
    surface1: hex(0xbcc0cc),
    surface2: hex(0xacb0be),
    overlay0: hex(0x9ca0b0),
    text: hex(0x4c4f69),
    subtext0: hex(0x6c6f85),
    accents: [
        hex(0xdc8a78),
        hex(0xdd7878),
        hex(0xea76cb),
        hex(0x8839ef),
        hex(0xd20f39),
        hex(0xe64553),
        hex(0xfe640b),
        hex(0xdf8e1d),
        hex(0x40a02b),
        hex(0x179299),
        hex(0x04a5e5),
        hex(0x209fb5),
        hex(0x1e66f5),
        hex(0x7287fd),
    ],
};

/// Catppuccin Frappé, a dark flavour.
pub const FRAPPE: Flavour = Flavour {
    name: "Catppuccin Frappé",
    dark: true,
    base: hex(0x303446),
    mantle: hex(0x292c3c),
    crust: hex(0x232634),
    surface0: hex(0x414559),
    surface1: hex(0x51576d),
    surface2: hex(0x626880),
    overlay0: hex(0x737994),
    text: hex(0xc6d0f5),
    subtext0: hex(0xa5adce),
    accents: [
        hex(0xf2d5cf),
        hex(0xeebebe),
        hex(0xf4b8e4),
        hex(0xca9ee6),
        hex(0xe78284),
        hex(0xea999c),
        hex(0xef9f76),
        hex(0xe5c890),
        hex(0xa6d189),
        hex(0x81c8be),
        hex(0x99d1db),
        hex(0x85c1dc),
        hex(0x8caaee),
        hex(0xbabbf1),
    ],
};

/// Catppuccin Macchiato, a dark flavour.
pub const MACCHIATO: Flavour = Flavour {
    name: "Catppuccin Macchiato",
    dark: true,
    base: hex(0x24273a),
    mantle: hex(0x1e2030),
    crust: hex(0x181926),
    surface0: hex(0x363a4f),
    surface1: hex(0x494d64),
    surface2: hex(0x5b6078),
    overlay0: hex(0x6e738d),
    text: hex(0xcad3f5),
    subtext0: hex(0xa5adcb),
    accents: [
        hex(0xf4dbd6),
        hex(0xf0c6c6),
        hex(0xf5bde6),
        hex(0xc6a0f6),
        hex(0xed8796),
        hex(0xee99a0),
        hex(0xf5a97f),
        hex(0xeed49f),
        hex(0xa6da95),
        hex(0x8bd5ca),
        hex(0x91d7e3),
        hex(0x7dc4e4),
        hex(0x8aadf4),
        hex(0xb7bdf8),
    ],
};

/// Catppuccin Mocha, the dark flavour the window has always used. Its mauve is the default accent.
pub const MOCHA: Flavour = Flavour {
    name: "Catppuccin Mocha",
    dark: true,
    base: hex(0x1e1e2e),
    mantle: hex(0x181825),
    crust: hex(0x11111b),
    surface0: hex(0x313244),
    surface1: hex(0x45475a),
    surface2: hex(0x585b70),
    overlay0: hex(0x6c7086),
    text: hex(0xcdd6f4),
    subtext0: hex(0xa6adc8),
    accents: [
        hex(0xf5e0dc),
        hex(0xf2cdcd),
        hex(0xf5c2e7),
        hex(0xcba6f7),
        hex(0xf38ba8),
        hex(0xeba0ac),
        hex(0xfab387),
        hex(0xf9e2af),
        hex(0xa6e3a1),
        hex(0x94e2d5),
        hex(0x89dceb),
        hex(0x74c7ec),
        hex(0x89b4fa),
        hex(0xb4befe),
    ],
};

/// Dracula. Published: background, current line, comment, foreground and the accent colours.
/// Derived: mantle, crust, surface1, surface2 and subtext0.
const DRACULA: Palette = Palette {
    dark: true,
    base: hex(0x282a36),
    mantle: hex(0x21222c),
    crust: hex(0x191a21),
    surface0: hex(0x44475a),
    surface1: hex(0x4d5066),
    surface2: hex(0x5a5e78),
    overlay0: hex(0x6272a4),
    text: hex(0xf8f8f2),
    subtext0: hex(0xbfc4d6),
    accent: hex(0xbd93f9),
    error: hex(0xff5555),
    success: hex(0x50fa7b),
    stripe: striped(hex(0x44475a)),
};

/// Nord. Published: nord0 to nord4 and the aurora and frost accents. Derived: mantle, crust,
/// overlay0 and subtext0.
const NORD: Palette = Palette {
    dark: true,
    base: hex(0x2e3440),
    mantle: hex(0x272c36),
    crust: hex(0x20242c),
    surface0: hex(0x3b4252),
    surface1: hex(0x434c5e),
    surface2: hex(0x4c566a),
    overlay0: hex(0x616e88),
    text: hex(0xd8dee9),
    subtext0: hex(0xa1abbd),
    accent: hex(0x88c0d0),
    error: hex(0xbf616a),
    success: hex(0xa3be8c),
    stripe: striped(hex(0x3b4252)),
};

/// Gruvbox Dark, hard contrast background for mantle. Published: the background, its hard
/// variant, the greys and the bright accent colours. Derived: crust.
const GRUVBOX_DARK: Palette = Palette {
    dark: true,
    base: hex(0x282828),
    mantle: hex(0x1d2021),
    crust: hex(0x161819),
    surface0: hex(0x3c3836),
    surface1: hex(0x504945),
    surface2: hex(0x665c54),
    overlay0: hex(0x928374),
    text: hex(0xebdbb2),
    subtext0: hex(0xbdae93),
    accent: hex(0xd3869b),
    error: hex(0xfb4934),
    success: hex(0xb8bb26),
    stripe: striped(hex(0x3c3836)),
};

/// Tokyo Night. Published: the background, dark background, highlight, gutter, comment and
/// foreground colours and the accents. Derived: crust.
const TOKYO_NIGHT: Palette = Palette {
    dark: true,
    base: hex(0x1a1b26),
    mantle: hex(0x16161e),
    crust: hex(0x111118),
    surface0: hex(0x292e42),
    surface1: hex(0x3b4261),
    surface2: hex(0x545c7e),
    overlay0: hex(0x565f89),
    text: hex(0xc0caf5),
    subtext0: hex(0xa9b1d6),
    accent: hex(0xbb9af7),
    error: hex(0xf7768e),
    success: hex(0x9ece6a),
    stripe: striped(hex(0x292e42)),
};

/// Solarized Dark. Published: base03, base02, base00, base0, base1 and the accent colours.
/// Derived: mantle, crust, surface1 and surface2.
const SOLARIZED_DARK: Palette = Palette {
    dark: true,
    base: hex(0x002b36),
    mantle: hex(0x00232c),
    crust: hex(0x001c24),
    surface0: hex(0x073642),
    surface1: hex(0x0b4453),
    surface2: hex(0x2f4b55),
    overlay0: hex(0x657b83),
    text: hex(0x93a1a1),
    subtext0: hex(0x839496),
    accent: hex(0x268bd2),
    error: hex(0xdc322f),
    success: hex(0x859900),
    stripe: striped(hex(0x073642)),
};

/// Solarized Light. Published: base3, base2, base1, base01, base02 and the accent colours.
/// Derived: crust, surface0, surface1 and surface2. The text is base02 and subtext0 is base01,
/// because base00 is below 4.5:1 on base3.
const SOLARIZED_LIGHT: Palette = Palette {
    dark: false,
    base: hex(0xfdf6e3),
    mantle: hex(0xeee8d5),
    crust: hex(0xe7dfc9),
    surface0: hex(0xdcd4bd),
    surface1: hex(0xd0c8b1),
    surface2: hex(0xc3bba4),
    overlay0: hex(0x93a1a1),
    text: hex(0x073642),
    subtext0: hex(0x586e75),
    accent: hex(0x6c71c4),
    error: hex(0xdc322f),
    success: hex(0x859900),
    stripe: striped(hex(0xdcd4bd)),
};

/// How a theme's accent is chosen.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AccentRule {
    /// The palette's own accent: a fixed theme, or a theme file's `accent` key.
    Own,
    /// A Catppuccin base. The accent is the theme file's `catppuccin_accent` when it names one,
    /// otherwise the window's Accent setting.
    Catppuccin {
        flavour: Flavour,
        pinned: Option<Accent>,
    },
}

/// A theme as the window lists it. The palette is complete except for the accent, which the
/// accent rule supplies.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Theme {
    pub name: String,
    palette: Palette,
    rule: AccentRule,
}

impl Theme {
    pub fn new(name: impl Into<String>, palette: Palette, rule: AccentRule) -> Self {
        Self {
            name: name.into(),
            palette,
            rule,
        }
    }

    /// The palette to draw with when the window's Catppuccin accent is `window`.
    pub fn palette(&self, window: Accent) -> Palette {
        match self.rule {
            AccentRule::Own => self.palette,
            AccentRule::Catppuccin { flavour, pinned } => Palette {
                accent: flavour.accent(pinned.unwrap_or(window)),
                ..self.palette
            },
        }
    }

    pub fn rule(&self) -> &AccentRule {
        &self.rule
    }

    /// True when the window's Accent setting changes this theme's accent.
    pub fn follows_accent_setting(&self) -> bool {
        matches!(self.rule, AccentRule::Catppuccin { pinned: None, .. })
    }

    /// The Catppuccin flavour behind this theme, when it has one.
    pub fn flavour(&self) -> Option<Flavour> {
        match self.rule {
            AccentRule::Own => None,
            AccentRule::Catppuccin { flavour, .. } => Some(flavour),
        }
    }
}

/// A Catppuccin flavour as a built-in theme whose accent follows the window's Accent setting.
pub fn catppuccin_theme(flavour: Flavour) -> Theme {
    Theme::new(
        flavour.name,
        flavour.palette(Accent::DEFAULT),
        AccentRule::Catppuccin {
            flavour,
            pinned: None,
        },
    )
}

/// The built-in themes, in the order the combo lists them.
pub fn builtin_themes() -> Vec<Theme> {
    let mut themes: Vec<Theme> = [MOCHA, MACCHIATO, FRAPPE, LATTE]
        .into_iter()
        .map(catppuccin_theme)
        .collect();
    themes.extend(
        [
            ("Dracula", DRACULA),
            ("Nord", NORD),
            ("Gruvbox Dark", GRUVBOX_DARK),
            ("Tokyo Night", TOKYO_NIGHT),
            ("Solarized Dark", SOLARIZED_DARK),
            ("Solarized Light", SOLARIZED_LIGHT),
        ]
        .map(|(name, palette)| Theme::new(name, palette, AccentRule::Own)),
    );
    themes
}

/// The built-in theme with this exact name.
pub fn builtin(name: &str) -> Option<Theme> {
    builtin_themes()
        .into_iter()
        .find(|theme| theme.name == name)
}

/// Makes egui draw with `palette`: its visuals, the window's text sizes, and the palette itself,
/// so `current` returns it. Call again when the palette changes.
pub fn apply(ctx: &Context, palette: &Palette) {
    let (preference, theme) = if palette.dark {
        (ThemePreference::Dark, egui::Theme::Dark)
    } else {
        (ThemePreference::Light, egui::Theme::Light)
    };
    ctx.set_theme(preference);
    ctx.all_styles_mut(sizing);
    ctx.set_visuals_of(theme, visuals(palette));
    ctx.data_mut(|data| data.insert_temp(active_id(), *palette));
}

/// The palette last given to `apply`. Before any call it is the default theme's.
pub fn current(ctx: &Context) -> Palette {
    ctx.data(|data| data.get_temp::<Palette>(active_id()))
        .unwrap_or_else(|| MOCHA.palette(Accent::DEFAULT))
}

fn active_id() -> Id {
    Id::unique(ACTIVE_PALETTE)
}

fn visuals(palette: &Palette) -> Visuals {
    let mut visuals = if palette.dark {
        Visuals::dark()
    } else {
        Visuals::light()
    };
    visuals.dark_mode = palette.dark;
    visuals.window_fill = palette.base;
    visuals.panel_fill = palette.base;
    visuals.faint_bg_color = palette.mantle;
    visuals.extreme_bg_color = palette.crust;
    visuals.code_bg_color = palette.surface0;
    visuals.window_stroke = Stroke::new(1.0, palette.surface0);
    visuals.window_corner_radius = CornerRadius::same(8);
    visuals.menu_corner_radius = CornerRadius::same(6);
    visuals.override_text_color = Some(palette.text);
    visuals.hyperlink_color = palette.accent;
    visuals.error_fg_color = palette.error;
    visuals.selection.bg_fill = with_alpha(palette.accent, 0x40);
    visuals.selection.stroke = Stroke::new(1.0, palette.accent);

    let widgets = &mut visuals.widgets;
    style_widget(
        &mut widgets.noninteractive,
        palette.base,
        Stroke::new(1.0, palette.surface0),
        palette.text,
    );
    style_widget(
        &mut widgets.inactive,
        palette.surface0,
        Stroke::NONE,
        palette.text,
    );
    style_widget(
        &mut widgets.hovered,
        palette.surface1,
        Stroke::new(1.0, palette.overlay0),
        palette.text,
    );
    style_widget(
        &mut widgets.active,
        palette.surface2,
        Stroke::new(1.0, palette.accent),
        palette.text,
    );
    style_widget(
        &mut widgets.open,
        palette.surface0,
        Stroke::new(1.0, palette.surface1),
        palette.text,
    );
    visuals
}

/// Slightly larger text and roomier controls than egui's defaults. Only sizes change, so each text style keeps its font family.
fn sizing(style: &mut Style) {
    text_size(style, TextStyle::Small, 13.0);
    text_size(style, TextStyle::Body, 15.5);
    text_size(style, TextStyle::Button, 15.5);
    text_size(style, TextStyle::Monospace, 14.0);
    text_size(style, TextStyle::Heading, 20.0);
    style.spacing.item_spacing = egui::vec2(8.0, 6.0);
    style.spacing.button_padding = egui::vec2(8.0, 4.0);
    style.spacing.interact_size.y = 26.0;
}

fn text_size(style: &mut Style, text_style: TextStyle, size: f32) {
    style
        .text_styles
        .entry(text_style)
        .or_insert_with(|| FontId::new(size, FontFamily::Proportional))
        .size = size;
}

fn style_widget(widget: &mut WidgetVisuals, fill: Color32, stroke: Stroke, text: Color32) {
    widget.bg_fill = fill;
    widget.weak_bg_fill = fill;
    widget.bg_stroke = stroke;
    widget.fg_stroke = Stroke::new(1.0, text);
    widget.corner_radius = CornerRadius::same(6);
    // Hover and press must not grow the frame past its layout rect.
    widget.expansion = 0.0;
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Relative luminance as WCAG 2 defines it.
    fn luminance(color: Color32) -> f64 {
        let linear = |channel: u8| {
            let value = f64::from(channel) / 255.0;
            if value <= 0.03928 {
                value / 12.92
            } else {
                ((value + 0.055) / 1.055).powf(2.4)
            }
        };
        0.2126 * linear(color.r()) + 0.7152 * linear(color.g()) + 0.0722 * linear(color.b())
    }

    fn contrast(a: Color32, b: Color32) -> f64 {
        let (la, lb) = (luminance(a), luminance(b));
        (la.max(lb) + 0.05) / (la.min(lb) + 0.05)
    }

    #[test]
    fn mocha_with_mauve_is_the_palette_the_window_used_before() {
        let palette = MOCHA.palette(Accent::Mauve);
        assert!(palette.dark);
        assert_eq!(palette.base, hex(0x1e1e2e));
        assert_eq!(palette.mantle, hex(0x181825));
        assert_eq!(palette.crust, hex(0x11111b));
        assert_eq!(palette.surface0, hex(0x313244));
        assert_eq!(palette.surface1, hex(0x45475a));
        assert_eq!(palette.surface2, hex(0x585b70));
        assert_eq!(palette.overlay0, hex(0x6c7086));
        assert_eq!(palette.text, hex(0xcdd6f4));
        assert_eq!(palette.subtext0, hex(0xa6adc8));
        assert_eq!(palette.accent, hex(0xcba6f7), "MAUVE");
        assert_eq!(palette.error, hex(0xf38ba8), "RED");
        assert_eq!(palette.success, hex(0xa6e3a1), "GREEN");
        assert_eq!(palette.stripe, with_alpha(hex(0x313244), 0x80), "STRIPE");
        assert_eq!(
            with_alpha(palette.accent, 0x40),
            Color32::from_rgba_unmultiplied(0xcb, 0xa6, 0xf7, 0x40),
            "the selection fill the window used before"
        );
    }

    #[test]
    fn every_flavour_and_accent_resolves() {
        for flavour in [LATTE, FRAPPE, MACCHIATO, MOCHA] {
            for accent in Accent::ALL {
                let palette = flavour.palette(accent);
                assert_eq!(
                    palette.accent,
                    flavour.accent(accent),
                    "{} {accent:?}",
                    flavour.name
                );
                assert_eq!(palette.error, flavour.accent(Accent::Red));
                assert_eq!(palette.success, flavour.accent(Accent::Green));
                assert_eq!(palette.base, flavour.base);
                assert_eq!(palette.dark, flavour.dark);
            }
        }
        for theme in builtin_themes() {
            if let Some(flavour) = theme.flavour() {
                for accent in Accent::ALL {
                    assert_eq!(
                        theme.palette(accent).accent,
                        flavour.accent(accent),
                        "{}",
                        theme.name
                    );
                }
            }
        }
    }

    #[test]
    fn accents_keep_catppuccin_order_and_round_trip_their_keys() {
        let labels: Vec<&str> = Accent::ALL.iter().map(|accent| accent.label()).collect();
        assert_eq!(
            labels,
            [
                "Rosewater",
                "Flamingo",
                "Pink",
                "Mauve",
                "Red",
                "Maroon",
                "Peach",
                "Yellow",
                "Green",
                "Teal",
                "Sky",
                "Sapphire",
                "Blue",
                "Lavender"
            ]
        );
        for (index, accent) in Accent::ALL.into_iter().enumerate() {
            assert_eq!(accent as usize, index);
            assert_eq!(Accent::from_key(accent.key()), Some(accent));
            assert_eq!(accent.key(), accent.label().to_lowercase());
        }
        assert_eq!(Accent::from_key("Mauve"), None, "keys are lowercase");
        assert_eq!(Accent::from_key("violet"), None);
        assert_eq!(Accent::DEFAULT, Accent::Mauve);
    }

    #[test]
    fn text_is_readable_on_every_builtin() {
        for theme in builtin_themes() {
            let palette = theme.palette(Accent::DEFAULT);
            let ratio = contrast(palette.text, palette.base);
            assert!(ratio >= 4.5, "{}: text on base is {ratio:.2}:1", theme.name);
        }
    }

    #[test]
    fn catppuccin_flavours_have_readable_text_for_every_accent() {
        for flavour in [LATTE, FRAPPE, MACCHIATO, MOCHA] {
            for accent in Accent::ALL {
                let palette = flavour.palette(accent);
                let ratio = contrast(palette.text, palette.base);
                assert!(
                    ratio >= 4.5,
                    "{} {accent:?}: text on base is {ratio:.2}:1",
                    flavour.name
                );
            }
        }
    }

    #[test]
    fn builtin_names_are_unique_and_the_default_is_listed() {
        let names: Vec<String> = builtin_themes()
            .into_iter()
            .map(|theme| theme.name)
            .collect();
        assert_eq!(names.len(), 10);
        for (index, name) in names.iter().enumerate() {
            assert!(!names[..index].contains(name), "duplicate {name}");
        }
        assert!(names.iter().any(|name| name == DEFAULT_THEME));
        assert_eq!(names[0], DEFAULT_THEME, "the default is listed first");
    }

    #[test]
    fn only_catppuccin_themes_follow_the_accent_setting() {
        for theme in builtin_themes() {
            assert_eq!(
                theme.follows_accent_setting(),
                theme.name.starts_with("Catppuccin "),
                "{}",
                theme.name
            );
        }
    }
}
