# OBS Media Randomizer

An OBS Studio source plugin that plays a random video or audio file from a folder when you press a hotkey. When the file finishes, it's completely unloaded, so nothing is held in memory or decoding while it's idle.

## Features

- Adds a **Media Randomizer** source you can put in any scene
- Plays video and audio files. You can limit a source to video only or audio only
- One hotkey plays a random file, and an optional second hotkey stops it
- Each file plays once, then closes and its decoder is unloaded
- The folder is rescanned on every press, so new files are picked up without restarting OBS. Hidden files (such as the `._` files macOS leaves on shared drives) are skipped
- Optionally searches subfolders
- Shuffle mode (default) plays every file in the folder once before any repeats, using a fast, high-quality random number generator
- **Loop mode:** keeps playing random files back to back, going through every file once before any repeats, until you stop it or switch scenes
- **Fades:** files can fade in and out (video and audio), with separate toggles for looped and single files and an adjustable duration
- **Auto-start:** starts playing automatically when you switch to a scene that contains the source, and works together with loop mode
- **Audio normalization (optional):** evens out volume differences between files, measured on the fly with no extra decoding
- Stops playback when you switch scenes, by default
- Audio comes through the source's own mixer channel, so volume, filters and monitoring work as normal
- Video is scaled to fit the source and centered, so the source's size and position in the scene stay the same between files. Audio files show nothing on screen, including any embedded album art
- Lightweight: folder scanning and file loading happen off OBS's render thread, only one file is ever loaded at a time (even when looping), and the plugin does almost nothing while idle

## Requirements

- OBS Studio 31 or newer
- Windows, macOS or Linux

## Installation

Download the latest build for your system from the [Releases](https://github.com/OngoGablogian/obs-media-randomizer/releases) page.

**Windows:** extract the zip into your OBS folder (usually `C:\Program Files\obs-studio`) and allow it to merge with the existing folders. That puts the files here:

- `obs-plugins\64bit\obs-media-randomizer.dll`
- `data\obs-plugins\obs-media-randomizer\` (locale and shader files)

**macOS:** extract the zip and run the `.pkg` installer inside.

**Ubuntu:** extract the zip and install the `.deb` package inside.

Restart OBS after installing.

## Usage

1. In the **Sources** panel, click **+** and choose **Media Randomizer**.
2. In the properties window, set **Media Folder** to the folder with your files.
3. Go to **File → Settings → Hotkeys**, find your source by name, and set a key for **Play Random File**. You can also set one for **Stop Playback**.
4. Press the hotkey to play a random file.

The hotkey only plays a file while the source is visible (on screen, in the studio mode preview, or in its properties window). If several Media Randomizer sources share the same hotkey, only the visible ones play.

You can also use the **Play Random File** and **Stop Playback** buttons in the source's properties to test it.

## Settings

| Setting | Default | Description |
|---|---|---|
| Media Folder | – | Folder to pick files from |
| Include subfolders | Off | Also search subfolders, up to 8 levels deep |
| File types | Video and audio files | Which files to pick from: video and audio, video only, or audio only |
| Repeats | Play every file once before any repeat | **Play every file once before any repeat:** shuffles through the whole folder, then starts a new round (never starting with the file that just played). **Never the same file twice in a row:** fully random except for back-to-back repeats. **Fully random:** any file can play at any time |
| Hotkey while a file is playing starts a new one | On | When off, presses during playback are ignored. In loop mode this skips to the next file |
| Loop random files until stopped or the scene changes | Off | Plays random files back to back. Every file plays once before any repeats, whatever the Repeats setting. Ends when you press **Stop Playback**, the source is hidden, or the scene changes |
| Fade looped files in and out | On | In loop mode, each file fades in at its start and out at its end |
| Fade single files in and out | Off | Same, for files started with the hotkey or button outside loop mode |
| Fade duration | 500 ms | Length of each fade-in and fade-out (50–5000 ms). Video and audio fade together. Hidden while both fade toggles are off. Pressing **Stop Playback** or switching scenes still cuts immediately |
| Start playing automatically when switching to a scene with this source | Off | Starts a file (or a loop, if loop mode is on) when you switch to a scene where the source is visible |
| Stop playback when the scene changes | On | Stops playback on any scene switch, even if the new scene also contains this source |
| Stop playback when the source is hidden | On | Stops playback when the source is hidden or its scene goes out of view |
| Normalize audio levels between files | Off | Measures each file's typical level while it plays and brings it toward the target, so loud and quiet files end up at a similar volume. The level is averaged in dB and ignores pauses, so short loud sounds (gunshots, explosions) don't make a mostly quiet file count as loud. Peaks are kept under about -1 dBFS. It takes about half a second to measure each new file, then only adjusts slowly, so the file's own loud and quiet moments are kept. Each file's measured level and gain are written to the OBS log |
| Target level | -20 dB | Average level files are brought toward when normalization is on (-30 to -10 dB). Shown only while normalization is on |
| Maximum boost | 18 dB | How much quiet files can be turned up (0 to 24 dB). Loud files can be turned down by up to 12 dB. Higher values bring very quiet files closer to the target but also raise their background noise. Shown only while normalization is on |
| Use hardware decoding when available | Off | Decodes video on the GPU instead of the CPU |
| Width / Height | Canvas size | Size of the source. Video is scaled to fit and centered |

**Video formats:** mp4, mkv, mov, webm, avi, flv, m4v, ts, mts, m2ts, mpg, mpeg, wmv, gif

**Audio formats:** mp3, flac, wav, ogg, oga, opus, m4a, aac, wma, aif, aiff

## Performance tips

- Videos that match your canvas resolution (for example 1080p H.264) are the cheapest to decode. 4K, HEVC or 10-bit AV1 video costs more for no visible benefit on a 1080p canvas.
- Turn on **Use hardware decoding** if your videos are large or high bitrate.
- Keep files on a local drive (an SSD if possible) rather than a network share.

## Building

This project is based on [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate).

**With GitHub Actions:** pushing to `main` builds Windows, macOS and Linux packages. You can download them from the workflow run's **Artifacts**. Pushing a version tag (for example `1.0.1`) also publishes a release with one zip per platform attached (no debug symbols or sources), named `<plugin>-<version>-<platform>.zip`.

**Locally:** follow the [obs-plugintemplate build instructions](https://github.com/obsproject/obs-plugintemplate/wiki/Quick-Start-Guide) for your platform, using the presets in `CMakePresets.json`.

## License

GPL-2.0. See [LICENSE](LICENSE).
