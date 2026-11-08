ElukkaNoise v@VERSION@ BETA - macOS (Universal VST3: Apple Silicon + Intel)
==========================================================================

ElukkaNoise is a noise riser / fall / FX instrument (VST3) for techno, hard
techno, industrial and psytrance. Hold a MIDI note and automate the MACRO knob:
0% to 100% is a riser, 100% to 0% a fall. This is a BETA build for testers.

Requirements
------------
- macOS 11 (Big Sur) or newer, Apple Silicon or Intel.
- A 64-bit VST3 host (Ableton Live 10.1+, FL Studio, Reaper, Cubase, Studio One,
  Bitwig, ...). Logic Pro and GarageBand do NOT load VST3; no AU is provided.

Installation
------------
1. Unzip this archive.
2. Copy the whole "ElukkaNoise.vst3" bundle to:
       ~/Library/Audio/Plug-Ins/VST3/            (current user)
   or  /Library/Audio/Plug-Ins/VST3/             (all users, needs admin)
3. Remove the download quarantine, otherwise macOS refuses to load the plugin
   because this beta is not notarized with Apple. Open Terminal and run:
       xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/ElukkaNoise.vst3
4. Start your host and rescan plugins. It appears as "ElukkaNoise" by "Elukka".

Using it
--------
- Load it on a MIDI track, hold or draw a note (any pitch: noise has none).
- Automate the "MACRO" parameter in your DAW. Every preset maps the macro to a
  musically useful transformation across its whole range.
- Presets come in two categories: Risers / Falls and FX. "Save" stores your own
  presets in ~/Library/Application Support/ElukkaNoise/Presets. Factory presets
  cannot be deleted; "Reload" restores a preset's saved values.
- "ADVANCED" opens the detailed controls: noise and filters, LFOs, trance gate
  and sample & hold, effects (compressor, delay, reverb with their own presets)
  and the modulation matrix.
- Delay and reverb presets are saved beside the instrument presets, in the
  Delay and Reverb folders.

Known limitations (beta)
------------------------
- Not notarized: see step 3 above.
- VST3 only; no AU and no standalone application.

Licence
-------
ElukkaNoise is free software under the GNU Affero General Public License v3
(see LICENSE in the source repository). It is built with JUCE, used under the
AGPLv3.

Reporting
---------
Please include: macOS version, Mac model, host and version, buffer size and
sample rate, the preset, and steps to reproduce.
