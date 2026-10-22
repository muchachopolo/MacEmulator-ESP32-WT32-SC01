# CPU core: Musashi integration

[Musashi](https://github.com/kstenerud/Musashi) (Karl Stenerud, v4.60) is a
portable 680x0 emulator in C. A patched copy lives in
`MacintoshESPEmulator/components/musashi/`; keep the licence headers of
Musashi, `m68kmmu.h` (MAME team) and `softfloat/` (John R. Hauser).
Switch list with defaults: [`CLAUDE.md`](../CLAUDE.md#build-switches).

## Files

| File | Role |
|---|---|
| `m68k.h`, `m68kcpu.h`, `m68kcpu.c` | API, CPU state, execute loop, exceptions |
| `m68kconf.h` | Configuration and the emulator's hooks and switches |
| `m68k_in.c` → `m68kmake` → `m68kops.c/.h` | Opcode DSL and its generator; the generated files are committed (regenerate after editing `m68k_in.c`) |
| `m68kmmu.h` | PMMU instructions (stub mode, below) |
| `m68kfpu.c`, `softfloat/` | FPU (compiled in, not used by these machines) |
| `mac_native.c` | Native replacements for hot Mac OS routines |
| `hot_iram.lf` | Linker fragment: hot handlers into IRAM |
| `m68kdasm.c` | Disassembler, **not compiled** (saves DRAM) |

### Code generation

`m68k_in.c` describes each instruction pattern once; `m68kmake` expands them
into ~2000 handlers and the table builder:

```bash
cd MacintoshESPEmulator/components/musashi
gcc -o m68kmake m68kmake.c
./m68kmake . m68k_in.c
```

The output does not depend on `m68kconf.h`. `m68k_in.c` puts
`EXT_RAM_BSS_ATTR` on the two big tables, so every regeneration places them
in PSRAM: `m68ki_instruction_jump_table[0x10000]` (256 KB of handler
pointers) and `m68ki_cycles[][0x10000]` (cycle counts per CPU type).

## Configuration

- **CPU type** is set at run time: `M68K_CPU_TYPE_68000` for the Classic,
  `M68K_CPU_TYPE_68030` under `MAC_ROM_LC_II`. `M68K_EMULATE_020/030` are ON
  (030 needs 020); 010/EC020/040 are OFF. The opcode generator emits handlers
  for every type, so no regeneration is needed when switching.
- Interrupts are autovectored through `mac_int_ack_handler()`; the RESET
  instruction calls `mac_reset_handler()`.
- **Off for speed:** prefetch emulation, address-error emulation,
  `M68K_EMULATE_BUS_ERROR` (nothing raises a bus error: unmapped reads return
  $FF), trace, function codes, PC monitor.
- **`M68K_SEPARATE_READS` ON:** instruction fetches use their own lean
  callbacks (`m68k_read_immediate_*`, in IRAM) that skip the data path's
  intercepts once `mac_post_boot` is set (frame 2000); during boot they fall
  back to the full callbacks, which some boot workarounds hook.
- Host only: `HOST_TRACE` turns on the per-instruction hook for the harness
  trace and profiler.

## 68030 support and fixes

- **`CPU_TYPE_IS_010_LESS` fix:** it evaluated to a constant 1 with 020
  emulation enabled, so 020/030 indexed addressing and exception frames were
  decoded the 68000 way. This was the real cause of the early "68020 mode
  breaks the boot" failure.
- **PMMU stub** (`m68kmmu.h`, `M68K_EMULATE_PMMU` OFF): PMOVE to/from TC, SRP,
  CRP, TT0, TT1 and MMUSR, PLOAD and PFLUSH(A) are accepted with correct
  effective-address lengths; PTEST does a software table walk. There is **no
  per-access translation**: the LC II ROM's MMU tables map the same addresses
  as plain 24-bit masking (verified), so translation would only cost time.
- Cache instructions (e.g. `$F4AC` at the LC II reset entry) are accepted.
- **FPU divide fix** (`softfloat.c`, `floatx80_div`): 68881 "unnormal" operands
  (exponent set, integer bit clear) are normalised first, as the FPU does. The
  division assumes normalised significands; with an unnormal divisor its
  correction loops ran ~2^60 times, seconds per FDIV (seen in a 1987 game).
  Normal operands give bit-identical results.

## Memory callbacks

`main/mac_memory.c` implements `m68k_read_memory_8/16/32`,
`m68k_write_memory_8/16/32` and the immediate-fetch variants. RAM, ROM and
VRAM have fast paths; everything else goes through `mem_read_byte` /
`mem_write_byte` ([memory map](memory-map.md)).

## Speed switches

Results are board measurements at LC II Finder idle (ESP32 exec cycles per
300 frames, lower is better). On the ESP32 the 32 KB flash/PSRAM cache is the
bottleneck, so code size and placement matter more than instruction count.

| Switch | Default | Effect |
|---|---|---|
| `MUSASHI_HOT_IRAM` (CMake) | ON | `m68k_execute` + the 128 most-executed handlers (~94% of executed instructions) in IRAM: **−49%** cycles, Finder 246 s → 172 s |
| `MAC_MEM_CALLBACK_IRAM` | 1 | Memory callbacks in IRAM: −9% |
| `M68K_INLINE_FETCH` | OFF | Inlining fetches grows every handler; OFF is −8% on the ESP32 |
| `M68K_INLINE_RAM` | OFF | Inlining RAM data accesses: faster on x86, ~16% slower on the ESP32 (I-cache) |
| `M68K_OP_INDEX` | OFF | 16-bit opcode index (handler/cycle tables in DRAM): only −3%, costs 128 KB PSRAM + 10 KB DRAM |

IRAM use with everything enabled is about 77.5 KB, with ~53 KB free. If the
link overflows IRAM, trim `hot_iram.lf` from the bottom.

## Native speed-ups (`mac_native.c`)

Principle: replace only the hottest pieces with C, guided by the host
profiler, and verify every replacement against the interpreter on the host
(RAM dumps and screenshots at fixed frames). Each has its own switch in
`m68kconf.h`; host counters print as `H native:` lines.

| Switch | What | Hook | Verification |
|---|---|---|---|
| `MAC_NATIVE_TRAPDISP` | A-line trap dispatcher: the Classic's System 7.5 RAM dispatcher (re-checked with a 218-byte compare on every trap) and the LC II ROM dispatcher at $A099B0, executed as C with Musashi's exact memory accesses, flags and cycle charges | A-line exception path (`m68ki_exception_1010`) | Byte-identical |
| `MAC_NATIVE_DCMP` | Resource decompressor 'dcmp' 3 | A-line word patched over the dcmp entry | Output-exact (`MAC_NATIVE_DCMP_CHECK=1` runs both on the host); takes no emulated time, so boot gets shorter |
| `MAC_NATIVE_LOOPS` | Long ROM loops entered at time-slice start | `M68K_SLICE_START_HOOK` (once per slice) | Byte-identical |

The A-line hook also feeds the idle fast-forward (`M68K_ALINE_HOOK` →
`mac_aline_hook`). Nothing is added to the per-instruction path.

The A-line **pre-hook** (`mac_native_aline_pre`) also lets the emulator run a
Toolbox call inside the Mac at a safe point (an event trap of the Finder, SR
mask 0): it pushes the routine's parameters and a return address pointing back
at the trap, then jumps through the trap table. `MAC_QUIT_REPAINT` uses it to
call `PaintBehind(WindowList, GrayRgn)` and `DrawMenuBar` after a full-screen
game quits; `MAC_DRIVE2` uses it to mount the second disk.

Host result: −81% (LC II) and −75% (Classic) instructions to reach the Finder.
Board (Classic, custom disk): Finder at ~63 s instead of ~105 s.

## Timing

Musashi charges cycles from its per-CPU tables. The 68030 table averages about
6.2 cycles per instruction against 12.8 for the 68000, so the LC II gets half
the CPU cycles per frame (`MAC_V8_CYCLE_SCALE` = 2), see
[architecture-overview](architecture-overview.md#frame-loop-mac_emu_run).

## Warning suppressions

Xtensa GCC 15.2 is strict; `components/musashi/CMakeLists.txt` disables the
warnings Musashi and SoftFloat trigger (shift-count-overflow, overflow,
array-bounds, implicit-fallthrough, sign-compare, ...). The host build
compiles Musashi with `-w`.
