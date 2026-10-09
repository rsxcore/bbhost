# Decompilation

bbhost runs the game's own code, so nothing has to be decompiled for the game
to work. Decompilation is how parts of the engine become changeable: a game
function rewritten as C++ in `src/decomp/` runs in the original's place, and
can then be read, fixed and extended like any other source - which is where
room for mods and fixes comes from that byte patches and hooks cannot give.

The rule is that a rewritten function must behave exactly like the
original. Where the result can be compared, a compare mode checks it against
the game's own version while the game runs.

## Where it stands

The 1.09 executable has about 169,000 functions. In a default run today:

| | Functions |
|---|---|
| Rewritten as source, on the decomp list (`src/decomp/`) | 10 |
| Replaced by bbhost code outside the list (written before the list existed) | 4, and the YEBIS resource builder bypassed |
| Hooked at the entry or at call sites (GX methods, the resource registry, live resolution, settings, key prompts, menus) | 117 |
| **Taken over in all** | **131** |
| With any code byte changed (including byte patches and redirected calls) | 154; 236 at 60 fps |
| The game's own code | everything else |

The TLS rewrite also changes one instruction at each of 17,127 sites in about
8,100 functions, to make the game's thread-local accesses work on the host.

So the decompilation has barely begun - deliberately: functions are taken over
when there is a reason to change them, not for their own sake.

### In the list

| Function | Address | What it does | Checked |
|---|---|---|---|
| `IsEventFlag` | `0x17cfc00` | reads one event flag | no calls observed (reached only through a table, if at all) |
| `SetEventFlag` | `0x17cfcc0` | writes one event flag | 81,687 calls compared, 0 differences |
| `GetEventFlagValue` | `0x17cfd80` | reads a run of flags as a number | 83,467 calls compared, 0 differences |
| `SetEventFlagValue` | `0x17d0060` | writes a run of flags | 692 calls compared, 0 differences |
| GX table reclaim (`sub_2aaa860`) | `0x2aaa860` | frees the GX layer's blocks of draw resource tables once the GPU is done with them | same walk as the game's, plus a wait when a pool runs empty (which used to fail an allocation and black out a frame) |
| flush wait (`sub_15d7030`) | `0x15d7030` | the render thread's wait for the GPU to finish a flush | the same condition, yielding the core instead of spinning on it |
| the game's `memcpy` (`sub_2a1a1b0`) | `0x2a1a1b0` | the engine's own memory copy, 1,440 call sites | checked by what it copies |
| parallel resource copy (`sub_23bde30`) | `0x23bde30` | the copy of streamed resource data across the engine's worker pool | copies on the calling thread; removes a ~1 s wait per area tour |
| effect ribbon tail, facing the eye (`sub_2cce7b0`) | `0x2cce7b0` | the last one to three points of an effect ribbon as vertices, the strip turned to the camera | 400,000 random strips against the game's own code (`tests/sfx_ribbon_test.cpp`), 21 calls compared in a world session: 0 differences |
| effect ribbon tail, along normals (`sub_2cceec0`) | `0x2cceec0` | the same for a ribbon laid along its points' normals | 400,000 random strips, 426 calls compared in a world session: 0 differences |

The two ribbon writers are rewritten for Windows. The game's versions keep
their arguments in the 128 bytes below the stack pointer - the red zone, which
the SysV convention the game is built for promises no signal will touch.
Windows has no red zone: an exception writes its frame there. The texture
write watch takes an exception at the first write to a page it watches, and
the ribbons' vertices often land in such pages, so on Windows the game read
its own pointers back as zero and crashed at `0x2cce9b5` (the crash the
community's "Intel 12th Gen+ SFX workaround" patch avoids by not drawing
those effects). The rewrites keep nothing below the stack pointer. Linux
skips the red zone when it delivers a signal, so only Windows crashed.

The four event-flag functions are every read and write the game makes through
its flag store - event scripts, Lua, talk scripts, the online session. With
them as source, every flag change is visible: `BBHOST_EVENT_FLAG_LOG=1` logs
each one, and plugins get a callback for each.

### Replaced outside the list

| Function | Address | Where |
|---|---|---|
| `SprjFlipper::Update`, the frame-time manager | `0x2434770` | `src/engine/frame_rate.cpp` |
| the frame loop's pooled-object borrow | `0x1467f60` | `src/engine/frame_pool.cpp` |
| YEBIS's per-stage resource builder | `0x15f7840` | `src/engine/yebis_bind.cpp` |
| the GX command-arena acquire | `0x2ad3b80` | `src/hle/runtime.cpp` |
| `DL_PANIC` | `0x24b55b0` | `src/hle/runtime.cpp` |

### The game's file formats

The same standard - reproduce the game's own bytes before changing anything -
applies to the data formats bbhost edits:

| Format | Where | Checked |
|---|---|---|
| Talk scripts (EzState `.esd`) | `src/engine/esd.h` | all 271 scripts parse and write back byte for byte (`tests/esd_test.cpp`) |
| Map layouts (MSB) | the randomizer, `src/engine/dream_mirror_layout.cpp` | all 2,690 layouts rebuild byte for byte (`tests/randomizer_files_test.cpp`) |
| AI script bundles (`luabnd`, `.luainfo`, `.luagnl`) | the randomizer | all 18 bundles rebuild byte for byte |
| Event scripts (EMEVD) | `src/engine/maiden_events.cpp` | checked by SHA-256 in and out, differences only where intended |
| Param definitions | `src/engine/paramdef.cpp` | the game's own definitions; all 62 tables match their rows |

## How a function takes the original's place

`src/decomp/decomp.h` keeps the list. Each entry gives the function's entry
address, the instruction bytes expected there, and the replacement.

- At load, once the executable's SHA-256 is the 1.09 build's, the first 14 or
  more bytes of the function become a jump to the replacement. A function
  whose bytes differ from what it was written against is refused and the
  original stays.
- Those bytes go into a trampoline that runs them and jumps back, so the
  original remains callable - for a compare run, and for the replacement to
  fall back on.
- A **leaf** is entered directly from the game's code, on the game's thread
  and stack. It touches only the game's memory and its own globals - no host
  thread-locals, locks, logging or allocation - and is compiled without the
  stack protector. It costs what the original did.
- A **hosted** function is entered through the same thunk as the system
  library calls and may do anything host code does.

Switches for a run:

| | |
|---|---|
| `BBHOST_DECOMP=0` | the game's code throughout |
| `BBHOST_DECOMP_OFF=name,...` | these functions stay the game's |
| `BBHOST_DECOMP_COMPARE=1` | functions with a compare mode run it instead |

## How one is checked

A compare mode runs the original and checks the replacement against it while
the game plays. A function that only reads runs both versions and compares
their results; one that writes predicts what the original will write, lets it
run, and checks the memory afterwards. A function becomes the default once a
long world session compares with zero differences, and a session with the
replacement in place performs within noise of one without.

Exactness is measured against the machine code, not a decompiler's reading of
it: shift counts masked to 5 bits, 32-bit arithmetic that wraps, exactly the
argument bits the original tests.

## Adding one

1. Find the function in a disassembler and name it from evidence: the
   binary's strings, Dark Souls III's RTTI (the same engine, with class names),
   the PS3 build's debug symbols, or a plain descriptive name.
2. Read its disassembly as well as a decompile, and list its callers.
3. Write it in `src/decomp/<area>.cpp` with its entry bytes and a compare mode.
4. Register it in `src/hle/runtime.cpp` before `decomp_install`.
5. Run a compare session (zero differences) and a performance session (within
   noise).

## Next

- **Player data**: adding and removing items, levelling up, paying echoes -
  item and progression mods, NG+ rules.
- **The lock-on camera** (`0x183ac60`): camera behaviour beyond what its params
  reach.
- **Damage and stamina**: the functions that apply attack params and attribute
  scaling, for balance changes params cannot express.
- **The event system's condition checks**: event-script logic as source, and
  new conditions for mods.
