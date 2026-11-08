ElukkaNoise v@VERSION@ BETA - Windows (64-bit VST3)
====================================================

ElukkaNoise is a noise riser / fall / FX instrument (VST3) for techno, hard
techno, industrial and psytrance. Hold a MIDI note and automate the MACRO knob:
0% to 100% is a riser, 100% to 0% a fall. This is a BETA build for testers.

Requirements
------------
- Windows 10 or 11, 64-bit (x64). No ARM64 build yet.
- A 64-bit VST3 host (Reaper, Ableton Live 11+, Cubase, Studio One, Bitwig, FL Studio, ...).
- No separate runtime installation is needed; the plugin is self-contained.

Installation
------------
1. Unzip this archive.
2. Copy the whole "ElukkaNoise.vst3" FOLDER (it is a bundle, not a single file) to:
       C:\Program Files\Common Files\VST3\
   You may need to confirm an administrator prompt.
3. Start your host and rescan plugins if it does not pick the instrument up
   automatically. It appears as "ElukkaNoise" by "Elukka" under Instruments.

Using it
--------
- Load it on a MIDI track, hold or draw a note (any pitch: noise has none).
- Automate the "MACRO" parameter in your DAW. Every preset maps the macro to a
  musically useful transformation across its whole range.
- Presets come in two categories: Risers / Falls and FX. "Save" stores your own
  presets as plain files in
      %APPDATA%\ElukkaNoise\Presets\        (*.elukkanoise)
  Factory presets cannot be deleted; "Reload" restores a preset's saved values.
  Deleting a preset from the plugin moves its file to the Recycle Bin.
- Delay and reverb presets are saved beside the instrument presets, in the
  Delay and Reverb folders.
- "ADVANCED" opens the detailed controls: noise and filters, LFOs, trance gate
  and sample & hold, effects (compressor, delay, reverb with their own presets)
  and the modulation matrix.

Known limitations (beta)
------------------------
- Windows builds were compiled and unit-tested on a build server, but this
  release has NOT yet been exercised by hand inside a Windows DAW. UI, MIDI,
  automation, presets and audio are expected to behave as on macOS; please report
  anything that does not.
- Only x64 is provided (no 32-bit, no ARM64).
- VST3 only; no standalone application.
- The binary is not code-signed: SmartScreen may warn when you unzip or copy it.

Licence
-------
ElukkaNoise is free software under the GNU Affero General Public License v3
(see LICENSE in the source repository). It is built with JUCE, used under the
AGPLv3.

Reporting
---------
Please include: Windows version, host and version, buffer size and sample rate,
the preset, and steps to reproduce.
