//! fgvk: a small settings window for framegen-vulkan frame generation profiles.

mod app;
mod config;
mod launch;
mod picker;
mod processes;
mod state;
mod theme;

fn main() -> eframe::Result {
    // Arguments hand off to the launch helper before any window or graphics context exists.
    if let Err(status) = launch::dispatch(std::env::args_os()) {
        std::process::exit(status);
    }
    let path = config::resolve_path(|key| std::env::var_os(key));
    let state_path = state::resolve_path(|key| std::env::var_os(key));
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
            Ok(Box::new(app::App::new(path, state_path)))
        }),
    )
}
