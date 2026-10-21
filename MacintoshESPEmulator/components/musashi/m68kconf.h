/* Musashi m68kconf.h - Configured for Macintosh Classic emulation on ESP32 */
/* 68000 only, minimal features for performance */

#ifndef M68KCONF__HEADER
#define M68KCONF__HEADER

#define OPT_OFF             0
#define OPT_ON              1
#define OPT_SPECIFY_HANDLER 2

#ifndef M68K_COMPILE_FOR_MAME
#define M68K_COMPILE_FOR_MAME      OPT_OFF
#endif

#if M68K_COMPILE_FOR_MAME == OPT_OFF

/* 68030 for the LC II ROM; 020 must be on (030 is a superset); 040 off */
#define M68K_EMULATE_010            OPT_OFF
#define M68K_EMULATE_EC020          OPT_OFF
#ifndef M68K_EMULATE_020
#define M68K_EMULATE_020            OPT_ON
#endif
#ifndef M68K_EMULATE_030
#define M68K_EMULATE_030            OPT_ON
#endif
#define M68K_EMULATE_040            OPT_OFF

#define M68K_SEPARATE_READS         OPT_ON
#define M68K_SIMULATE_PD_WRITES     OPT_OFF

/* Forward declarations for Mac callback handlers */
extern int mac_int_ack_handler(int level);
extern void mac_reset_handler(void);
/* A-line trap hook (mac_emulator.c): called on the A-line exception path
 * only (not per instruction) with the trap word and its address. Used by
 * the idle fast-forward (MAC_IDLE_FF) to spot the Process Manager idle
 * loop. */
extern void mac_aline_hook(unsigned int trap_word, unsigned int trap_pc);
#define M68K_ALINE_HOOK(IR, PPC)    mac_aline_hook(IR, PPC)

/* Native A-line trap dispatcher (mac_native.c, plan step 3b). Runs the
 * known trap dispatchers (Classic System 7 RAM dispatcher, LC II ROM
 * $A099B0) as C with Musashi-exact semantics, memory accesses and cycle
 * charges, called on the A-line exception path only. Host-verified
 * byte-identical (RAM dumps + screenshots) against MAC_NATIVE_TRAPDISP=0. */
#ifndef MAC_NATIVE_TRAPDISP
#define MAC_NATIVE_TRAPDISP         1
#endif
#if MAC_NATIVE_TRAPDISP
extern void mac_native_aline(void);
#define M68K_ALINE_POST_HOOK()      mac_native_aline()
#endif

/* Native resource decompressor 'dcmp' 3 (mac_native.c): output-exact, but
 * it runs in zero emulated time, so runs are not cycle-identical to OFF.
 * MAC_NATIVE_DCMP_CHECK=1 (host only) also runs the 68k original on every
 * call and compares. DC_TRAP is the A-line word patched over the dcmp's
 * entry; $A89F is _Unimplemented's own number, never executed on purpose. */
#ifndef MAC_NATIVE_DCMP
#define MAC_NATIVE_DCMP             1
#endif
#ifndef MAC_NATIVE_DCMP_CHECK
#define MAC_NATIVE_DCMP_CHECK       0
#endif
#define DC_TRAP                     0xA89F
#if MAC_NATIVE_DCMP
extern int mac_native_aline_pre(unsigned int ir);
#define M68K_ALINE_PRE_HOOK(IR)     mac_native_aline_pre(IR)
#endif

/* Native ROM loop accelerators (mac_native.c): long-running ROM loops at
 * fixed addresses (LC II RAM test, ROM checksum, heap walk, wait loop;
 * Classic delay loop) run as C when a time slice starts inside them, with
 * Musashi-exact semantics and per-instruction cycle charges, so the slice
 * ends on the same instruction as interpreted. Called once per
 * m68k_execute() (per slice), never per instruction. */
#ifndef MAC_NATIVE_LOOPS
#define MAC_NATIVE_LOOPS            1
#endif
#if MAC_NATIVE_LOOPS
extern int mac_native_slice(void);
#define M68K_SLICE_START_HOOK()     mac_native_slice()
#endif

/* Use autovectored interrupts (matches Mac hardware) */
#define M68K_EMULATE_INT_ACK        OPT_SPECIFY_HANDLER
#define M68K_INT_ACK_CALLBACK(A)    mac_int_ack_handler(A)

#define M68K_EMULATE_BKPT_ACK       OPT_OFF
#define M68K_EMULATE_TRACE          OPT_OFF

/* Enable RESET instruction callback for Mac */
#define M68K_EMULATE_RESET          OPT_SPECIFY_HANDLER
#define M68K_RESET_CALLBACK()       mac_reset_handler()

#define M68K_CMPILD_HAS_CALLBACK    OPT_OFF
#define M68K_CMPILD_CALLBACK(v,r)

#define M68K_RTE_HAS_CALLBACK       OPT_OFF
#define M68K_RTE_CALLBACK()

#define M68K_TAS_HAS_CALLBACK       OPT_OFF
#define M68K_TAS_CALLBACK()

#define M68K_ILLG_HAS_CALLBACK      OPT_OFF
#define M68K_ILLG_CALLBACK(opcode)  0

#define M68K_EMULATE_FC             OPT_OFF
#define M68K_SET_FC_CALLBACK(A)

#define M68K_MONITOR_PC             OPT_OFF
#define M68K_SET_PC_CALLBACK(A)

#define M68K_INSTRUCTION_HOOK       OPT_OFF
#define M68K_INSTRUCTION_CALLBACK(pc)

/* Disable prefetch emulation for speed — saves a branch per instruction fetch.
 * Only obscure self-modifying code depends on prefetch timing;
 * the Mac Classic ROM and System software do not. */
#define M68K_EMULATE_PREFETCH       OPT_OFF

/* Disable address error emulation for speed — saves setjmp per execute call
 * and alignment check per memory access. Mac software is word-aligned. */
#define M68K_EMULATE_ADDRESS_ERROR  OPT_OFF

#define M68K_LOG_ENABLE             OPT_OFF
#define M68K_LOG_1010_1111          OPT_OFF
#define M68K_LOG_FILEHANDLE         stderr

/* Bus-error recovery (m68k_pulse_bus_error / MMU faults): costs a copy of
 * D0-D7/A0-A7 into REG_DA_SAVE on EVERY instruction plus a setjmp per
 * m68k_execute() call. Nothing in this emulator raises a bus error (PMMU is
 * off, unmapped reads return $FF), so it is off for speed. Turn ON only if
 * a device model starts calling m68k_pulse_bus_error(). */
#ifndef M68K_EMULATE_BUS_ERROR
#define M68K_EMULATE_BUS_ERROR      OPT_OFF
#endif

/* Inline RAM fast paths (mac_memory.c owns the limits; a limit of 0 sends
 * every access to the m68k_read/write_memory_* / read_immediate_* callbacks,
 * which set the limit once their own RAM fast path is valid: overlay off for
 * data, mac_post_boot for fetches; never with MAC_DIAG=1). Accesses in the
 * Classic RAM alias range (>= ram_size), the last 3 bytes of RAM, low memory
 * below $0A20 (write guards) and all devices still use the callbacks, so the
 * behaviour is identical; only the call is skipped.
 *   M68K_INLINE_RAM   data reads/writes (m68ki_read/write_*_fc)
 *   M68K_INLINE_FETCH instruction/immediate fetches (m68ki_read_imm_16/32);
 *                     grows every opcode handler a little (flash/I-cache on
 *                     ESP32), so it is a separate A/B switch. */
#ifndef M68K_INLINE_RAM
#define M68K_INLINE_RAM             OPT_OFF  /* A/B 2026-10-21: suspected ESP32 I-cache thrash */
#endif
#ifndef M68K_INLINE_FETCH
#define M68K_INLINE_FETCH           OPT_OFF
#endif
#if M68K_INLINE_RAM || M68K_INLINE_FETCH
extern unsigned char *mac_fast_ram;
extern unsigned int   mac_fast_rd_lim;    /* data reads:  addr < lim          */
extern unsigned int   mac_fast_wr_span;   /* data writes: addr-$A20 < span    */
extern unsigned int   mac_fast_fetch_lim; /* fetches:     addr < lim          */
#endif

/* ESP32 A/B switch (default OFF): dispatch through a 16-bit opcode->index
 * table (128KB, PSRAM) plus small handler/cycle tables in internal DRAM
 * (2048 entries: 8KB fn ptrs + 2KB cycles; 1968 used), instead of the 256KB
 * jump table + 64KB active cycle table, both in PSRAM. Halves the PSRAM
 * bytes touched per dispatch (1 x 2B instead of 4B + 1B in two tables)
 * and shrinks the random-access footprint in the 32KB cache from 320KB to
 * 128KB. Output is identical (verified on host). Costs +128KB PSRAM,
 * 10KB DRAM. Host x86: ~2-4% slower (no PSRAM there). */
#ifndef M68K_OP_INDEX
#define M68K_OP_INDEX               OPT_OFF
#endif

/* Disable PMMU - not needed for 68000 */
#define M68K_EMULATE_PMMU           OPT_OFF

/* ESP32 is 32-bit, no 64-bit acceleration */
#define M68K_USE_64_BIT             OPT_OFF

/* Host-only per-instruction trace (host/Makefile TRACE=1); never set on ESP32 */
#ifdef HOST_TRACE
#undef  M68K_INSTRUCTION_HOOK
#undef  M68K_INSTRUCTION_CALLBACK
#define M68K_INSTRUCTION_HOOK       OPT_SPECIFY_HANDLER
void host_trace_hook(unsigned int pc);
#define M68K_INSTRUCTION_CALLBACK(pc) host_trace_hook(pc)
#endif

#endif /* M68K_COMPILE_FOR_MAME */

#endif /* M68KCONF__HEADER */
