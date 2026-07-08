# LumaKey for macOS

macOS port of LumaKey for Intel Macs (tested target: macOS Monterey). A menu
bar utility that controls the built-in display brightness with global hotkeys
and shows a bottom-center OSD popup, mirroring the Windows version.

## Hotkeys

- `Option + Shift + ]` = brightness up
- `Option + Shift + [` = brightness down

One-hand friendly. `Cmd + Shift + [ / ]` is deliberately not used: a global
hotkey would shadow the standard tab-switching shortcuts in browsers and
editors. To change the combo, edit the `kHotkeyModifiers` / `kHotkeyUpKey` /
`kHotkeyDownKey` constants at the top of `main.swift`.

The same actions are also available from the menu bar icon (sun symbol):
`Brightness +`, `Brightness −`, `Quit LumaKey`.

## Build

Requires only Command Line Tools (no full Xcode):

```sh
xcode-select --install   # once, if not installed yet
./build.sh
```

This produces the `LumaKey` binary next to the sources.

## Run

```sh
./LumaKey &
```

The app has no Dock icon; look for the sun icon in the menu bar. No special
permissions are required: global hotkeys via `RegisterEventHotKey` do not need
Accessibility or Input Monitoring access.

## Configuration

Reads `settings.ini` from the same directory as the binary (created with
defaults on first run), same format as the Windows version:

```ini
[Brightness]
Step=10
```

A `LumaKey.log` file is written next to the binary for diagnostics.

## How brightness is controlled

The primary path is the private `DisplayServices` framework
(`DisplayServicesGetBrightness` / `DisplayServicesSetBrightness`), which works
on Monterey for the built-in panel. On Intel Macs there is an additional
fallback through the legacy `IODisplayConnect` IOKit service. Both are loaded
dynamically via `dlopen`/`dlsym`, so the binary starts even if one of them is
missing. Which paths are available is written to `LumaKey.log` at startup.

External monitors are not supported (same limitation as the Windows version).

## Autostart

System Preferences → Users & Groups → Login Items → add the `LumaKey` binary.
