//! fgvk: a small settings window for framegen-vulkan frame generation profiles.

mod analysis;
mod app;
mod capture_ui;
mod config;
mod dll_locate;
mod launch;
mod launcher_conf;
mod live;
mod picker;
mod processes;
mod state;
mod theme;
mod themes;
mod ui_conf;

fn main() -> eframe::Result {
    // Arguments hand off to the launch helper before any window or graphics context exists.
    if let Err(status) = launch::dispatch(std::env::args_os()) {
        std::process::exit(status);
    }
    let _window_lock = match state::window_lock_directory(|key| std::env::var_os(key)) {
        Some(directory) => match state::lock_window(&directory) {
            Ok(Some(lock)) => lock,
            Ok(None) => {
                eprintln!("framegen-vulkan: the settings window is already open");
                return Ok(());
            }
            Err(error) => {
                eprintln!("framegen-vulkan: cannot lock the settings window: {error}");
                std::process::exit(1);
            }
        },
        None => {
            eprintln!("framegen-vulkan: cannot locate the settings window lock directory");
            std::process::exit(1);
        }
    };
    let path = config::resolve_path(|key| std::env::var_os(key));
    let launcher_path = launcher_conf::resolve_path(|key| std::env::var_os(key));
    let state_path = state::resolve_path(|key| std::env::var_os(key));
    let themes = themes::Catalog::open(
        themes::resolve_dir(|key| std::env::var_os(key)),
        ui_conf::resolve_path(|key| std::env::var_os(key)),
    );
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
            theme::apply(&cc.egui_ctx, &themes.palette());
            Ok(Box::new(app::App::new(
                path,
                state_path,
                launcher_path,
                themes,
            )))
        }),
    )
}
