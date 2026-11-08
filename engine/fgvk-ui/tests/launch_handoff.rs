//! Runs the real `fgvk` executable with arguments against a fake launch helper. Every invocation
//! passes at least one argument, so the settings window is never opened by these tests.

#![cfg(unix)]

use std::fs;
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};
use std::process::{Command, Output};

const FAKE_HELPER: &str = "#!/bin/sh\nfor argument in \"$@\"; do\n  printf '%s\\n' \"$argument\"\ndone\nexit 23\n";

fn scratch_directory(name: &str) -> PathBuf {
    let directory = std::env::temp_dir().join(format!(
        "fgvk-launch-handoff-{name}-{}",
        std::process::id()
    ));
    let _ = fs::remove_dir_all(&directory);
    fs::create_dir_all(&directory).expect("create scratch directory");
    directory
}

fn write_executable(path: &Path, contents: &str, mode: u32) {
    fs::create_dir_all(path.parent().expect("parent")).expect("create parent");
    fs::write(path, contents).expect("write script");
    fs::set_permissions(path, fs::Permissions::from_mode(mode)).expect("set mode");
}

fn run_fgvk(binary: &Path, arguments: &[&str], helper_override: Option<&Path>) -> Output {
    let mut command = Command::new(binary);
    command.env_clear().args(arguments);
    if let Some(helper) = helper_override {
        command.env("FGVK_LAUNCH_HELPER", helper);
    }
    command.output().expect("run fgvk")
}

#[test]
fn arguments_reach_the_helper_unchanged_with_its_exit_status() {
    let directory = scratch_directory("override");
    let helper = directory.join("fgvk-launch");
    write_executable(&helper, FAKE_HELPER, 0o755);

    let output = run_fgvk(
        Path::new(env!("CARGO_BIN_EXE_fgvk")),
        &["--help", "%command%", "", "a b", "--", "Game.exe"],
        Some(&helper),
    );

    assert_eq!(output.status.code(), Some(23), "{output:?}");
    assert_eq!(
        String::from_utf8_lossy(&output.stdout),
        "--help\n%command%\n\na b\n--\nGame.exe\n"
    );
    let _ = fs::remove_dir_all(&directory);
}

#[test]
fn installed_layout_finds_the_helper_beside_the_binary() {
    let prefix = scratch_directory("installed");
    let binary = prefix.join("bin/fgvk");
    fs::create_dir_all(binary.parent().expect("bin")).expect("create bin");
    fs::copy(env!("CARGO_BIN_EXE_fgvk"), &binary).expect("copy fgvk into the layout");
    write_executable(
        &prefix.join("libexec/framegen-vulkan/fgvk-launch"),
        FAKE_HELPER,
        0o755,
    );

    let output = run_fgvk(&binary, &["%command%"], None);

    assert_eq!(output.status.code(), Some(23), "{output:?}");
    assert_eq!(String::from_utf8_lossy(&output.stdout), "%command%\n");
    let _ = fs::remove_dir_all(&prefix);
}

#[test]
fn missing_helper_exits_127_with_a_clear_message() {
    let directory = scratch_directory("missing");
    let missing = directory.join("absent/fgvk-launch");

    let output = run_fgvk(
        Path::new(env!("CARGO_BIN_EXE_fgvk")),
        &["%command%"],
        Some(&missing),
    );

    assert_eq!(output.status.code(), Some(127), "{output:?}");
    let stderr = String::from_utf8_lossy(&output.stderr);
    assert!(stderr.contains("launch helper not found"), "{stderr}");
    assert!(stderr.contains("FGVK_LAUNCH_HELPER"), "{stderr}");
    assert!(output.stdout.is_empty(), "{output:?}");
    let _ = fs::remove_dir_all(&directory);
}

#[test]
fn helper_without_execute_permission_exits_126() {
    let directory = scratch_directory("not-executable");
    let helper = directory.join("fgvk-launch");
    write_executable(&helper, FAKE_HELPER, 0o644);

    let output = run_fgvk(
        Path::new(env!("CARGO_BIN_EXE_fgvk")),
        &["%command%"],
        Some(&helper),
    );

    assert_eq!(output.status.code(), Some(126), "{output:?}");
    assert!(
        String::from_utf8_lossy(&output.stderr).contains("cannot start launch helper"),
        "{output:?}"
    );
    let _ = fs::remove_dir_all(&directory);
}
