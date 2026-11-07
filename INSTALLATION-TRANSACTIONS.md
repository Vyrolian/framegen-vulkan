# Native installation transactions

This is the implementation reference for atomic file replacement and rollback during native MAKO Renderer installation with the standalone archive installer. User instructions remain in the [Renderer README](engine/README.md#direct-linux-installation).

## Contract and limits

The installer prepares replacement files separately from their destination and retains previous files until the native installation succeeds. Each destination replacement is atomic; the complete installation is a sequence of replacements and removals with rollback. A reader can observe files from different generations during that sequence. `active-renderer.json` records the selected version after the payload work is complete.

| Property | Standalone archive |
| --- | --- |
| Replacement primitive | Adjacent `replacement` file, then `mv -fT` |
| Previous regular file | Adjacent `cp -Pp` copy |
| Previous symlink | Preserve the link itself with `cp -Pp` |
| Failure recovery | `EXIT` cleanup restores in reverse order while replacing and not committed; `HUP`, `INT`, and `TERM` exit through that cleanup |
| Native success boundary | `install_committed=1` after every destination operation |
| Configuration | Installation leaves configuration alone |
| Durability | No explicit fsync or durable transaction journal |

The transaction does not provide automatic recovery after power loss, `SIGKILL`, or another termination that prevents cleanup from running. It does not acquire a shared installation lock across the installer, the configuration writers, and Remote Play. Run one installer at a time with games using MAKO closed and configuration edits stopped.

The rollback set consists of explicit file paths. It does not restore the directory tree, undo unrelated writes, or coordinate with another process. Directories created during preparation can remain after failure. Temporary files and backups normally disappear on completion; cleanup errors are reported and can leave artifacts.

## Canonical owners

| Owner | Responsibility |
| --- | --- |
| [mako-installer](engine/scripts/mako-installer) | Standalone manifest validation, staging, destination preparation, commit, rollback and uninstall |
| [mako-remote-play](engine/scripts/mako-remote-play) and [`mako_remote_play/`](engine/scripts/mako_remote_play/) | Native Remote Play override, its atomic file helpers, and its restoration command |
| [package-local.sh](engine/scripts/package-local.sh) | Standalone archive layout, version file, checksummed payload manifest, private/public Vulkan manifests and package verification |

## Shared native selection and identity

At the default user-local prefix, the standalone archive stores libraries under `~/.local/lib{,32}/` and supplies shared manifests under `~/.local/share/mako-render/vulkan/` and `~/.local/share/vulkan/implicit_layer.d/`. A custom `MAKO_INSTALL_PREFIX` relocates its payload and state together.

| Record | Meaning |
| --- | --- |
| `~/.local/share/mako-render/active-renderer.json` | Schema-1 selected native identity: `owner` is `standalone`, with `version` |
| `~/.local/share/mako-render/installer/installed-files.sha256` | Relative-path ownership record, with hashes of the installed content |

Direct development deployment uses the same manifest resolver to verify the libraries actually selected by private and registered manifests. It retains package identity and the ownership record as the release baseline; local build hashes belong to the development status record. See [Renderer usage](engine/README.md#usage) for the launch contract.

## File replacement and backup mechanics

The installer creates each staged replacement beside its destination with the requested mode. Every manifest entry is copied to staging and checked against its SHA-256 before any selected destination is replaced. An existing destination symlink is replaced without writing through it. Parent directories must permit creation and rename.

For each existing destination, the installer allocates an adjacent `.mako-install-*` directory. A regular file is copied into it, and a symlink is preserved as a symlink. An absent path is recorded without a backup file. Every replacement uses rename, never an in-place write, so backups remain intact.

On failure, the installer restores backups with `mv -fT` in reverse order and removes paths that were originally absent. It attempts every restoration even if one fails. A failed restoration retains its available backup beside the destination and reports where it is.

## Native Remote Play override

The override is a separate, opt-in Steam transaction, not an additional Renderer payload destination. `engine/scripts/mako_remote_play/remote_play_core.py` owns Steam's native x86_64 `streaming_client`, its `.mako-original` backup and `.mako-original.sha256` checksum, and `~/.config/mako-render/native-remote-play.json`. The shared command snapshots those paths before it changes them and restores the previous state when an installation fails.

A nonblocking exclusive lease rejects mutations during a stream or another mutation. The self-contained launch payload keeps a shared lease across the exec chain; exact executable detection also guards streams launched before installation. Native ELF identity, executable mode, user ownership, checksum, and symlink checks precede replacement. An intact installed wrapper is idempotent. After launcher migrations, changed packaged payload content is refreshed only for an existing intact enabled override, using the verified original's ownership and permissions; the refresh defers during a stream and retains profiles and power tables.

Removal checks the stored wrapper hash and original checksum. It restores the original only when the managed wrapper remains installed. If Steam already replaced it with a native client, removal retires the stale backup and state without overwriting that client. The standalone archive uninstaller invokes the shared restoration command before removing payloads or configuration and stops on failure. An older install without that command refuses cleanup when override bookkeeping or backups exist. System-package removal and manual deletion require prior restoration with `mako-remote-play restore-before-uninstall`.

## Standalone sequence

1. `install_payload()` checks for the packaged manifest and version, validates the version and the relative-path and hash syntax of the new manifest and any existing ownership manifest, and obtains the installer's normal confirmation.
2. It installs `EXIT`, `HUP`, `INT`, and `TERM` traps and stages under `$install_prefix/.mako-install-*`. Every manifest entry is copied with preserved mode and checked against its SHA-256 before any selected destination is replaced.
3. The installer computes `installed-files.sha256` from the staged content and creates `active-renderer.json` there.
4. `prepare_install_destination()` prepares every payload destination with an adjacent `.mako-install-*` directory containing `previous` when the destination existed and `replacement` when new content will be installed. Previous symlinks are copied as links. This finishes for all destinations before `install_replacing=1`.
5. An old ownership entry absent from the new manifest is scheduled for removal only when its current file still matches the old hash. Modified obsolete files are preserved. Current payload paths are replacement targets even if their previous contents were modified. The ownership manifest and active identity are prepared last, in that order.
6. After `install_replacing=1`, the destination loop performs `mv -fT` for replacements and `rm -f` for scheduled removals. Only after the complete loop does it set `install_committed=1` and invoke `finish_install()` to discard staging and backups and remove traps.
7. Completion notification occurs after commit. The installer does not start any interactive application.

Before replacement starts, failure only needs staging cleanup. During replacement, `finish_install()` restores every prepared destination in reverse order from `previous`, or removes it if originally absent. Restoration failures retain available `previous` backups beside the destination and report their locations. The installer returns failure when the native transaction fails.

## Boundaries outside the transaction

- Manual archive extraction, `cmake --install`, and native uninstallation do not pass through these complete native-install transactions. An atomic helper used by one of those paths does not imply the surrounding operation has this rollback contract.
- Configuration and profile files, diagnostics history, installed Flatpak extensions, and the licensed `Lossless.dll` are outside the native payload rollback set. Standalone configuration removal is an explicit uninstall option.
- Adjacent backup naming supports error reporting and manual recovery, not automatic replay. There is no persistent operation journal or startup scan that can safely infer and complete an interrupted installation from leftover directories. Preserve reported backups when investigating restoration failures.

## Validation and maintenance

| Portable evidence | Covered behavior |
| --- | --- |
| [test-mako-installer.sh](engine/scripts/test-mako-installer.sh) | Manifest validation, corrupt late payload before replacement, injected final identity-rename failure, restored payload, ownership and identity records, new-file removal, staging cleanup, permission failures, uninstall preservation, and Remote Play restoration ordering |
| [test-mako-remote-play.py](engine/scripts/test-mako-remote-play.py) | Override install, refresh, restore, lock, and refusal behavior against temporary Steam files |
| [test-mako-launch.sh](engine/scripts/test-mako-launch.sh) | Standalone launcher discovery and activation from the installed prefix |

Run the focused installer checks from the repository root:

```bash
bash engine/scripts/test-mako-installer.sh
python3 engine/scripts/test-mako-remote-play.py
```

These use temporary files and synthetic payloads; they do not install into the user's active environment. Permission cases in the standalone script require a non-root test process. The suites exercise specific injected failures, not every syscall, signal timing, or filesystem.

Follow [TESTING.md](TESTING.md) for the broader portable gates and package evidence proportional to an implementation change. When moving this contract, update the owning installer or helper, its file set or packaging generator, focused failure coverage, and this reference together.
