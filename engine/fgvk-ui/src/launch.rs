//! Routes `fgvk` invocations before the settings window or graphics context is created.

use std::ffi::OsString;
use std::os::unix::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::Command;

/// Overrides the launch helper path. Development builds outside an install use it.
pub const HELPER_ENV: &str = "FGVK_LAUNCH_HELPER";

const HELPERS_FROM_BINARY: &str = "../libexec/framegen-vulkan";

/// Exit status when the launch helper cannot be found.
const STATUS_HELPER_MISSING: i32 = 127;
/// Exit status when the launch helper exists but cannot be started.
const STATUS_HELPER_NOT_RUNNABLE: i32 = 126;

/// What `fgvk` does with its command-line arguments.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Action {
    /// No arguments: open the settings window.
    Gui,
    /// Arguments present: run `helper` with `args` in place of this process.
    Exec {
        helper: PathBuf,
        label: &'static str,
        args: Vec<OsString>,
    },
}

/// Chooses the action for a full argument vector whose first item is the program name.
///
/// Pure: the caller supplies the environment lookup and the executable path, so the split is
/// testable without touching the process. The helper's existence is not checked here.
pub fn plan(
    argv: impl IntoIterator<Item = OsString>,
    lookup: impl Fn(&str) -> Option<OsString>,
    current_exe: Option<&Path>,
) -> Result<Action, String> {
    let mut argv = argv.into_iter();
    let _program = argv.next();
    let mut args: Vec<OsString> = argv.collect();
    if args.is_empty() {
        return Ok(Action::Gui);
    }
    let (name, label) = if args[0] == "cli" {
        args.remove(0);
        ("fgvk-cli", "CLI helper")
    } else if args[0] == "uninstall" {
        args.remove(0);
        args.push("--uninstall".into());
        ("fgvk-installer", "uninstall helper")
    } else {
        ("fgvk-launch", "launch helper")
    };
    let helper = helper_path(name, lookup, current_exe)?;
    Ok(Action::Exec {
        helper,
        label,
        args,
    })
}

/// Resolves the helper from `FGVK_LAUNCH_HELPER`, else from the installed layout.
fn helper_path(
    name: &str,
    lookup: impl Fn(&str) -> Option<OsString>,
    current_exe: Option<&Path>,
) -> Result<PathBuf, String> {
    if name == "fgvk-launch" {
        if let Some(path) = lookup(HELPER_ENV).filter(|value| !value.is_empty()) {
            return Ok(PathBuf::from(path));
        }
    }
    let binary =
        current_exe.ok_or_else(|| format!("cannot locate the fgvk executable for {name}"))?;
    let directory = binary
        .parent()
        .ok_or_else(|| format!("cannot locate the fgvk executable directory for {name}"))?;
    Ok(directory.join(HELPERS_FROM_BINARY).join(name))
}

/// Runs the selected helper. Returns `Ok(())` when the settings window should open.
///
/// On success this function does not return: the process image becomes the helper. A returned
/// `Err` carries the exit status to use after printing the reason.
pub fn dispatch(argv: impl IntoIterator<Item = OsString>) -> Result<(), i32> {
    let current_exe = std::env::current_exe().ok();
    match plan(argv, |key| std::env::var_os(key), current_exe.as_deref()) {
        Ok(Action::Gui) => Ok(()),
        Ok(Action::Exec {
            helper,
            label,
            args,
        }) => Err(hand_off(&helper, label, &args)),
        Err(message) => {
            eprintln!("framegen-vulkan: {message}");
            Err(STATUS_HELPER_MISSING)
        }
    }
}

/// Replaces this process with `helper`, passing `args` unchanged. Returns only on failure.
fn hand_off(helper: &Path, label: &str, args: &[OsString]) -> i32 {
    if !helper.is_file() {
        eprintln!("framegen-vulkan: {label} not found: {}", helper.display());
        if label == "uninstall helper" {
            eprintln!("framegen-vulkan: Use your package manager for system packages.");
        } else if label == "launch helper" {
            eprintln!("framegen-vulkan: Reinstall framegen-vulkan or set {HELPER_ENV}.");
        }
        return STATUS_HELPER_MISSING;
    }
    let error = Command::new(helper).args(args).exec();
    eprintln!(
        "framegen-vulkan: cannot start {label} {}: {error}",
        helper.display()
    );
    STATUS_HELPER_NOT_RUNNABLE
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashMap;
    use std::os::unix::ffi::OsStringExt;

    fn argv(items: &[&str]) -> Vec<OsString> {
        items.iter().map(OsString::from).collect()
    }

    fn no_environment(_: &str) -> Option<OsString> {
        None
    }

    fn exe(path: &'static str) -> Option<&'static Path> {
        Some(Path::new(path))
    }

    #[test]
    fn no_arguments_open_the_window() {
        let action = plan(
            argv(&["fgvk"]),
            no_environment,
            exe("/home/u/.local/bin/fgvk"),
        );
        assert_eq!(action, Ok(Action::Gui));
    }

    #[test]
    fn an_empty_argument_vector_also_opens_the_window() {
        let action = plan(Vec::new(), no_environment, exe("/home/u/.local/bin/fgvk"));
        assert_eq!(action, Ok(Action::Gui));
    }

    #[test]
    fn any_argument_routes_to_the_helper_unchanged() {
        let action = plan(
            argv(&[
                "fgvk",
                "%command%",
                "--",
                "Game.exe",
                "",
                "a b",
                "--flag=value",
            ]),
            no_environment,
            exe("/home/u/.local/bin/fgvk"),
        );
        assert_eq!(
            action,
            Ok(Action::Exec {
                label: "launch helper",
                helper: PathBuf::from("/home/u/.local/bin/../libexec/framegen-vulkan/fgvk-launch"),
                args: argv(&["%command%", "--", "Game.exe", "", "a b", "--flag=value"]),
            })
        );
    }

    #[test]
    fn help_flags_are_forwarded_not_parsed() {
        for flag in ["--help", "-h"] {
            let action = plan(
                argv(&["fgvk", flag]),
                no_environment,
                exe("/opt/fgvk/bin/fgvk"),
            );
            assert_eq!(
                action,
                Ok(Action::Exec {
                    label: "launch helper",
                    helper: PathBuf::from("/opt/fgvk/bin/../libexec/framegen-vulkan/fgvk-launch"),
                    args: argv(&[flag]),
                }),
                "flag {flag}"
            );
        }
    }

    #[test]
    fn non_utf8_arguments_are_preserved_byte_for_byte() {
        let raw = OsString::from_vec(vec![b'g', 0xff, b'e']);
        let action = plan(
            vec![OsString::from("fgvk"), raw.clone()],
            no_environment,
            exe("/usr/bin/fgvk"),
        );
        let Ok(Action::Exec { args, .. }) = action else {
            panic!("expected a helper hand-off, got {action:?}");
        };
        assert_eq!(args, vec![raw]);
    }

    #[test]
    fn helper_environment_override_wins_over_the_installed_layout() {
        let environment: HashMap<&str, OsString> =
            HashMap::from([(HELPER_ENV, OsString::from("/dev/tree/fgvk-launch"))]);
        let action = plan(
            argv(&["fgvk", "%command%"]),
            |key| environment.get(key).cloned(),
            exe("/home/u/.local/bin/fgvk"),
        );
        assert_eq!(
            action,
            Ok(Action::Exec {
                label: "launch helper",
                helper: PathBuf::from("/dev/tree/fgvk-launch"),
                args: argv(&["%command%"]),
            })
        );
    }

    #[test]
    fn an_empty_helper_override_is_ignored() {
        let action = plan(
            argv(&["fgvk", "%command%"]),
            |key| (key == HELPER_ENV).then(OsString::new),
            exe("/home/u/.local/bin/fgvk"),
        );
        assert_eq!(
            action,
            Ok(Action::Exec {
                label: "launch helper",
                helper: PathBuf::from("/home/u/.local/bin/../libexec/framegen-vulkan/fgvk-launch"),
                args: argv(&["%command%"]),
            })
        );
    }

    #[test]
    fn arguments_without_any_helper_location_are_an_error() {
        let no_exe = plan(argv(&["fgvk", "%command%"]), no_environment, None);
        let root_exe = plan(argv(&["fgvk", "%command%"]), no_environment, exe("/"));
        for action in [no_exe, root_exe] {
            let message = action.expect_err("no helper location");
            assert!(message.contains("fgvk-launch"), "{message}");
        }
    }

    #[test]
    fn gui_needs_no_helper_location() {
        assert_eq!(plan(argv(&["fgvk"]), no_environment, None), Ok(Action::Gui));
    }

    #[test]
    fn helper_path_is_never_taken_from_the_arguments() {
        let action = plan(
            argv(&["fgvk", "FGVK_LAUNCH_HELPER=/evil"]),
            no_environment,
            exe("/usr/bin/fgvk"),
        );
        let Ok(Action::Exec { helper, args, .. }) = action else {
            panic!("expected a helper hand-off, got {action:?}");
        };
        assert_eq!(
            helper,
            PathBuf::from("/usr/bin/../libexec/framegen-vulkan/fgvk-launch")
        );
        assert_eq!(args, argv(&["FGVK_LAUNCH_HELPER=/evil"]));
    }
}
