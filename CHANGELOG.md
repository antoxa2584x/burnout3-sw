# Changelog

All notable changes to this project are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[SemVer](https://semver.org/).

## [0.2.5] - 2026-11-01

### Changed
- Faster movie colour conversion (#6): the menu background movie
  (`_FEMain.xmv`) slowed the menus to 7-25 fps on the Switch, with the CPU
  busy decoding it. The YUV-to-RGB step, a third of that time, now does four
  pixels at once (NEON on the Switch, SSE2 on x86) with bit-identical output.

## [0.2.4] - 2026-10-21

### Fixed
- The sun's lens flare shone through bridges and buildings (#10): the
  GPU's visibility test results, which the game dims the flare by, were
  never written. Vulkan and GL now count them with occlusion queries.
- Traffic cars changed colour from frame to frame (#11): cars sharing a
  paletted skin all took the palette of the first one drawn.
- v0.2.1 ran much slower than v0.2 with sys-clk installed (#9): the
  `NFSU2_*_MHZ` clocks were ignored whenever sys-clk was running, leaving
  the CPU at stock 1020 MHz. They are ignored only under Horizon OC now.

## [0.2] - 2026-10-11

### Fixed
- Particles: sparks, boost exhaust flames, light glows (traffic lights,
  street lamps) and the sun lens flare. They are immediate-mode quads with
  world positions in fixed-function mode, which the executor took as
  already on screen (RECOMP_INLINE_FFP=1, set by the port).

## [0.1] - 2026-10-11

First Switch/Linux release on the NFS toolkit fork.

### Added
- The 3D world: races, the car-select garage, motion blur and bloom (the
  scene composite was an immediate-mode quad the executor dropped), fog.
- Vulkan: reflection cube highlights (alpha-only clears), picture quality
  from the NFSU2 port (mipmaps, anisotropic filtering, FXAA, resolution
  scale).
- Driver Details > Settings > Gameplay: Resolution Scale, Anti-Aliasing and
  Anisotropic Filtering rows (Vulkan), saved in `b3x_options.txt`.
- Races keep real time below 60 fps (time-based simulation steps;
  `B3_FRAME_LOCK=1` for the original one step per frame).

### Fixed
- No engine sound: heap buffers whose numbers collided with contiguous
  physical addresses were read from the wrong memory by the APU.
- Profiles saved after racing came back "Unusable": the kernel's key
  exports sat inside the game's own data and were overwritten.

## [0.2.5] - 2026-11-01

The project now runs on the shared xboxrecomp runtime instead of its own fork,
and the title boots and plays through its own code. Needs xboxrecomp
#138-#156 (on its `main`): the vertex-program interpreter, register combiners,
x87 precision control, SSE compare predicates, multi-pad USB, the DPC queue lock
and per-flip frame dumps.

### Added
- Every game mode plays: Race, Time Attack, Road Rage and Crash for one player;
  Race, Road Rage, Double Impact and Team Crush in two-player split-screen; and
  Party Crash.
- Two controllers connected by default (`RECOMP_USB_PADS=2`); pad 2 is XInput
  pad 2 or `p2-` pad-script steps.
- `--headless`, `--record out.mp4` and `--seconds N`: run with no window, and
  encode every presented frame with ffmpeg when the run ends.
- A window by default, titled with the XBE title, fps and draw count.
- `Setup.cmd` / `tools/setup.ps1`: one-click setup from a clean machine. It
  checks and offers to install the tools, fetches the toolkit, unpacks your disc
  image, regenerates, builds and leaves a launcher.
- `regen.sh`: the toolkit pipeline in order. `config/seed_functions.json`:
  function entries only a run can find.
- Hang watchdog and a NaN trap (`BO3_TRAP_NAN`) in `main.c` for debugging.
- Docs: `architecture.md`, `running.md`, and `boot.md`, which records each fix
  between the entry point and a playable race.
- CONTRIBUTING, CONTRIBUTORS, ROADMAP, this changelog, and the MIT LICENSE file.

### Changed
- The build links the xboxrecomp runtime libraries (`../xboxrecomp`) instead of
  in-tree copies.
- Format and engine research notes moved to `docs/research/`.

### Removed
- The in-tree runtime fork (`src/kernel`, `src/d3d`, `src/audio`, `src/apu`,
  `src/input`, `src/nv2a`, `src/platform`) and the hand-written game layer
  (`fe_menu`, `rw_renderer`, `rw_bridge`, the pseudo-3D road, fake physics,
  loaders). The title draws its own menus and races now.
- `src/nv2a`'s captured pushbuffer and font data, which came from the game, and
  its xemu-derived code.
- The old Python tools (`tools/`); the toolkit's versions replace them.
- `CLAUDE.md` from tracking.
- Stale docs describing the fork (`d3d8ltcg_device_context.md`,
  `docs/technical/`, `kernel-imports.md`), and `renderware_layouts.md`, which was
  taken from RenderWare SDK headers.

### Fixed
- The README described a build that did not run (#1, reported by
  [@VincxBlox](https://github.com/VincxBlox)).
