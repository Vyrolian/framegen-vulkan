//! Catppuccin Mocha colours applied to egui visuals.

use egui::{
    style::WidgetVisuals, Color32, CornerRadius, FontFamily, FontId, Stroke, Style, TextStyle,
    ThemePreference, Visuals,
};

pub const BASE: Color32 = Color32::from_rgb(0x1e, 0x1e, 0x2e);
pub const MANTLE: Color32 = Color32::from_rgb(0x18, 0x18, 0x25);
pub const CRUST: Color32 = Color32::from_rgb(0x11, 0x11, 0x1b);
pub const SURFACE0: Color32 = Color32::from_rgb(0x31, 0x32, 0x44);
pub const SURFACE1: Color32 = Color32::from_rgb(0x45, 0x47, 0x5a);
pub const SURFACE2: Color32 = Color32::from_rgb(0x58, 0x5b, 0x70);
pub const OVERLAY0: Color32 = Color32::from_rgb(0x6c, 0x70, 0x86);
pub const TEXT: Color32 = Color32::from_rgb(0xcd, 0xd6, 0xf4);
pub const SUBTEXT0: Color32 = Color32::from_rgb(0xa6, 0xad, 0xc8);
pub const MAUVE: Color32 = Color32::from_rgb(0xcb, 0xa6, 0xf7);
pub const RED: Color32 = Color32::from_rgb(0xf3, 0x8b, 0xa8);
pub const GREEN: Color32 = Color32::from_rgb(0xa6, 0xe3, 0xa1);

/// Forces dark mode and applies the palette, text sizes and spacing to egui.
pub fn apply(ctx: &egui::Context) {
    ctx.set_theme(ThemePreference::Dark);
    ctx.all_styles_mut(sizing);

    let mut visuals = Visuals::dark();
    visuals.dark_mode = true;
    visuals.window_fill = BASE;
    visuals.panel_fill = BASE;
    visuals.faint_bg_color = MANTLE;
    visuals.extreme_bg_color = CRUST;
    visuals.code_bg_color = SURFACE0;
    visuals.window_stroke = Stroke::new(1.0, SURFACE0);
    visuals.window_corner_radius = CornerRadius::same(8);
    visuals.menu_corner_radius = CornerRadius::same(6);
    visuals.override_text_color = Some(TEXT);
    visuals.hyperlink_color = MAUVE;
    visuals.error_fg_color = RED;
    visuals.selection.bg_fill = Color32::from_rgba_unmultiplied(0xcb, 0xa6, 0xf7, 0x40);
    visuals.selection.stroke = Stroke::new(1.0, MAUVE);

    let widgets = &mut visuals.widgets;
    style_widget(
        &mut widgets.noninteractive,
        BASE,
        Stroke::new(1.0, SURFACE0),
        TEXT,
    );
    style_widget(&mut widgets.inactive, SURFACE0, Stroke::NONE, TEXT);
    style_widget(
        &mut widgets.hovered,
        SURFACE1,
        Stroke::new(1.0, OVERLAY0),
        TEXT,
    );
    style_widget(&mut widgets.active, SURFACE2, Stroke::new(1.0, MAUVE), TEXT);
    style_widget(
        &mut widgets.open,
        SURFACE0,
        Stroke::new(1.0, SURFACE1),
        TEXT,
    );

    ctx.set_visuals(visuals);
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
