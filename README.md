# ffmpeg-proxygen

An Apple-Silicon-oriented command-line tool for creating H.264 QuickTime proxy files from a folder of camera media.

```sh
./ffmpeg-proxygen /Volumes/CameraMedia /Volumes/Proxies
```

The proxy tree mirrors every source subfolder. Each output uses the same source base name with a `.mov` extension, for example `A001C001_0101AB.mov` becomes `A001C001_0101AB.mov` in the matching output subfolder.

## Output characteristics

- H.264 High Profile, encoded with `h264_videotoolbox`
- 4 Mbps target video bitrate (4 Mbps maximum rate, 8 Mb buffer)
- source resolution by default; use `--1080p` or `--720p` to set the output height while retaining aspect ratio
- source timing/frame rate is retained, including variable frame rates
- every source audio stream is stream-copied, preserving its codec, channels, sample rate, and layout
- for sources with more than two discrete channels and no source speaker-layout declaration, removes FFmpeg's generated 5.1 layout tag so Premiere recognises the proxy as matching discrete multichannel audio
- source global metadata, video-stream metadata, chapters, and source timecode are copied; the timecode is written as a QuickTime timecode track
- VideoToolbox hardware decode + scale + encode is attempted first. Unsupported decode formats retry with CPU decode/scale while still using the Apple media engine for H.264 encode.

## Blackmagic RAW (`.braw`)

`.braw` files are supported through the installed Blackmagic RAW SDK. On first use, the CLI builds the included `braw-sdk-bridge` native helper, which:

- uses the SDK to decode RGB frames to a pipe consumed directly by FFmpeg;
- at `--1080p` or `--720p`, uses the smallest native BRAW decode scale that has at least the requested height, then FFmpeg makes the final frame; source-resolution output decodes at the source's full native size;
- streams native little-endian PCM audio into FFmpeg, retaining the source channel count, sample rate, bit depth, and samples;
- reads the BRAW source frame rate and timecode; and
- embeds the available camera model, lens, reel, scene/take, recording-date, and BRAW viewing metadata in the MOV metadata tags.

Install **Blackmagic RAW** (not only DaVinci Resolve) and the Xcode Command Line Tools before processing BRAW. The standard macOS installer puts the SDK at the location the tool detects automatically. If it is installed elsewhere, supply its `Mac/Libraries` directory:

```sh
ffmpeg-proxygen --braw-sdk-libraries '/path/to/Blackmagic RAW SDK/Mac/Libraries' INPUT OUTPUT
```

The BRAW SDK supplies decoded source PCM rather than a container stream FFmpeg can copy verbatim; the proxy muxes those unchanged samples as the matching PCM codec.

Camera/lens metadata is copied when it is exposed by FFmpeg and supported by the QuickTime MOV metadata format. Some manufacturer-specific metadata (notably data carried in proprietary atoms) cannot be retained after a transcode by FFmpeg.

## Options

```text
./ffmpeg-proxygen INPUT_FOLDER OUTPUT_FOLDER [options]

  -j, --jobs N              simultaneous files (default: a performance-aware value, max 8)
  --ffmpeg-threads N        threads per FFmpeg process; 0 lets FFmpeg decide
  --1080p                   scale to 1080 pixels high (aspect ratio retained)
  --720p                    scale to 720 pixels high (aspect ratio retained)
  --overwrite               replace already-completed output files
  --dry-run                 show the mapped jobs without writing files
  --ffmpeg PATH             use a particular FFmpeg executable
  --ffprobe PATH            use a particular FFprobe executable
```

Existing outputs are skipped unless `--overwrite` is used. Outputs are first written to a hidden temporary `.mov` in the destination directory and renamed into place only after FFmpeg succeeds, so interrupted encodes do not leave a proxy that looks complete.

## Requirements

Install FFmpeg with VideoToolbox support (the Homebrew package has it):

```sh
brew install ffmpeg
```

The tool needs both `ffmpeg` and `ffprobe` on `PATH`. It is intended for Apple Silicon Macs; it requires the `h264_videotoolbox` encoder.

For BRAW clips it additionally needs Blackmagic RAW and Xcode Command Line Tools (`xcode-select --install`) so it can build the bundled SDK bridge once.

## Throughput tuning

The default runs several files concurrently so the GPU/media engine and CPU-side demux, audio handling, and filesystem work can overlap. The best `--jobs` value varies by source codec, storage, and Mac model. Start with the default, then try `-j 6` or `-j 8` when source and destination are on fast storage. For CPU-decoded formats, lower `--jobs` or `--ffmpeg-threads` if the machine becomes oversubscribed.
