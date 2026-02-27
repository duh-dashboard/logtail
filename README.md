# Log Tail Widget

Tails a log file or the systemd journal in real time. Supports both file watching (via `inotify`) and streaming `journalctl` output. The source and line buffer size are configurable.

## Requirements

- Linux
- Qt 6.2+ (Widgets)
- CMake 3.21+
- C++20 compiler
- [`widget-sdk`](https://github.com/duh-dashboard/widget-sdk) installed
- `systemd` (optional, for journal mode)

## Build

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=~/.local
cmake --build build
cmake --install build --prefix ~/.local
```

The plugin installs to `~/.local/lib/dashboard/plugins/`.

## Configuration

Configured from the in-widget settings panel.

| Setting | Description |
|---|---|
| **Source** | Path to a log file, or leave empty to use the systemd journal |
| **Unit filter** | `journalctl -u` unit name to filter journal output (journal mode only) |
| **Line buffer** | Maximum number of lines retained in the display |

## Notes

- File mode uses `QFileSystemWatcher` to detect new content without polling.
- Journal mode spawns `journalctl -f` via `QProcess`.
- Both modes are Linux-only.

## License

GPL-3.0-or-later — see [LICENSE](LICENSE).
