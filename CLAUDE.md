# CLAUDE.md

## Rules

1. Ask when unclear. If intent, architecture, or requirements are ambiguous, ask before coding.
2. Flag uncertainty. If an approach, dependency, or technical detail is uncertain, say so before proceeding.
3. Challenge bad direction. If my request conflicts with settled practice or likely long-term maintainability, point it out and suggest a better path.
4. End with omissions. After each task, state what you changed and what you intentionally did not do.

## Documentation

Four rules everywhere in comments and docstrings.

1. What, why, how, in that order, and only as much as is needed. In prose, never restate the signature; add units, ranges and nullability, or say nothing.
2. Sentence case. Capitalize sentence starts, command descriptions, and section titles after icons. Preserve the case of commands, identifiers, URLs, and icon shortcodes.
3. No archaeology. Do not write prose about the history of a class throughout the life cycle of this repository or what an earlier implementation did.
4. Concise. Short sentences, active voice, one idea each, one term per concept per file. A set of cases wants a table; a flow wants a diagram.

Document the code using ASD-STE100-inspired Simplified Technical English: use short, direct sentences, one term per concept, active voice, explicit conditions, and avoid idioms, unnecessary synonyms, or ambiguous wording. Focus documentation on intent, constraints, side effects, and non-obvious behavior;
Write for an engineer who knows the language but not this system.
Read [CONTRIBUTING.md](CONTRIBUTING.md) before writing or reviewing code documentation.

## Project

`glyph` is an SKSE64 plugin (C++23, MSVC, x64) for Skyrim SE 1.5.97 and AE 1.6.x (Steam and GOG). It
draws floating actor nameplates as an ImGui overlay injected into the game's D3D11 pipeline, and
builds to a single Address Library-backed `glyph.dll`. Skyrim VR is deliberately unsupported
(`ENABLE_SKYRIM_VR OFF`, and `SKSEPlugin_Load` rejects non-SE/AE runtimes).

Dependencies come from vcpkg: ImGui (dx11 + win32 + freetype + wchar32), spdlog, xbyak, NanoSVG,
nlohmann-json, GTest. CommonLibSSE-NG is pulled by `FetchContent` from `main`.

## Commands

Requires `VCPKG_ROOT`. All presets use the `x64-windows-static` triplet and the static MSVC runtime
(`/MT`, `/EHa`).

```powershell
.\build.bat                 # clang-format -i, configure, clang-tidy, Release glyph.dll, doxide+mkdocs
.\build.bat --skip-tidy     # same, minus static analysis (much faster iteration)
.\test.bat                  # configure if needed, build all 6 gtest targets, run each exe
.\deploy.bat                # zip dll + ini + glyph.project.json + assets/, install as an MO2 mod
.\purge.bat                 # remove that mod folder and its modlist.txt entry
```

`deploy.bat` and `purge.bat` read `GLYPH_MO2_MODS` and `GLYPH_MO2_PROFILE` (defaults point at a
Nolvus install) and name the mod from `vcpkg.json`'s `version-string`.

Finer-grained, for when the batch files are too coarse:

```powershell
cmake --preset default                                        # VS 2022 generator -> build/
cmake --build build --config Release --target glyph -- /m:1   # plugin only
ctest --test-dir build -C Release --output-on-failure         # all tests
ctest --test-dir build -C Release -R GraffitoBasis            # tests matching a regex
build\Release\glyph_test_settings.exe --gtest_filter=Foo.Bar  # one gtest case
clang-format --dry-run --Werror src/*.cpp src/*.hpp           # what CI's format gate runs
```

Build-system facts that are easy to trip over:

- `-- /m:1` is intentional. `CommonLibSSE` is compiled `/MP1` (see `CMakeLists.txt`) because its
  templates exhaust RAM under parallel `cl.exe` and crash with `STATUS_ACCESS_VIOLATION`. Clean
  builds are slow; incremental ones are fine.
- clang-tidy runs out-of-band against a **Ninja sidecar** compile database in `build-cdb/`
  (`cmake --preset compile-db`, then `scripts/_normalize_compile_db.py` rewrites it for clang-cl
  driver mode), never through `CMAKE_CXX_CLANG_TIDY`. The VS generator emits no
  `compile_commands.json`, which is also why `.clangd` points at `build-cdb`. `build.bat`
  regenerates the sidecar when `CMakeLists.txt` is newer than it — do that after adding or removing
  sources. `.clang-tidy` sets `WarningsAsErrors: ''`, so analysis is advisory.
- CI is three workflows: `build.yml` (clang-format gate, then `ci-windows` + `ci-windows-release`,
  uploads `glyph.dll` and `glyph.pdb`), `test.yml` (`ci-windows-tests` then
  `ctest --preset ci-windows-release`), and `sonar.yml` (SonarCloud through the build wrapper).
- The `windows-tests` and `ci-windows-tests` build presets must list **every** gtest target. CI
  builds only what those `targets` arrays name, while `ctest` runs everything
  `gtest_discover_tests` registered, so a missing target fails CI as `<target>_NOT_BUILT`.
  `test.bat` keeps its own target list and never calls `ctest`, so this gap stays green locally.
- The version lives in three places that must move together: `vcpkg.json` `version-string`
  (deploy/purge mod-folder name), `src/Version.hpp` macros (read by `scripts/_clean_docs.py`), and
  the `REL::Version` literal in `SKSEPluginInfo` in `src/main.cpp`.

## Architecture

### Two-thread producer/consumer contract (the central invariant)

Everything in `Renderer` is split by thread affinity. Mixing them up is the main source of crashes
here.

- **Game thread** (`RendererSnapshot.cpp`, entered via `SKSE::GetTaskInterface()->AddTask()`) is the
  only place `RE::*` game objects may be dereferenced. `UpdateSnapshot_GameThread()` scans
  `RE::ProcessLists`, resolves names, levels, factions and relationships, runs occlusion and
  TrueHUD/moreHUD queries, and publishes a `std::vector<ActorDrawData>` of **plain data** under
  `snapshotLock`, together with the `allowOverlay` and `allowDeck` atomics.
- **Render thread** (`Renderer.cpp`, `RendererLayout.cpp`, `RendererEffects.cpp`) copies the
  snapshot under the lock, then projects, smooths, lays out and draws. It never touches game state.
- `QueueSnapshotUpdate_RenderThread()` requests the next update; `updateQueued` coalesces so at most
  one task is ever in flight.

There is one deliberate, documented exception: `Occlusion::GetCameraInfo()` reads `RE::PlayerCamera`
and is called from the render thread every frame for focus selection and Graffito targeting, where a
torn read is a benign one-frame glitch. `Occlusion.hpp` states this. Do not widen the exception
without the same kind of written argument.

Practical rules:

- A function suffixed `_GameThread`, `_RenderThread`, or `...RT` states its affinity — respect it.
  `GameState::CanDrawOverlay()` is game-thread only; the render thread reads its cached result
  through `Renderer::IsOverlayAllowedRT()`.
- Anything the render thread needs about an actor must be added to `ActorDrawData` in
  `RendererInternal.hpp` and filled in on the game thread. Never put an `RE::Actor*` in it.
- Console overrides (`glyph title`, `glyph icon`) live in `ActorOverrides`, a store behind its own
  **leaf mutex**: never take `Settings::Mutex()`, `snapshotLock`, or the `BadgeTextures` mutex while
  holding it. The game thread resolves a record into `ActorDrawData::overrides`, an immutable
  `shared_ptr`; the render thread only reads that pointer, and usually destroys the record when the
  last frame copy dies. Both are safe because the record holds std strings only. Caps live in
  `RenderConstants.hpp`: `MAX_EXTRA_BADGES` (4) per actor, `MAX_OVERRIDE_ICON_NAMES` (32) per
  session. Overrides are session-only and never written to disk; `EraseDynamic()` drops records for
  runtime FormIDs (at or above `0xFF000000`) on load and new game, because those IDs are recycled.

### Renderer translation units

`Renderer.hpp` is the only public surface (`Draw`, `TickRT`, `IsOverlayAllowedRT`,
`PrepareDeckCaptureRT`, and the enable/identity entry points). `RendererInternal.hpp` holds the
shared types and state, and **five** TUs include it: `Renderer.cpp` (frame orchestration, `Draw()`,
focus selection, hot-reload driver), `RendererSnapshot.cpp` (game thread), `RendererLayout.cpp`
(measurement, `FormatString`, badges), `RendererEffects.cpp` (outline, glow, particles, per-effect
dispatch), and `Deck.cpp`. Mutable state lives in `RendererState` and `SnapshotState` behind the
`GetState()` and `GetSnapshotState()` function-local statics — not namespace-scope globals.

### Hooks and frame entry

`Hooks::Install()` installs three hooks:

| Hook                                          | Site                 | Role                                         |
|-----------------------------------------------|----------------------|----------------------------------------------|
| `BSGraphics::Renderer::CreateD3DAndSwapChain` | thunk call (`0xE8`)  | D3D11 init, device-change detection          |
| `HUDMenu::PostDisplay`                        | vtable[6]            | normal per-frame draw                        |
| `IDXGISwapChain::Present`                     | COM vtable[8]        | fallback for upscalers that skip PostDisplay |

Only creation and `PostDisplay` can bootstrap ImGui; `Present` cannot, and it never retries.
Initialization is lazy and re-entrancy-safe (acquire-load on `initialized`, 5 s retry backoff, CAS
guard) because either hook can fire first. Fonts, particle textures, badge SVGs, and the GPU
post-process, DepthClip, SceneMeter and Graffito pipelines are created on the first frame that has a
device, and re-created after a device change. Hook failures are logged, not fatal.

### ImGui draw-callback bracketing

The GPU features do not fork ImGui. They bracket draws with `ImDrawList::AddCallback` pairs that save
and restore *only* the state they touch, so they compose:

`SceneMeter::CaptureCallback` (before any glyph draws — the meter must never read the overlay's own
text back), then `TextPostProcess::BeginGlowCapture` / `BeginDivideCapture`, then per-plate
`Graffito::Apply/RestoreCallback` (vertex shader and VS CB slot 0 only) and
`DepthClip::Apply/RestoreCallback` (pixel shader, CB0, SRV1), then `RenderSampling` push/pop for PS
sampler slot 0 and the `ParticleTextures` blend and sampler callbacks, then `End...AndComposite` —
each end callback followed by `ImDrawCallback_ResetRenderState`.

When adding a pass, keep the discipline: narrow save/restore, LIFO pairing, no nesting across
brackets, and a failure path that leaves the frame rendering exactly as it did before the feature
existed. Every one of these degrades to a no-op rather than hard-failing.

### Settings, hot reload, and the binding table

- `Settings::Load()` parses `Data/SKSE/Plugins/glyph.ini` (~1400 lines): `[General]`, `[Tier0..19]`,
  `[SpecialTitleN]`, `[HonorificN]`, `[RegisterN]`, `[Labels]`, `[LevelDelta]`, `[Icons]`,
  `[Focus]`, `[Graffito]`, `[Quiet]`, `[DeathRite]`, `[Compat]`, `[Candlelight]`, `[DepthClip]`,
  `[Deck]`.
- The pipeline is documented at the top of `Settings.cpp`: `ResetToDefaults()`, then the line loop,
  then `ClampAndValidate()`, then `Generation()++` as a release store. Each key is offered to the
  active indexed section parser first, then to `Format`/`InfoFormat`, then to the `kSettings` map
  keyed on the raw key; leftovers are counted and warned about. Scalar keys therefore match **by name
  regardless of which section they appear in**. A missing INI short-circuits and does not advance
  `Generation()`.
- Scalars are declared **once** in the `kSettings` descriptor table (shape in `SettingsBinding.hpp`,
  table in `Settings.cpp`): key, alias, target pointer, default, validation rule. Add new scalars
  there. Member initializers in `Settings.hpp` are compile-time placeholders, not the operative
  defaults. `defaultValue` must match the target's pointee type exactly (`0.0f` for `float*`,
  `std::string("x")` for `std::string*`) — a mismatch throws uncaught `std::bad_variant_access` on
  the first default reset and aborts plugin loading.
- Accessors return references (`Settings::Distance()`, `Settings::Glow()`, and so on) guarded by
  `Settings::Mutex()`, a `shared_mutex`. The render thread does **not** read them per draw: `Draw()`
  captures a `RenderSettingsSnapshot` once per frame when `Settings::Generation()` changes, and every
  render-thread function takes that snapshot by const ref. Views into settings strings expire on
  refresh even though the snapshot object lives on.
- Hot reload (`ReloadKey`, a win32 virtual key; the shipped INI uses 118, which is F7): the render
  thread sets `reloadRequested`, snapshot updates pause (`pauseSnapshotUpdates`), `Settings::Load()`
  runs off the render thread, then `reloadCompleted` releases the pause and clears the caches.

### Assets

Custom assets ship GUID-obfuscated and are resolved through `glyph.project.json` by
`ProjectManifest`: fonts by role (`name`, `level`, `title`, `ornament`), tier badges in rank order,
particle sprites by style token, and the bubble-pop sprite. `assets/` is gitignored — it ships in the
release archive, not the repo. `duotone/` (the Font Awesome icon library) is deliberately *not*
obfuscated, because `[Icons]` selects icons by semantic name. Every loader falls back gracefully when
the manifest is missing: FA icons for badges, procedural sprites for particles, INI `*FontPath` keys
for fonts.

## Tests

`tests/` links **no game code** — CommonLibSSE, ImGui and `RE::Actor` cannot link in the harness. Two
patterns coexist:

- **Direct** — runtime-independent code is tested for real. `test_graffito.cpp` includes
  `GraffitoMath.hpp` and `GraffitoShaderContract.hpp`; `glyph_test_deck` compiles `src/DeckUtils.cpp`
  and `src/DeckPng.cpp` into the target; `glyph_test_console` compiles `src/ActorOverrides.cpp` and
  includes `ConsoleParse.hpp`; `test_utils.cpp` includes `NameFit.hpp`, `RasterQuality.hpp` and
  `RenderConstants.hpp`. Prefer this. Factoring pure logic into a runtime-free header or TU so it can
  be tested directly is the right move.
- **Mirrored** — `test_settings.cpp`, `test_label_format.cpp` and parts of `test_utils.cpp`
  re-implement the logic under test (INI parsing helpers, color and easing math, `ClassifyDelta`,
  `FormatString`, `LabelFor`). These mirrors go stale silently. When you change the production code,
  update the mirror in the same change and say so.

Register a new gtest target in three places: `CMakeLists.txt` (with `gtest_discover_tests`),
`test.bat`, and the `targets` arrays of both `windows-tests` and `ci-windows-tests` in
`CMakePresets.json`. CI builds through `ci-windows-tests`, so a target missing from that array is
never built in CI even though it passes locally.

## Generated and untracked paths

- `docs/` and `site/` are generated by doxide and mkdocs and are gitignored. The one exception is
  `docs/main.html`: `.gitignore` un-ignores it (`docs/*` then `!docs/main.html`) because it is the
  hand-written MkDocs Material theme override that `custom_dir: docs` loads. It is source, not
  output. Do not hand-edit anything else under `docs/`.
- `build/`, `build-cdb/`, `assets/`, `graphify-out/` and `.understand-anything/` are gitignored.
- `plans/` and `specs/`, when present, hold dated implementation plans and design documents for
  larger features. They are untracked working notes, useful as background on the systems they
  describe.
