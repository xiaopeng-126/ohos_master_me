# master_me for HarmonyOS PC

This repository is the HarmonyOS PC adaptation of
[trummerschlunk/master_me](https://github.com/trummerschlunk/master_me).

![master_me running on HarmonyOS PC](docs/masterme-v3-live.jpeg)

The port keeps the original Faust DSP as the audio source of truth and
replaces the desktop DPF/Pugl window with a HarmonyOS Stage application and ArkUI
front end. The native module builds the generated DPF/Faust processor into
`libmasterme.so`, connects OHAudio full-duplex device I/O, and exposes audio,
parameter and meter control to ArkTS.

## Current scope

- HarmonyOS 2-in-1 Stage application under `harmony/`.
- Native NAPI bridge for the original stereo mastering processor.
- Easy/Expert control surface with the five upstream easy presets.
- OHAudio 48 kHz stereo full-duplex capture, DSP and render pipeline.
- Live input/output meters, loudness history, callback health and stream control.
- Microphone permission, device-change tracking and throttled stream recovery.
- ARM64 and x86_64 native build targets for HarmonyOS PC and emulators.
- Upstream DSP, DPF and presets remain unchanged under `plugin/`, `pregen/` and
  `dpf/`.

## Requirements

- DevEco Studio with HarmonyOS SDK 26.0.0 or newer.
- The BiSheng native compiler and the `2in1` emulator or device toolchain.
- `ohpm` and `hvigorw` available from the DevEco Studio command-line environment.

## Open in DevEco Studio

1. Open `harmony/` as the project root.
2. Select the HarmonyOS PC or 2-in-1 target.
3. Build the `entry` module.

From a terminal with DevEco Studio installed:

```powershell
cd harmony
ohpm install
$env:REFRESH_CPP_MODULE = 'true'
hvigorw assembleHap --mode module -p product=default -p module=entry@default --no-daemon
```

The unsigned HAP is written to
`harmony/entry/build/default/outputs/default/entry-default-unsigned.hap`.
Configure a signing profile in DevEco Studio before installing it on a physical
device.

## Native API

The `libmasterme.so` module exposes:

- `createEngine(sampleRate, bufferSize)`
- `destroyEngine(handle)`
- `loadPreset(handle, presetIndex)`
- `setParameter(handle, parameterIndex, value)`
- `getParameter(handle, parameterIndex)`
- `processInterleaved(handle, input, output, frameCount)`
- `getMeterSnapshot(handle)`
- `startAudio(handle)`
- `stopAudio(handle)`
- `getAudioStatus(handle)`

`input` and `output` are `Float32Array` objects containing interleaved stereo
samples. The process API returns the current input/output LUFS-S and peak values.
The audio status API reports stream state, callback frames, underruns, overflows,
device changes, recovery count and the last error.

## Validation status

The full-duplex audio path, microphone permission, live meters, stop control and
engine self-test have been verified on a HarmonyOS API 26 `2in1` emulator.
Physical-device routing, microphone quality, hot-plug fault injection and signed
release installation remain to be validated on HarmonyOS PC hardware.

## Upstream

- Project: `trummerschlunk/master_me`
- Upstream revision imported for this port: `ea93d78a2b0406805c4993ba95badc0acee0a92c`
- DPF, DPF-Widgets and Pugl are vendored at the revisions pinned by the upstream
  project, so no recursive submodule checkout is required.
- License: GPL-3.0-or-later, see `LICENSE`.
