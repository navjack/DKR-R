# macOS port notes — DKR-R (Diddy Kong Racing Recompiled)

**Status: 11 of 12 pipeline stages complete.** The matching decomp ELF builds and verifies
byte-exact. The one remaining failure is the static-recompilation step, which rejects the
entrypoint. Not yet a built app.

## Correction to an earlier assessment

An earlier pass concluded the preparation stage was "Windows-only" because `Build-macOS.sh` names a
`Prepare-DKR-Runtime-macOS.sh` that is absent from the repo root. **Wrong** — it lives in
`scripts/`, alongside a full macOS path (`Prepare-Game-Source-macOS.sh`,
`apply-dependency-patches.sh`, `Package-macOS-Build-Kit.sh`). Only the top-level `.cmd` wrappers
are Windows-bound.

## ROM

Requires **US 1.0 / v77**, sha1 `0cb115d8716dbbc2922fda38e533b9fe63bb9670` after byte-order
normalisation. In this set that is **`Diddy Kong Racing (USA) (En,Fr).z64`** — *not*
`Diddy Kong Racing (U) [!].z64`, which is a different dump
(sha1 `6d96743d46f8c0cd0edb0ec5600b003c89b93755`) and is rejected.

## Running it unattended

The script resolves the ROM from a launcher config JSON, then a GUI picker, then a stdin prompt.
The config is checked first, so create it and the whole run is non-interactive:

```bash
mkdir -p runtime/config
cat > runtime/config/rom-source.json <<'JSON'
{ "path": "/path/to/roms/Diddy Kong Racing (USA) (En,Fr).z64" }
JSON
```

## Required environment on macOS

Three host issues must be worked around. All are environment-level; nothing under `extern/` was
edited (that tree is governed by the checksummed patch pipeline in `patches/manifest.json`).

```bash
export PATH="$PWD/.macos-tools:/opt/homebrew/opt/make/libexec/gnubin:$PATH"
export LIBRARY_PATH="$(brew --prefix pcre2)/lib:$LIBRARY_PATH"
./scripts/Prepare-DKR-Runtime-macOS.sh --skip-deps --jobs 8
```

**1. GNU Make 4.x is required.** The decomp's `tools/Makefile` creates its object directories with
GNU Make's shell-assignment operator:

```make
DUMMY != mkdir -p $(sort $(dir $(dkr_assets_tool_OBJ_FILES)))
```

`!=` needs GNU Make 4.0+; macOS ships **3.81** as `/usr/bin/make`, where that line is silently a
no-op and the compile fails with `unable to open output file … No such file or directory`. Install
with `brew install make` (provides `gmake`); the prepare script already prefers `gmake` when found.

**2. pcre2 does not link.** `tools/Makefile:26` reads
`LDFLAGS += -I/opt/homebrew/lib -lpcre2-8` — `-I` is an *include* path flag where `-L` is meant, so
`-lpcre2-8` never resolves and `dkr_assets_tool` fails to link. `LIBRARY_PATH` works around it.

**3. `readelf` does not exist on macOS.** The entry-resolution stage requires `nm` and `readelf`,
searching `mips-linux-gnu-*`, `mips64-linux-gnu-*`, `llvm-*`, then bare. macOS ships no `readelf` at
all, and Homebrew's LLVM is keg-only so `llvm-readelf` is not on `PATH` — the stage then fails with
no message. Fixed with a two-symlink shim rather than prepending all of Homebrew LLVM (which would
swap the entire toolchain):

```bash
mkdir -p .macos-tools
ln -sf /opt/homebrew/opt/llvm/bin/llvm-readelf .macos-tools/llvm-readelf
ln -sf /opt/homebrew/opt/llvm/bin/llvm-nm      .macos-tools/llvm-nm
```

## Watch out: build the tools with the `all` target

`tools/Makefile` has a `recomp` target (line 82) that downloads the IDO 5.3 compiler for macOS into
`tools/ido-recomp/macos/`, and it is only reachable via `all`. Building a single target such as
`gmake -C tools dkr_assets_tool` silently skips IDO, and the decomp build then dies with
`tools/ido-recomp/macos/cc: No such file or directory`. Run plain `gmake -C tools`.

## What works

Stages 1-11 all pass:

1. Pinned decomp checkout at `06cf09ba` (+ submodules)
2. ROM located, byte order normalised, SHA-1 verified
3. MIPS binutils built from source (`mips64-elf-*`, GNU Binutils 2.46, supports `elf32-bigmips`)
4. splat split the ROM (11 MB, 89.46% into defined segments)
5. `dkr_assets_tool` built; assets extracted
6. **Matching decomp ELF built and verified byte-exact:**

```
CRC 1: 0x53D440E7  Calculated: 0x53D440E7  (Good)
CRC 2: 0x7519B011  Calculated: 0x7519B011  (Good)
Verify: OK
```

7. Entry function resolved, dependencies fetched, pinned patches applied
8. N64Recomp and RSPRecomp built

The reusable MIPS binutils at `extern/dkr-decomp/tools/binutils/` are worth knowing about — they
satisfy other decomps' `mips-elf`/`mips64-elf` needs without MacPorts or `sudo`.

## The entrypoint blocker — SOLVED (a skipped patch, not a missing fix)

`N64Recomp` failed the translation stage with one line and nothing else:

```
Could not find entrypoint function
```

### The underlying defect (upstream N64Recomp)

`N64Recomp/src/elf.cpp:65` carried upstream's own admission of the bug:

```cpp
// TODO this never fires, the check is broken due to signedness
if (elf_config.has_entrypoint && value == elf_config.entrypoint_address && type == ELFIO::STT_FUNC)
```

`entrypoint_address` is **`int32_t`** (`include/recompiler/context.h:140`); `value` is
**`ELFIO::Elf64_Addr`** (unsigned 64-bit). Every N64 vram is >= `0x80000000`, i.e. negative as a
signed 32-bit int, so it sign-extends to `0xFFFFFFFF8.......` and never equals the symbol's
`0x000000008.......`. The check is dead for **every** N64 address.

A second path at `elf.cpp:118` matches on ROM offset instead:

```cpp
if (elf_config.has_entrypoint && rom_address == 0x1000 && type == ELFIO::STT_FUNC)
```

`0x1000` is the conventional N64 boot offset, so on a normal layout this quietly covers for the
broken check — which is why the bug survives upstream unnoticed. DKR's layout defeats it:

| Section | ROM offset | Flags |
|---|---|---|
| `.header` | `0x1000` | WA (the 64-byte ROM header, not code) |
| `.boot` | `0x1040` | WA |
| `.main` | **`0x2400`** | WA**X** — the actual code |

With `.header` occupying `0x1000`, no `STT_FUNC` can have `rom_address == 0x1000`, the fallback
never fires, and the dead comparison is exposed.

### The actual root cause (this repo)

**DKR-R had already diagnosed and fixed this.** The repo ships
`patches/n64recomp/0001-fix-high-vram-entrypoint-comparison.patch`, registered in
`patches/manifest.json`, which rewrites the comparison exactly as needed.

It never applied. `scripts/Apply-Dependency-Patches.sh:22` gated every dependency on:

```python
if not (repo / ".git").is_dir():
    print(f"[SKIP] {dependency['name']}: checkout not present at {repo}")
```

**A submodule's `.git` is a file, not a directory** — here a 34-byte
`gitdir: ../.git/modules/N64Recomp` pointer. `is_dir()` is therefore False for every submodule
dependency, and the prepare log shows the consequence:

```
[SKIP] N64Recomp: checkout not present at .../extern/n64-modern-runtime/N64Recomp
```

N64ModernRuntime (a plain clone, `.git` a real directory) got all 13 of its patches; N64Recomp — the
only dependency that is a submodule — silently got none, on every run and every platform.

**Fix** (`scripts/Apply-Dependency-Patches.sh`): test `.exists()` instead of `.is_dir()` so both
layouts are detected. The script's own "partially applied or context-shifted" branch then does
`git checkout -- .` and re-applies cleanly, so no manual repair of `extern/` is needed.

With that, the pinned patch applies and translation succeeds against the resolver's own entrypoint
choice (`entrypoint = 0x80065D40`, `mainproc`):

```
Found entrypoint, original name: mainproc
Function count: 3826
```

> Editing `elf.cpp` by hand also makes translation work, and that is what unblocked this first —
> but it is the wrong fix: the patch pipeline is the mechanism that is broken, and a hand-edit is
> reverted by any `--force` prepare. Fix the script, not the dependency.

Ruled out along the way (each verified, not assumed): the entrypoint address (both `mainproc` and
the `0x80000400` boot symbol), the symbol type (`FUNC GLOBAL`, correct), the section flags (`.main`
carries `SHF_EXECINSTR`, so `elf.cpp:341` does not skip it), `.mdebug` (absent, and correctly
configured as such), and `use_absolute_symbols` — note the `entrypoint` symbol at `0x80000400` is
`SHN_ABS`, and `elf.cpp:46` registers ABS functions without ever setting `found_entrypoint_func`,
so enabling that option cannot help.

## RT64 is opt-in

`Prepare-DKR-Runtime-macOS.sh` skips the renderer unless `--build-renderer` is passed:

```
[INFO] RT64 checkout skipped. Add --build-renderer when renderer integration is ready.
```

Without it the run still exits 0 — it builds only the CPU-side compile probe
(`DKR_RUNTIME_BUILD_RT64=OFF`) — and `Build-macOS.sh` then refuses with
`RT64 is missing. Prepare the pinned renderer dependency first.`

Ignore the `nextBoundary` line in `runtime-recomp/resolved-runtime-dependencies.json` ("Register DKR
with librecomp, implement SDL audio/input callbacks..."). It is a **hardcoded string literal** in the
prepare script's JSON emitter, not a computed status; the repo already implements all of it
(`game_main.cpp`, `rt64_renderer.cpp`, `f3ddkr_rt64.cpp`, `runtime_input.cpp`, `runtime_ui.cpp`, and
`runtime_metal_layer.mm`).

Two macOS fixes are handled by the prepare script itself, so do **not** apply them by hand:
symlinking `metal-cpp` from plume's contrib into `rt64/src/contrib/`, and adding the missing
`<stdlib.h>` to hlslpp's `scalar.h` for `labs()` — the same AppleClang bug patched manually in the
other ports in this batch.

## Build (complete — DKR-R.app runs)

```bash
cd <project-root>
export PATH="$PWD/.macos-tools:/opt/homebrew/opt/make/libexec/gnubin:$PATH"
export LIBRARY_PATH="$(brew --prefix pcre2)/lib:$LIBRARY_PATH"
./scripts/Prepare-DKR-Runtime-macOS.sh --skip-deps --build-renderer --jobs 8
./Build-macOS.sh
./scripts/Vendor-Dylibs-macOS.sh dist/DKR-R-1.0.0-macOS-arm64/DKR-R.app
```

`--build-renderer` is **required**; without it the run still exits 0 but builds only the CPU probe.

Result: 18/18 CTest tests pass, the Controller Pak round-trip self-test passes, and the build reports
`Graphics backend: RT64 Metal (native Apple path)`.

The prepare script's caveat — *"It does not yet promise a playable executable"* — is outdated in
practice. The app boots, validates the ROM, initialises RT64 on Metal, opens audio at 48 kHz and
22.05 kHz, runs the recompiled entrypoint and renders (`completed-f3ddkr-tasks=348` in 12 s), then
**shuts down cleanly** — notably without the plume/Metal teardown over-release that affects the other
rt64-based apps in this batch.

`Build-macOS.sh` scans the staged package and **fails the build** if it finds game data, so the ROM
is never bundled. Pass it at runtime:

```bash
DKR-R.app/Contents/MacOS/DKR-R "/path/to/Diddy Kong Racing (USA) (En,Fr).z64" [config-dir] [timeout-seconds]
```

The third argument is a watchdog that exits after N seconds — useful for scripted smoke tests.

## Bundling: the upstream package is not self-contained

`Build-macOS.sh` signs the app but leaves it dependent on Homebrew:

- the binary's only `LC_RPATH` is `/opt/homebrew/lib`
- `libSDL2-2.0.0.dylib` is still loaded from `/opt/homebrew/opt/sdl2-compat/...`
- there is no `Contents/Frameworks` at all

`scripts/Vendor-Dylibs-macOS.sh` (added here) fixes it. Three things that make this less obvious
than it looks, each of which was a real bug earlier in this batch:

1. **dyld searches `LC_RPATH` entries in order.** An `@rpath` load command is only as good as that
   list, so a clean-looking `otool -L` proves nothing — the Homebrew rpath must actually be deleted.
   Verify with `otool -l <bin> | grep -A2 LC_RPATH`.
2. **sdl2-compat `dlopen`s `libSDL3.dylib`** by name, searching `@loader_path` first. It is not a
   link-time dependency, so dependency walkers miss it entirely and it must be vendored by hand.
3. **Resolve the SDL2 path from the binary's own load command**, not from `brew --prefix sdl2` —
   that prefix reports the sdl2-compat keg while the recorded install name is
   `/opt/homebrew/opt/sdl2/...`, and `install_name_tool -change` only rewrites an exact match.

Everything else the binary links is a system framework (AppKit, Metal, QuartzCore, CoreGraphics,
IOKit, libc++), so SDL2 + SDL3 are the only two dylibs to vendor.

Verified after bundling: sole `LC_RPATH` is `@executable_path/../Frameworks`, zero Homebrew
references, `codesign --verify --deep --strict` reports *valid on disk* and *satisfies its Designated
Requirement*, and the copy installed at `/Applications/DKR-R.app` boots and renders from `cd /`.

Note `assets/` is staged into `Contents/MacOS`, not `Contents/Resources`. That is unusual — codesign
rejects non-Mach-O files in `MacOS` in some layouts — but it signs and verifies cleanly here, so it
is left alone.

## Launcher HiDPI fix (not macOS-specific)

On a Retina display the launcher menu drew into the **top-left quadrant** at half scale, and the
sidebar text was sheared off on the left ("DKR-R" rendered as "IKR-R").

The window is created with `SDL_WINDOW_ALLOW_HIGHDPI` (`runtime_platform.cpp:221`), so the SDL
renderer's output surface is in **pixels** while ImGui lays out and emits vertices in **points**.
`run_startup_screen()` presented without ever calling `SDL_RenderSetScale`:

```cpp
ImGui::Render();
SDL_SetRenderDrawColor(renderer, 6, 9, 14, 255);
SDL_RenderClear(renderer);
ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData());
```

Both symptoms share one cause. `ImGui_ImplSDLRenderer2_RenderDrawData` multiplies its clip
rectangles by `FramebufferScale` before calling `SDL_RenderSetClipRect`, so the clip rects were
already in pixel space while the vertices were still in point space — the sidebar text was not
mispositioned, it was being scissored by a rectangle computed at twice the coordinates.

**Fix** (`runtime-recomp/src/game/runtime_ui.cpp`), matching Dear ImGui's own
`example_sdl2_sdlrenderer2`:

```cpp
const ImVec2 framebuffer_scale = ImGui::GetIO().DisplayFramebufferScale;
SDL_RenderSetScale(renderer, framebuffer_scale.x, framebuffer_scale.y);
```

This is a **general** DKR-R bug, not a macOS one — it reproduces on any HiDPI Windows or Linux
display. It survives upstream because the CI has no HiDPI machine.

Note the RT64 in-game path is unaffected: it has its own presentation, and
`runtime_metal_layer.mm:15` correctly sets `metalLayer.contentsScale = window.backingScaleFactor`.

Unrelated and left alone: the FPS overlay's resolution readout (`runtime_ui.cpp`) reports
`SDL_GetWindowSize`, i.e. points rather than pixels.

## Status: COMPLETE

Installed at `/Applications/DKR-R.app`.
