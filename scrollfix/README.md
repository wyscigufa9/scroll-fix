# ScrollFix

ScrollFix is a lightweight Windows utility that filters accidental mouse-wheel
direction reversals caused by a worn or dirty mechanical encoder.

It runs quietly in the notification area and processes wheel events only when
they occur. There is no polling loop, background service, network access,
telemetry, or diagnostic logging.

## Features

- Filters isolated wheel ticks reported in the wrong direction.
- Handles vertical and horizontal scrolling independently.
- Preserves normal middle-click behavior by default.
- Can bypass filtering while Ctrl or Alt is held.
- Includes a tray menu for pausing, reloading configuration, and managing
  autostart.
- Uses a small native C++ executable with minimal CPU and memory usage.

## Requirements

- Windows 10 or Windows 11
- A 64-bit system

No installation or administrator rights are required.

## Usage

1. Download `ScrollFix.exe` from the
   [Releases](../../releases/latest) page.
2. Store it in a permanent directory.
3. Run `ScrollFix.exe`.
4. Right-click the ScrollFix icon in the notification area to access its
   options.

Choose **Start with Windows** from the tray menu if you want ScrollFix to start
automatically after signing in.

> [!NOTE]
> Windows SmartScreen may warn about an unsigned executable. If you built or
> downloaded it from a source you trust, choose **More info** and then
> **Run anyway**.

## Building from source

Install Visual Studio 2022 with the **Desktop development with C++** workload,
then run:

```powershell
.\build.ps1
```

The release executable will be created at:

```text
build\Release\ScrollFix.exe
```

To produce a debug build:

```powershell
.\build.ps1 -Configuration Debug
```

The project can also be built with CMake 3.20 or newer:

```powershell
cmake -S . -B build-cmake
cmake --build build-cmake --config Release
```

## Configuration

On first launch, ScrollFix creates:

```text
%LOCALAPPDATA%\ScrollFix\config.ini
```

After changing the file, select **Reload configuration** from the tray menu.

```ini
[filter]
enabled=1
filter_vertical=1
filter_horizontal=1
strict_gesture_lock=1
score_reset_ms=220
block_middle_button=0
bypass_with_ctrl=1
bypass_with_alt=1
configuration_version=13
direction_switch_score=360
maximum_event_delta=960
```

| Setting | Description |
| --- | --- |
| `enabled` | Enables or pauses the filter. |
| `filter_vertical` | Filters vertical wheel input. |
| `filter_horizontal` | Filters horizontal wheel input. |
| `strict_gesture_lock` | Rejects opposite-direction ticks until a genuine direction change is confirmed. |
| `score_reset_ms` | Clears accumulated direction evidence after this idle period. |
| `direction_switch_score` | Evidence required to accept a direction reversal. Lower values react faster; higher values filter more aggressively. |
| `maximum_event_delta` | Rejects unusually large, malformed wheel events. |
| `block_middle_button` | Set to `1` to disable wheel clicks when a faulty mouse activates auto-scroll accidentally. |
| `bypass_with_ctrl` | Bypasses filtering while Ctrl is held. |
| `bypass_with_alt` | Bypasses filtering while Alt is held. |

Values outside their supported ranges are clamped when loaded.

## Command-line options

Autostart can also be managed from PowerShell:

```powershell
.\ScrollFix.exe --install-autostart
.\ScrollFix.exe --remove-autostart
```

The autostart entry is stored per user under
`HKCU\Software\Microsoft\Windows\CurrentVersion\Run`.

## How it works

ScrollFix uses the Windows low-level mouse hook. Events matching the currently
accepted direction pass through immediately. Suspicious opposite-direction
events are discarded until their cumulative movement provides enough evidence
of an intentional reversal.

Events are never queued or replayed, so the filter does not intentionally add
scrolling delay. Injected wheel events are ignored.

## Limitations

ScrollFix reduces symptoms; it cannot repair a physically damaged encoder.
Touchpads and free-spinning wheels may naturally change direction very
quickly, so they can require less aggressive configuration. Cleaning or
replacing a failing encoder remains the permanent solution.

### Anti-cheat compatibility

Compatibility with FACEIT Anti-Cheat, Riot Vanguard, and other anti-cheat
software has not been tested. ScrollFix uses a Windows low-level mouse hook,
which an anti-cheat may block or treat as unexpected input-related software.
Use ScrollFix alongside anti-cheat software at your own risk. If in doubt,
close ScrollFix before launching a protected game.

## Uninstall

1. Disable **Start with Windows** from the tray menu.
2. Exit ScrollFix.
3. Delete `ScrollFix.exe`.
4. Optionally delete `%LOCALAPPDATA%\ScrollFix`.

## License

Copyright (c) 2026 wyscigufa9.

Distributed under the [MIT License](LICENSE).
