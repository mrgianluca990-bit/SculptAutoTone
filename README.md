# SCULPT AUTO TONE — macOS beta 0.1

**A small, experimental standalone Mac app** that analyzes your live guitar input and automatically switches between Neural Amp Modeler (`.nam`) profiles. It does not record or write guitar audio to disk, and does not need a DAW.

## What is actually implemented

- Native Mac standalone app with interface input/output selection (Core Audio through JUCE)
- Real-time guitar monitoring through actual `.nam` models using the NeuralAudio C++ inference engine
- Folder import of up to 24 locally owned `.nam` files (A1/A2 support depends on NeuralAudio model compatibility)
- An *offline* approximate distortion/drive characterization of each capture, using generated test signals (no guitar recording)
- A simple, rule-based live input analyzer (playing strength and attack activity)
- Automatic live NAM selection with stable-decision hold and ~43 ms crossfade
- LOCK and AUTO toggles, likes/dislikes during the session
- Optional cabinet IR (`.wav`) through JUCE convolution
- No cloud, account, telemetry or audio recording

## Not implemented yet

- A machine-learning model that understands *your intended tone*
- Automatic downloads/search on TONE3000
- Automatic best-match selection based on subjective timbral preference
- Automatic IR selection per amp
- Permanent preference memory
- AU/VST3 export
- Fully verified glitch-free switching on every Mac/interface/sample rate

**Important:** This is a source beta. It has not been compiled or run on physical macOS hardware here. The inference API and JUCE build APIs were reviewed, and the separate selection algorithm passed local C++ tests. The macOS app itself needs compiling and testing on a Mac. Do not treat it as an already validated production build.

## Build on a Mac

Requires:
- Mac with macOS 12 or newer (Intel or Apple Silicon)
- Xcode Command Line Tools (`xcode-select --install`)
- Git and CMake 3.22+ (`brew install cmake`, or https://cmake.org)
- Internet during the *first* build: CMake downloads JUCE and NeuralAudio with its submodules

1. Unzip the archive.
2. Open **Terminal**, navigate to the extracted folder, and run:

```bash
chmod +x build_mac.command
./build_mac.command
```

The script builds `Sculpt AutoTone.app` and tries to open it. If macOS blocks the unsigned, locally built app, use Finder → right-click → Open, and allow microphone/audio input in System Settings → Privacy & Security → Microphone if prompted.

Alternatively:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0
cmake --build build -j 4
```

**No precompiled `.app` is included.** A verified, signed/notarized binary needs a macOS build and test environment.

## First guitar test (headphones, low volume)

1. Connect a guitar to an audio interface's Hi-Z/instrument input. Do not connect an amplifier speaker output to the audio interface.
2. Put on headphones and start with headphone/interface output level LOW.
3. Open Sculpt AutoTone → **AUDIO / SSL**, choose your audio interface as input and output, choose **48 kHz**, buffer **128 or 256 samples** initially.
4. Prepare a folder containing **4–8 compatible `.nam` captures** (clean, crunch, drive, lead). Model files are **not included** because their licenses vary.
5. Press **CARICA NAM**, select the folder. Loading runs while audio is stopped; for the first beta that is intentional.
6. For amp-only NAM, click **CARICA CAB IR** and select your own short speaker-cabinet WAV IR. Without it the app applies only a protective 5.5 kHz lowpass; this is not a replacement for cab IR.
7. Play normally. The currently selected NAM name is shown. The first can sound immediately; automatic proposals begin after a short window of stable playing. There is a minimum 7-second spacing between automatic switches.
8. Try **LOCK: ON**, then release. **♥ MI PIACE** and **NON MI PIACE** give simple ranking feedback for this session.

## Limits / known problems

- First release deliberately uses **a rough tone-character/playing-dynamics heuristic**, not an objective 'best NAM' search. It may prefer the wrong musical style. Honest feedback is the purpose of this beta.
- NAM/IR absolute level depends on interface input calibration. NeuralAudio's recommended dB adjustments are used with a capped range, but the default audio-interface input reference is assumed to be +12 dBu. Be careful with gain and monitor volume.
- Profiling generates sine-wave probes and may be slow for complex captures.
- Inputs and model outputs are mono, repeated to stereo. The cabinet IR is mono in this build.
- Changing sample rate after loading models requires reloading the model folder. For best compatibility set the interface to 48 kHz first.
- Loading a folder pauses the audio engine. Switches *between already-loaded models* are crossfaded in the audio callback.
- UI preference buttons update the current session only; there is no recording or preference file.
- Very heavy NAM models and some A2 formats may take substantial CPU or fail to load. If the audio glitches, try fewer models, a larger buffer or lighter capture.
- Only first 24 NAM captures in the selected folder are loaded.

## Debug / tests

Standalone decision logic is independent of JUCE. On macOS or Linux you can run:

```bash
c++ -std=c++20 -O2 tests/test_brain.cpp -o /tmp/autotone_brain_test
/tmp/autotone_brain_test
```

## Technology / license information

- JUCE: https://github.com/juce-framework/JUCE (JUCE license terms apply (AGPLv3 or commercial license, subject to their terms))
- NeuralAudio: https://github.com/mikeoliphant/NeuralAudio (MIT; transitive dependencies have their own terms)
- Neural Amp Modeler: https://github.com/sdatkinson/neural-amp-modeler

The ZIP includes only source code authored for the beta and a build script. Downloaded third-party code is **not** bundled. Review dependency licenses before redistribution or commercialization.
