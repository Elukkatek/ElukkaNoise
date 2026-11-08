ElukkaNoise v@VERSION@ BETA - Linux (x86-64 VST3)
=================================================

ElukkaNoise is a noise riser / fall / FX instrument (VST3) for techno, hard
techno, industrial and psytrance. Hold a MIDI note and automate the MACRO knob:
0% to 100% is a riser, 100% to 0% a fall. This is a BETA build for testers.

Requirements
------------
- x86-64 Linux with glibc 2.35 or newer (Ubuntu 22.04+, Debian 12+, Fedora 36+,
  Arch, etc.). The build was made on Ubuntu 22.04.
- A VST3 host: Reaper, Bitwig Studio, Ardour 7+, Carla, Qtractor, ...
- Runtime libraries (normally already present on any desktop system):
    libasound2  libfreetype6  libfontconfig1  libx11-6  libxext6  libxrandr2
    libxinerama1  libxcursor1  libgl1
  Debian/Ubuntu:  sudo apt install libasound2 libfreetype6 libfontconfig1 \
                      libx11-6 libxext6 libxrandr2 libxinerama1 libxcursor1 libgl1
  Fedora:         sudo dnf install alsa-lib freetype fontconfig libX11 libXext \
                      libXrandr libXinerama libXcursor mesa-libGL

Installation
------------
1. Unzip this archive.
2. Copy the whole "ElukkaNoise.vst3" DIRECTORY (it is a bundle) to one of:
       ~/.vst3/                 (current user only, no root needed)
       /usr/lib/vst3/           (all users)
       /usr/local/lib/vst3/
   e.g.   mkdir -p ~/.vst3 && cp -r ElukkaNoise.vst3 ~/.vst3/
3. Rescan plugins in your host. It appears as "ElukkaNoise" by "Elukka".

Using it
--------
- Load it on a MIDI track, hold or draw a note (any pitch: noise has none).
- Automate the "MACRO" parameter in your DAW. Every preset maps the macro to a
  musically useful transformation across its whole range.
- Presets come in two categories: Risers / Falls and FX. "Save" stores your own
  presets as plain files in
      ~/.config/ElukkaNoise/Presets/        (*.elukkanoise)
  Factory presets cannot be deleted; "Reload" restores a preset's saved values.
  Deleting a preset from the plugin moves its file to the freedesktop Trash.
- Delay and reverb presets are saved beside the instrument presets, in the
  Delay and Reverb folders.
- "ADVANCED" opens the detailed controls: noise and filters, LFOs, trance gate
  and sample & hold, effects (compressor, delay, reverb with their own presets)
  and the modulation matrix.

Known limitations (beta)
------------------------
- Linux builds were compiled and unit-tested on a build server, but this release
  has NOT yet been exercised by hand inside a Linux DAW. Please report anything
  that misbehaves.
- X11 only; under Wayland your host runs the plugin through XWayland, which is
  normally fine.
- Only x86-64 is provided (no aarch64).
- VST3 only; no standalone application.

Troubleshooting
---------------
- Plugin not found: confirm ~/.vst3/ElukkaNoise.vst3/Contents/x86_64-linux/ElukkaNoise.so
  exists, then rescan / clear the plugin cache in your host.
- "cannot open shared object file": a runtime library above is missing. Run
      ldd ~/.vst3/ElukkaNoise.vst3/Contents/x86_64-linux/ElukkaNoise.so | grep "not found"
  and install the package that provides it.
- "version `GLIBC_2.xx' not found": your distribution is older than the build
  target. Use a newer distro or build from source (see the project's BUILDING.md).
- Blank or tiny UI: check your host's plugin-window scaling options (HiDPI).

Licence
-------
ElukkaNoise is free software under the GNU Affero General Public License v3
(see LICENSE in the source repository). It is built with JUCE, used under the
AGPLv3.

Reporting
---------
Please include: distribution and version, host and version, buffer size and
sample rate, the preset, steps to reproduce, and the output of the ldd command
above if the plugin fails to load.
