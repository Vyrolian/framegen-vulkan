#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
repository_root="$(cd "$repo_root/.." && pwd)"
cd "$repo_root"

release_remote="${FGVK_RELEASE_REMOTE:-origin}"
requested_version=""
ci_skip_suffix=""
if [[ "${FGVK_RELEASE_SKIP_TESTS:-0}" == "1" ]]; then
    ci_skip_suffix=" [skip ci]"
fi

usage() {
    cat <<'EOF'
Usage: scripts/publish-package.sh [--version X.Y.Z]

Builds, verifies, and publishes framegen-vulkan. When --version is supplied, the
script updates and commits engine/VERSION first. After publishing, it commits
the generated Renderer release pin and Arch recipe automatically.
engine/RELEASE_NOTES.md must contain the matching manually curated heading.
EOF
}

while (($#)); do
    case "$1" in
        --version)
            if (($# < 2)); then
                echo "--version requires a value." >&2
                usage >&2
                exit 2
            fi
            requested_version="$2"
            shift 2
            ;;
        --version=*)
            requested_version="${1#*=}"
            shift
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [[ -n "$requested_version" && ! "$requested_version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "Release version must use X.Y.Z format: $requested_version" >&2
    exit 2
fi

for command in gh git node python3; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "Required command not found: $command" >&2
        exit 1
    fi
done
if [[ "$(uname -s)" != "Linux" ]]; then
    echo "Publishing builds the archives natively and runs only on Linux; this host is $(uname -s)." >&2
    exit 2
fi
# The archives are built on this host after the optional version commit, so check the native
# build tools before anything is pushed.
for command in cargo fakeroot flatpak flatpak-builder makepkg; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "Publishing needs $command on the host to build the archives. Install it and try again." >&2
        exit 1
    fi
done

release_branch="$(git branch --show-current)"
if [[ "$release_branch" != "main" ]]; then
    echo "Publish from main; current branch is $release_branch." >&2
    exit 1
fi

if [[ -n "$(git status --porcelain --untracked-files=normal)" ]]; then
    echo "Working tree has uncommitted changes. Commit or stash them before publishing." >&2
    exit 1
fi

notes_version="${requested_version:-$(tr -d '[:space:]' < VERSION)}"
manual_release_notes="$(
    node "$repository_root/scripts/read-release-notes.mjs" \
        "$repo_root/RELEASE_NOTES.md" "framegen-vulkan" "$notes_version"
)"

if ! git remote get-url "$release_remote" >/dev/null 2>&1; then
    echo "Release remote '$release_remote' is not configured." >&2
    exit 1
fi

release_repository="$(git remote get-url "$release_remote")"
release_repository="${release_repository#git@github.com:}"
release_repository="${release_repository#https://github.com/}"
release_repository="${release_repository%.git}"
if [[ ! "$release_repository" =~ ^[A-Za-z0-9._-]+/[A-Za-z0-9._-]+$ ]]; then
    echo "Could not determine a GitHub owner/repository from remote '$release_remote'." >&2
    exit 1
fi

if [[ "${release_repository,,}" == eugeniosegala/mako ]]; then
    echo "Set the release remote to the framegen-vulkan repository before publishing." >&2
    exit 1
fi

if command -v sha256sum >/dev/null 2>&1; then
    checksum_command=(sha256sum)
elif command -v shasum >/dev/null 2>&1; then
    checksum_command=(shasum -a 256)
else
    echo "Required command not found: sha256sum or shasum" >&2
    exit 1
fi

if ! gh auth status >/dev/null 2>&1; then
    echo "GitHub CLI is not authenticated. Run: gh auth login -h github.com" >&2
    exit 1
fi

git fetch "$release_remote" \
    "refs/heads/main:refs/remotes/$release_remote/main" --tags --quiet
remote_main_commit="$(git rev-parse "refs/remotes/$release_remote/main")"
if [[ "$(git rev-parse HEAD)" != "$remote_main_commit" ]]; then
    echo "Local main must exactly match $release_remote/main before publishing. Push or synchronize the release commit first." >&2
    exit 1
fi

if [[ -n "$requested_version" ]]; then
    current_version="$(tr -d '[:space:]' < VERSION)"
    if [[ "$current_version" != "$requested_version" ]]; then
        printf '%s\n' "$requested_version" > VERSION
        git add VERSION
        git commit -m "Release framegen-vulkan v$requested_version$ci_skip_suffix"
        git push "$release_remote" "$release_branch"
    fi
fi

version="$(tr -d '[:space:]' < VERSION)"
if [[ ! "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "engine/VERSION must use X.Y.Z format: $version" >&2
    exit 1
fi

tag="v$version"
archive="out/framegen-vulkan-v$version-linux.tar.xz"
flatpak_archive="out/framegen-vulkan-v$version-flatpaks.tar.xz"
source_commit="$(git rev-parse HEAD)"

tag_exists=false
if git rev-parse -q --verify "refs/tags/$tag" >/dev/null; then
    tag_commit="$(git rev-list -n 1 "$tag")"
    if [[ "$tag_commit" != "$source_commit" ]]; then
        echo "Tag $tag does not point at HEAD. Publish from its intended commit or bump VERSION." >&2
        exit 1
    fi
    tag_exists=true
fi

# Build with the same native builders as complete local release candidates. Host SDK selection
# must not change the compiled presentation contract: package-local.sh compiles against the pin.
scripts/package-local.sh "$archive"
checksum="$("${checksum_command[@]}" "$archive" | awk '{print $1}')"
scripts/package-flatpaks.sh "$flatpak_archive"
flatpak_checksum="$("${checksum_command[@]}" "$flatpak_archive" | awk '{print $1}')"

pin_renderer_release() {
    node "$repo_root/scripts/pin-renderer-release.mjs" \
        "$repo_root/renderer-release.json" \
        "$version" \
        "$tag" \
        "$source_commit" \
        "$release_repository" \
        "$archive" \
        "$checksum" \
        "$flatpak_archive" \
        "$flatpak_checksum" \
        "$@"
}

# The Arch recipe is derived from the newly built archive pin, then the exact
# package that passes verification is retained and recorded for publication.
pin_renderer_release
python3 "$repo_root/dist/arch/sync-release-pin.py" \
    "$repo_root/renderer-release.json" \
    "$repo_root/dist/arch/PKGBUILD"
"$repo_root/dist/arch/check-release-pin.sh"
arch_pkgver="$(sed -n 's/^pkgver=\(.*\)$/\1/p' "$repo_root/dist/arch/PKGBUILD" | head -n 1)"
arch_pkgrel="$(sed -n 's/^pkgrel=\(.*\)$/\1/p' "$repo_root/dist/arch/PKGBUILD" | head -n 1)"
if [[ "$arch_pkgver" != "$version" || -z "$arch_pkgrel" ]]; then
    echo "Synchronized Arch package identity is invalid: ${arch_pkgver:-missing}-${arch_pkgrel:-missing}" >&2
    exit 1
fi
arch_package="out/framegen-vulkan-bin-${arch_pkgver}-${arch_pkgrel}-x86_64.pkg.tar.zst"
source_commit_epoch="$(git show -s --format=%ct "$source_commit")"
SOURCE_DATE_EPOCH="$source_commit_epoch" \
    "$repo_root/dist/arch/verify-release-package.sh" "$archive" "$arch_package"
arch_checksum="$("${checksum_command[@]}" "$arch_package" | awk '{print $1}')"
pin_renderer_release "$arch_package" "$arch_checksum"

notes_file="$(mktemp "${TMPDIR:-/tmp}/fgvk-release-notes.XXXXXX")"
cleanup() {
    rm -f "$notes_file"
}
trap cleanup EXIT

cat > "$notes_file" <<EOF
$manual_release_notes

## 🎮 In-game considerations

> [!TIP]
> **Try the game’s V-Sync setting both on and off.** Neither setting is universally best: the result depends on the game, its FPS cap, VRR, and the compositor. Keep whichever option feels smoother and more responsive for that game.

Every game, renderer, and display setup behaves differently. Compare Frame Generation settings one setting at a time. If Frame Generation does not work, test Fullscreen, Borderless Fullscreen, and Windowed; none is universally best. Changing display mode can change GPU cost. Restart after changing a setting labelled **(Restart)**. Flow Scale and Lighter FG Model normally apply live and may cause one brief hitch while framegen-vulkan replaces their private resources.

- **Quality and latency tuning:** Higher multipliers and lower real-frame rates can increase ghosting and input latency. Compare multipliers per game.

> [!IMPORTANT]
> framegen-vulkan does not contain or distribute Lossless Scaling, \`Lossless.dll\`, or extracted proprietary model payloads. Frame Generation reads selected resources at runtime from a lawful, user-supplied Lossless Scaling installation. framegen-vulkan does not alter the user's DLL file, and translated resources remain process-local. Users are responsible for complying with the terms applicable to their copy.

## Installation

### Choose the right download

| Release file | Use it for | Installation owner |
| --- | --- | --- |
| \`$(basename "$archive")\` | General Linux and direct user-local installation, including SteamOS | The included graphical installer manages files under \`~/.local\`. |
| \`$(basename "$arch_package")\` | Arch Linux or a traditional writable Arch-based distribution—not SteamOS | Pacman manages the system-wide files under \`/usr\`. |
| \`$(basename "$flatpak_archive")\` | Flatpak games, launchers, and emulators | The archive contains separate framegen-vulkan Vulkan extensions for the supported Freedesktop runtimes; it does not install the host CLI. |

GitHub's automatically generated **Source code** ZIP and tarball contain source code, not ready-to-run framegen-vulkan packages.

### Host archive

Download \`$(basename "$archive")\`, extract it into a new folder, then use the included graphical installer:

1. Double-click **Install framegen-vulkan**.
2. Choose **Execute** if your file manager asks.
3. Confirm the installation. The installer verifies the package, installs it under \`~/.local\`, safely updates existing files, and preserves profiles.

To remove it later, run \`~/.local/bin/fgvk-installer --uninstall\` from a terminal. The removal keeps your profiles unless you explicitly choose to remove them.

The host archive includes 64-bit and 32-bit Vulkan layers; the CLI is 64-bit.

Configure profiles in \`~/.config/framegen-vulkan/conf.toml\`, then validate them with \`~/.local/bin/fgvk-cli validate\`. Set the licensed DLL path in the profile when using Frame Generation, then use \`~/.local/bin/fgvk %command%\` for a direct Steam launch. Run \`~/.local/bin/fgvk\` without arguments to open the settings window.

### Arch Linux package

Download \`$(basename "$arch_package")\` and leave it unextracted. Open a terminal in the folder containing the downloaded file, then install the verified system-wide package with:

\`\`\`bash
sudo pacman -U ./$(basename "$arch_package")
\`\`\`

The \`./\` prefix refers to the terminal's current folder; this command does not download the package. If you saved the file elsewhere, use its full path instead. If pacman reports \`could not find or read package\`, check the path and exact filename, or download the package again if it is missing.

The package installs framegen-vulkan under \`/usr\`, preserves profiles under \`~/.config/framegen-vulkan\`, and does not touch a user-local Renderer under \`~/.local\`. Use either the system package or the user-local installation as the active owner for a user; see the [Arch package guide](https://github.com/$release_repository/blob/main/engine/dist/arch/README.md) for coexistence details.

After installation, create a game profile in \`~/.config/framegen-vulkan/conf.toml\`, validate it with \`fgvk-cli validate\`, and use \`/usr/bin/fgvk %command%\` as that native Steam or Proton game's launch option. Installing the package alone does not activate framegen-vulkan globally.

### Flatpak runtime extensions

Download and extract \`$(basename "$flatpak_archive")\`. It contains one self-contained framegen-vulkan extension for each supported Flatpak runtime. Install the extension matching the application runtime, for example:

\`\`\`bash
flatpak install --user ./org.freedesktop.Platform.VulkanLayer.fgvkrender-26.08.flatpak
\`\`\`

## Updating an existing framegen-vulkan installation

1. Quit every game or application currently using framegen-vulkan.
2. For a user-local installation, download the newer host archive, extract it into a new folder, and run **Install framegen-vulkan** again. For the Arch package, download the newer \`.pkg.tar.zst\` and run \`sudo pacman -U\` on it.
3. If you use Flatpak applications, download the matching Flatpak archive and reinstall the extension for each runtime you use.
4. Restart the game. Revalidate the configuration with \`fgvk-cli validate\` if you changed the DLL path or profiles.

Keep the previous archives until the new version has been tested with your games.

## Before you play

- Test framegen-vulkan per game. Start with Frame Generation disabled, then enable it and compare the result.
- If you use Frame Generation, confirm the detected \`Lossless.dll\` path before launching. Leaving it blank permits normal discovery.
- Do not combine framegen-vulkan with another frame-generation Vulkan wrapper for the same game.

EOF

if [[ "$tag_exists" == false ]]; then
    git tag -a "$tag" -m "framegen-vulkan v$version"
fi
git push "$release_remote" "$release_branch"
git push "$release_remote" "$tag"

if gh release view "$tag" --repo "$release_repository" >/dev/null 2>&1; then
    node "$repository_root/scripts/upload-release-assets.mjs" \
        "$release_repository" "$tag" \
        "$archive" "$arch_package" "$flatpak_archive"
    gh release edit "$tag" \
        --repo "$release_repository" \
        --title "framegen-vulkan v$version" \
        --notes-file "$notes_file" \
        --latest
else
    gh release create "$tag" "$archive" "$arch_package" "$flatpak_archive" \
        --repo "$release_repository" \
        --title "framegen-vulkan v$version" \
        --latest \
        --notes-file "$notes_file" \
        --verify-tag
fi

release_metadata_paths=(
    engine/renderer-release.json
    engine/dist/arch/PKGBUILD
)
if ! git -C "$repository_root" diff --quiet -- "${release_metadata_paths[@]}"; then
    git -C "$repository_root" add "${release_metadata_paths[@]}"
    git -C "$repository_root" commit -m "Pin framegen-vulkan v$version$ci_skip_suffix"
    git -C "$repository_root" push "$release_remote" "$release_branch"
fi

echo "Published: https://github.com/$release_repository/releases/tag/$tag"
echo "Committed the matching framegen-vulkan release pin and Arch recipe."
