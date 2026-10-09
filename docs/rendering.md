# Rendering

## The problem

Bloodborne's renderer was written for the PS4's GPU, an AMD GCN part. The game
draws through FromSoftware's graphics layer, GX, which sits on Sony's Gnm
library: GX turns the engine's draws into PM4 command buffers - the packet
format the GPU's command processor reads - and the game ships its shaders as
GCN machine code. A PC has neither a GCN command processor nor a way to run
GCN shaders directly.

An emulator would answer this at the lowest level: decode every packet,
keep the GPU's registers, and reconstruct each draw from them. bbhost started
there and is moving up a level, to where the engine's intent is still visible.
Dark Souls III on PC, built on the same engine, puts Direct3D 11 under the
same GX layer; bbhost does the equivalent with Vulkan.

## How a frame is drawn today

**GX hooks.** bbhost hooks GX's draw, dispatch, resource-creation and state
methods. When the game draws, bbhost records the draw's inputs from the
engine's own objects - its shaders, vertex layout, state descriptions,
textures and constant buffers - and leaves a small token in the command
buffer. When the command processor reaches the token, the draw is built from
those inputs. No packet is decoded and no register file is consulted for a
draw.

**The command processor.** bbhost still walks the game's command buffers, for
what has not moved to tokens: compute dispatches, labels and fences the game
waits on, and some clears and copies recognised from their packets. Compute
dispatches go through an emulated register file, by decision for now: they
are tightly interleaved with the constant engine's memory traffic, and most
of them are fills and copies that never run a shader, which bbhost recognises
and performs directly.

**Recording.** The command processor leaves the Vulkan calls to a thread of
its own, `bb-record`. Each draw goes into a packet, and most of its other
commands - barriers, fills, copies and the like - into the same ordered
stream as typed records (`src/host/recorder.cpp`, `src/host/stream_ops.h`).
`bb-record` replays the stream into command buffers, begins and ends each one
and hands it to the thread that submits, while the command processor goes on
to the next draw. With `BBHOST_STREAM_SUBMIT=0` the command processor begins,
ends and submits the command buffers itself, to compare.

**Shaders.** GCN shaders are converted to SPIR-V when the game creates them
(the few that cannot be, at their first draw), so most pipelines are ready
before they are needed. There are two compilers:

- the *translator* (`src/gcn/translate.cpp`) reproduces the GCN program's
  execution faithfully - one invocation per lane, with the execution mask as
  data - and handles every shader the game uses;
- the *lifter* (`src/gcn/lift.cpp`) decompiles a shader into typed,
  structured SPIR-V, the form a person could read and edit. It is used for
  every pixel shader it accepts, and for vertex shaders that keep their vertex
  formats in the shader (when the driver fast-links pipelines, as NVIDIA's
  does, vertex formats are specialisation constants and the lifts are pixel
  shaders); the rest keep the translation. `tools/lift_verify.py` compares the
  two compilers' output byte for byte.

All 13,394 vertex, pixel and compute shaders in the game's bundles translate
to valid SPIR-V (`gcn2spv --check`). Domain, hull and local shaders run on the
host GPU's own tessellation stages; no geometry or export shader has been seen
in use.

**Textures and memory.** The game's memory is shared with the GPU, as on the
PS4: its GPU-visible parts (about 5 GB) are imported into Vulkan, because
shaders that cannot be fully resolved read memory through a page table. Textures are detiled on the GPU, and bbhost notices when the game
rewrites one by write-protecting its pages. Render targets are Vulkan images
sized from the engine's own views, and can be re-created at any resolution
while the game runs, the way the PC engine's GX device resizes.

**Native passes.** Several parts of the frame no longer run the game's GPU
path at all:

- YEBIS, the post-processing library (bloom, glare, depth of field, tone
  mapping): every stage's resources and constants are built by bbhost.
- Scaleform, the menu renderer: its draws are taken from its hardware
  abstraction layer.
- Depth snapshots, the on-screen overlay and FSR 1 upscaling are bbhost's
  own.
- DLSS, as anti-aliasing at the render size (DLAA), below.

## DLSS

DLSS needs what a temporal upscaler always needs: the scene's colour before
the HUD, its depth, every pixel's motion since the last frame, and a sub-pixel
jitter that moves the scene a little each frame. The game has three of the
four. YEBIS's motion blur already takes the colour, the depth snapshot and a
velocity pass of its own (`7ea47480+d3c8bb21`): it rebuilds each pixel's clip
position from the depth, reprojects it with a clip-to-previous-clip matrix the
game hands it every frame, and blends in the characters' velocity map, which
the scene renderer draws at half size. DLSS runs right after that pass
(`src/host/dlss.cpp`):

1. motion vectors at the render size (`shaders/dlss_mv.comp`): each pixel's
   clip position rebuilt from the depth snapshot with that pass's own
   constants, reprojected by the scene's camera, and the characters' velocity
   map blended over it. The camera is the scene constants every G-buffer draw
   reads - its position (dwords 183, 187, 191) and the camera-relative
   view-projection (200..215), since the scene draws `VP * (world - camera)` -
   copied on the GPU from the first such draw's binding each frame. The
   pass's own clip-to-previous-clip matrix is no good to DLSS: the game makes
   it in single precision from world positions in the thousands, and while
   the camera turned steadily the shift that best matched one frame to the
   next was 4.2 pixels away from it on average, -7 to +5 from one frame to
   the next - a blur forgives that, DLSS laid its history that far off and the
   picture smeared and combed in motion. From the two frames' VP and the
   camera's move between them (nearly equal floats, whose difference is
   exact) the vectors are within a quarter of a pixel of the best match on
   every frame of a turn or a walk. The pass's matrix is the fallback for a
   frame whose camera was not caught. Everything is read on the GPU, from the
   bindings the game's own draws use: those constants come in dynamic
   buffers the GPU copies into place just before the draw, so the CPU's view
   of that memory can be an earlier frame's;
2. DLSS on the scene colour (depth of field already composited into it), the
   depth snapshot and those vectors;
3. its colour copied back over the scene colour, keeping the alpha the blur
   reads (`shaders/dlss_merge.comp`). Motion blur, bloom, the tone map and the
   HUD then run on it as on the game's own.

The jitter is the fourth: a Halton (2, 3) viewport offset on every draw of the
scene into the depth buffer DLSS reads. Full-screen passes keep their
viewport, since they read the jittered buffers pixel for pixel, and so does
the HUD, which draws without a depth test. The game's own AA pass is off while
DLSS runs.

Three more things in the game's data had to be taken care of. The pass's
motion points the other way from where the picture goes - turning, the scene moves
31 pixels a frame to the left where it says 31 to the right; a blur does not
mind, DLSS took its history from the wrong side, and the picture shook while
moving and for a while after. The pass's matrix (the fallback) is noisy even
for a still camera - 2^-11 where the identity has 0, a third of a pixel every
frame - so one within two ulps of the identity in every entry is taken as the
identity; whole, because while the camera eases in after the player stops
some entries are that small and real. And the velocity map has 8 bits a
component - a step is 4.5 pixels at 1280 wide, and rest is 127 where 127.5
would be zero - so a character takes the camera's exact motion wherever the
map agrees with it to within a step, and the map's own only where it really
moved.

NGX itself is loaded at run time: its core is the driver's `_nvngx.dll`, which
exports the API by name, so bbhost has declarations of its own for the calls
and structures it uses and links no NVIDIA code. The core's own entry points
are not always the SDK's wrappers (its `Init_ProjectID` takes no loader entry
points and the version before the common info), and its parameter maps are
MSVC C++ objects whose methods are called through the vtable in MSVC's order,
checked with a value written and read back before anything else.

Upscaling - a scene rendered smaller than the post-processing and the HUD -
is the next step: the engine renders everything at one size today.

## Native or emulated

| Operation | Today |
|---|---|
| Draws | native: built from the engine's objects at a token |
| Tessellated draws | native: host tessellation stages |
| Post-processing (YEBIS) | native resource building; the game's shaders |
| Menus (Scaleform) | native draw building from the HAL |
| Fills and copies without a shader | recognised and performed directly |
| Other compute dispatches | emulated register state, the game's shaders |
| Clears and target copies | partly recognised from packets |
| Labels and fences | emulated, on the command processor |
| Shaders | translated or decompiled to SPIR-V |
| Resource identity | the game's memory addresses, plus write tracking |

## What comes next

- Compute dispatches built from the engine's objects, like the draws, so the
  register file can go.
- Resources identified by the engine's own objects and generations rather than
  by guest address and write tracking.
- Clears and copies from their GX calls rather than from packets, after which
  the PM4 command processor only orders work.
- More shaders decompiled rather than translated, as a step towards shaders as
  editable source.

## Tools

| Tool | What it does |
|---|---|
| `gcn2spv` | translates and validates every shader in the game's bundles |
| `gcndis` | disassembles GCN programs |
| F12 in game | dumps the frame, every render target and the draw list into a folder of its own (`tools/f12_check.py` reads it; `BBHOST_F12_PNG=1` for PNG) |
| `BBHOST_CAPTURE_DRAW` + `drawreplay` | captures one draw with its inputs and replays it outside the game |
| `BBHOST_VK_VALIDATE=1` | runs with the Vulkan validation layer |
| `BBHOST_GPU_PROFILE=1` | per-pass GPU timings |
| `bbhost --stream-selftest[=N]` | checks the command stream headless against a CPU model, N rounds (16 by default), and exits with 0 when it matched; run it with `BBHOST_VK_VALIDATE=1` too |
