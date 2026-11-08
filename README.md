# ElukkaNoise

Noise riser / fall / FX instrument (VST3) for techno, hard techno, industrial and psytrance.
Hold a MIDI note and automate the **MACRO** knob: 0 % to 100 % is a riser, 100 % to 0 % a fall.
Built with JUCE 8.0.4 and CMake, same toolchain and conventions as ElukkaSynth. See `BUILDING.md`.
Website with the downloads: https://elukkatek.github.io/ElukkaNoise/ (`docs/`, served by GitHub Pages).
Source and beta builds: https://github.com/Elukkatek/ElukkaNoise (GitHub Actions builds the Windows x64 and Linux x64
VST3 on every push and attaches them to a release on a `v*` tag; the macOS universal build is made locally).
Free software under the GNU Affero General Public License v3 (`LICENSE`); JUCE is used under the AGPLv3.

## How it is put together

```
Source/
  Params.h          parameter layout (host-automatable), choice lists, modulation destinations, macro curve, the
                    effect macro-map model (v0.9.20), state migrations
  PluginProcessor   MIDI splitting (sample-accurate), transport, host state (XML with preset name, UI state, the two
                    effect presets and their macro mappings)
  Presets.*         33 factory instrument presets (compiled in: Init, 16 Risers / Falls, 16 FX; v0.9.21), each naming
                    the delay and reverb presets it loads with; user presets as XML files, categories, save/rename/delete
  FxPresets.*       delay and reverb presets (v0.9.20): 10 + 11 factory, user files, each with its own macro mapping
                    and (reverb) character; the live mapping the engine reads
  DSP/Engine.*      the instrument: control-rate modulation + per-sample voice, then delay and reverb
  DSP/Noise.h       stereo white / pink / brown generator (xorshift, level-matched, sample-rate independent)
  DSP/Filter.h      stereo SVF (Simper), 12 / 24 dB, low / band / high pass crossfadable, drive; separate 12 dB HP.
                    Resonance 0..1 = Q 0.71..5.5 (v0.9.10; it was up to 12.7, a much narrower, thinner peak).
                    Filter 2 (v0.9.21) is a second instance after the main filter, out of the path at its default
  DSP/LFO.h         LFO (7 waves) / sample & hold with slew, retrigger or song-locked sync
  DSP/Gate.h        trance gate: 16-step sequencer (Off / On / Tie), 1..16 steps, swing, AHDSR per step
  DSP/Compressor.h  two-knob compressor (v0.9.11): Amount = threshold 0..-42 dB with ratio 1..8:1, Speed = 30/400 ms
                    .. 1/30 ms attack/release, RMS detection, soft knee. Makeup (v0.9.13) predicts the gain reduction
                    from the curve at the level-weighted average level and adds it back through the same attack /
                    release, so it keeps the loudness and follows Amount changes without lag
  DSP/Delay.h       delay built for noise (v0.9.19): synced divisions 1/1..1/32 or free time, crossfading read heads
                    (a time change is exact at once, no pitch bend), one Filter knob (second-order low / high pass in
                    the loop), mono / stereo / ping-pong / wide ping-pong with Width and Offset, modulation, ducking,
                    a loop limiter so feedback 100 % sustains
  DSP/Reverb.h      reverb built for noise (v0.9.17): 8-line FDN, 4 diffusers per channel, shelving damping (highs and
                    lows die faster than the mids, Decay = T60 at 1 kHz), per-line modulation, width, ducking from the
                    dry level, pre-delay, seven characters (internals sets, picked by the reverb presets), an activity
                    measure. Replaced the 4-line ElukkaSynth FDN
  UI/Widgets.h      black look-and-feel, grain texture, Knob (ring, badge, right-click window; effect knobs show their
                    preset's macro mapping), Choice / Toggle / Meter / Led
  UI/FxMapEditor.h  the Macro window of an effect (v0.9.20): one row per knob, on / off, Add or Range, depth or
                    start / end, curve, Restore
  UI/MacroKnob.h    the big macro: ring, grain field that brightens with the filter, pan / width marker
  UI/Pages.*        advanced pages, each built from titled panels with evenly spaced controls: Noise & Filter
                    (v0.9.23: one knob grid, NOISE / LEVEL views; FILTER and FILTER 2 are identical rows; the
                    high pass lives on the main screen and the left column), LFOs (LFO 1 / 2 tabs with a wave display, S&H RANDOM beside them, as in ElukkaSynth), Trance Gate
                    (step grid, envelope view, STEPS / ENVELOPE views), Effects (compressor, delay, reverb; drag to
                    reorder; each effect's preset menu, MACRO button and TIME / TONE views), Mod Matrix (the Output tab was folded into NOISE in v0.9.22)
  PluginEditor.*    one 780 x 450 window. Main screen: big macro, five knobs under it. ADVANCED (in the header) folds
                    the macro and the five knobs into a column on the left and shows the pages on the right
Tests/RenderTest.cpp  offline test suite, --sweep, --bench, --host, --ui, --wav
Tests/ClickProbe.h    click detector for noise (first-difference outliers against the local spread)
```

### Modulation model (v0.9.3: a modulation matrix, as in ElukkaSynth, with Serum's aux source)

Everything is a parameter. 16 matrix slots each route a **source** (Macro, LFO 1, LFO 2, S&H, Velocity) through a
**curve** and an **amount** (-100..+100 %) to a **destination**, optionally scaled by an **aux source**. Any of 34
destinations can be modulated: every continuous knob of the instrument (filter, filter 2, noise, LFO rates / depths / smoothing,
S&H, every gate envelope and timing knob, pan, noise stereo, compressor amount / speed, level, amp, note attack / release).
The delay and reverb are not in the matrix since v0.9.20: each effect preset carries its own macro mapping (below).

- Sources are unipolar (v0.9.8, as in ElukkaSynth): every source runs 0..1 (LFOs and S&H after their Depth) and pushes
  the knob up from its value by up to the amount (down for a negative amount). "Modulate with" starts the amount at
  what is left above the knob, so knob value + amount = 100 %.
- Curve bends the source (negative = early, positive = late; symmetric for bipolar sources).
- Aux source multiplies the amount by the aux value (0..1). With Macro as the aux,
  the macro decides how far an LFO or the S&H moves the destination: 0 % = not at all.
- Each control block (16 samples) the slots are summed per destination in that parameter's normalised 0..1 space (a log
  cutoff moves in octaves), clipped, and ramped per sample inside the block. LFO / S&H values come from the previous
  block (16 samples), which lets LFOs modulate each other's rate and depth.

### Effect macro mapping (v0.9.20: three independent layers)

The instrument, the delay and the reverb are three independent sound-design layers. The instrument preset defines the
noise machine; the delay preset defines the delay sound *and* how MACRO moves it; the reverb preset the same for the
reverb. Loading an instrument preset leaves both effects, their presets and the macro position alone; loading an effect
preset touches only that effect. The same instrument works with any delay / reverb pairing.

- Each effect preset holds, per continuous knob of the effect (delay: Mix, Feedback, Filter, Width, Offset, Mod, Duck;
  reverb: Mix, Decay, Size, Tone, Width, Mod, Pre-Delay, Duck), an entry: on /
  off, **Add** (the knob is the reference, the macro adds a depth of -100..+100 % of the knob's range) or **Range**
  (the macro travels from a start value to an end value, the knob ignored), and a curve (early / late). The engine
  evaluates it at control rate in the knob's normalised space, like the matrix, and clips to the knob's range; the
  stored knob is never written, so sweeping the macro back and forth any number of times cannot drift.
- Both mappings work at once (at 75 % the delay may add feedback and width while the reverb adds decay and size).
- The mapping is plugin state (saved with the project and in the effect preset file), not host parameters: the
  parameter list is unchanged apart from the two character menus that went away, and MACRO stays the one automatable
  control. The engine reads it from atomics; a preset change is rare and every effect value glides.
- The effect knobs show the mapping as a ring (the reach) and a dot (where the macro has it), like matrix
  modulation; a knob under a Range mapping is drawn dimmed. The **MACRO** button in each panel opens the Macro window
  (`UI/FxMapEditor.h`) to change entries and **Restore** the preset's saved mapping.

Working with it, the ElukkaSynth way:
- Right-click a knob: type a value, reset it, or "Modulate with" Macro / LFO 1 / LFO 2 / S&H / Velocity. The window
  lists the knob's routings with their amount, aux source and Remove. New routings start at a sensible amount (a
  unipolar source reaches the end of the knob's range; a bipolar one swings as far as the nearer end allows).
- Double-click a knob to type a value; Alt-click resets it. Typing reads what the knob shows: "50" or "50%" on a
  percentage knob, "2k" / "2 kHz" for frequencies, "1.5 s" / "1500 ms" for times, "30L" / "C" for pan, "pink" or a
  percentage for the noise colour.
- A modulated knob shows a ring over the range its modulation can reach, a dot where the modulation has it right now,
  and a small wave badge: drag the badge up or down to scale all of that knob's amounts together.
- The MOD MATRIX tab lists the rows in use (source, curve, amount, destination, aux, clear) plus one empty row: pick a
  source there to add a routing.

The main screen has five small knobs: NOISE (colour), MOVEMENT (LFO 1 depth), DELAY and REVERB (the two effects' mix,
the same parameters as Wet in the advanced section) and HIGH PASS (the high pass in front of the filter, so it also thins
what goes into the delay and reverb). Delay and reverb are switched on in every preset (mix 0 unless the preset uses
them). An effect at mix 0 is skipped and cleared, so it costs no CPU; when it starts again its input fades into the
buffer over 10 ms, so the echoes never replay a hard cut-in.

EFFECTS (v0.9.11): compressor, delay and reverb run in any order: the panel on top goes first. Drag a panel by its
title: it follows the mouse and the others slide to their new places; dropping it sets the "Effects Order" parameter
(six orders; a swap dips the delay and reverb mixes for 12 ms, so it never clicks). The compressor starts first, at
Amount 0 (off) in every preset. Its graph (v0.9.12) is the transfer curve for the current amount: input level across,
output up, 1:1 to the knee (the one handle, at the threshold), then flatter by the ratio; the area under the curve fills
to the incoming level. The MOD MATRIX tab
has a MACRO RANGE bar: the macro's 0..100 % plays only floor..ceiling of its travel, so a DAW automation from 0 to
100 % can skip a dead start or end.

Every preset uses delay feedback 60 % and reverb low pass 1.2 kHz / high pass 110 Hz (the old tone 10 %) (the defaults; the delay has no saturation stage since
v0.9.3). Every riser / fall also shares: reverb size 100 %, decay 6.7 s, pre-delay 10 ms; delay low pass 20 kHz, high
pass 150 Hz. Noise Vary is off by default (every note starts the same noise), in every preset.

### Stereo pan (v0.9.24)

Pan is a stereo pan, not a balance control: the left and right channels are panned separately (constant power each).
Panning right slides the left channel across to the right edge while the right channel stays where it is (mirrored
to the left), so the whole image moves and stays wide instead of one side fading out; at 50 % right the left noise
sits in the middle (-3 dB on the left speaker, where the old balance pan faded it to -5.3 dB). Summing the channels
into one side would change the level by how much noise they share (Stereo below 100 %) or how anti-correlated the
extra width makes them (above 100 %), so a gain from those settings keeps the level within 0.1 dB across the pan.
Mono noise (Stereo 0 %) pans exactly as before. The width stage (Stereo above 100 %) still runs before the pan.

### Filter 2 (v0.9.21)

A second state-variable filter after the main one (noise -> high pass -> filter -> filter 2 -> envelope and gate),
for the brightness of the sound: its own Type (low / band / high pass), Slope (12 / 24 dB), Cutoff, Resonance and
(v0.9.22) Drive in a FILTER 2 panel laid out like the FILTER panel. It defaults to a 12 dB low pass at 20 kHz without resonance
and is then out of the signal path (bit-identical to before it existed, no CPU): a 20 kHz low pass would still take
the top octave off white noise. When a knob or the matrix moves it off that default it crossfades in over ~10 ms,
and out again on the way back (its state is cleared), so sweeping the cutoff down from 20 kHz is click-free. Type
and slope crossfade like the main filter's. Filter 2 Cutoff, Resonance and Drive are matrix destinations
(appended to the table, so saved slots keep their indices); a typical use is the macro closing it on a fall or
opening it on a rise, independent of the main filter's resonant sweep.

### Trance gate

Each opening step retriggers the noise like a note: with Noise Vary off (every factory preset) every step restarts
the same noise burst, so the steps sound like one sample played again; with Vary on each step plays fresh, random noise.
A tied step continues the noise. The gate always runs in note values of the host tempo (no free-running mode since
v0.9.7). Retrig (on by default): the pattern restarts at step 1 on every note, counted from that note; off: the steps
follow the song position. 16 steps, each Off, On (opens with a fresh attack) or Tie (continues the step before without a new attack), pattern
length 1..16 (right-click a step to end the pattern there), swing 50..75 %, Random (a new pattern every cycle), and a
Patterns menu (library patterns, clear, invert, shift, remove ties, randomise). Each opened step runs attack, hold,
decay, sustain until Step Len, then release; the envelope view draws exactly what the engine does for one step. A
synced gate whose Gate Rate is modulated (by the macro, say) moves its division in whole octaves, so the steps stay on
the song grid: Rolling Gate Rise uses this to go 1/8, 1/16, 1/32 as the macro rises.

Presets are just parameter values, including the macro map and routing, and never touch MACRO itself (the DAW owns
it). Loading a preset resets every other parameter to its default first, then applies the preset's values.

### S&H Jump (fixed at 15 % since v0.9.16)

Each new S&H value is pushed a little away from the previous one, so the S&H rarely lingers near the same value. The
new random value is measured as a fraction t of the room between the previous value and the edge on its side, then
bent with 1 - (1 - t)^n, n = 8^0.15 = 1.37: a value close to the previous one moves noticeably, one already near the
edge barely changes. Measured over 20 000 steps the average step grows from 33 % to 41 % and steps under 10 % drop from 19 % to 14 %. v0.9.15 had a Jump
knob; the user picked 15 % by ear and the knob was removed. LFO 1 / 2 (S&H and Smooth Random waves) stay plain random.

### Reverb (v0.9.17: rebuilt for noise)

v0.9.23: the Low Pass and High Pass knobs became one **Tone** knob, like the delay's Filter (left: the low pass closes,
20 kHz -> 500 Hz, the tail darker; right: the high pass rises, 80 Hz -> 2 kHz, the tail thinner; -75 % is the old
default of 1.2 kHz), and Mod Rate is gone: the rate follows the Mod depth (0.3 Hz at 0, 0.53 Hz at the default 25 %,
3 Hz at full). The text below describes the engine, whose low pass and high pass the Tone knob now sets.

Broadband noise shows up every flaw a reverb has (ringing modes, flutter, low-end pile-up), so the reverb was replaced
with one designed around it: input high pass and band limit, pre-delay (0..200 ms, glided), four allpass diffusers per
channel (different lengths left and right), then an 8-line FDN with Hadamard feedback. Each line has its own
modulation phase (four quadrature LFOs), two short allpasses, a second-order high shelf, a DC blocker, a second-order
low shelf and its decay gain. Two orthogonal tap sets give left and right; Width scales the side signal (0 = mono,
100 % = full; the mono sum keeps half of the lines, so it never cancels: -3 dB).

- **Decay** is the T60 at 1 kHz (the decay gain is set against the loop filters' peak response and the interpolation's
  loss, so the mids ring exactly as long as the knob says: 3 s -> 2.9 s, 12 s -> 11 s measured).
- **Low Pass** is the corner above which the tail decays faster (Clean Hall: five times faster), **High Pass** cuts
  the input and sets the corner (2.2x) below which the tail decays about twice as fast. So the tail darkens as it
  fades and a long tail never turns into a rumble; the previous reverb's 150-500 Hz band rang twice as long as its
  mids at the dark riser setting. With Low Pass at 1.2 kHz the band that rings longest is 300-600 Hz.
- **Duck** (the feature that matters most here): a 3 ms RMS of the dry input, followed with a 1 ms attack / 50 ms
  release against the loudest level of the last two seconds, pushes the wet level down by up to the Duck amount. On a
  1/16 trance gate the wet drops 14 dB on every pulse and is fully back in the gap, so the chops stay sharp and the
  reverb blooms between them; on a held sweep it is a steady level, not pumping. Measured on Rolling Gate Rise: the
  chops still dip 12 dB at 50 % macro with the reverb on.
- **Mod** / **Rate**: delay modulation depth (up to 12 samples at 44.1 kHz times the character's scale) and speed
  (0.05..5 Hz). The default 25 % at 0.5 Hz is below audibility as pitch movement and keeps the modes from standing.
- **Characters** are internals sets (diffusion, size scale, modulation scale, the high and low decay ratios, input
  band): Tight Space, Clean Hall (the default), Endless, Dark Bloom, Bright Space, Modulated, Psy Space. Since
  v0.9.20 a reverb preset picks one (the menu went); the internals glide when the preset changes. Frozen / Freeze
  were dropped (the user chose to leave freeze out).
- Every value glides (60 ms; size 250 ms, so a size change bends pitch instead of clicking) and the coefficients update
  every 128 samples whatever the host block size; jumping any reverb knob, the character or Mix between extremes every
  0.3 s mid-note does not click. The tail runs on through note-off, a closed gate and bypass (buffers are never cleared
  while audible).
- Macro integration comes from the reverb preset (v0.9.20, see Presets): Ducked Bloom is the ducking + bloom
  combination (duck 70 %, Mix +30 % late, Decay +25 %), Clean Space brings the reverb in late, Endless Space grows
  it in every direction, Frozen Atmosphere takes Decay from 4 to 20 s.
- Measured against the previous reverb at the same settings (`ElukkaNoiseTest --probe-reverb`): spectral ripple 2.4 /
  3.2 dB (was 3.6 / 4.3), flutter 0.12 / 0.28 (was 0.16 / 0.16), stereo correlation 0.0 (was 0.1-0.2), the same wet
  level at the riser setting. Linear interpolation in the modulated lines costs the highs some decay (above 5 kHz
  about half the mids' T60 at long settings), which is in keeping with the design.
- CPU (`ElukkaNoiseTest --bench`, Apple M1, one core): the reverb costs about 0.35 % at 44.1 / 48 kHz and 0.9 % at
  96 kHz per instance, twice the previous 4-line reverb (8 lines, 4 diffusers, second-order shelves). A preset with
  the reverb running sits at 0.65 % at 48 kHz (was 0.45 %), eight instances at 4.5 % (was 3.1 %); presets without
  reverb are unchanged. v0.9.18 took the reverb from 0.48 % to 0.35 % of real time on its own
  (`ElukkaNoiseTest --bench-reverb`): the loop allpasses became power-of-two rings with one shared write index, and
  the per-line arithmetic (allpasses, shelves, DC blocker, decay gain, output taps) runs on two 4-wide SIMD registers.
  What is left is mostly the delay-line reads and writes themselves. No allocation, lock or division in the audio
  path; the coefficient update runs once per 128 samples. The output is unchanged (-88 dB against the v0.9.17 render).

### Delay (v0.9.19: rebuilt for noise)

v0.9.23: Mod Rate is gone; the modulation rate follows the Mod depth (0.3 Hz at 0 to 3 Hz at full), as in the reverb.

Two lines (the right one at the left time times 1 + Offset), each read through a pair of crossfading heads: a new
time (a division, the Time knob, a tempo change) fades in over 30 ms, so synced echoes land exactly on the grid at
once and nothing bends. A slow sine (left and right in quadrature, up to 6 ms) adds the wobble when Mod is up.

- **Filter** is one knob for the repeats' tone: centre is neutral (second-order low pass at 20 kHz, high pass at
  100 Hz); left closes the low pass down to 350 Hz (and lifts the high pass to 150 Hz, so dark never means muddy);
  right lifts the high pass up to 5 kHz. The filters sit in the loop, so every repeat evolves: at -100 % the 4th
  echo keeps none of the 1st's energy above 2 kHz, at +100 % a quarter of its lows.
- **Modes**: Mono (one line, centred), Stereo (two lines, the input kept), Ping-Pong (the input summed, the bounce at
  full level and feedback once per round trip, as the old ping-pong worked, so the presets' 60 % still means what it
  did), Wide Ping-Pong (the stereo input kept, every echo swaps sides, feedback per hop). The modes are mixing
  weights that glide, so a mode change crossfades. **Width** scales the side signal: 0 = mono echoes, 100 % = full
  (the mono sum never cancels). **Offset** -50..+50 % shifts the right line's time for a swing or dotted feel.
- **Feedback** 100 % sustains: a limiter in the loop (3 ms RMS, 1 ms attack / 200 ms release, smooth gain above
  -12 dBFS) holds the level a little above a typical noise input and never clips; 60 % falls 27 dB in 1.8 s.
- **Duck** works like the reverb's: the dry level pushes the echoes down (14 dB on a 1/16 pulse) and lets them fill
  the gaps. The Ducked Gate Echo preset is 1/8 ping-pong at Duck 60 % for the gated presets.
- The v0.9.19 character menu became the delay preset menu in v0.9.20 (see Presets). Saturated Echo was dropped
  (the user chose to leave saturation out).
- Divisions run 1/1 to 1/32 in time order (whole, half, dotted and triplet quarters and eighths, dotted sixteenth,
  sixteenth, its triplet, thirty-second). Projects and presets saved before v0.9.19 are migrated on load: the old
  division list, the Ping Pong switch (now Mode) and Low Pass / High Pass (now one Filter value) map over.
- Macro integration comes from the delay preset (v0.9.20, see Presets): Clean Rhythmic brings the echoes in late with
  a little more feedback and width, Wide Ping-Pong opens up wetter, longer and wider, Dark Industrial feeds back into
  a wall while the Filter closes, Psy Feedback adds feedback, wobble and speed.
- CPU (`ElukkaNoiseTest --bench-delay` / `--bench`, Apple M1): the delay alone costs 0.09 % of a core at 48 kHz
  (the old one 0.04 %: two second-order filters, two read heads, the detector and limiter). A preset with delay and
  reverb running sits at 0.7-0.85 % at 48 kHz, eight instances at 6.3 %; 1.6-1.9 % at 96 kHz (v0.9.20: every
  preset now runs both effects through the default effect presets, so the sums rose from 5.1 %). No allocation or
  lock in the audio path; the glides update every 128 samples and the loop filter coefficients, the mode weights
  and the modulation depth ramp per sample inside each piece (v0.9.20: a per-piece step clicked on dark material
  when a preset changed).

### Advanced UI (v0.9.23: one grid, views per section)

Every knob row of the advanced section uses the same cell (62 x 72) and the same spacing, centred in its panel, so
the knobs line up from page to page. A section with more controls than fit one clean row shows them in **views**,
picked with small buttons beside the panel title (the chosen view is remembered while the window is open):

- NOISE: **NOISE** (Colour, Stereo, Pan, Vary) / **LEVEL** (Level, Attack, Release, Velocity, Retrigger).
- TRANCE GATE: **STEPS** (Division, Depth, Swing, Step Len, Steps) / **ENVELOPE** (Attack, Hold, Decay, Sustain,
  Release); the step grid, the switches and the envelope view stay.
- DELAY: **TIME** (Sync, Mode, Time, Feedback, Mix, Duck) / **TONE** (Filter, Width, Offset, Mod); On, the preset menu
  and MACRO sit in the title strip, as in the reverb.
- REVERB: **TIME** (Size, Decay, Pre, Mix, Duck) / **TONE** (Tone, Width, Mod).

Three knob pairs were merged so the rows stay short without losing much: Stereo + Width -> **Stereo** (0 % = mono,
100 % = independent left / right noise, 200 % = extra wide; one matrix destination, Noise Stereo), the reverb's Low
Pass + High Pass -> **Tone**, and Mod + Mod Rate -> **Mod** for both effects (the rate follows the depth). Projects,
instrument presets and effect presets saved before are migrated (below).

### Click-free design

- Note on: the envelope continues from wherever it is (overlapping notes and retriggers never restart from zero).
- Note off: release envelope (5 ms .. 8 s); delay and reverb tails ring on by themselves.
- Effect preset changes (v0.9.20): every effect value glides, times crossfade, modes crossfade, the reverb's internals
  glide; the buffers are never cleared, so the tails run on through a preset change. Delay and reverb presets switched
  in every 0.4 s at macro 0, 50 and 100 %, mid-note and through the tails, do not click.
- Retrigger: LFO / S&H / gate phases restart, but every output is smoothed (LFO Smooth, S&H Smooth, gate attack /
  release, and the control-block ramps), so a restart is a fast move, not a jump.
- Filter type and slope changes crossfade over ~10 ms from the same state; all other parameters glide (10 ms).
- Delay time changes crossfade between two read heads (30 ms, exact at once); every delay and reverb value glides (size over 250 ms, the rest 60 ms, the
  decay gain per sample); wet levels fade on bypass and the buffers are kept, so turning an effect off and on never
  cuts a tail.
- Output: a gentle soft clip above 0.85 so resonant peaks never hard clip.

### Noise level and sample rates

White noise keeps its spectral density at every sample rate (level scales with sqrt(rate)), the brown integrator keeps
its corner frequency, and above 48 kHz a fixed 20 kHz low pass keeps the audible band the same, so a sweep sounds and
measures the same at 44.1, 48 and 96 kHz.

## Presets

Three independent preset systems: instrument, delay, reverb. An instrument preset (v0.9.21) is the instrument's
parameters plus the **names** of a delay preset and a reverb preset: loading it sets the instrument, then loads those
two effect presets (their knobs, their macro mappings, the reverb's character) and keeps the MACRO position. It never
stores the effects' contents, so after loading one you can pick another delay or reverb from the effect panels and
only that effect changes. Saving an instrument preset stores the names of the effect presets selected at the time
(the save dialog says which, and warns when an effect has been edited since its preset was loaded: those edits only
survive as an effect preset of their own). A preset saved before v0.9.21 has no names and leaves the effects as they
are; a name that no longer exists (a deleted user effect preset) loads Clean Rhythmic / Clean Space.

Init: white noise through a 500 Hz high pass (v0.9.22; filter open, no resonance, drive, gate or modulation, delay
and reverb Off).
It sits at the top of every preset menu.

### The factory collection (v0.9.21)

Every preset is built as a complete sound: noise, filters, modulation, gate and the two effect presets chosen to
complement each other rather than all being extreme at once (a spinning instrument gets a steady echo, a busy delay
gets a plain hall, two presets have no delay at all). In every one the MACRO is the progression (risers / falls) or
the intensity (FX), and the level is kept within 8 dB between neighbouring macro positions.

Risers / Falls (16), with their delay and reverb presets:

| Preset | Sound and macro behaviour | Delay | Reverb |
|---|---|---|---|
| Clean Noise Rise (default) | the reference: 24 dB low-pass sweep from a rumble to full-band noise, level eased 6 dB on the way up | Clean Rhythmic | Clean Space |
| Subtle Tension | restrained: pink-white noise, a gentle sweep capped by Filter 2 (6 -> 12 kHz), width late | Clean Rhythmic | Tight Space |
| Deep Atmosphere | brown noise turning pink / white as it opens wide, a 2-bar pan drift | Atmospheric | Endless Space |
| Whiteout | bright, dense: filter fully open, lows thinning late, drive and compression rising | Off | Bright Hall |
| Spinning Rise | a sine pan that accelerates into a spin as the filter opens | Clean Rhythmic | Endless Space |
| Spiral | a filter LFO that speeds up and deepens with the macro (0.5 Hz / 25 % -> 11 Hz / 100 %) over a 1-bar pan | Wide Ping-Pong | Clean Space |
| Wide Rotation | accelerating pan plus a 1-bar width breath, the filter opening underneath | Off | Bright Hall |
| Helix | a triangle on the resonant cutoff and a slow sine on the pan, both speeding up | Modulated Echo | Tight Space |
| Industrial Pressure | dense, resonant, driven 24 dB low pass; resonance and drive climb late, lows thin out | Dark Industrial | Dark Bloom |
| Machine Overload | a square-wave filter chop getting faster, deeper, more resonant and more saturated | Dark Feedback | Tight Space |
| Acidic Rise | screaming 12 dB band pass plucked by a 1/16 saw, the band lifting, drive coming in | Psy Feedback | Clean Space |
| Rolling Gate Rise | synced gate doubling 1/8 -> 1/16 -> 1/32 and hardening as the filter opens | Ducked Gate Echo | Ducked Bloom |
| Endless Horizon | pink noise breathing with a 4-bar sine; the effects carry it | Atmospheric | Endless Space |
| Psychedelic Ascent | 1/16 S&H steps spreading with the macro (aux) over a stereo sine that speeds up | Psy Feedback | Clean Space |
| Blackout Fall | for 100 -> 0 %: bright and thin at the top, lows and a resonant peak return as it falls, the tails grow | Fall Echo | Fall Wash |
| Reverse Spin Fall | for 100 -> 0 %: a stepped (saw) spin that slows and darkens into a wash that grows on the way down | Off | Fall Wash |

FX (16):

| Preset | Sound and macro behaviour | Delay | Reverb |
|---|---|---|---|
| Trance Pulse | classic 1/16 gate; the macro deepens the gate and brightens | Clean Rhythmic | Dark Bloom |
| Industrial Stutter | 1/32 chops with rests; the macro hardens the gate, adds resonance and drive | Ducked Gate Echo | Tight Space |
| Broken Machine | irregular swung 1/16 pattern under a 1/8 stepped-random filter | Dark Feedback | Tight Space |
| Wide Gate | 1/8 pulses alternating sides (a 1/4 square on the pan), wide | Off | Clean Space |
| Hypnotic Pulse | repetitive 1/8 pulses under a 1-bar filter sine and a 2-bar pan drift | Clean Rhythmic | Ducked Bloom |
| Echo Gate | sparse hits every three 1/16 steps; the macro brings in and feeds back the echoes, blooms the reverb | Clean Rhythmic | Ducked Bloom |
| Spacious Gate | half-note swells (40 ms attack, 120 ms release) of pink noise into an endless space | Off | Endless Space |
| Acid Noise | screaming band pass under a 1/16 saw and a 1/16 gate, the band lifting with the macro | Psy Feedback | Clean Space |
| Machine Sweep | a 1/8 saw-up throws the resonant cutoff two decades, a 1/2 square swaps sides; resonance and drive rise | Dark Industrial | Tight Space |
| Psy Filter | a 1-bar triangle on the resonant cutoff deepening with the macro, a stereo sine speeding up | Wide Ping-Pong | Psy Atmosphere |
| Wobble Noise | a 1/4 sine swings a resonant low pass; the macro deepens it and adds resonance | Off | Clean Space |
| Metallic Motion | ringing, driven band pass shimmering with a 6 Hz sine (faster with the macro) over a slow pan | Clean Rhythmic | Bright Hall |
| Random Circuit | 1/16 S&H on a resonant band pass under a 1/16 gate; the macro (aux) spreads the steps | Clean Rhythmic | Tight Space |
| Psy Chaos | acid plucks: 1/16 S&H on a 12 dB band pass, On-Tie-On-Off steps; the macro lifts, spreads and drives | Psy Feedback | Psy Atmosphere |
| Glitch Storm | 1/32 S&H throwing a band pass around and (with the macro) across the stereo field, swung 1/32 stutter | Dark Feedback | Off |
| Digital Machinery | 1/8 S&H with no smoothing stepping the cutoff in hard jumps under a Long-Short gate | Ducked Gate Echo | Tight Space |

Features demonstrated: all three noise colours and the colour as a macro destination; the main filter in all three
types and both slopes with resonance and drive; Filter 2 as a tone cap; the 12 dB high pass thinning the lows late;
LFO 1 / 2 with sine, triangle, saw up / down, square and S&H waves, free and synced (4 bars to 1/16), retriggered and
song-locked, with rate and depth as macro destinations; the S&H synced at 1/8, 1/16 and 1/32 with and without
smoothing and the macro as its aux source; the trance gate at 1/4, 1/8, 1/16 and 1/32 with patterns, ties, swing,
slow and fast envelopes and the rate and depth as macro destinations; pan, width and level as destinations; the
compressor; and all delay modes, the ducked and fall effect presets and every reverb character through the effect
presets. Not used because they do not exist: delay saturation (removed in v0.9.3), a reverb freeze (Frozen
Atmosphere's Range mapping stands in), velocity (left at 0 % so the presets play the same from any keyboard).

The two Fall presets are designed for automation from 100 % down to 0 %: their effect presets (Fall Echo, Fall Wash)
pull the mix and decay *down* with the macro, so the echoes and the 9 s wash take over as the dry noise darkens and
fades. The gated FX presets are built on the synced gate, so they need the host transport running.

User presets: `~/Library/Application Support/ElukkaNoise/Presets/<Category>/<name>.elukkanoise` (XML, format
version 3 since v0.9.21: the effect preset names). Factory presets cannot be deleted or renamed, and (v0.9.25) a user
preset cannot take a factory name: it would be shadowed by the factory entry, so Reload would throw the edits away.
A name is stored as typed without the characters a file name cannot hold (`" # @ , ; : < > * ^ | ? \ /`, leading
dots), so "Rise, Fall: A" is saved as "Rise Fall A" and the menu, the file and the project agree on it. **Reload**
brings a preset's saved values (and its effect presets) back.

### Delay and reverb presets (v0.9.20)

Each effect panel has its own preset menu (factory, user, Save as, Delete) and a MACRO button. A delay preset is the
delay's knobs plus its macro mapping; a reverb preset the reverb's knobs, its character and its mapping. Depths are in
the knob's range; "late" means a curve that keeps most of the change for the top of the macro.

Delay: Clean Rhythmic (the default: 1/8 D ping-pong, Mix +35 % late, Feedback +15 %, Width +20 %), Wide Ping-Pong
(Mix +40 %, Feedback +30 %, Width +40 %), Dark Industrial (1/8 T wide ping-pong, ducked; Feedback +40 %, Filter -50 %,
Mix +30 %), Psy Feedback (1/16; Feedback +35 % into the loop limiter, Mod +50 %, Mix +30 %),
Atmospheric (1/2 wide, dark; Mix +30 % late, Width and Feedback +20 %), Ducked Gate Echo (1/8 ping-pong at 12 %, Duck
60 %, Mix +20 % late), Dark Feedback, Modulated Echo, Fall Echo (v0.9.21, for falls: 1/8 ping-pong at 35 % / feedback
65 % with the macro pulling both down 30 %, so they grow as it falls), Off (v0.9.21: mix 0, no mapping).

Reverb: Clean Space (the default: Clean Hall, Mix +30 % late, Decay +15 %), Endless Space (Decay +40 %, Size +30 %,
Mix +35 %, Width +30 %), Dark Bloom (Tone -90 %, ducked; Decay +30 %, Mix +35 % late), Bright Hall (Tone -25 %; Decay, Width and Mix +30 %), Psy Atmosphere (Decay +25 %, Width +40 %, Mod +50 %), Frozen Atmosphere (Decay in
Range mode 4 s -> 20 s, Mix +40 % late, Duck +40 %: towards a sustained texture without a freeze switch, so nothing
jumps), Tight Space, Ducked Bloom (duck 70 %, Mix +30 % late, Decay +25 %: the ducking + bloom combination), Modulated
Space, Fall Wash (v0.9.21, for falls: a 9 s endless wash at 45 % with the macro pulling the mix and decay down), Off.

User effect presets: `.../Presets/Delay/<name>.elukkafx` and `.../Presets/Reverb/<name>.elukkafx` (XML with a format
version, the kind, the parameters, the Map entries and, for the reverb, the character). Deleting one never touches the
other effect's presets or the instrument presets. Projects saved before v0.9.20 get the effect preset that matches their
old character menu (mapping and character only; their knobs stay).

## Test results (v0.9.26, 8 Nov 2026, Apple M1, macOS 26.3)

`ElukkaNoiseTest`: 251 checks, all passing, also under AddressSanitizer and UndefinedBehaviorSanitizer. v0.9.26: the
tail length is 5.6 s by default, 26.5 s with a 20 s decay and 0.3 s with both effects off; All Sound Off leaves the
next block at exactly 0 while All Notes Off still sounds; synced 1/1 at 40 bpm echoes at 3 s (1/2) with nothing at
1.5 or 2 s; echoes do not come back after the delay was off for 1 s or 0.4 s; pink noise brightness at 48 / 96 kHz
0.442 / 0.419 (white itself moves 0.797 / 0.829 with the rate); "C", "30L" and "pink" parse as values, "abc" and
"loud" do not; the reverb's Macro window is 420 x 294 in a 400 x 300 space. v0.9.25: 243 checks (a
`RelWithDebInfo` build of the test program with `-fsanitize=address,undefined`; the one report was in the test harness
itself and is fixed). v0.9.25: a factory name or an empty name is refused for an instrument or effect preset, "Rise,
Fall: A/B" is saved and listed as "Rise Fall AB", Rename follows the same rules, a corrupt effect preset file leaves
the current preset alone, a v0.9.22 project's reverb Low Pass mapping (Add -60 %) loads as Tone Add -30 % and a High
Pass Range 20..80 % as Tone Range 60..90 % (the low pass wins when both were mapped, an existing Tone mapping is
kept), and a call-out box and a dialog opened from the editor are deleted with it so nothing stays modal; v0.9.24: the stereo pan keeps the level within 0.06 dB from hard left to
hard right at Stereo 0 / 100 / 200 %, mono noise still pans 7.66 dB right at 50R, hard right leaves the left silent and
at Stereo 100 % / 50R the left speaker drops 3.0 dB; v0.9.23: Noise Stereo 0 % = mono, 100 % = independent noise, 200 % = wider
still; the mod rate follows the depth, 0.3 / 0.53 / 0.95 / 3 Hz at 0 / 25 / 50 / 100 %; Tone -75 % = low pass
1.26 kHz, +50 % = high pass 400 Hz, Tone -87 % darkens and +78 % thins a wet tail as the old 800 Hz / 1 kHz filters
did; a v0.9.22 project migrates Width 40 % -> Noise Stereo 40 %, Low Pass 700 Hz / High Pass 90 Hz -> Tone -91 %, and
its matrix slots on Width, Level and Filter 2 Drive keep their meaning; an old reverb preset file with Low Pass 8 kHz
loads as Tone -25 %; a v0.9.22 instrument preset file with Width 150 % loads as Noise Stereo 150 % with its slot
re-indexed; every reverb and delay knob jumping between extremes still never clicks. v0.9.22: Filter 2 Drive at 100 % lowers the crest factor 3.35 -> 2.19
without clicks; 35 destinations; Init's high pass is 500 Hz). The v0.9.21 preset rework added: 16 + 16 factory presets, every one naming
a factory delay and reverb preset; every factory preset loads the effect presets it names; loading Spinning Rise
selects Clean Rhythmic and Endless Space with their knobs, mapping and character and keeps the macro position;
changing only the delay (or only the reverb) afterwards leaves the instrument and the other effect untouched; another
instrument preset selects its own effect presets; a saved instrument preset carries the effect preset names (format
version 3) and no effect parameters, and loading it selects them; a preset without names leaves the effects alone and
a preset naming a missing one gets Clean Rhythmic / Clean Space; factory instrument and effect presets cannot be
deleted, saving under a factory name is refused; an edited effect knob marks its preset modified
until a preset is loaded again; and every preset as a complete sound, effects on: held at 0 / 25 / 50 / 75 / 100 %
no peak over 0.85, no level jump over 8 dB between neighbouring positions, nothing below -32 dB, a 0 -> 100 -> 0 %
sweep through the note and the tails without a click, and the tails decaying (the falls' 9 s wash is 20 dB down 3 s
later). Every preset also passes the effects-off loop (rise brighter than start, mono-compatible, no clicks),
instrument presets switched while a note is held bring their effect presets along, and the five sweep risers keep
the cutoff rising up to 100 % (checked against the knob plus the preset's push). Filter 2 checks: the defaults are an open 12 dB low
pass; after an open main filter a 300 Hz low pass, a 5 kHz high pass and a 1 kHz band pass shape white noise as
expected (12 and 24 dB); at its default the filter is out of the path, a hair below 20 kHz it takes the top octave
off, and moving the cutoff between 20 kHz and 1 kHz every 0.25 s mid-note never clicks; the macro closing it
through the matrix darkens the sound and leaves the knob untouched; type and slope switched every 0.25 s mid-note
never click; its settings round-trip through the project state and an instrument preset; all 34 destinations
follow the matrix. v0.9.20 added the effect preset checks: 8 + 9 factory presets with the
brief's names, every value in range and every mapping valid; a new instance starts on Clean Rhythmic / Clean Space;
changing the delay preset changes only the delay (every other parameter and the reverb's mapping compared before and
after), the reverb preset only the reverb, and an instrument preset keeps both effect presets, their mappings and the
macro position; at 75 % the delay's feedback and width and the reverb's decay and size move by their presets' depths
at once; three 0 -> 100 -> 0 % sweeps change no stored parameter and the effective values return exactly; Frozen
Atmosphere's Range mapping gives 4.00 / 20.0 s whatever the knob says and a mapping switched off is the knob; delay and
reverb presets switched in every 0.4 s at macro 0, 50 and 100 % mid-note and through the tails never click and the
tails keep ringing; instrument presets switched while a note is held keep the effect presets; user delay and reverb
presets save and load back with an edited Range mapping, the character and a format version, Restore brings the saved
mapping back, factory presets cannot be deleted and deleting one effect's preset leaves the other's; a v0.9.19 state
migrates (matrix slots that pointed at the effects become empty, Level and Comp Amount keep their meaning, the
character menus pick the effect presets, the knobs stay); the project state round-trips the effect presets, the
character and an edited mapping. v0.9.19 added the delay checks: a 1/1 echo lands within 1 ms and Offset
+30 % puts the right echo at 1.3x; a time change is exact at once and the crossfade never clicks under held noise;
the Filter evolves the repeats both ways; ping-pong alternates at full level with feedback squared on the third echo,
width 0 centres it, wide ping-pong swaps sides; feedback 100 % sustains at a steady level with peak 0.99; ducking
drops the echoes 53 dB on a pulse and recovers fully; Rolling Gate Rise's echoes grow 26 dB over the macro; thirteen
delay controls jumping between extremes mid-note never click; a tempo change lands the next echo on the new grid and a
tempo ramp never clicks; the echoes outlive the note and a closed gate; the extreme setting stays bounded; old
parameters migrate. v0.9.17 added the reverb checks: Decay holds at 1 kHz within 15 % at 0.5,
3 and 12 s and at 44.1 / 96 kHz; with Low Pass 4 kHz the 8-16 kHz band decays in 0.6 s against 3.7 s for the mids and
50-100 Hz in 2.2 s; width 0 gives identical channels, 100 % a correlation of 0.01 with a -3 dB mono sum; flutter and
ripple below the old reverb's; ducking drops the wet 14 dB on a 1/16 pulse and recovers fully in the gap; Rolling Gate
Rise's tail grows 21 dB over the macro while the chops still dip 12 dB; eleven reverb controls jumping between extremes
mid-note never click; the tail outlives the note and a closed gate; the extreme setting stays bounded. v0.9.16 fixed
the S&H Jump at 15 %. v0.9.14 added the compressor On switch: off is bit-identical to amount 0,
switching at full amount does not click. v0.9.13 added makeup loudness checks: held notes within 0.6 dB of the
uncompressed loudness at 50 / 100 %, slow and fast, on three presets; amount swept 0 -> 100 % or jumped 0 -> 80 % stays
within 0.5 dB on 400 ms windows; `ElukkaNoiseTest --makeup` prints the full table. v0.9.11 added the compressor: 70 % narrows a riser's 25 -> 100 % rise
from 8.4 to 1.9 dB, makeup brings the loudness back within 0.1 dB, speed settles in 4.7 ms fast / 81 ms slow, full fast
compression on gated noise switched in mid-note does not click, its place in the chain matters; every preset renders
bit-identical to v0.9.10 with the compressor at 0. v0.9.10 added: macro range, effects order (different sound, click-free
swaps), reverb low / high pass, resonance range. The wider resonance changed preset levels by -2.9..+3.7 dB; out_level
was rebalanced so every preset is within about 0.7 dB of its old level through the sweep, 1.5 dB at the top of risers.
v0.9.8 added: unipolar sources, "Modulate with" amount = 100 % minus the
knob, the new LFO / S&H defaults. The factory presets were converted from bipolar to unipolar routings; rendered
before and after, every one matches the old sound to about -100 dB once past the first second of a note, where a
fix now makes a note start from the current cutoff / pan / gain instead of the values from the last note. v0.9.7 added: a note 0.1 s into the song starts the gate's steps on the
note with Retrig on, within 1.8 ms, and keeps them on the song's 1/16 grid with Retrig off. v0.9.6 added: Init is plain white noise, its 8 kHz / 1 kHz band ratio
within 0.4 dB of reference white noise, the macro changes nothing; switching the gate on and off mid-step never clicks.
v0.9.5 added: with Vary off gate steps 3 and 6 hold identical noise,
correlation 1.000; with Vary on they are unrelated, 0.047. v0.9.4 added: typed values read as shown, e.g. 50 = 50 %, and every float
parameter's displayed text types back to the same value):

- every factory preset through a 2 s rise, hold, 1.5 s fall and release: finite, no clipping, brighter at 100 % than
  at 0 %, mono sum within 4.2 dB, zero clicks (worst outlier 7.0 sigma, threshold 8);
- the five stretched risers: cutoff still rising between 90, 95 and 100 % and topping out within 2 % of the target;
- modulation matrix: with the macro as aux, LFO 1 moves the cutoff 0 / 0.30 / 0.60 (normalised) at macro 0 / 50 / 100 %;
  all 40 destinations follow the matrix; feedback 60 % and tone 10 % in every preset, delay saturation gone; the three
  new presets are band pass + 1/16 S&H on the cutoff + gate;
- 10 % macro steps every 0.1 s and instant 0 -> 100 -> 0 % jumps: no clicks; brightness 100 ms after a jump is
  99.6 % of the final value (the smoothing is quick);
- overlapping, retriggered and 10 ms repeated notes: no clicks; release behaviour as designed;
- all 18 presets switched in every 0.2 s while a note is held: no clicks;
- delay and reverb on in all 18 presets, Vary off in all, the shared space setup on all 9 risers / falls; the DELAY knob
  (reverb off) and the REVERB knob (delay off) each add at least 3 dB of tail in every preset; either knob jumping up
  mid-note never clicks;
- gate: ties, swing 66 % timing, hold + decay to 30 % sustain, random steps at 1/32 with a 7-step pattern, all
  click-free; Rolling Gate Rise measures exactly 1/8, 1/16 and 1/32 at 10, 50 and 90 %;
- level envelope of a sweep at 48 kHz / 64 and 2048 samples and 44.1 kHz within 0.4 dB of 48 kHz / 512, 96 kHz within 1.2 dB;
- full resonance, drive, +24 dB, delay feedback 100 %, 20 s reverb, 50 Hz LFO and 200 Hz S&H: bounded and decaying;
- synced 1/16 gate step count, 1/4 LFO period (461.4 ms at 130 bpm) and 1/8 delay time (230.0 ms) correct;
- project state (macro, edits, preset name, UI state) and preset files round-trip; user preset save, load, rename,
  delete and factory protection work (in a temporary folder);
- mono output bus renders; bypass keeps the note state.

`ElukkaNoiseTest --host`: the built universal (arm64 + x86_64) VST3 loads through JUCE's VST3 hosting, exposes
MACRO, renders, round-trips state and opens its editor (780x450).

CPU (`--bench`, one instance, percentage of one core, 5 s renders):

| | 44.1 kHz / 64 | 48 kHz / 512 | 96 kHz / 64 |
|---|---|---|---|
| Clean Noise Rise | 0.8 % | 0.8 % | 1.8 % |
| Hypnotic Pulse (synced LFOs, gate) | 0.8 % | 0.8 % | 1.9 % |
| Rolling Gate Rise (sequencer gate) | 0.8 % | 0.8 % | 1.9 % |
| 8 instances, sum | 6.2 % | 6.3 % | 14.9 % |

(v0.9.20: every preset runs the delay and the reverb through the default effect presets, so every row carries both
effects; before, presets without effects sat at 0.3 %. v0.9.21: the rows are within run-to-run noise of v0.9.20;
Filter 2 is out of the path at its default, so no factory preset pays for it.)

No audio-thread allocation or locks: buffers are sized in `prepareToPlay`, parameters are read from atomics, the
noise is generated inline, and an idle voice / silent effect is skipped.

## Release audit (v0.9.25 / v0.9.26)

Before the first public beta the whole source was reviewed (DSP, processor and presets, UI), rebuilt with all
warnings, and the test suite run under the address and undefined-behaviour sanitizers. Fixed: no licence file
(now AGPLv3), tester notes for Linux and Windows that described ElukkaSynth, user presets saved under a factory
name being shadowed by it, preset names diverging from their file names, the preset dialogs and call-out boxes
outliving the editor (a window left open while the host removed the plugin could crash on the next click), a
corrupt effect preset file changing the current name without loading, and the macro mappings on the reverb's old
Low Pass / High Pass knobs being dropped instead of becoming Tone mappings. The audio path has no allocation, lock,
string or file access; every table index is clamped; the migrations run once and in order.

Fixed in v0.9.26, the minors the audit had listed as known:

- `getTailLengthSeconds` follows the sound: the release, then the longer of the reverb decay (1.3 x T60 plus the
  pre-delay) and the delay's echoes down to -60 dB, capped at 30 s (5.6 s for the default sound, 0.3 s with both
  effects off).
- All Sound Off (a host's panic button) silences the voice and both tails at once; All Notes Off still releases.
- A synced delay division the 4.2 s buffer cannot hold (1/1 below 57 bpm) drops an octave (1/2, then 1/4), so the
  echoes stay on the grid instead of landing at 4.2 s.
- The pink noise filter's three poles are moved to the same frequencies at every sample rate (and each section's
  low-frequency gain kept), so Colour has the same tilt at 48, 96 and 192 kHz. Before, at 96 kHz the 3.9 kHz
  section sat at 8.6 kHz.
- An idle effect's buffers are cleared in pieces (16 x real time) over the control blocks that follow, not in one
  fill on the audio thread; an effect needed again before the clear is complete gets the rest at once. An effect
  counts as idle when its mix is below -80 dB and its fade-out is done (about 0.25 s after the knob reaches 0).
- The Macro window shrinks with the editor (420-640 px wide, rows 18-26 px), so it is never clipped at small scales.
- A typed knob value is applied only when it reads as one (a digit, or one of the parameter's own words such as
  "C" or "pink"); clicking away from the value box reverts it instead of applying.
- Clicking an effect panel's title without moving it no longer writes the order (no automation touch).
- Windows device names (CON, NUL, COM1, ...) get an underscore as preset names.
- The editor scale follows the smaller of the window's width and height ratios, so a host that sizes the window
  oddly never clips the view; the reverb's diffuser write index is masked (no overflow in long sessions); the
  pre-delay glide is the same 23 ms at every rate.

Still known: the Mac host-window flag (`hidesOnDeactivate`) that keeps Live's device window up is set and never
restored; it is a deliberate workaround and only matters while the plugin window is open.

## Not yet done

- A listening pass in Live for the v0.9.21 factory collection (32 presets designed and measured offline: levels,
  brightness, tails, clicks; the WAVs in `ListeningTests/` are the 0 -> 100 -> 0 % sweeps), Filter 2 and the v0.9.20
  effect presets (tune knob values and macro depths by ear), and for the v0.9.17 reverb (ducking, the bloom, the
  characters).
- Windows and Linux builds have only been exercised by CI (compiled, loaded like a host, the 251-check suite
  run); nobody has used them in a DAW yet.
- The macOS build is not notarized (see `packaging/README-macos.txt`).

## Design decisions worth knowing

- Mono instrument (last note sustains, any held note keeps it sounding). Polyphony adds nothing to unpitched noise.
- MIDI pitch is ignored; velocity scales the level through the Velocity parameter (0 % by default).
- The synced trance gate follows the song position; a free-running gate (Sync off) can have its rate driven by the
  macro. Synced LFOs with Retrig off also lock to the song position.
- v0.9.25 maps a v0.9.20-0.9.22 reverb Low Pass / High Pass macro mapping onto Tone assuming the old knobs ran
  500 Hz-20 kHz and 80 Hz-2 kHz (log), the ranges Tone's two halves cover; Mod Rate mappings are dropped. Only
  projects from those unreleased versions are affected.
- v0.9.24 changed Pan from a balance control to a stereo pan without a new parameter: mono noise is unchanged, but a
  saved sound with Stereo above 0 % and the pan off centre (or moved by an LFO) keeps more of its width than before.
- Projects, instrument presets and effect presets saved before v0.9.23 are migrated: Width folds into Noise Stereo
  (width x stereo below 100 %; a width over 100 % carries over when the noise was fully independent), the reverb's
  Low Pass / High Pass become the nearest Tone (the low pass wins when both were set, so a preset with both loses its
  high pass), the Mod Rate values are dropped (the rate now follows the depth), and matrix slots are re-indexed
  (Width -> Noise Stereo). Automation of width, rev_lp, rev_hp, dly_mod_rate and rev_mod_rate is lost.
- v0.9.21: the factory collection was replaced (the 18 presets up to v0.9.20 are gone; a project saved with one keeps
  its parameters, only the preset name no longer matches a menu entry). Instrument presets name their delay and
  reverb presets (by name, so a user effect preset can be referenced; a missing name falls back to the defaults).
  The sources are unipolar and written directly (the bipolar-conversion helpers of v0.9.8 are gone with the old
  presets).
- Projects saved before v0.9.3 lose their macro map and LFO / S&H routings (they became the modulation matrix) and
  delay saturation; factory presets were all converted.
- Projects saved before v0.9.20: the delay and reverb left the modulation matrix, so a slot that pointed at an effect
  becomes empty (the other slots are re-indexed and keep their meaning); the reverb and delay character menus pick
  the effect preset whose mapping and character the project gets, its knobs stay. Instrument presets (factory and user
  files) no longer carry delay / reverb settings: loading one leaves the effects as they are, so the factory risers'
  old space setups are now effect presets (Ducked Gate Echo, Ducked Bloom, ...). Mapping entries are plugin state, not
  host parameters; the rev_char and dly_char parameters are gone.
- Projects and user presets saved before v0.9.19 are migrated: the delay's division, Ping Pong switch and Low Pass /
  High Pass become Division, Mode and Filter (an approximation: LP 6 kHz -> Filter -30 %, HP 300 Hz -> +28 %). The
  new knobs start at their defaults (width 100 %, offset 0, mod 0 at 0.5 Hz, duck 0, Clean Echo).
- Projects saved before v0.9.17 keep their reverb knob values but play them through the new reverb, which is brighter
  at the start of the tail and tighter in the lows at the same settings; the new knobs start at their defaults (width
  100 %, mod 25 % at 0.5 Hz, duck 0, Clean Hall).
- Projects saved before v0.9.10 lose the reverb Tone (now Low Pass / High Pass at the old 10 % sound) and sound
  less resonant (wider resonance).
- Projects saved before v0.9.8 with LFO / S&H routings will sound different (sources are unipolar now) and lose the
  LFO Unipolar switch; factory presets were converted and sound the same.
- Projects saved before v0.9.7 lose the gate's Sync setting (the gate is always synced now) and get gate Retrig on.
- Projects saved with v0.9.1 that used SPACE lose it (the parameter was replaced by the DELAY and REVERB knobs).
- Projects saved with v0.9.0 that used the old gate Pattern menu lose that setting (the parameter was replaced by the
  step sequencer); all factory presets were converted.
- Gain compensation is a macro-map entry on Level (presets pull the level down a few dB as the filter opens).
- The JUCE VST3 wrapper adds its hidden MIDI CC parameters (hosts list 128 real ones plus those).

## Licence

ElukkaNoise is free software: you can redistribute it and/or modify it under the terms of the GNU Affero General
Public License as published by the Free Software Foundation, version 3 (`LICENSE`). It is distributed without any
warranty. The bundled JUCE framework (`JUCE/`) is used under its AGPLv3 option (`JUCE/LICENSE.md`).
