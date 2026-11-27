# OBS Random Clip

An OBS Studio source plugin that plays a random video clip from a folder when you press a hotkey. When the clip finishes, it disappears and is completely unloaded, so nothing is held in memory or decoding while it's idle.

## Features

- Adds a **Random Clip Player** source you can put in any scene
- One hotkey plays a random clip, and an optional second hotkey stops it
- Each clip plays once, then the video closes and its decoder is unloaded
- The folder is rescanned on every press, so new clips are picked up without restarting OBS
- Optionally searches subfolders and avoids playing the same clip twice in a row
- **Loop mode:** keeps playing random clips back to back, going through every clip in the folder once before any repeats, until you stop it or switch scenes
- **Auto-start:** starts playing automatically when you switch to a scene that contains the source, and works together with loop mode
- Stops the clip when you switch scenes by default
- Clip audio comes through the source's own mixer channel, so volume, filters and monitoring work as normal
- Clips are scaled to fit the source and centered, so the source's size and position in the scene stay the same between clips
- Lightweight: folder scanning and clip loading happen off OBS's render thread, only one clip is ever loaded at a time (even when looping), and the plugin does almost nothing while idle

## Requirements

- OBS Studio 31 or newer
- Windows, macOS or Linux

## Installation

Download the latest build for your system from the [Releases](https://github.com/OngoGablogian/obs-random-clip/releases) page.

**Windows:** extract the zip into your OBS folder (usually `C:\Program Files\obs-studio`) and allow it to merge with the existing folders. That puts the files here:

- `obs-plugins\64bit\obs-random-clip.dll`
- `data\obs-plugins\obs-random-clip\locale\en-US.ini`

**macOS:** run the `.pkg` installer.

**Ubuntu:** install the `.deb` package.

Restart OBS after installing.

## Usage

1. In the **Sources** panel, click **+** and choose **Random Clip Player**.
2. In the properties window, set **Clip Folder** to the folder with your videos.
3. Go to **File → Settings → Hotkeys**, find your source by name, and set a key for **Play Random Clip**. You can also set one for **Stop Clip**.
4. Press the hotkey to play a random clip.

The hotkey only plays a clip while the source is visible (on screen, in the studio mode preview, or in its properties window). If several Random Clip Player sources share the same hotkey, only the visible ones play.

You can also use the **Play Random Clip** and **Stop Clip** buttons in the source's properties to test it.

## Settings

| Setting | Default | Description |
|---|---|---|
| Clip Folder | – | Folder to pick clips from |
| Include subfolders | Off | Also search subfolders, up to 8 levels deep |
| Don't play the same clip twice in a row | On | Skips the clip that played last |
| Hotkey while a clip is playing starts a new clip | On | When off, presses during a clip are ignored. In loop mode this skips to the next clip |
| Loop random clips until stopped or the scene changes | Off | Plays random clips back to back. Every clip plays once before any repeats. Ends when you press **Stop Clip**, the source is hidden, or the scene changes |
| Start playing automatically when switching to a scene with this source | Off | Starts a clip (or a loop, if loop mode is on) when you switch to a scene where the source is visible |
| Stop the clip when the scene changes | On | Stops the clip on any scene switch, even if the new scene also contains this source |
| Stop the clip when the source is hidden | On | Stops the clip when the source is hidden or its scene goes out of view |
| Use hardware decoding when available | Off | Decodes clips on the GPU instead of the CPU |
| Width / Height | Canvas size | Size of the source. Clips are scaled to fit and centered |

**Supported formats:** mp4, mkv, mov, webm, avi, flv, m4v, ts, mts, m2ts, mpg, mpeg, wmv, gif

## Performance tips

- Clips that match your canvas resolution (for example 1080p H.264) are the cheapest to decode. 4K, HEVC or 10-bit AV1 clips cost more for no visible benefit on a 1080p canvas.
- Turn on **Use hardware decoding** if your clips are large or high bitrate.
- Keep clips on a local drive (an SSD if possible) rather than a network share.

## Building

This project is based on [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate).

**With GitHub Actions:** pushing to `main` builds Windows, macOS and Linux packages. You can download them from the workflow run's **Artifacts**. Pushing a version tag (for example `1.0.0`) also creates a draft release with the packages attached.

**Locally:** follow the [obs-plugintemplate build instructions](https://github.com/obsproject/obs-plugintemplate/wiki/Quick-Start-Guide) for your platform, using the presets in `CMakePresets.json`.

## License

GPL-2.0. See [LICENSE](LICENSE).
