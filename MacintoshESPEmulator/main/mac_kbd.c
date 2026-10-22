/*
 * Keyboard input: see mac_kbd.h.
 */
#include "mac_kbd.h"
#include "mac_memory.h"
#include "m68k.h"
#include <string.h>
#include <esp_log.h>

#if MAC_KEYBOARD

static const char *TAG = "kbd";

/* Pending transitions: bit 7 = key up (the ADB convention), low 7 bits =
 * ADB key code. kbd_key() and the consumers run in the emulation loop. */
#define KQ_LEN 32
static uint8_t kq[KQ_LEN];
static int kq_h, kq_n;

void kbd_key(uint8_t adb_code, bool down)
{
    if (kq_n >= KQ_LEN) return;                     /* typing faster than the Mac reads */
    kq[(kq_h + kq_n) % KQ_LEN] = (uint8_t)((adb_code & 0x7F) | (down ? 0 : 0x80));
    kq_n++;
}

static int kq_pop(void)
{
    if (!kq_n) return -1;
    uint8_t k = kq[kq_h];
    kq_h = (kq_h + 1) % KQ_LEN;
    kq_n--;
    return k;
}

static void kq_push_front(uint8_t k)
{
    if (kq_n >= KQ_LEN) return;
    kq_h = (kq_h + KQ_LEN - 1) % KQ_LEN;
    kq[kq_h] = k;
    kq_n++;
}

#if MAC_ROM_LC_II
/* ---- LC II: ADB keyboard on the Egret/Cuda HLE ---- */
static uint8_t kb_addr = 2, kb_handler = 1;
static uint8_t kb_in[2];
static int kb_in_n;

bool kbd_adb_pending(void) { return kq_n > 0; }

void kbd_adb_take_r0(uint8_t out[2])
{
    kb_in_n = 0;
    for (int i = 0; i < 2; i++) {
        int k = kq_pop();
        out[i] = k < 0 ? 0xFF : (uint8_t)k;
        if (k >= 0) kb_in[kb_in_n++] = (uint8_t)k;
    }
}

void kbd_adb_untake(void)
{
    while (kb_in_n > 0) kq_push_front(kb_in[--kb_in_n]);
}

void kbd_adb_reset(void)
{
    kb_addr = 2;
    kb_handler = 1;
}

uint8_t kbd_adb_addr(void) { return kb_addr; }
uint8_t kbd_adb_handler(void) { return kb_handler; }

void kbd_adb_listen_r3(uint8_t a0, uint8_t a1)
{
    if (a1 == 0x00 || a1 == 0xFD || a1 == 0xFE)
        kb_addr = a0 & 0x0F;
    else if (a1 == 0x01 || a1 == 0x02 || a1 == 0x03)
        kb_handler = a1;
}

void kbd_frame(void) { }

#else
/* ---- Classic: mirror of the ROM's ADB keyboard handler ($403AC0) ---- */

#define KEYMAP      0x0174      /* 16 bytes, bit (k & 7) of byte k >> 3 */
#define KEY_LAST    0x0184      /* byte: last key code */
#define KEY_TIME    0x0186      /* long: Ticks of the last key-down */
#define KEY_REPTIME 0x018A      /* long: Ticks of the last (auto-)repeat */
#define KEY_AUTOMSG 0x0216      /* long: message GetOSEvent repeats, 0 = none */
#define TICKS       0x016A
#define WMGR_PORT   0x09DE

/* ADB raw code -> virtual key code (the KMAP of the Apple keyboards): only
 * Control, the arrows and the right-hand modifiers differ. */
static uint8_t vkey(uint8_t raw)
{
    switch (raw) {
    case 0x36: return 0x3B;     /* Control */
    case 0x3B: return 0x7B;     /* left */
    case 0x3C: return 0x7C;     /* right */
    case 0x3D: return 0x7D;     /* down */
    case 0x3E: return 0x7E;     /* up */
    case 0x7B: return 0x38;     /* right Shift */
    case 0x7C: return 0x3A;     /* right Option */
    case 0x7D: return 0x3B;     /* right Control */
    default:   return raw;
    }
}

/* US layout (KCHR 0), virtual key code -> {plain, shifted}; 0 = no char. */
static const char us_chars[0x80][2] = {
    [0x00] = {'a','A'}, [0x01] = {'s','S'}, [0x02] = {'d','D'}, [0x03] = {'f','F'},
    [0x04] = {'h','H'}, [0x05] = {'g','G'}, [0x06] = {'z','Z'}, [0x07] = {'x','X'},
    [0x08] = {'c','C'}, [0x09] = {'v','V'}, [0x0B] = {'b','B'}, [0x0C] = {'q','Q'},
    [0x0D] = {'w','W'}, [0x0E] = {'e','E'}, [0x0F] = {'r','R'}, [0x10] = {'y','Y'},
    [0x11] = {'t','T'}, [0x12] = {'1','!'}, [0x13] = {'2','@'}, [0x14] = {'3','#'},
    [0x15] = {'4','$'}, [0x16] = {'6','^'}, [0x17] = {'5','%'}, [0x18] = {'=','+'},
    [0x19] = {'9','('}, [0x1A] = {'7','&'}, [0x1B] = {'-','_'}, [0x1C] = {'8','*'},
    [0x1D] = {'0',')'}, [0x1E] = {']','}'}, [0x1F] = {'o','O'}, [0x20] = {'u','U'},
    [0x21] = {'[','{'}, [0x22] = {'i','I'}, [0x23] = {'p','P'}, [0x24] = {0x0D,0x0D},
    [0x25] = {'l','L'}, [0x26] = {'j','J'}, [0x27] = {'\'','"'}, [0x28] = {'k','K'},
    [0x29] = {';',':'}, [0x2A] = {'\\','|'}, [0x2B] = {',','<'}, [0x2C] = {'/','?'},
    [0x2D] = {'n','N'}, [0x2E] = {'m','M'}, [0x2F] = {'.','>'}, [0x30] = {0x09,0x09},
    [0x31] = {' ',' '}, [0x32] = {'`','~'}, [0x33] = {0x08,0x08}, [0x35] = {0x1B,0x1B},
    [0x41] = {'.','.'}, [0x43] = {'*','*'}, [0x45] = {'+','+'}, [0x47] = {0x1B,0x1B},
    [0x4B] = {'/','/'}, [0x4C] = {0x03,0x03}, [0x4E] = {'-','-'}, [0x51] = {'=','='},
    [0x52] = {'0','0'}, [0x53] = {'1','1'}, [0x54] = {'2','2'}, [0x55] = {'3','3'},
    [0x56] = {'4','4'}, [0x57] = {'5','5'}, [0x58] = {'6','6'}, [0x59] = {'7','7'},
    [0x5B] = {'8','8'}, [0x5C] = {'9','9'},
    [0x60] = {0x10,0x10}, [0x61] = {0x10,0x10}, [0x62] = {0x10,0x10}, [0x63] = {0x10,0x10},
    [0x64] = {0x10,0x10}, [0x65] = {0x10,0x10}, [0x67] = {0x10,0x10}, [0x6D] = {0x10,0x10},
    [0x6F] = {0x10,0x10}, [0x76] = {0x10,0x10}, [0x78] = {0x10,0x10}, [0x7A] = {0x10,0x10},
    [0x72] = {0x05,0x05}, [0x73] = {0x01,0x01}, [0x74] = {0x0B,0x0B}, [0x75] = {0x7F,0x7F},
    [0x77] = {0x04,0x04}, [0x79] = {0x0C,0x0C},
    [0x7B] = {0x1C,0x1C}, [0x7C] = {0x1D,0x1D}, [0x7D] = {0x1F,0x1F}, [0x7E] = {0x1E,0x1E},
};

static inline bool km_get(const uint8_t *r, uint8_t vk) { return (r[KEYMAP + (vk >> 3)] >> (vk & 7)) & 1; }

static inline void wr32(uint8_t *r, uint32_t a, uint32_t v)
{
    r[a] = (uint8_t)(v >> 24); r[a + 1] = (uint8_t)(v >> 16);
    r[a + 2] = (uint8_t)(v >> 8); r[a + 3] = (uint8_t)v;
}

static inline uint32_t rd32(const uint8_t *r, uint32_t a)
{
    return ((uint32_t)r[a] << 24) | ((uint32_t)r[a + 1] << 16) | ((uint32_t)r[a + 2] << 8) | r[a + 3];
}

/* mac_emulator.c: ROM PostEvent mirror (0 posted, 1 masked, -1 no element) */
int mac_post_event_msg(uint8_t *r, int what, uint32_t message);

static void classic_key(uint8_t *r, uint8_t k)
{
    bool up = (k & 0x80) != 0;
    uint8_t vk = vkey(k & 0x7F);

    wr32(r, KEY_AUTOMSG, 0);                        /* any transition stops auto-key */
    if (up) r[KEYMAP + (vk >> 3)] &= (uint8_t)~(1u << (vk & 7));
    else    r[KEYMAP + (vk >> 3)] |= (uint8_t)(1u << (vk & 7));
    r[KEY_LAST] = vk;

    uint8_t ch;
    bool shift = km_get(r, 0x38), caps = km_get(r, 0x39), ctrl = km_get(r, 0x3B);
    ch = (uint8_t)us_chars[vk][shift ? 1 : 0];
    if (!ch) return;                                /* modifier / no char: KeyMap only */
    if (caps && ch >= 'a' && ch <= 'z') ch = (uint8_t)(ch - 'a' + 'A');
    if (ctrl && ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'))) ch &= 0x1F;

    uint32_t msg = (2u << 16) | ((uint32_t)vk << 8) | ch;   /* ADB address 2 */
    if (!up) {
        uint32_t t = rd32(r, TICKS);
        wr32(r, KEY_TIME, t);
        wr32(r, KEY_REPTIME, t);
        wr32(r, KEY_AUTOMSG, msg);
    }
    int rc = mac_post_event_msg(r, up ? 4 : 3, msg);
    static int log_n;
    if (log_n < 40) {
        log_n++;
        ESP_LOGI(TAG, "key %s vk %02X char %02X -> %d", up ? "up" : "down", vk, ch, rc);
    }
}

/* Post the queued keys at a point where the 68k could take an interrupt
 * (SR mask 0), like the Classic click FIFO in mac_emulator.c. */
void kbd_frame(void)
{
    static int wait;
    uint8_t *r = mac_mem_get_ram();
    if (!kq_n || !r || !mac_post_boot) return;
    uint32_t wmgr = rd32(r, WMGR_PORT);
    if (wmgr == 0 || wmgr == 0xFFFFFFFF) {          /* no event system yet: drop */
        kq_n = 0;
        return;
    }
    if ((m68k_get_reg(NULL, M68K_REG_SR) & 0x0700) && ++wait < 30) return;
    wait = 0;
    int k;
    while ((k = kq_pop()) >= 0) classic_key(r, (uint8_t)k);
}

bool kbd_adb_pending(void) { return false; }
void kbd_adb_take_r0(uint8_t out[2]) { out[0] = out[1] = 0xFF; }
void kbd_adb_untake(void) { }
void kbd_adb_reset(void) { }
uint8_t kbd_adb_addr(void) { return 0xFF; }
uint8_t kbd_adb_handler(void) { return 1; }
void kbd_adb_listen_r3(uint8_t a0, uint8_t a1) { (void)a0; (void)a1; }
#endif /* MAC_ROM_LC_II */

#else  /* !MAC_KEYBOARD */
void kbd_key(uint8_t adb_code, bool down) { (void)adb_code; (void)down; }
void kbd_frame(void) { }
#endif
