# OpenGT4 Steam Deck Edition — experimental Linux port

Based on OpenGT4. This fork is modified and unofficial, and is not the official
OpenGT4 release. It retains GPLv3 and the additional terms in NOTICE.

## Status

A native x86_64 Linux build and an AppImage packaging path are provided. Linux
movie decoding uses FFmpeg through a child process and retains the existing
frame queue, field conversion, timing, and movie audio mixer. The Windows launcher
has not been ported. The AppImage wrapper provides disc selection and remembers
the selected path. Steam Deck gameplay, performance, movie sound synchronization,
controller navigation, saves, and suspend/resume require testing with your disc.

No game data or PS2 BIOS is included. Supply your own Gran Turismo 4 NTSC-U
SCUS-97328 ISO; upstream tests version 2.00. Keep the ISO outside the AppImage.

## Build on Linux

Use an x86_64 Linux host, GCC or Clang, CMake 3.20+, Ninja, Python 3, and FFmpeg.
On Ubuntu 22.04/24.04, install the development dependencies:

```sh
sudo apt-get update
sudo apt-get install build-essential cmake ninja-build git python3 pkg-config \
  libgl1-mesa-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev \
  libxi-dev libxfixes-dev libasound2-dev libpulse-dev libudev-dev ffmpeg
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux --target opengt4 launcher_tests media_tests sound_tests movie_posix_tests --parallel 4
ctest --test-dir build-linux --output-on-failure \
  -R '^(launcher_config|launcher_config_negative|native_media|native_sound|menu_music_prefix_negative|movie_posix)$'
./build-linux/opengt4 "/absolute/path/to/your/Gran Turismo 4.iso" --fullscreen
```

These are selected asset-independent tests, not the complete upstream suite.
The movie test generates synthetic MPEG-2 with FFmpeg and checks progressive and
interlaced decoding, missing decoder failure, loop shutdown, child reaping, and
temporary-file cleanup. It stubs the GPU and audio device, so it does not establish
visual fidelity or sound synchronization.

Do not use `-march=native`, fast-math, or unsafe floating-point optimizations.
Build on an older compatible distribution (the CI uses Ubuntu 22.04) to avoid
requiring newer glibc than the target SteamOS installation. The host GL drivers
remain system-provided. Ubuntu 24.04 local builds are not a compatibility promise.

## Build a local test AppImage

Download the official x86_64 linuxdeploy AppImage. The tested tool checksum is:

```sh
curl -fL https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage -o /tmp/linuxdeploy-x86_64.AppImage
printf '%s\n' 'df908b19f1ed1cdafa79508d415aa12d6c56ce4d56d4149aa8c09286718706e5  /tmp/linuxdeploy-x86_64.AppImage' | sha256sum -c -
chmod +x /tmp/linuxdeploy-x86_64.AppImage
tools/package_appimage.sh build-linux /tmp/linuxdeploy-x86_64.AppImage /usr/bin/ffmpeg
```

The upstream `continuous` asset can change: a checksum mismatch must stop the
build until the replacement has been reviewed. Packaging bundles the decoder and
runtime libraries via linuxdeploy. It stages explicit binary/licence inputs,
never the checkout or ISO. The result is in `dist/` with a SHA-256 file.

This packaging path is for local testing. Before publishing downloadable releases,
complete the bundled dependency licence/source audit described by THIRD_PARTY.txt
inside the image; retain attribution and provide matching corresponding source.
Do not replace the inherited licence with PolyForm or include extracted game data.

## First run on Steam Deck

1. Copy the AppImage and your own ISO to the Deck. Keep the ISO in a stable folder.
2. In Desktop Mode, mark the AppImage executable and launch it. The wrapper uses
   host `kdialog` or `zenity` for the first disc selection. If neither is installed,
   run it from Konsole with your ISO as its first argument:

   ```sh
   ./OpenGT4-Steam-Deck-experimental-x86_64.AppImage "/home/deck/Games/GT4/your-disc.iso"
   ```

3. The selected path is remembered under
   `${XDG_CONFIG_HOME:-$HOME/.config}/opengt4-steam-deck/disc-path`.
   Use `--choose-disc` to select a different image, or pass its path explicitly.
4. Add the AppImage to Steam as a non-Steam game. Use native Linux execution;
   do not force Proton. Start in Gaming Mode after the Desktop Mode smoke test.
5. Saves and settings use SDL's OpenGT4 preferences directory, normally
   `~/.local/share/OpenGT4/OpenGT4/`. `--save-dir` overrides the save directory.
   Nothing is saved inside the read-only AppImage mount.

Arguments after the ISO are forwarded to the game. An existing remembered ISO
lets the wrapper start without the Windows launcher. Disc selection in Gaming Mode
is not yet controller-validated; perform initial selection in Desktop Mode.
For FUSE failures, try `APPIMAGE_EXTRACT_AND_RUN=1 ./OpenGT4-...AppImage`.

## On-device acceptance checklist

- Confirm disc recognition, intro/menu videos, menu BGM and effects.
- Enter Arcade, complete races on multiple tracks, return through results.
- Test GT Mode purchase, licence/event progression, save, quit, and reload.
- Check both analog sticks, D-pad, triggers, buttons, pause, and Steam Input.
- Measure frame times and audio delay at 1280x800; report settings and Deck model.
- Check video/audio sync during movies and a sustained 20–30 minute race session.
- Test suspend/resume, offline launch, paths containing spaces, and relocated ISO.

Record actual results and logs before marking any release Steam Deck compatible.
