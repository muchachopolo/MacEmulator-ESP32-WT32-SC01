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
#define M68K_EMULATE_020            OPT_ON
#define M68K_EMULATE_030            OPT_ON
#define M68K_EMULATE_040            OPT_OFF

#define M68K_SEPARATE_READS         OPT_ON
#define M68K_SIMULATE_PD_WRITES     OPT_OFF

/* Forward declarations for Mac callback handlers */
extern int mac_int_ack_handler(int level);
extern void mac_reset_handler(void);

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

/* Disable PMMU - not needed for 68000 */
#define M68K_EMULATE_PMMU           OPT_OFF

/* ESP32 is 32-bit, no 64-bit acceleration */
#define M68K_USE_64_BIT             OPT_OFF

#endif /* M68K_COMPILE_FOR_MAME */

#endif /* M68KCONF__HEADER */
