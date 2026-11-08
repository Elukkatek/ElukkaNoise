# Building ElukkaNoise

ElukkaNoise is a JUCE 8 / CMake project. It builds a VST3 instrument (`ElukkaNoise_VST3`) and an
offline test program (`ElukkaNoiseTest`). There is no standalone app.

JUCE 8.0.4 is a git submodule in `JUCE/`, so clone with it:

```bash
git clone --recurse-submodules https://github.com/Elukkatek/ElukkaNoise.git
```

(or `git submodule update --init` in an existing clone).

## macOS (universal arm64 + x86_64)

Xcode command line tools and CMake 3.22+.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target ElukkaNoise_VST3 ElukkaNoiseTest -j8
./build/ElukkaNoiseTest_artefacts/Release/ElukkaNoiseTest
```

The VST3 is copied into `~/Library/Audio/Plug-Ins/VST3` after building (turn off with
`-DELUKKA_COPY_PLUGIN_AFTER_BUILD=OFF`).

## Linux (x86-64)

```bash
sudo apt install build-essential cmake pkg-config \
    libasound2-dev libfreetype-dev libfontconfig1-dev \
    libx11-dev libxext-dev libxrandr-dev libxinerama-dev libxcursor-dev libgl1-mesa-dev
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target ElukkaNoise_VST3 ElukkaNoiseTest --parallel
```

Result: `build/ElukkaNoise_artefacts/Release/VST3/ElukkaNoise.vst3/` (copy it to `~/.vst3/`).

## Windows (x64)

Visual Studio 2022 with the "Desktop development with C++" workload and CMake 3.22+.

```bat
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target ElukkaNoise_VST3 ElukkaNoiseTest --parallel
```

Result: `build\ElukkaNoise_artefacts\Release\VST3\ElukkaNoise.vst3\` (copy the folder to
`C:\Program Files\Common Files\VST3\`). The C++ runtime is linked statically.

Windows and Linux are built by GitHub Actions (`.github/workflows/build.yml`): every push builds both,
loads the VST3 like a host and runs the test suite; a `v*` tag also creates a GitHub release with the two
ZIPs. The macOS universal build is made on a Mac and uploaded to the release by hand.

## Test program

```
ElukkaNoiseTest                  the test suite (exit code 1 on failure)
ElukkaNoiseTest --sweep [preset] level and brightness with the macro held at 0/25/50/75/100 %
ElukkaNoiseTest --bench          CPU benchmark at 44.1/48/96 kHz, 64 and 512 samples, 1 and 8 instances
ElukkaNoiseTest --host <vst3>    load the built bundle through JUCE's VST3 hosting
ElukkaNoiseTest --ui <dir>       save editor snapshots as PNG
ElukkaNoiseTest --wav <dir>      render a rise / hold / fall of every preset to WAV for listening
```

## Packaging a beta ZIP

```bash
scripts/package.sh macos build/ElukkaNoise_artefacts/Release/VST3/ElukkaNoise.vst3 dist
```
