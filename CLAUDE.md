# Burnout 3: Takedown (Xbox) static recompilation — notes for Claude

Xbox NTSC-U Burnout 3: Takedown (title 0x4541005B, XDK 5849, Criterion
RenderWare, D3D8LTCG), lifted to C with xboxrecomp and built for Linux and
Nintendo Switch (libnx NRO). Started 2026-10-08 from the NFS Carbon port
(`..\nfsc-xbox`, itself from NFSU1/NFSU2; read `..\nfsu2-xbox\CLAUDE.md` for
everything the runtime does on Horizon). Repo: git@github.com:antoxa2584x/burnout3-sw.git, branch `switch` (fork of sp00nznet/burnout3; `main` follows upstream, remote `upstream`). core.fileMode is off: files on /mnt/c look executable.

Upstream has a Windows-only Burnout 3 port on the stock toolkit
(github.com/sp00nznet/burnout3, software NV2A, races at 2-4 fps); a read-only
clone is in `/root/b3x/ref/burnout3` (its docs/boot.md lists what the title
needed). This port uses the NFS toolkit fork instead (GPU renderers, Switch).

## Rules

- Never commit game data (disc, `default.xbe`, `switch_sd/`) or generated C
  (`gen/`). Ask before committing or pushing.
- The Switch build reads the **unpacked** disc at `sdmc:/switch/b3x/game/`.
- Renderer work targets Vulkan (nv2a_vk), as in the NFS ports.
- Linux test hygiene as NFSU2: one run at a time, kill by PID (`run/pid`)
  with **-9** (b3_recomp ignores SIGTERM; live.sh now kill -9s the previous
  pid). Never `pkill -f` a pattern that matches your own shell. A stale
  instance shares run.log, pad.cmd and the FRAME_TRACE flag with the new one.

## Layout and builds

| Where | What |
|---|---|
| `/root/b3x/` (WSL) | `game/` extracted disc, `gen/` lifted C, `build-linux/` (GL), `build-vk-linux/` (Vulkan, lavapipe), `run/`, `ref/burnout3` |
| `/root/b3x/run.sh` | run `SECS` with `PAD=` script, dumps every `DUMPN` frames to `run/d/` |
| `/root/b3x/live.sh` | start with a live pad file `run/pad.cmd` (`VK=1 BIN=...build-vk-linux/b3_recomp` for Vulkan, `EXTRA="VAR=1 ..."`) |
| `/root/b3x/step.sh "<pad line>" N` | press, wait for N new dumps, newest frame -> scratchpad last.png |
| `/root/b3x/nav.sh globe\|car\|race` | from boot to that screen with the saved profile (after live.sh) |
| `/root/b3x/peek.py PID ADDR '*PTR+OFF'` | read guest memory of a running build |
| `xboxrecomp/` | vendored toolkit = nfsc-xbox's + the changes below |

- Disc: `xboxrecomp\Burnout 3 - Takedown (USA).iso`, `python3 -m tools.xiso unpack`.
- Regenerate: `tools/regen.sh` (`LIFT_ONLY=1` after manual-override edits;
  `TRACE_FUNCS=<json list>` adds `[TRACE]` entry logging).
- Linux: `cmake --build /root/b3x/build-linux -j12`; Vulkan:
  `/root/b3x/build-vk-linux` (`-DNFSU2_VULKAN=ON`).
- Saves land in `/root/b3x/game/UDATA/4541005b/` (profile "BURNOUT" made there).

## Findings

- **Library hooks remapped from Carbon** (bodies byte-identical apart from
  addresses): memmove 0x2442A0, _ftol2 0x244DF0, DSOUND DSP ack 0x2F7EC2,
  AC97 reset 0x2FE736 (lock 0x2F3F70/0x2F3F92, re-arm 0x2FE540, table
  0x300448). XPP is the same code as Carbon's (seeds from upstream).
- **D3D8LTCG hooks** (same device layout as NFS, own conventions): device
  0x35D6A0 via 0x35FB48; BlockOnTime 0x351770 (fence in eax, flags on the
  stack, ret 4); vblank handler 0x356DA0 (block in **eax**, = device+0x1C28);
  PGRAPH handler 0x357000 (ecx); ISR 0x356AD0; GetDisplayFieldStatus
  0x34E0A0 (VBlankCount = device+0x1DE8 = block+0x1C0). `B3_D3D_TRACE=1`
  logs vblank/PGRAPH/slow fence waits.
- **Upstream lifter commits ported** into the vendored toolkit: 64796df +
  5d507e7 (x87 precision control, PC=24), a006905 (x87 unordered), 206b733
  (all SSE compares; `cmpltss` was UNIMPL), the comiss half of 894ceef (its
  lahf half was already done differently here).
- **Intro movie never advanced (fixed, toolkit):** `in al, dx` was a no-op.
  D3D's vblank handler reads the video field from port 0x80C0 bit 5 into
  device+0x1DF8; Burnout runs an interlaced mode, so GetDisplayFieldStatus
  reported field 1 forever and the XMV player (GetNextFrame 0x2CCB1C, called
  from 0x1C93D0) never started its clock (only field 2 starts it). The
  lifter now emits `xbox_PortIn/xbox_PortOut` (test_port_io.py); the runtime
  (kernel_bridge.c) flips 0x80C0 bit 5 every vblank.
- **Viewport constants (toolkit, from upstream 0e9cff9):** SET_VIEWPORT_OFFSET
  / _SCALE now also land in vertex constants c[59]/c[58].
- **Surface bytes per pixel (toolkit, from upstream 777c1cd):** pitch / clip
  width gave 5-12 "bytes a pixel" for Burnout's narrow menu clips; the GL
  back end then rebuilt (and cleared) the back buffer at the wrong width.
  surface_bpp() now reads SET_SURFACE_FORMAT.
- **Off-screen batch cull drops Burnout draws:** with it on, the car-select
  thumbnails vanished. main.c sets `RECOMP_CULL=0` (cull_batch's corner test
  needs a look before it is turned back on).
- **3D world missing (fixed, toolkit pb_exec):** Burnout renders every 3D
  scene into an off-screen 640x480 target (0x82B24000 at car select), then
  downsamples it (320x240 0x82C50000, 160x120 0x82C78000/0x82C84000 blur)
  and draws it onto the back buffer with one quad (scene + bloom, two
  texcoord sets). Those quads are immediate mode with the position in
  SET_VERTEX_DATA2F_M slot 0 (0x1880/0x1884), which pb_exec ignored: only
  SET_VERTEX3F/4F and DATA4F slot 0 completed a vertex. Now (as xemu) the
  last component of attribute 0 through any SET_VERTEX_DATA* completes it,
  and each immediate vertex carries every attribute written in the batch
  (float4 from attr_const), not just texcoord0. Race, garage car and motion
  blur appear on GL and Vulkan.
- **Garage black, fog (fixed, toolkit gl_psh):** the shaders read the fog
  factor as the raw oFog.x. Burnout's vertex programs write a fog distance
  (garage: oFog = min(z, 0)) and its final combiners lerp to the fog colour
  by it, so the garage came out as pure fog colour (black). Now, as xemu,
  the fog unit runs on it for vertex-program draws: SET_FOG_MODE
  linear/exp/exp2 (+abs) with SET_FOG_PARAMS, and factor 1 when
  SET_FOG_ENABLE is off (uniform u_fogp, nv2a_fog_params). Fixed-function
  draws keep the raw read (no fog generation there yet).
- **Car paint washed pink on Vulkan (fixed, toolkit nv2a_vk):** the paint
  adds SPECULAR_FOG_FACTOR0 x (alpha of the reflection cube, thresholded),
  and the cube faces (6 x 128x128 at 0x82CB6000 + n*0x10000) are cleared
  with CLEAR_SURFACE 0x83: alpha alone. Vulkan skipped clears with a
  partial colour mask, so the face alpha stayed 1 and the whole body got
  the highlight. vk_clear now draws a masked triangle for those
  (clear_pipe). GL was right (glClear honours the mask) but has no cube
  maps, so its paint shows no reflection.
- **No engine sound (fixed, toolkit):** the engine loops (16 voices,
  v04E-v05D, from pveh/*/CARn.hwd) are loaded into heap memory at low VAs
  (0x01A6xxxx). MmGetPhysicalAddress passed those through unchanged, the
  same numbers as contiguous-arena physical addresses, so the APU read the
  arena window (zeros). Such ambiguous low VAs now get bus address
  VA | 0x04000000 (XBOX_PHYS_LOWVA_TAG); apu_phys, dma_resolve and OHCI
  bus_to_va map it back. `RECOMP_APU_VOICES=1` lists every voice (format,
  bins, buffer, peak) every 5 s.
- **Saves "Unusable" after playing (fixed, toolkit):** profiles carry a
  dword-sum checksum at +0xE810 and an XCalculateSignature (non-roamable)
  over 0xE9EC bytes, keyed from XboxHDKey/XboxSignatureKey (zeros). The
  kernel data exports lived at fixed 0x00740000, inside Burnout's .data
  (0x3B2360-0x76B93C); racing overwrote the keys, so later saves were signed
  with garbage. Kernel data, fake PRCB/TLS and the TLS block now sit in a
  runtime block placed above the image (0x00779000 here, xbox_RtDataPlace).
  Broken saves keep valid data: re-signed copies are in switch_sd/repaired.
  `RECOMP_NO_MIRRORS=1` maps no RAM mirrors (as on Switch).
- **Slow-motion races below 60 fps (fixed, src/recomp_manual.c):** the
  main loop (0x00016ADC) runs N fixed 1/60 s steps a frame (tick counter
  0x4A1EB4, clock constant 1/60 at 0x49C120); N = sub_001B58E0(min 1, max
  4). Its state (0x4D5338) has a mode the game sets via 0x4D5390: 1 counts
  real TSC ticks, 0 (races) always answers 1 -- the Xbox holds 60 fps. At
  48 fps the race clock ran at 0.80x; on Switch 150 mph looked like 40. The
  override runs mode 0 as mode 1: game time 1.00x at any frame rate.
  B3_FRAME_LOCK=1 restores one step per frame. /root/b3x/clockscan.py PID
  finds float clocks in guest RAM and prints their rate vs wall time.
- **Picture options in Driver Details > Settings > Gameplay (2026-10-11,
  Vulkan build):** Resolution Scale 1x/1.5x/2x/2.5x, Anti-Aliasing (FXAA)
  Off/On, Anisotropic Filtering Off/2x..16x, as NFSU2's Video rows. Renderer
  side ported from nfsu2-xbox 48b1b27 + 77b9a83 (nv2a_vk_scale_pct and
  rescale_surfaces, mips, aniso, FXAA, square pixels; gl_vsh u_snap_y).
  Screen: sub_00084E20 builds rows into the screen object 0x55AC44 (+6 row
  count, 0x14-byte rows from +8: label pointer, value-id list, count, 4);
  sub_000EF170 (wrapped) gets a 5-row copy for widget 0x550ACC; values via
  sub_000EF600; input sub_00084F90 (wrapped): pad 0x4AE724 + 0x188 *
  [0x4AED45], +0x10/+0x1C left/right, +0x28/+0x34 up/down; redraw
  sub_000F0CB0. Text: Data/Globalus.bin string table at [[0x4D532C]+0xC]
  (0xF91 UTF-16 entries), replaced by a longer copy with the new texts.
  Saved in b3x_options.txt (sdmc:/switch/b3x/ or the working directory),
  loaded at boot (main.c b3x_options_load).
- **Particles missing: sparks, boost flames, light glows, sun flare (fixed,
  toolkit pb_exec):** they are immediate-mode quads (inl 1) with world
  positions in fixed-function mode. batch_is_ffp() took every inline or
  immediate batch as pre-transformed (the NFS ports' 2D quads), so they drew
  at x ~5000. `RECOMP_INLINE_FFP=1` (set in main.c) runs them through the
  composite matrix like arrays. FRAME_TRACE shows them as `xform 0 ... inl 1`
  with world-sized v0 when broken.
- **Race training video (TrD_*.xmv):** not skippable in the game. The
  screen at 0x000151D0 opens it; frames are stepped by sub_00014C80 ->
  sub_001C93D0; movie object at screen+0x1D0, +0x61 = end of file (set when
  GetNextFrame returns 2-3), +0x63 = new frame. sub_000154A0 waits on
  screen+0x231 (= movie+0x61). A skip would set that flag; not done.
- **Debug tools added (toolkit):** `RECOMP_FRAME_TRACE=<flag file>` (pb_exec:
  one line per draw/clear/other-subchannel method of the next frame, with
  the CPU-transformed first vertices; touch the file again for another
  frame); `RECOMP_VK_DUMP_SURF=<va>[,<va>...]` (nv2a_vk: dump those surfaces
  with each RECOMP_GL_DUMP frame, as _s.bmp, _s1.bmp, ...;
  `RECOMP_VK_DUMP_RAW=1` adds .raw BGRA files with alpha); FRAME_TRACE draw
  lines also show the first vertex's diffuse, attribute 3's layout, the
  final combiner, alpha ICW0, factor0 and blend; `RECOMP_GL_TRACE=1` on Vulkan also logs each change
  of render target (pitch/bpp/clip).
- **Black bars top and bottom (fixed, present crop):** Burnout draws 448
  lines (rows 16-463) of its 480-line buffers everywhere after the intros,
  as on PS2; TV overscan hid the rest. main.c sets RECOMP_PRESENT_CROP_Y=16
  (toolkit: GL and Vulkan present, blit and FXAA) so the picture fills the
  screen height.
- **Sun flare through bridges and buildings (#10, fixed, toolkit):** the
  race draws its sun sprite (one ~20x27 quad into the 640x480 scene
  target, depth tested) inside a D3D visibility test (CLEAR_REPORT_VALUE,
  SET_ZPASS_PIXEL_COUNT_ENABLE 1/0, GET_REPORT) and scales the lens flare
  by the pixel count. Nothing wrote the reports, so every test stayed
  incomplete (status 0xFFFFFFFF, slots from device+0x7D4; D3D code
  0x0034DF80-0x0034E09D) and the flare always shone. pb_exec now hands
  CLEAR/GET to the back end (Nv2aBackend.report); Vulkan and GL run each
  counted draw in an occlusion query and write count + status 0 a frame or
  two later (nv2a_pb_write_report), render scale taken out. GET_REPORT's
  param is (phys & 0x0EFFFFFF) | 0x01000000, so report_va picks whichever
  of the two addresses D3D marked incomplete. Forced to 0 the glare goes.
- **Traffic colours swapping (#11, fixed, toolkit):** traffic skins are
  512x256 P8 textures (0x0B) shared by several cars with a palette each.
  Both texture caches keyed by address/format/size and checked once a
  frame, so every car got the first-drawn car's palette that frame. The
  palette VA is now part of the key for 0x0B.
- "You have accepted an invitation to play" box after Start on every boot:
  not investigated (A dismisses it).

## Status (2026-10-10)

- Linux GL and Vulkan: EA/Criterion/RenderWare intros, title, profile create,
  save and load, menus, training videos; races render the full 3D world
  with motion blur and drive (35-38 fps GL, ~25 fps Vulkan on lavapipe);
  car select shows the lit garage and the car with its reflections.
  Engine sound, saving after a race and race speed below 60 fps fixed
  2026-10-11.
- Switch: `b3x.nro` (GL) and `b3x-vulkan.nro` built and staged in
  `switch_sd/switch/b3x/` with the disc (2026-10-08); not run on Eden or
  hardware yet. Icon cropped from the title screen; loader logo still NFSU2's.
