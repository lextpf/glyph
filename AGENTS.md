# AGENTS.md

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

## Project map

glyph is a Windows C++23 SKSE plugin that draws actor nameplates with D3D11 and ImGui. It uses
CommonLibSSE-NG and supports Skyrim SE and AE, including GOG. VR is disabled in the build.

| Location | Responsibility |
| --- | --- |
| `src/main.cpp`, `Hooks*`, `GameState*` | Plugin lifecycle, engine hooks, and overlay gates |
| `src/Renderer*` | Actor snapshots, projection, layout, and drawing |
| `src/TextEffects*`, `TextPostProcess*` | Text effects and GPU compositing |
| `src/Graffito*`, `DepthClip*`, `SceneMeter*` | World plates, depth clipping, and scene metering |
| `src/Settings*` | INI parsing, defaults, bindings, and hot reload |
| `src/ConsoleCommands*`, `ConsoleParse.hpp`, `ActorOverrides*` | Console parsing and session edits |
| `src/Deck*` | Card composition and PNG export |
| `src/BadgeTextures*`, `ParticleTextures*`, `ProjectManifest*` | Runtime asset loading |
| `tests/test_*.cpp` | GoogleTest suites |
| `glyph.ini`, `glyph.project.json` | Shipped configuration and asset manifest |
| `scripts/`, `doxide.yml`, `mkdocs.yml` | Build and documentation support |

Keep subsystem declarations and implementations paired as `PascalCase.hpp` and `PascalCase.cpp`.
Keep renderer and text-effect work in the existing split translation units. Treat `build/`,
`build-cdb/`, `docs/`, `site/`, and `graphify-out/` as generated output.

## Architecture constraints

- Resolve actors on the game thread. Publish and copy `ActorDrawData` under `snapshotLock`; it must
  not contain engine pointers. Render-thread engine reads are limited to the camera and renderer
  singleton access documented in `Renderer.hpp`.
- Respect `_GameThread`, `_RenderThread`, and `RT` affinity markers. Keep render caches on the render
  thread and preserve the snapshot queue, reload handshake, and documented lock ordering.
- `kSettings` in `Settings.cpp` owns scalar runtime defaults. Struct initializers are placeholders;
  indexed defaults have separate builders and parsers. Check bindings, reload snapshots, shipped
  `glyph.ini`, and tests when changing a setting. Shipped values can intentionally differ from defaults.
- Console actor overrides last for the game session. Preserve their limits and INI visibility gates;
  console edits do not write `glyph.ini`.

## Build and validation

Run commands from the repository root in PowerShell. The preset workflow requires CMake 3.21+,
Visual Studio 2022, v143 toolset 14.44.35207, and `VCPKG_ROOT` pointing to a vcpkg checkout.
Presets use `x64-windows-static`. Dependency downloads require network access.

| Command | Purpose |
| --- | --- |
| `cmake --preset default` | Configure the local Visual Studio build |
| `cmake --build build --config Release --target glyph -- /m:1` | Build `build/Release/glyph.dll` |
| `cmake --build --preset windows-tests` | Build all configured test executables |
| `ctest --preset windows-release` | Run the local Release tests |
| `.\test.bat` | Configure if needed, build, and run all test suites |
| `.\build.bat` | Format sources, configure, analyze, build the plugin, and generate available docs |
| `.\build.bat --skip-tidy` | Run the full build pipeline without clang-tidy |

`build.bat` formats all source files and does not run tests. Inspect its formatting changes.
`test.bat` can pause on configure or build failure; use the preset commands for unattended checks.
The `compile-db` preset creates a Ninja sidecar for analysis; do not build that sidecar directly.
clang-tidy diagnostics are advisory, but a failing invocation fails `build.bat`.

Add or update tests for non-trivial parsing, math, serialization, state, API, and bug-fix changes.
Use descriptive cases such as `TEST(DeckLayoutTest, UsesTheLimitingDimension)`. New test executables
need CMake registration and inclusion in the applicable presets and test script. Prefer testing
production helpers directly. Some existing tests mirror game-dependent logic; keep those copies
consistent with their cited sources when changing that logic.

Run the checks appropriate to the change and report their results. If runtime or rendering checks
need Skyrim, state what could not be verified. Include screenshots for visible rendering changes.

## Assets and delivery

Do not commit ignored runtime assets, Font Awesome Pro files, generated binaries, or local Skyrim/MO2
paths. `assets/` is ignored; runtime files come from the deployment package.

Review `deploy.bat` and its resolved `GLYPH_MO2_MODS` and `GLYPH_MO2_PROFILE` paths before deployment.
It replaces an MO2 mod directory and updates `modlist.txt`; `purge.bat` removes that installation.
Use these scripts only when deployment or removal is within the requested task.

Keep commits and PRs focused. Use a relevant emoji and imperative commit summary, for example
`✅ Cover color rules`. PR descriptions explain the problem, resulting behavior, tradeoffs, and
validation, with applicable issue links and rendering screenshots.
