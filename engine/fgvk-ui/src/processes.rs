//! Lists the running programs of the user who started the launcher, for the "Pick running" window.
//!
//! Identity follows the layer's matcher in `engine/fgvk-common/src/configuration/detection.cpp`
//! (`identifyProcess` and `isExcludedLauncher`), so a suggested entry is one the layer can match.
//! Everything here is read-only. No process is signalled or modified, and an unreadable or
//! vanished process is skipped rather than reported.

use std::collections::BTreeMap;
use std::ffi::OsStr;
use std::fs::{self, File};
use std::io::Read;
use std::os::unix::fs::MetadataExt;
use std::path::Path;
use std::sync::OnceLock;

/// Each `/proc` file is read at most this far. A maps file of a large Wine process can be megabytes.
const READ_LIMIT: u64 = 4 * 1024 * 1024;
const LAUNCHER_REGISTRY: &str = include_str!(concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/../fgvk-common/launcher_exclusions.json"
));
const VULKAN_LIBRARY: &str = "libvulkan.so";

/// One process that the layer could identify.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Process {
    pub pid: u32,
    /// Target of `/proc/<pid>/exe`, when it is readable.
    pub exe: Option<String>,
    /// First mapped `.exe` path of a Wine or Proton process.
    pub windows_exe: Option<String>,
    /// Thread name from `/proc/<pid>/comm`, trimmed.
    pub comm: String,
    /// The `active_in` entry this process suggests.
    pub entry: String,
    pub vulkan: bool,
    pub wine: bool,
    /// The identity is a launcher that the layer ignores.
    pub launcher: bool,
}

impl Process {
    /// Builds the identity from raw `/proc` values. `maps` must be the file read once for this
    /// process. Returns `None` when there is no name to suggest. A Wine or Proton process with no
    /// mapped `.exe` is also `None`. Its only names would be generic helper names such as
    /// `wineserver`, and listing one would make the layer match every such helper.
    fn new(pid: u32, exe: Option<String>, maps: &str, comm: &str) -> Option<Self> {
        let comm = comm.trim().to_owned();
        let wine = exe.as_deref().is_some_and(is_wine_path);
        let windows_exe = if wine { first_windows_exe(maps) } else { None };
        if wine && windows_exe.is_none() {
            return None;
        }
        let entry = suggested_entry(windows_exe.as_deref(), exe.as_deref(), &comm)?;
        let launcher = windows_exe
            .as_deref()
            .or(exe.as_deref())
            .and_then(basename)
            .is_some_and(is_launcher_name);
        Some(Self {
            pid,
            vulkan: maps.contains(VULKAN_LIBRARY),
            wine,
            launcher,
            entry,
            exe,
            windows_exe,
            comm,
        })
    }

    /// The full Windows path when mapped, else the executable path, else the thread name.
    pub fn display_path(&self) -> &str {
        self.windows_exe
            .as_deref()
            .or(self.exe.as_deref())
            .unwrap_or(&self.comm)
    }
}

/// One row of the picker: every process that suggests the same entry.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Group {
    pub entry: String,
    pub processes: usize,
    /// Path of the first process in the group, shown under the entry.
    pub path: String,
    pub vulkan: bool,
    pub wine: bool,
    pub launcher: bool,
    /// Lowercase entry, paths and thread names, for the search box.
    search: Vec<String>,
}

/// Lists the processes owned by the user who started the launcher.
pub fn scan_current_user() -> Vec<Process> {
    let Ok(owner) = fs::metadata("/proc/self") else {
        return Vec::new();
    };
    scan(Path::new("/proc"), owner.uid(), std::process::id())
}

/// Reads `proc_root`, keeping numeric entries owned by `uid` and skipping `own_pid`. A process that
/// vanishes or cannot be read is left out. Results are sorted by PID.
pub fn scan(proc_root: &Path, uid: u32, own_pid: u32) -> Vec<Process> {
    let Ok(entries) = fs::read_dir(proc_root) else {
        return Vec::new();
    };
    let mut processes: Vec<Process> = entries
        .flatten()
        .filter_map(|entry| {
            let pid = parse_pid(&entry.file_name())?;
            if pid == own_pid {
                return None;
            }
            let dir = entry.path();
            if fs::metadata(&dir).ok()?.uid() != uid {
                return None;
            }
            read_process(&dir, pid)
        })
        .collect();
    processes.sort_by_key(|process| process.pid);
    processes
}

/// Groups processes by suggested entry. Groups with a Vulkan process come first, then by name.
pub fn group(processes: &[Process]) -> Vec<Group> {
    let mut by_entry: BTreeMap<&str, Group> = BTreeMap::new();
    for process in processes {
        let group = by_entry
            .entry(process.entry.as_str())
            .or_insert_with(|| Group {
                entry: process.entry.clone(),
                processes: 0,
                path: process.display_path().to_owned(),
                vulkan: false,
                wine: false,
                launcher: false,
                search: Vec::new(),
            });
        group.processes += 1;
        group.vulkan |= process.vulkan;
        group.wine |= process.wine;
        group.launcher |= process.launcher;
        remember(&mut group.search, &process.entry);
        remember(&mut group.search, process.display_path());
        if let Some(exe) = &process.exe {
            remember(&mut group.search, exe);
        }
        remember(&mut group.search, &process.comm);
    }

    let mut groups: Vec<Group> = by_entry.into_values().collect();
    groups.sort_by_cached_key(|group| {
        (
            !group.vulkan,
            group.entry.to_lowercase(),
            group.entry.clone(),
        )
    });
    groups
}

/// The rows to show: optionally only Vulkan groups, then those whose entry, path or thread name
/// contains the query. The query is trimmed and matched case-insensitively. Order is kept.
pub fn visible<'a>(groups: &'a [Group], query: &str, vulkan_only: bool) -> Vec<&'a Group> {
    let needle = query.trim().to_lowercase();
    groups
        .iter()
        .filter(|group| group.vulkan || !vulkan_only)
        .filter(|group| needle.is_empty() || group.search.iter().any(|name| name.contains(&needle)))
        .collect()
}

/// True when `entry` is already in the profile. Matching is exact, as the Add button does it.
pub fn is_listed(entry: &str, active_in: &[String]) -> bool {
    active_in.iter().any(|listed| listed == entry)
}

/// Returns `active_in` followed by each picked entry that is not already there. The result is
/// the same length as `active_in` when nothing new was picked.
pub fn with_added(active_in: &[String], picked: &[String]) -> Vec<String> {
    let mut entries = active_in.to_vec();
    for entry in picked {
        if !is_listed(entry, &entries) {
            entries.push(entry.clone());
        }
    }
    entries
}

fn read_process(dir: &Path, pid: u32) -> Option<Process> {
    let exe = fs::read_link(dir.join("exe"))
        .ok()
        .map(|path| path.to_string_lossy().into_owned());
    let comm = read_text(&dir.join("comm")).unwrap_or_default();
    // maps is read once. It supplies both the Wine path and the Vulkan check.
    let maps = read_text(&dir.join("maps")).unwrap_or_default();
    Process::new(pid, exe, &maps, &comm)
}

/// Reads at most `READ_LIMIT` bytes. Bytes that are not UTF-8 are replaced.
fn read_text(path: &Path) -> Option<String> {
    let mut bytes = Vec::new();
    File::open(path)
        .ok()?
        .take(READ_LIMIT)
        .read_to_end(&mut bytes)
        .ok()?;
    Some(String::from_utf8_lossy(&bytes).into_owned())
}

fn parse_pid(name: &OsStr) -> Option<u32> {
    let text = name.to_str()?;
    if text.is_empty() || !text.bytes().all(|byte| byte.is_ascii_digit()) {
        return None;
    }
    text.parse().ok()
}

/// The layer's Wine test. It is case-sensitive, as in `detection.cpp`.
fn is_wine_path(path: &str) -> bool {
    path.contains("wine") || path.contains("proton")
}

/// The first `.exe` path in a maps file, extracted as `detection.cpp` does it. The path starts at
/// the first `/` on the line, or after the last space when the line has no `/`. The match ignores
/// ASCII case.
fn first_windows_exe(maps: &str) -> Option<String> {
    maps.lines().find_map(|line| {
        let bytes = line.as_bytes();
        if bytes.len() < 4 || !bytes[bytes.len() - 4..].eq_ignore_ascii_case(b".exe") {
            return None;
        }
        let start = match line.find('/') {
            Some(index) => index,
            None => line.rfind(' ')? + 1,
        };
        let path = &line[start..];
        (!path.is_empty()).then(|| path.to_owned())
    })
}

/// Windows executable basename, else executable basename, else thread name.
fn suggested_entry(windows_exe: Option<&str>, exe: Option<&str>, comm: &str) -> Option<String> {
    windows_exe
        .and_then(basename)
        .or_else(|| exe.and_then(basename))
        .or((!comm.is_empty()).then_some(comm))
        .map(str::to_owned)
}

/// Last path component, splitting on `/` and `\` as the layer's launcher check does.
fn basename(path: &str) -> Option<&str> {
    path.rsplit(['/', '\\'])
        .next()
        .filter(|name| !name.is_empty())
}

fn is_launcher_name(name: &str) -> bool {
    launcher_executables()
        .iter()
        .any(|excluded| excluded.eq_ignore_ascii_case(name))
}

fn launcher_executables() -> &'static [String] {
    static EXECUTABLES: OnceLock<Vec<String>> = OnceLock::new();
    EXECUTABLES.get_or_init(|| parse_launcher_registry(LAUNCHER_REGISTRY).unwrap_or_default())
}

/// Collects every `executables` name from the registry. Returns `None` when the text is not a
/// JSON array. The registry is embedded, so a unit test guards its shape.
fn parse_launcher_registry(text: &str) -> Option<Vec<String>> {
    let value: serde_json::Value = serde_json::from_str(text).ok()?;
    let rules = value.as_array()?;
    Some(
        rules
            .iter()
            .filter_map(|rule| rule.get("executables")?.as_array())
            .flatten()
            .filter_map(|name| name.as_str().map(str::to_owned))
            .collect(),
    )
}

fn remember(names: &mut Vec<String>, name: &str) {
    let lower = name.to_lowercase();
    if !names.contains(&lower) {
        names.push(lower);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::fs::symlink;
    use std::path::PathBuf;
    use std::time::{SystemTime, UNIX_EPOCH};

    /// A temporary `/proc`-like directory, removed when the test ends.
    struct Fixture(PathBuf);

    impl Fixture {
        fn new(tag: &str) -> Self {
            let nanos = SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .map(|d| d.as_nanos())
                .unwrap_or(0);
            let root = std::env::temp_dir()
                .join(format!("fgvk-procs-{tag}-{}-{nanos}", std::process::id()));
            fs::create_dir_all(&root).expect("create fixture root");
            Self(root)
        }

        fn root(&self) -> &Path {
            &self.0
        }

        /// Adds `/proc/<pid>` with the given files. Each argument is optional, as in `/proc`.
        fn process(&self, pid: &str, exe: Option<&str>, comm: Option<&str>, maps: Option<&str>) {
            let dir = self.0.join(pid);
            fs::create_dir_all(&dir).expect("create pid directory");
            if let Some(target) = exe {
                symlink(target, dir.join("exe")).expect("create exe link");
            }
            if let Some(text) = comm {
                fs::write(dir.join("comm"), text).expect("write comm");
            }
            if let Some(text) = maps {
                fs::write(dir.join("maps"), text).expect("write maps");
            }
        }

        fn owner(&self) -> u32 {
            fs::metadata(&self.0).expect("fixture metadata").uid()
        }
    }

    impl Drop for Fixture {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }

    const NATIVE_MAPS: &str = "\
00400000-00452000 r-xp 00000000 08:01 1234 /opt/games/factorio/bin/x64/factorio
7f0000000000-7f0000100000 r--p 00000000 08:01 99 /usr/lib/libc.so.6
";

    const WINE_MAPS: &str = "\
7f0000000000-7f0000100000 r--p 00000000 08:01 99 /usr/lib/wine/x86_64-windows/ntdll.dll
7f0000200000-7f0000300000 r--p 00000000 08:01 42 /usr/lib/libvulkan.so.1
7f0000400000-7f0000500000 r--p 00000000 08:01 77 /home/u/Games/Some Game/SomeGame.EXE
7f0000600000-7f0000700000 r--p 00000000 08:01 78 /home/u/Games/Other/Other.exe
";

    fn process(exe: Option<&str>, maps: &str, comm: &str) -> Process {
        Process::new(7, exe.map(str::to_owned), maps, comm).expect("process has a name")
    }

    #[test]
    fn native_process_suggests_the_exe_basename() {
        let found = process(
            Some("/opt/games/factorio/bin/x64/factorio"),
            NATIVE_MAPS,
            "factorio\n",
        );
        assert_eq!(found.entry, "factorio");
        assert_eq!(found.comm, "factorio");
        assert!(!found.wine);
        assert!(!found.vulkan);
        assert!(!found.launcher);
        assert_eq!(found.display_path(), "/opt/games/factorio/bin/x64/factorio");
    }

    #[test]
    fn wine_process_uses_the_first_mixed_case_exe_mapping() {
        let found = process(
            Some("/home/u/.steam/compat/GE-Proton9/files/bin/wine64-preloader"),
            WINE_MAPS,
            "SomeGame.EXE\n",
        );
        assert!(found.wine);
        assert_eq!(
            found.windows_exe.as_deref(),
            Some("/home/u/Games/Some Game/SomeGame.EXE")
        );
        assert_eq!(found.entry, "SomeGame.EXE");
        assert_eq!(found.display_path(), "/home/u/Games/Some Game/SomeGame.EXE");
    }

    #[test]
    fn windows_exe_is_only_taken_for_wine_processes() {
        let found = process(Some("/opt/games/native/launcher"), WINE_MAPS, "launcher");
        assert!(!found.wine);
        assert_eq!(found.windows_exe, None);
        assert_eq!(found.entry, "launcher");
    }

    #[test]
    fn wine_helpers_without_a_mapped_exe_are_not_listed() {
        // A Vulkan-mapped helper still has no Windows executable, so it is dropped even with
        // "Only Vulkan apps" turned off.
        const HELPER_MAPS: &str = "\
7f0000200000-7f0000300000 r--p 00000000 08:01 42 /usr/lib/libvulkan.so.1
7f0000000000-7f0000100000 r--p 00000000 08:01 99 /usr/lib/wine/x86_64-windows/ntdll.dll
";
        assert_eq!(
            Process::new(
                11,
                Some("/usr/lib/wine/wineserver".to_owned()),
                HELPER_MAPS,
                "wineserver"
            ),
            None
        );
        assert_eq!(
            Process::new(
                12,
                Some("/home/u/.steam/compat/GE-Proton9/files/bin/wine64-preloader".to_owned()),
                HELPER_MAPS,
                "wine64-preloader"
            ),
            None
        );
    }

    #[test]
    fn missing_exe_link_falls_back_to_the_thread_name() {
        let found = Process::new(9, None, NATIVE_MAPS, "Stardew Valley\n").expect("named");
        assert_eq!(found.exe, None);
        assert_eq!(found.entry, "Stardew Valley");
        assert_eq!(found.display_path(), "Stardew Valley");
    }

    #[test]
    fn process_without_any_name_is_not_listed() {
        assert_eq!(Process::new(9, None, "", "   \n"), None);
        assert_eq!(Process::new(9, Some("/".to_owned()), "", ""), None);
    }

    #[test]
    fn vulkan_is_detected_from_maps() {
        assert!(process(Some("/opt/x/game"), WINE_MAPS, "game").vulkan);
        assert!(!process(Some("/opt/x/game"), NATIVE_MAPS, "game").vulkan);
    }

    #[test]
    fn suggested_entry_prefers_windows_then_exe_then_comm() {
        assert_eq!(
            suggested_entry(Some("/a/Win.exe"), Some("/b/wine"), "wine"),
            Some("Win.exe".to_owned())
        );
        assert_eq!(
            suggested_entry(None, Some("/b/native"), "comm"),
            Some("native".to_owned())
        );
        assert_eq!(suggested_entry(None, None, "comm"), Some("comm".to_owned()));
        assert_eq!(suggested_entry(None, None, ""), None);
        assert_eq!(suggested_entry(None, Some("/b/"), ""), None);
    }

    #[test]
    fn launcher_registry_parses_and_flags_listed_launchers() {
        let names = parse_launcher_registry(LAUNCHER_REGISTRY).expect("registry is valid JSON");
        assert!(names.iter().any(|name| name == "EADesktop.exe"));
        assert!(names.iter().any(|name| name == "UbisoftConnect.exe"));
        assert!(names.iter().any(|name| name == "RockstarService.exe"));
        assert!(!names
            .iter()
            .any(|name| name.eq_ignore_ascii_case("Launcher.exe")));
        assert_eq!(parse_launcher_registry("{}"), None);

        assert!(is_launcher_name("eadesktop.exe"));
        assert!(!is_launcher_name("Launcher.exe"));

        let wine = process(
            Some("/x/proton/files/bin/wine"),
            "7f00-7f01 r--p 0 08:01 5 /home/u/EA/ealauncher.exe\n",
            "ealauncher.exe",
        );
        assert!(wine.launcher);
        assert_eq!(wine.entry, "ealauncher.exe");
    }

    #[test]
    fn group_counts_processes_and_merges_flags() {
        let processes = vec![
            process(Some("/opt/a/alpha"), NATIVE_MAPS, "alpha"),
            process(Some("/opt/a/alpha"), WINE_MAPS, "alpha"),
            process(Some("/opt/b/beta"), NATIVE_MAPS, "beta"),
        ];
        let groups = group(&processes);
        assert_eq!(groups.len(), 2);
        let alpha = groups
            .iter()
            .find(|g| g.entry == "alpha")
            .expect("alpha group");
        assert_eq!(alpha.processes, 2);
        assert!(alpha.vulkan);
        assert!(!alpha.wine);
        assert_eq!(alpha.path, "/opt/a/alpha");
        let beta = groups
            .iter()
            .find(|g| g.entry == "beta")
            .expect("beta group");
        assert_eq!(beta.processes, 1);
        assert!(!beta.vulkan);
    }

    #[test]
    fn groups_sort_vulkan_first_then_by_name_ignoring_case() {
        let processes = vec![
            process(Some("/x/beta"), NATIVE_MAPS, "beta"),
            process(Some("/x/Alpha"), NATIVE_MAPS, "Alpha"),
            process(Some("/x/zeta"), WINE_MAPS, "zeta"),
            process(Some("/x/gamma"), WINE_MAPS, "gamma"),
        ];
        let order: Vec<String> = group(&processes).into_iter().map(|g| g.entry).collect();
        assert_eq!(order, ["gamma", "zeta", "Alpha", "beta"]);
    }

    #[test]
    fn search_matches_entry_path_and_thread_name_case_insensitively() {
        let processes = vec![
            process(Some("/opt/games/launch"), NATIVE_MAPS, "xyzzy"),
            process(Some("/usr/bin/other"), NATIVE_MAPS, "other"),
        ];
        let groups = group(&processes);

        let by_entry: Vec<&str> = visible(&groups, "  LAUNCH ", false)
            .iter()
            .map(|g| g.entry.as_str())
            .collect();
        assert_eq!(by_entry, ["launch"]);

        assert_eq!(visible(&groups, "games/", false).len(), 1);
        assert_eq!(visible(&groups, "XYZ", false).len(), 1);
        assert!(visible(&groups, "nothing like this", false).is_empty());
        assert_eq!(visible(&groups, "", false).len(), 2);
    }

    #[test]
    fn vulkan_only_filter_keeps_order_and_drops_other_groups() {
        let processes = vec![
            process(Some("/x/plain"), NATIVE_MAPS, "plain"),
            process(Some("/x/vulkan"), WINE_MAPS, "vulkan"),
        ];
        let groups = group(&processes);
        let shown: Vec<&str> = visible(&groups, "", true)
            .iter()
            .map(|g| g.entry.as_str())
            .collect();
        assert_eq!(shown, ["vulkan"]);
        assert_eq!(visible(&groups, "", false).len(), 2);
    }

    #[test]
    fn dedupe_matches_the_add_button_and_never_repeats_entries() {
        let existing = vec!["Game.exe".to_owned(), "factorio".to_owned()];
        assert!(is_listed("Game.exe", &existing));
        assert!(!is_listed("game.exe", &existing));

        let merged = with_added(
            &existing,
            &[
                "factorio".to_owned(),
                "Other.exe".to_owned(),
                "Other.exe".to_owned(),
            ],
        );
        assert_eq!(merged, ["Game.exe", "factorio", "Other.exe"]);

        let unchanged = with_added(&existing, &["Game.exe".to_owned()]);
        assert_eq!(unchanged.len(), existing.len());
    }

    #[test]
    fn scan_keeps_owned_numeric_entries_and_reads_their_files() {
        let fixture = Fixture::new("scan");
        fixture.process(
            "100",
            Some("/opt/games/factorio/bin/x64/factorio"),
            Some("factorio\n"),
            Some(NATIVE_MAPS),
        );
        fixture.process("200", Some("/opt/x/self"), Some("self\n"), Some(""));
        fixture.process("300", None, Some("nameonly\n"), None);
        fixture.process("self", Some("/opt/x/ignored"), Some("ignored\n"), None);
        fixture.process("400", None, Some("  \n"), None);
        fs::write(fixture.root().join("500"), "a regular file").expect("write file");

        let owner = fixture.owner();
        let listed = scan(fixture.root(), owner, 200);
        let pids: Vec<u32> = listed.iter().map(|p| p.pid).collect();
        assert_eq!(pids, [100, 300]);
        assert_eq!(listed[0].entry, "factorio");
        assert_eq!(listed[1].entry, "nameonly");
    }

    #[test]
    fn scan_drops_wine_helpers_and_keeps_wine_games_with_a_windows_exe() {
        let fixture = Fixture::new("helpers");
        fixture.process(
            "100",
            Some("/usr/lib/wine/wineserver"),
            Some("wineserver\n"),
            Some(NATIVE_MAPS),
        );
        fixture.process(
            "200",
            Some("/usr/lib/wine/wine64-preloader"),
            Some("SomeGame.EXE\n"),
            Some(WINE_MAPS),
        );
        let listed = scan(fixture.root(), fixture.owner(), 1);
        let pids: Vec<u32> = listed.iter().map(|p| p.pid).collect();
        assert_eq!(pids, [200]);
        assert_eq!(listed[0].entry, "SomeGame.EXE");
    }

    #[test]
    fn scan_skips_every_entry_when_the_owner_differs() {
        let fixture = Fixture::new("foreign");
        fixture.process("100", Some("/opt/games/game"), Some("game\n"), Some(""));
        let owner = fixture.owner();
        assert_eq!(scan(fixture.root(), owner, 1).len(), 1);
        assert!(scan(fixture.root(), owner.wrapping_add(1), 1).is_empty());
    }

    #[test]
    fn scan_of_a_missing_root_is_empty() {
        let missing = std::env::temp_dir().join("fgvk-procs-does-not-exist");
        assert!(scan(&missing, 0, 1).is_empty());
    }

    #[test]
    fn oversized_files_are_read_only_up_to_the_cap() {
        let fixture = Fixture::new("cap");
        let big = fixture.root().join("big");
        fs::write(&big, vec![b'a'; READ_LIMIT as usize + 1024]).expect("write big file");
        let text = read_text(&big).expect("readable");
        assert_eq!(text.len(), READ_LIMIT as usize);
    }

    #[test]
    fn pid_names_must_be_plain_digits() {
        assert_eq!(parse_pid(OsStr::new("42")), Some(42));
        assert_eq!(parse_pid(OsStr::new("+42")), None);
        assert_eq!(parse_pid(OsStr::new("self")), None);
        assert_eq!(parse_pid(OsStr::new("")), None);
    }
}
