# Summary: 2026-06-16 — Diagnostics + Audio Init Fixes

## What We Did

### Phase 1: Diagnostics Architecture (Previous Sessions)
- Rewrote `MonitoringDebugView` with 9 sections showing real C++ state
- Changed `EngineDebugManager` to read C++ atomics directly via `diag*` accessors
- Removed all derived/duplicated Swift state — single source of truth

### Phase 2: Core Engine Fixes (This Session)

**1. Dual-engine root cause fixed** (`EngineDebugManager.swift`)
- `EngineDebugManager` created its own `AudioEngineBridge()` — separate from `AudioService`
- Two independent `AudioEngine` instances with separate `AudioUnit`s, state machines, atomics
- Fix: `engine` now references `DependencyContainer.shared.audioService.engine`

**2. Guard removal** (`EngineDebugManager.swift:poll()`)
- `guard isInitialized` blocked `poll()` when engine not initialized
- Diagnostics showed Swift defaults (`Created`, `noErr`) instead of real C++ state
- Fix: removed guard, `poll()` always reads C++ atomics

**3. Computed `engineState`** (`EngineDebugManager.swift`)
- Was a stored `String` that could contradict `diagEngineState`
- Fix: now `switch diagEngineState` — always matches C++ state machine

**4. Already-initialized guard** (`EngineDebugManager.swift:initialize()`)
- Clicking Initialize on already-running engine caused issues
- Fix: `guard !engine.isInitialized()` + auto-start polling

**5. Mouse crash fix** (`TimelineMetalView.swift`)
- `@MainActor func mouseEntered(with:)` generated ObjC selector `mouseEnteredWithEvent:`
- NSTrackingArea sends `mouseEntered:` → unrecognized selector
- Fix: explicit `@MainActor @objc(mouseEntered:)` — correct selector name

**6. Atomic cross-thread reads** (`AudioDeviceManager.hpp/cpp`)
- `m_sampleRate`/`m_bufferSize` read from audio callback thread without atomic
- Fix: `std::atomic<SampleRate>`, `std::atomic<BufferSize>` with `.load()`/`.store()`

**7. Explicit output device** (`AudioDeviceManager.cpp`)
- Element 0 (output bus) never had `kAudioOutputUnitProperty_CurrentDevice` set
- Fix: set output device explicitly on element 0, log confirmation

**8. AudioUnitRender -10863 fix** (`AudioDeviceManager.cpp`)
- Core Audio gave 236 frames but input AUHAL expected 256
- `os_log_error` on every failed callback in realtime thread
- Fix: clamp `numFrames` to device buffer size, rate-limit error log (first 5 + every 1000th)

**9. Auto-polling on init** (`EngineDebugManager.swift`)
- `EngineDebugManager` now auto-starts polling when engine is already initialized

**10. Pointer logging** (all files)
- `[Engine] this=0x... state=Running`
- `[Bridge] self=0x... engine=0x...`
- `[DeviceManager] this=0x...`
- Every state transition logs `this` pointer for identity verification

## Files Modified
- `DAW/Features/Debug/EngineDebugManager.swift` — dual-engine fix, computed state, auto-poll
- `DAW/Features/Timeline/TimelineMetalView.swift` — ObjC selector fix
- `DAW/AudioEngine/Device/AudioDeviceManager.cpp` — output device, render fix, atomics
- `DAW/AudioEngine/Device/AudioDeviceManager.hpp` — atomic members, new diagnostics

## What's Still There
- `AudioUnitRender -10863` may persist if input device buffer truly doesn't match
- `linkd.autoShortcut` / `DetachedSignatures` errors are macOS noise, not our code
- Need to verify the render fix in Console.app
