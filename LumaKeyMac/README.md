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

Simplest way: System Preferences → Users & Groups → Login Items → add the
`LumaKey` binary.

Alternatively, a user LaunchAgent starts it in the first seconds after login
(this is the earliest possible start for a menu bar app — it needs the user's
GUI session, so a pre-login LaunchDaemon is not an option). First copy the
binary to a stable location outside the source tree, since `settings.ini` and
`LumaKey.log` are written next to it:

```sh
mkdir -p ~/Applications/LumaKey
cp LumaKey ~/Applications/LumaKey/
```

Then save this as `~/Library/LaunchAgents/com.lumakey.plist` (adjust the
paths to your username):

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>com.lumakey</string>
    <key>ProgramArguments</key>
    <array>
        <string>/Users/YOU/Applications/LumaKey/LumaKey</string>
    </array>
    <key>RunAtLoad</key>
    <true/>
    <key>WorkingDirectory</key>
    <string>/Users/YOU/Applications/LumaKey</string>
    <key>ProcessType</key>
    <string>Interactive</string>
</dict>
</plist>
```

Load it (also starts the app immediately):

```sh
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/com.lumakey.plist
```

`KeepAlive` is deliberately not set, so `Quit LumaKey` from the menu quits it
until the next login instead of respawning it. To remove the autostart:

```sh
launchctl bootout gui/$(id -u)/com.lumakey
rm ~/Library/LaunchAgents/com.lumakey.plist
```
