# LumaKey

LumaKey is a minimal native Windows tray utility for controlling the built-in laptop display brightness with global hotkeys. It is intended for Windows installations, including Windows on MacBook hardware via Boot Camp, where the built-in display brightness is exposed through Windows/WMI.

LumaKey uses native C++ and the Win32 API. It does not use .NET, Python, Electron, or external dependencies.

## Hotkeys

- `Win + Shift + ]` = brightness up
- `Win + Shift + [` = brightness down

The same actions are also available from the tray icon right-click menu:

- `Brightness +`
- `Brightness -`
- `Exit`

## Build

1. Open **x64 Native Tools Command Prompt for VS 2022**.
2. Change to the `LumaKey` directory.
3. Run:

```bat
build.bat
```

If the build succeeds, it produces `LumaKey.exe`.

## Run

Run `LumaKey.exe`. The app starts without a console window and places an icon in the Windows system tray.

Right-click the tray icon to increase brightness, decrease brightness, or exit. Exiting removes the tray icon and unregisters the global hotkeys.

## Autostart

The simplest autostart setup is a shortcut in the Windows Startup folder:

1. Build `LumaKey.exe`.
2. Press `Win + R`.
3. Run `shell:startup`.
4. Create a shortcut to `LumaKey.exe` in that folder.

Windows will start LumaKey automatically after sign-in.

## Configuration

LumaKey reads `settings.ini` from the same directory as `LumaKey.exe`.

```ini
[Brightness]
Step=10
```

Change `Step` to control how much each hotkey or tray menu action changes brightness. Valid values are `1` through `100`. If `settings.ini` is missing, LumaKey creates it with the default value. If `Step` is missing or invalid, LumaKey uses `10`.

## Known limitation

LumaKey v0.1 targets the built-in laptop display brightness exposed through Windows/WMI. External monitors are not supported in v0.1.
