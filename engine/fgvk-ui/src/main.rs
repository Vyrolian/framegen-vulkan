//! fgvk: a small settings window for framegen-vulkan frame generation profiles.

mod app;
mod config;
mod picker;
mod processes;
mod theme;

fn main() -> eframe::Result {
    let path = config::resolve_path(|key| std::env::var_os(key));
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_title("fgvk")
            .with_app_id("fgvk")
            .with_inner_size([460.0, 660.0]),
        ..Default::default()
    };
    eframe::run_native(
        "fgvk",
        options,
        Box::new(move |cc| {
            theme::apply(&cc.egui_ctx);
            Ok(Box::new(app::App::new(path)))
        }),
    )
}
