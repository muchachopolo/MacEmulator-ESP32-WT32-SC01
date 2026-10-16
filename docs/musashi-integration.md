# Musashi 68000 CPU Integration

## What is Musashi?

Musashi is a portable Motorola 68000 series CPU emulator written in C. It supports the full 68000 family: 68000, 68010, 68EC020, 68020, 68030, and 68040. It was designed for MAME but works standalone.

**Source**: Copied into `MacintoshESPEmulator/components/musashi/` (not referenced externally).

## Code Generation Architecture

Musashi uses a unique two-stage build:

```
m68k_in.c  ──►  m68kmake (host tool)  ──►  m68kops.c + m68kops.h
 (DSL)              (code generator)         (generated C)
```

- **`m68k_in.c`** is NOT valid C. It's a domain-specific language that defines ~518 opcode primitives with cycle counts for each CPU type, using macros like `M68KMAKE_OP(name, size, ea_mode, ea_reg)`.
- **`m68kmake.c`** is a host-side tool that reads `m68k_in.c` and generates:
  - `m68kops.c` — 1,967 handler functions + opcode dispatch table builder (~37,000 lines, 836KB)
  - `m68kops.h` — Function declarations + extern table references
- The generated files are committed to the repo. Regeneration is only needed if `m68k_in.c` is modified.

### Why code generation?

A direct C implementation would be ~200,000+ lines (65,536 possible opcodes). The DSL lets Musashi define 518 instruction patterns with wildcard fields (register, size, effective address mode), then the generator expands them into all concrete opcodes. This is both more maintainable and produces faster dispatch than a switch statement.

## File Inventory

| File | Role | Compiled? |
|------|------|-----------|
| `m68k.h` | Public API (init, execute, get/set registers) | Header only |
| `m68kconf.h` | Configuration (CPU types, callbacks) | Header only |
| `m68kcpu.h` | Internal CPU state, macros | Header only |
| `m68kcpu.c` | CPU core (execute loop, exception handling) | Yes |
| `m68kops.c` | Generated opcode handlers + dispatch table | Yes |
| `m68kops.h` | Generated handler declarations | Header only |
| `m68kfpu.c` | FPU emulation (included by m68kcpu.c) | Via include |
| `m68kmmu.h` | MMU emulation (included by m68kcpu.c) | Via include |
| `m68kdasm.c` | Disassembler | **No** (excluded to save 256KB) |
| `m68kmake.c` | Code generator (host tool only) | No |
| `m68k_in.c` | Opcode DSL source | No (input to m68kmake) |
| `softfloat/` | IEEE 754 floating point for FPU | Via include |

## Configuration (m68kconf.h)

Current settings (68000 only, minimal features):

```c
#define M68K_EMULATE_010            OPT_OFF
#define M68K_EMULATE_EC020          OPT_OFF
#define M68K_EMULATE_020            OPT_OFF
#define M68K_EMULATE_030            OPT_OFF   // Future: OPT_ON
#define M68K_EMULATE_040            OPT_OFF

#define M68K_EMULATE_INT_ACK        OPT_SPECIFY_HANDLER
#define M68K_INT_ACK_CALLBACK(A)    mac_int_ack_handler(A)

#define M68K_EMULATE_RESET          OPT_SPECIFY_HANDLER
#define M68K_RESET_CALLBACK()       mac_reset_handler()

#define M68K_EMULATE_PREFETCH       OPT_ON    // Better 68000 accuracy
#define M68K_EMULATE_ADDRESS_ERROR  OPT_ON    // 68000 alignment traps
#define M68K_EMULATE_PMMU           OPT_OFF   // No MMU on 68000
#define M68K_USE_64_BIT             OPT_OFF   // ESP32 is 32-bit
```

## Required Callbacks

Musashi requires the host to implement memory access functions:

```c
// Memory reads (must handle full address decode)
unsigned int m68k_read_memory_8(unsigned int address);
unsigned int m68k_read_memory_16(unsigned int address);
unsigned int m68k_read_memory_32(unsigned int address);

// Memory writes
void m68k_write_memory_8(unsigned int address, unsigned int value);
void m68k_write_memory_16(unsigned int address, unsigned int value);
void m68k_write_memory_32(unsigned int address, unsigned int value);

// Disassembler reads (same as memory reads in our case)
unsigned int m68k_read_disassembler_8(unsigned int address);
unsigned int m68k_read_disassembler_16(unsigned int address);
unsigned int m68k_read_disassembler_32(unsigned int address);
```

These are implemented in `mac_memory.c`.

## Large Data Structures

Musashi has two very large BSS arrays:

| Array | Size | Location |
|-------|------|----------|
| `m68ki_instruction_jump_table[0x10000]` | 256KB (65536 function pointers) | PSRAM via `EXT_RAM_BSS_ATTR` |
| `m68ki_cycles[5][0x10000]` | 320KB (5 CPU types * 64K entries) | PSRAM via `EXT_RAM_BSS_ATTR` |

These are initialized at runtime by `m68ki_build_opcode_table()` (called from `m68k_set_cpu_type()`). They MUST be in PSRAM because internal DRAM is only 180KB.

**Performance note**: The jump table is accessed on every instruction dispatch. PSRAM latency (~80ns vs ~10ns for SRAM) adds overhead. At 7.83MHz emulated clock this is acceptable, but may matter for 68030 speeds.

## Warning Suppressions

GCC 15.2.0 (Xtensa cross-compiler) is very strict. Musashi triggers many warnings-as-errors:

- `-Wno-shift-count-overflow` — softfloat 64-bit shifts on 32-bit platform
- `-Wno-overflow` — softfloat constant truncation
- `-Wno-array-bounds` — FPU type-punning (`fx80_to_double`)
- `-Wno-format-overflow` — disasm sprintf (not compiled but referenced)
- `-Wno-implicit-function-declaration` — cross-file callback references
- Plus: unused-variable, sign-compare, maybe-uninitialized, implicit-fallthrough, parentheses

## Upgrading to 68030

To enable 68030 emulation in the future:

1. **m68kconf.h**: Set `M68K_EMULATE_030 OPT_ON` and `M68K_EMULATE_PMMU OPT_ON`
2. **mac_emulator.c**: Change `m68k_set_cpu_type(M68K_CPU_TYPE_68030)`
3. **ROM**: Use a 68030-era ROM (Color-Classic.ROM or LC-II.ROM in `ROM/`)
4. **Performance**: 68030 runs at higher clock speeds — may need to reduce cycles per frame or optimize memory access
5. **No regeneration needed**: `m68kops.c` already contains handlers for all CPU types
6. **Cycle table**: `NUM_CPU_TYPES=5` already allocates slots for 68030 (index 2)
