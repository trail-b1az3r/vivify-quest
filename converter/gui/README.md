# Vivify Quest Converter (desktop app)

Turns a PC Vivify bundle (`bundleWindows2021.vivify` or
`bundleWindows2019.vivify`) into `bundleAndroid2021.vivify`, the file Vivify
Quest loads first from a map's folder. A map that ships it needs no conversion
on the headset. It is the same converter the mod runs, at the revision shown in
the window.

## Using it

1. Drop PC bundles or whole map folders onto the window. You can also click
   **Add bundles...** or **Add map folder...**, or type a path. For a folder,
   it picks `bundleWindows2021.vivify`, or `bundleWindows2019.vivify` if that
   is the only one.
2. Click **Convert all**. Bundles convert one after another, and the window
   stays usable while they do.
3. Each bundle's `bundleAndroid2021.vivify` is written next to it. Use
   **Save as...** to write it somewhere else.

Select a bundle to see the converter's report (what it translated, and every
shader it left alone and why) and what the bundle was built for. **Copy
details** puts all of that on the clipboard, for a bug report.

The options on the left start at what the mod uses on the headset, and
**Mod defaults** puts them back. Hover over **(?)** for what each one does.

You can also pass bundles or map folders on the command line to have them
queued at start.

## Downloads

Get **VivifyQuestConverter-app-windows-x64**, **-linux-x64** or
**-macos-universal** from the **Converter library** workflow's artifacts.

- **Windows:** run `Vivify Quest Converter.exe`.
- **macOS:** the app is not signed. Right-click it, then choose **Open** the
  first time.
- **Linux:** needs an X11 or XWayland session. The **Add...** buttons need
  `zenity` or `kdialog`; without them, drop files on the window or type a path.

## Building

```
cmake -S converter -B build -DVIVIFY_CONVERT_GUI=ON
cmake --build build --config Release --target vivify-convert-gui
```

GLFW 3.4, Dear ImGui 1.91.9b and portable-file-dialogs are fetched at fixed
versions. On Linux, install the X11 and OpenGL headers first:

```
sudo apt-get install libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl-dev libxkbcommon-dev
```
