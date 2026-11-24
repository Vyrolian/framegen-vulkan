//! Finds Lossless.dll in the Steam libraries when the user presses Auto-locate. Nothing here runs on
//! its own: the renderer uses only the configured path.

use std::fs;
use std::path::{Path, PathBuf};

/// Steam roots under the home directory: native, ~/.steam, Debian, Flatpak and Snap installs.
const HOME_ROOTS: [&str; 5] = [
    ".local/share/Steam",
    ".steam/steam",
    ".steam/debian-installation",
    ".var/app/com.valvesoftware.Steam/.local/share/Steam",
    "snap/steam/common/.local/share/Steam",
];
const DLL_IN_LIBRARY: &str = "steamapps/common/Lossless Scaling/Lossless.dll";

/// Library paths listed in a Steam `libraryfolders.vdf` (`"path"  "/mnt/games/SteamLibrary"` lines).
pub fn library_paths(vdf: &str) -> Vec<PathBuf> {
    vdf.lines()
        .filter_map(|line| line.trim().strip_prefix("\"path\""))
        .filter_map(|rest| {
            let value = rest.trim().strip_prefix('"')?.strip_suffix('"')?;
            (!value.is_empty()).then(|| PathBuf::from(value.replace("\\\\", "\\")))
        })
        .collect()
}

/// The first regular file `Lossless.dll` in the Steam roots and every library they list.
pub fn locate(home: Option<&Path>, xdg_data: Option<&Path>) -> Option<PathBuf> {
    let mut roots: Vec<PathBuf> = Vec::new();
    if let Some(xdg) = xdg_data {
        roots.push(xdg.join("Steam"));
    }
    if let Some(home) = home {
        roots.extend(HOME_ROOTS.iter().map(|root| home.join(root)));
    }
    for root in roots {
        let mut libraries = vec![root.clone()];
        if let Ok(text) = fs::read_to_string(root.join("steamapps/libraryfolders.vdf")) {
            libraries.extend(library_paths(&text));
        }
        for library in libraries {
            let candidate = library.join(DLL_IN_LIBRARY);
            if candidate.is_file() {
                return Some(candidate);
            }
        }
    }
    None
}

/// `locate` with `HOME` and `XDG_DATA_HOME` from the environment; empty values are ignored.
pub fn locate_from_env() -> Option<PathBuf> {
    let var = |name: &str| {
        std::env::var_os(name)
            .filter(|value| !value.is_empty())
            .map(PathBuf::from)
    };
    locate(var("HOME").as_deref(), var("XDG_DATA_HOME").as_deref())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn library_paths_reads_path_lines() {
        let vdf = "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"/home/u/.local/share/Steam\"\n\t}\n\t\"1\"\n\t{\n\t\t\"path\"\t\t\"/mnt/games/SteamLibrary\"\n\t\t\"label\"\t\t\"\"\n\t}\n}\n";
        assert_eq!(
            library_paths(vdf),
            vec![
                PathBuf::from("/home/u/.local/share/Steam"),
                PathBuf::from("/mnt/games/SteamLibrary"),
            ]
        );
    }

    #[test]
    fn locate_finds_a_second_library_and_ignores_directories() {
        let tmp = std::env::temp_dir().join(format!("fgvk-dll-locate-{}", std::process::id()));
        let home = tmp.join("home");
        let lib2 = tmp.join("lib2");
        let vdf_dir = home.join(".local/share/Steam/steamapps");
        fs::create_dir_all(&vdf_dir).unwrap();
        fs::write(
            vdf_dir.join("libraryfolders.vdf"),
            format!("\"path\"\t\t\"{}\"\n", lib2.display()),
        )
        .unwrap();
        let directory_dll =
            home.join(".local/share/Steam/steamapps/common/Lossless Scaling/Lossless.dll");
        fs::create_dir_all(&directory_dll).unwrap();
        let file_dll = lib2.join("steamapps/common/Lossless Scaling/Lossless.dll");
        fs::create_dir_all(file_dll.parent().unwrap()).unwrap();
        fs::write(&file_dll, "synthetic fixture, not a model").unwrap();

        assert_eq!(locate(Some(&home), None), Some(file_dll.clone()));

        fs::remove_file(&file_dll).unwrap();
        assert_eq!(locate(Some(&home), None), None);

        fs::remove_dir_all(&tmp).unwrap();
    }
}
