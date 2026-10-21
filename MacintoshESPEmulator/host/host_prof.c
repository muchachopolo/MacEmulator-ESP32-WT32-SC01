/* Host-only instruction profiler (make TRACE=1, --profile).
 *
 * Called from host_trace_hook() once per executed 68k instruction, before
 * the instruction runs. It never calls m68k_read_memory_*: opcodes come
 * straight from the RAM array (mac_mem_get_ram) or the embedded ROM blob,
 * so the emulation sees no extra bus accesses.
 *
 * Attribution:
 *  - Instructions (the proxy for host cost) and Musashi cycles. The cycles
 *    of an instruction are the drop in m68ki_remaining_cycles between this
 *    hook and the next one (exception processing included); the first
 *    instruction of each m68k_execute() slice gets no cycles.
 *  - A-line traps: an $Axxx opcode pushes {trap, return PC = PC+2, SP}. The
 *    entry pops when the PC is back at the return address with SP at or
 *    above the saved SP, or as soon as SP rises above the saved SP (trap
 *    returned elsewhere: autopop, longjmp-style unwinds, stack switches).
 *    Every instruction counts "inclusive" for each distinct trap on the
 *    stack (above the innermost interrupt) and "self" for the innermost.
 *  - Interrupts: the PC reaching an autovector target (VBR+$64..$7C) pushes
 *    an IRQ frame that pops when SP rises above the entry SP (RTE). Traps
 *    called inside an interrupt are charged to that interrupt context, not
 *    to the trap it interrupted.
 *  - Code regions: 4 KB pages of ROM (by ROM file offset) or RAM, plus an
 *    exact per-PC count for ROM and RAM. */
#ifdef HOST_TRACE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "mac_memory.h"
#include "m68k.h"

#if MAC_ROM_LC_II
#if MAC_V8_CUDA
extern const uint8_t prof_rom[]     asm("_binary_Color_Classic_ROM_start");
extern const uint8_t prof_rom_end[] asm("_binary_Color_Classic_ROM_end");
#else
extern const uint8_t prof_rom[]     asm("_binary_LC_II_ROM_start");
extern const uint8_t prof_rom_end[] asm("_binary_LC_II_ROM_end");
#endif
#else
extern const uint8_t prof_rom[]     asm("_binary_Classic_ROM_start");
extern const uint8_t prof_rom_end[] asm("_binary_Classic_ROM_end");
#endif

extern int m68ki_remaining_cycles;
extern long host_frame_count(void);

long prof_from = -1, prof_to = -1;      /* frame window; from<0 = off */
const char *prof_out = NULL;            /* also write the report here */
int  prof_enabled;                      /* set by main when --profile* given */
int  prof_top = 25;                     /* rows per table (--profile-top) */

/* ---------------- trap names (Inside Macintosh / Traps.a numbering) ---- */
struct trapname { uint16_t w; const char *n; };
static const struct trapname os_names[] = {   /* $A0xx, flags bits 8-10 masked */
    {0x00,"Open"},{0x01,"Close"},{0x02,"Read"},{0x03,"Write"},{0x04,"Control"},
    {0x05,"Status"},{0x06,"KillIO"},{0x07,"GetVolInfo"},{0x08,"Create"},{0x09,"Delete"},
    {0x0A,"OpenRF"},{0x0B,"Rename"},{0x0C,"GetFileInfo"},{0x0D,"SetFileInfo"},
    {0x0E,"UnmountVol"},{0x0F,"MountVol"},{0x10,"Allocate"},{0x11,"GetEOF"},
    {0x12,"SetEOF"},{0x13,"FlushVol"},{0x14,"GetVol"},{0x15,"SetVol"},{0x16,"InitQueue"},
    {0x17,"Eject"},{0x18,"GetFPos"},{0x19,"InitZone"},{0x1A,"GetZone"},{0x1B,"SetZone"},
    {0x1C,"FreeMem"},{0x1D,"MaxMem"},{0x1E,"NewPtr"},{0x1F,"DisposePtr"},
    {0x20,"SetPtrSize"},{0x21,"GetPtrSize"},{0x22,"NewHandle"},{0x23,"DisposeHandle"},
    {0x24,"SetHandleSize"},{0x25,"GetHandleSize"},{0x26,"HandleZone"},
    {0x27,"ReallocHandle"},{0x28,"RecoverHandle"},{0x29,"HLock"},{0x2A,"HUnlock"},
    {0x2B,"EmptyHandle"},{0x2C,"InitApplZone"},{0x2D,"SetApplLimit"},{0x2E,"BlockMove"},
    {0x2F,"PostEvent"},{0x30,"OSEventAvail"},{0x31,"GetOSEvent"},{0x32,"FlushEvents"},
    {0x33,"VInstall"},{0x34,"VRemove"},{0x35,"OffLine"},{0x36,"MoreMasters"},
    {0x37,"ReadParam"},{0x38,"WriteParam"},{0x39,"ReadDateTime"},{0x3A,"SetDateTime"},
    {0x3B,"Delay"},{0x3C,"CmpString"},{0x3D,"DrvrInstall"},{0x3E,"DrvrRemove"},
    {0x3F,"InitUtil"},{0x40,"ResrvMem"},{0x41,"SetFilLock"},{0x42,"RstFilLock"},
    {0x43,"SetFilType"},{0x44,"SetFPos"},{0x45,"FlushFile"},{0x46,"GetTrapAddress"},
    {0x47,"SetTrapAddress"},{0x48,"PtrZone"},{0x49,"HPurge"},{0x4A,"HNoPurge"},
    {0x4B,"SetGrowZone"},{0x4C,"CompactMem"},{0x4D,"PurgeMem"},{0x4E,"AddDrive"},
    {0x4F,"RDrvrInstall"},{0x50,"RelString"},{0x51,"ReadXPRam"},{0x52,"WriteXPRam"},
    {0x54,"UprString"},{0x55,"StripAddress"},{0x56,"LowerText"},{0x57,"SetAppBase"},
    {0x58,"InsTime"},{0x59,"RmvTime"},{0x5A,"PrimeTime"},{0x5B,"PowerOff"},
    {0x5C,"MemoryDispatch"},{0x5D,"SwapMMUMode"},{0x5E,"NMInstall"},{0x5F,"NMRemove"},
    {0x60,"HFSDispatch"},{0x61,"MaxBlock"},{0x62,"PurgeSpace"},{0x63,"MaxApplZone"},
    {0x64,"MoveHHi"},{0x65,"StackSpace"},{0x66,"NewEmptyHandle"},{0x67,"HSetRBit"},
    {0x68,"HClrRBit"},{0x69,"HGetState"},{0x6A,"HSetState"},{0x6C,"InitFS"},
    {0x6D,"InitEvents"},{0x6E,"SlotManager"},{0x6F,"SlotVInstall"},{0x70,"SlotVRemove"},
    {0x71,"AttachVBL"},{0x72,"DoVBLTask"},{0x75,"SIntInstall"},{0x76,"SIntRemove"},
    {0x77,"CountADBs"},{0x78,"GetIndADB"},{0x79,"GetADBInfo"},{0x7A,"SetADBInfo"},
    {0x7B,"ADBReInit"},{0x7C,"ADBOp"},{0x7D,"GetDefaultStartup"},{0x7E,"SetDefaultStartup"},
    {0x7F,"InternalWait"},{0x80,"GetVideoDefault"},{0x81,"SetVideoDefault"},
    {0x82,"DTInstall"},{0x83,"SetOSDefault"},{0x84,"GetOSDefault"},{0x8F,"DeferUserFn"},
    {0x90,"SysEnvirons"},{0x91,"Translate24To32"},{0x92,"EgretDispatch"},
    {0x98,"HWPriv"},{0xDD,"PPC"},{0xAC,"FSMDispatch"},{0xAD,"Gestalt"},
};
static const struct trapname tb_names[] = {   /* $A8xx-$ABxx, autopop bit masked */
    {0x800,"SoundDispatch"},{0x80C,"RGetResource"},{0x80D,"Count1Resources"},
    {0x80E,"Get1IxResource"},{0x80F,"Get1IxType"},{0x810,"Unique1ID"},
    {0x815,"SCSIDispatch"},{0x816,"Pack8"},{0x817,"CopyMask"},{0x81A,"HOpenResFile"},
    {0x81F,"Get1Resource"},{0x820,"Get1NamedResource"},{0x822,"ResourceDispatch"},
    {0x823,"AliasDispatch"},{0x82A,"ComponentDispatch"},{0x82B,"Pack9"},{0x82C,"Pack10"},
    {0x82D,"Pack11"},{0x82E,"Pack12"},{0x82F,"Pack13"},{0x830,"Pack14"},{0x831,"Pack15"},
    {0x833,"ScrnBitMap"},{0x835,"FontMetrics"},{0x837,"MeasureText"},{0x838,"CalcMask"},
    {0x839,"SeedFill"},{0x83A,"ZoomWindow"},{0x83B,"TrackBox"},{0x83E,"PinRect"},
    {0x850,"InitCursor"},{0x851,"SetCursor"},{0x852,"HideCursor"},{0x853,"ShowCursor"},
    {0x854,"FontDispatch"},{0x855,"ShieldCursor"},{0x856,"ObscureCursor"},
    {0x858,"BitAnd"},{0x85D,"BitTst"},{0x85E,"BitSet"},{0x85F,"BitClr"},
    {0x860,"WaitNextEvent"},{0x861,"Random"},{0x862,"ForeColor"},{0x863,"BackColor"},
    {0x865,"GetPixel"},{0x867,"LongMul"},{0x868,"FixMul"},{0x869,"FixRatio"},
    {0x86A,"HiWord"},{0x86B,"LoWord"},{0x86C,"FixRound"},{0x86D,"InitPort"},
    {0x86E,"InitGraf"},{0x86F,"OpenPort"},{0x870,"LocalToGlobal"},{0x871,"GlobalToLocal"},
    {0x872,"GrafDevice"},{0x873,"SetPort"},{0x874,"GetPort"},{0x875,"SetPortBits"},
    {0x876,"PortSize"},{0x877,"MovePortTo"},{0x878,"SetOrigin"},{0x879,"SetClip"},
    {0x87A,"GetClip"},{0x87B,"ClipRect"},{0x87C,"BackPat"},{0x87D,"ClosePort"},
    {0x87E,"AddPt"},{0x87F,"SubPt"},{0x880,"SetPt"},{0x881,"EqualPt"},{0x882,"StdText"},
    {0x883,"DrawChar"},{0x884,"DrawString"},{0x885,"DrawText"},{0x886,"TextWidth"},
    {0x887,"TextFont"},{0x888,"TextFace"},{0x889,"TextMode"},{0x88A,"TextSize"},
    {0x88B,"GetFontInfo"},{0x88C,"StringWidth"},{0x88D,"CharWidth"},{0x88E,"SpaceExtra"},
    {0x88F,"OSDispatch"},{0x890,"StdLine"},{0x891,"LineTo"},{0x892,"Line"},{0x893,"MoveTo"},
    {0x894,"Move"},{0x895,"ShutDown"},{0x896,"HidePen"},{0x897,"ShowPen"},
    {0x898,"GetPenState"},{0x899,"SetPenState"},{0x89A,"GetPen"},{0x89B,"PenSize"},
    {0x89C,"PenMode"},{0x89D,"PenPat"},{0x89E,"PenNormal"},{0x89F,"Unimplemented"},
    {0x8A0,"StdRect"},{0x8A1,"FrameRect"},{0x8A2,"PaintRect"},{0x8A3,"EraseRect"},
    {0x8A4,"InverRect"},{0x8A5,"FillRect"},{0x8A6,"EqualRect"},{0x8A7,"SetRect"},
    {0x8A8,"OffsetRect"},{0x8A9,"InsetRect"},{0x8AA,"SectRect"},{0x8AB,"UnionRect"},
    {0x8AC,"Pt2Rect"},{0x8AD,"PtInRect"},{0x8AE,"EmptyRect"},{0x8AF,"StdRRect"},
    {0x8B0,"FrameRoundRect"},{0x8B1,"PaintRoundRect"},{0x8B5,"ScriptUtil"},
    {0x8B6,"StdOval"},{0x8BD,"StdArc"},{0x8C5,"StdPoly"},{0x8D1,"StdRgn"},
    {0x8D2,"FrameRgn"},{0x8D3,"PaintRgn"},{0x8D4,"EraseRgn"},{0x8D5,"InverRgn"},
    {0x8D6,"FillRgn"},{0x8D7,"BitMapToRegion"},{0x8D8,"NewRgn"},{0x8D9,"DisposeRgn"},
    {0x8DA,"OpenRgn"},{0x8DB,"CloseRgn"},{0x8DC,"CopyRgn"},{0x8DD,"SetEmptyRgn"},
    {0x8DE,"SetRectRgn"},{0x8DF,"RectRgn"},{0x8E0,"OffsetRgn"},{0x8E1,"InsetRgn"},
    {0x8E2,"EmptyRgn"},{0x8E3,"EqualRgn"},{0x8E4,"SectRgn"},{0x8E5,"UnionRgn"},
    {0x8E6,"DiffRgn"},{0x8E7,"XorRgn"},{0x8E8,"PtInRgn"},{0x8E9,"RectInRgn"},
    {0x8EA,"SetStdProcs"},{0x8EB,"StdBits"},{0x8EC,"CopyBits"},{0x8ED,"StdTxMeas"},
    {0x8EE,"StdGetPic"},{0x8EF,"ScrollRect"},{0x8F0,"StdPutPic"},{0x8F1,"StdComment"},
    {0x8F2,"PicComment"},{0x8F3,"OpenPicture"},{0x8F4,"ClosePicture"},
    {0x8F5,"KillPicture"},{0x8F6,"DrawPicture"},{0x8F8,"ScalePt"},{0x8F9,"MapPt"},
    {0x8FA,"MapRect"},{0x8FB,"MapRgn"},{0x8FC,"MapPoly"},{0x8FD,"PrGlue"},
    {0x8FE,"InitFonts"},{0x8FF,"GetFontName"},{0x900,"GetFNum"},{0x901,"FMSwapFont"},
    {0x902,"RealFont"},{0x903,"SetFontLock"},{0x904,"DrawGrowIcon"},{0x905,"DragGrayRgn"},
    {0x906,"NewString"},{0x907,"SetString"},{0x908,"ShowHide"},{0x909,"CalcVis"},
    {0x90A,"CalcVisBehind"},{0x90B,"ClipAbove"},{0x90C,"PaintOne"},{0x90D,"PaintBehind"},
    {0x90E,"SaveOld"},{0x90F,"DrawNew"},{0x910,"GetWMgrPort"},{0x911,"CheckUpdate"},
    {0x912,"InitWindows"},{0x913,"NewWindow"},{0x914,"DisposeWindow"},{0x915,"ShowWindow"},
    {0x916,"HideWindow"},{0x917,"GetWRefCon"},{0x918,"SetWRefCon"},{0x919,"GetWTitle"},
    {0x91A,"SetWTitle"},{0x91B,"MoveWindow"},{0x91C,"HiliteWindow"},{0x91D,"SizeWindow"},
    {0x91E,"TrackGoAway"},{0x91F,"SelectWindow"},{0x920,"BringToFront"},
    {0x921,"SendBehind"},{0x922,"BeginUpdate"},{0x923,"EndUpdate"},{0x924,"FrontWindow"},
    {0x925,"DragWindow"},{0x926,"DragTheRgn"},{0x927,"InvalRgn"},{0x928,"InvalRect"},
    {0x929,"ValidRgn"},{0x92A,"ValidRect"},{0x92B,"GrowWindow"},{0x92C,"FindWindow"},
    {0x92D,"CloseWindow"},{0x930,"InitMenus"},{0x931,"NewMenu"},{0x932,"DisposeMenu"},
    {0x933,"AppendMenu"},{0x934,"ClearMenuBar"},{0x935,"InsertMenu"},{0x936,"DeleteMenu"},
    {0x937,"DrawMenuBar"},{0x938,"HiliteMenu"},{0x939,"EnableItem"},{0x93A,"DisableItem"},
    {0x93B,"GetMenuBar"},{0x93C,"SetMenuBar"},{0x93D,"MenuSelect"},{0x93E,"MenuKey"},
    {0x946,"GetMenuItemText"},{0x948,"CalcMenuSize"},{0x949,"GetMenuHandle"},
    {0x94B,"PlotIcon"},{0x94C,"FlashMenuBar"},{0x94D,"AppendResMenu"},{0x94E,"PinRect"},
    {0x94F,"DeltaPoint"},{0x950,"CountMItems"},{0x954,"NewControl"},{0x955,"DisposeControl"},
    {0x957,"ShowControl"},{0x958,"HideControl"},{0x959,"MoveControl"},{0x95C,"SizeControl"},
    {0x95D,"HiliteControl"},{0x960,"GetControlValue"},{0x963,"SetControlValue"},
    {0x968,"TrackControl"},{0x969,"DrawControls"},{0x96C,"FindControl"},
    {0x96D,"Draw1Control"},{0x96E,"Dequeue"},{0x96F,"Enqueue"},{0x970,"GetNextEvent"},
    {0x971,"EventAvail"},{0x972,"GetMouse"},{0x973,"StillDown"},{0x974,"Button"},
    {0x975,"TickCount"},{0x976,"GetKeys"},{0x977,"WaitMouseUp"},{0x978,"UpdateDialog"},
    {0x97B,"InitDialogs"},{0x97C,"GetNewDialog"},{0x97D,"NewDialog"},{0x97F,"IsDialogEvent"},
    {0x980,"DialogSelect"},{0x981,"DrawDialog"},{0x982,"CloseDialog"},
    {0x983,"DisposeDialog"},{0x985,"Alert"},{0x98B,"ParamText"},{0x98D,"GetDialogItem"},
    {0x991,"ModalDialog"},{0x992,"DetachResource"},{0x993,"SetResPurge"},
    {0x994,"CurResFile"},{0x995,"InitResources"},{0x996,"RsrcZoneInit"},
    {0x997,"OpenResFile"},{0x998,"UseResFile"},{0x999,"UpdateResFile"},
    {0x99A,"CloseResFile"},{0x99B,"SetResLoad"},{0x99C,"CountResources"},
    {0x99D,"GetIndResource"},{0x99E,"CountTypes"},{0x99F,"GetIndType"},
    {0x9A0,"GetResource"},{0x9A1,"GetNamedResource"},{0x9A2,"LoadResource"},
    {0x9A3,"ReleaseResource"},{0x9A4,"HomeResFile"},{0x9A5,"SizeRsrc"},
    {0x9A6,"GetResAttrs"},{0x9A7,"SetResAttrs"},{0x9A8,"GetResInfo"},{0x9A9,"SetResInfo"},
    {0x9AA,"ChangedResource"},{0x9AB,"AddResource"},{0x9AD,"RemoveResource"},
    {0x9AF,"ResError"},{0x9B0,"WriteResource"},{0x9B1,"CreateResFile"},
    {0x9B2,"SystemEvent"},{0x9B3,"SystemClick"},{0x9B4,"SystemTask"},{0x9B5,"SystemMenu"},
    {0x9B6,"OpenDeskAcc"},{0x9B7,"CloseDeskAcc"},{0x9B8,"GetPattern"},{0x9B9,"GetCursor"},
    {0x9BA,"GetString"},{0x9BB,"GetIcon"},{0x9BC,"GetPicture"},{0x9BD,"GetNewWindow"},
    {0x9BE,"GetNewControl"},{0x9BF,"GetMenu"},{0x9C0,"GetNewMBar"},{0x9C1,"UniqueID"},
    {0x9C2,"SysEdit"},{0x9C3,"KeyTranslate"},{0x9C4,"OpenRFPerm"},{0x9C5,"RsrcMapEntry"},
    {0x9C6,"Secs2Date"},{0x9C7,"Date2Secs"},{0x9C8,"SysBeep"},{0x9C9,"SysError"},
    {0x9CC,"TEInit"},{0x9CE,"TextBox"},{0x9D2,"TENew"},{0x9D3,"TEUpdate"},{0x9DA,"TEIdle"},
    {0x9E0,"Munger"},{0x9E1,"HandToHand"},{0x9E2,"PtrToXHand"},{0x9E3,"PtrToHand"},
    {0x9E4,"HandAndHand"},{0x9E5,"InitPack"},{0x9E6,"InitAllPacks"},{0x9E7,"Pack0"},
    {0x9E8,"Pack1"},{0x9E9,"Pack2"},{0x9EA,"Pack3"},{0x9EB,"Pack4"},{0x9EC,"Pack5"},
    {0x9ED,"Pack6"},{0x9EE,"Pack7"},{0x9EF,"PtrAndHand"},{0x9F0,"LoadSeg"},
    {0x9F1,"UnloadSeg"},{0x9F2,"Launch"},{0x9F3,"Chain"},{0x9F4,"ExitToShell"},
    {0x9F5,"GetAppParms"},{0x9F6,"GetResFileAttrs"},{0x9F7,"SetResFileAttrs"},
    {0x9F8,"MethodDispatch"},{0x9F9,"InfoScrap"},{0x9FF,"Debugger"},
    {0xA00,"OpenCPort"},{0xA01,"InitCPort"},{0xA02,"CloseCPort"},{0xA03,"NewPixMap"},
    {0xA0A,"PenPixPat"},{0xA0B,"BackPixPat"},{0xA0E,"FillCRect"},{0xA12,"FillCRgn"},
    {0xA14,"RGBForeColor"},{0xA15,"RGBBackColor"},{0xA16,"SetCPixel"},{0xA17,"GetCPixel"},
    {0xA18,"GetCTable"},{0xA19,"GetForeColor"},{0xA1A,"GetBackColor"},
    {0xA1B,"GetCCursor"},{0xA1C,"SetCCursor"},{0xA1D,"AllocCursor"},{0xA1E,"GetCIcon"},
    {0xA1F,"PlotCIcon"},{0xA29,"GetDeviceList"},{0xA2A,"GetMainDevice"},
    {0xA2B,"GetNextDevice"},{0xA2C,"TestDeviceAttribute"},{0xA31,"SetGDevice"},
    {0xA32,"GetGDevice"},{0xA33,"Color2Index"},{0xA34,"Index2Color"},{0xA36,"RealColor"},
    {0xA37,"GetSubTable"},{0xA4F,"CalcCMask"},{0xA50,"SeedCFill"},{0xA51,"CopyDeepMask"},
    {0xA52,"HighLevelFSDispatch"},{0xA68,"HelpDispatch"},{0xA90,"InitPalettes"},
    {0xAA2,"PaletteDispatch"},{0xB1D,"QDExtensions"},{0xBC3,"NQDMisc"},
};

/* Canonical key: OS traps $A0nn (flag bits 8-10 dropped), toolbox traps
 * $A8nn-$ABnn (autopop bit 10 dropped). */
static inline uint16_t trap_key(uint16_t w)
{
    if (w & 0x0800) return 0xA800 | (w & 0x03FF);
    return 0xA000 | (w & 0x00FF);
}

static const char *trap_name(uint16_t key)
{
    if (key & 0x0800) {
        for (size_t i = 0; i < sizeof tb_names / sizeof *tb_names; i++)
            if (tb_names[i].w == (key & 0xFFF)) return tb_names[i].n;
    } else {
        for (size_t i = 0; i < sizeof os_names / sizeof *os_names; i++)
            if (os_names[i].w == (key & 0xFF)) return os_names[i].n;
    }
    return "?";
}

/* ---------------- counters ---------------- */
#define NKEYS 0x1000                /* index = key & 0xFFF */
static uint64_t t_calls[NKEYS], t_incl[NKEYS], t_incl_cyc[NKEYS];
static uint64_t t_self[NKEYS], t_self_cyc[NKEYS];
static uint64_t t_stamp[NKEYS];
static uint16_t t_word[NKEYS];      /* last raw trap word seen */

/* call sites: (trap key, PC) -> count, open addressing */
#define CS_N 65536
static struct { uint32_t pc; uint16_t key; uint32_t n; } cs[CS_N];
static void cs_add(uint16_t key, uint32_t pc)
{
    uint32_t h = (pc * 2654435761u ^ key) & (CS_N - 1);
    for (int i = 0; i < CS_N; i++, h = (h + 1) & (CS_N - 1)) {
        if (cs[h].n && cs[h].pc == pc && cs[h].key == key) { cs[h].n++; return; }
        if (!cs[h].n) { cs[h].pc = pc; cs[h].key = key; cs[h].n = 1; return; }
    }
}
static void cs_print(FILE *o, uint16_t key)
{
    uint32_t best[2] = {0}, bpc[2] = {0};
    for (int h = 0; h < CS_N; h++)
        if (cs[h].n && cs[h].key == key) {
            if (cs[h].n > best[0]) { best[1] = best[0]; bpc[1] = bpc[0]; best[0] = cs[h].n; bpc[0] = cs[h].pc; }
            else if (cs[h].n > best[1]) { best[1] = cs[h].n; bpc[1] = cs[h].pc; }
        }
    for (int k = 0; k < 2 && best[k]; k++) fprintf(o, " %08X", bpc[k]);
}

#define ROM_PAGES 256               /* 1 MB / 4 KB */
#define RAM_PAGES 2048              /* up to 8 MB */
#define OTH_PAGES 4096              /* 24-bit space / 4 KB */
static uint64_t rom_pg[ROM_PAGES], rom_pg_cyc[ROM_PAGES];
static uint64_t ram_pg[RAM_PAGES], ram_pg_cyc[RAM_PAGES];
static uint64_t oth_pg[OTH_PAGES], oth_pg_cyc[OTH_PAGES];
static uint32_t *rom_pc, *ram_pc;   /* exact per-PC (word) counts */

static uint64_t n_instr, n_cyc, n_irq, n_irq_cyc, n_ipl, n_ipl_cyc;
static uint64_t n_irq_lvl[8], n_irq_entries[8];
static uint64_t n_notrap, n_notrap_cyc, n_irq_self, n_irq_self_cyc;
static long     first_frame = -1, last_frame = -1;

/* active context stack */
#define STK 64
static struct { uint32_t ret, sp; uint16_t key; uint8_t irq; } stk[STK];
static int sp_n;

/* previous instruction, for cycle accounting */
static int      prev_valid;
static int      prev_rem;
static int      prev_kind;          /* 0 rom, 1 ram, 2 other */
static uint32_t prev_page;
static int      prev_ctx_len;       /* inclusive traps of prev instr */
static uint16_t prev_ctx[STK];
static int      prev_top;           /* -1 none, -2 irq, else key */
static int      prev_irq, prev_ipl;

static uint32_t irq_vec[8];         /* autovector targets, levels 1-7 */
static long     irq_vec_frame = -1;

static uint8_t *ram;
static uint32_t ram_size, rom_size;

static void read_vectors(void)
{
    uint32_t vbr = m68k_get_reg(NULL, M68K_REG_VBR);
    for (int l = 1; l <= 7; l++) {
        uint32_t a = (vbr + 0x60 + 4 * l) & (ram_size - 1);
        irq_vec[l] = ((uint32_t)ram[a] << 24) | (ram[a + 1] << 16) | (ram[a + 2] << 8) | ram[a + 3];
        irq_vec[l] &= 0xFFFFFF;
    }
}

/* Charge the previous instruction's cycles now that we know them. */
static inline void charge_prev_cycles(void)
{
    if (!prev_valid) return;
    int rem = m68ki_remaining_cycles;
    if (rem > prev_rem) { prev_valid = 0; return; }   /* new slice: unknown */
    uint64_t c = (uint64_t)(prev_rem - rem);
    n_cyc += c;
    if (prev_kind == 0) rom_pg_cyc[prev_page] += c;
    else if (prev_kind == 1) ram_pg_cyc[prev_page] += c;
    else oth_pg_cyc[prev_page] += c;
    for (int i = 0; i < prev_ctx_len; i++) t_incl_cyc[prev_ctx[i]] += c;
    if (prev_top >= 0) t_self_cyc[prev_top] += c;
    else if (prev_top == -2) n_irq_self_cyc += c;
    else n_notrap_cyc += c;
    if (prev_irq) n_irq_cyc += c;
    if (prev_ipl) n_ipl_cyc += c;
    prev_valid = 0;
}

void prof_instr(uint32_t pc)
{
    long f = host_frame_count();
    if (f < prof_from || (prof_to >= 0 && f >= prof_to)) {
        prev_valid = 0;
        sp_n = 0;   /* context is rebuilt when the window opens */
        return;
    }
    if (!ram) {
        ram = mac_mem_get_ram();
        ram_size = mac_mem_get_ram_size();
        rom_size = (uint32_t)(prof_rom_end - prof_rom);
        rom_pc = calloc(rom_size / 2, sizeof *rom_pc);
        ram_pc = calloc(ram_size / 2, sizeof *ram_pc);
    }
    if (first_frame < 0) first_frame = f;
    last_frame = f;
    if (f != irq_vec_frame) { read_vectors(); irq_vec_frame = f; }

    charge_prev_cycles();

    uint32_t sp = m68k_get_reg(NULL, M68K_REG_SP);
    uint32_t sr = m68k_get_reg(NULL, M68K_REG_SR);

    /* locate code */
    const uint8_t *code = NULL;
    int kind;
    uint32_t page, pc24 = pc & 0xFFFFFF;
#if MAC_ROM_LC_II
    /* The 68030 keeps the high byte of 24-bit-mode addresses (master
     * pointer flags) in the PC; the V8 decoder ignores it, as here. */
    int in_rom = pc24 >= 0xA00000 && pc24 < 0xB00000;
    int in_ram = pc24 < ram_size;
#else
    int in_rom = pc24 >= 0x400000 && pc24 < 0x500000;
    int in_ram = pc24 < 0x400000;
#endif
    uint32_t off;
    if (in_rom) {
        off = pc & (rom_size - 1);
        code = prof_rom + off;
        kind = 0; page = off >> 12;
        rom_pc[off >> 1]++;
    } else if (in_ram) {
        off = pc24 & (ram_size - 1);
        code = ram + off;
        kind = 1; page = off >> 12;
        ram_pc[off >> 1]++;
    } else {
        kind = 2; page = pc24 >> 12;
    }

    /* pop returned contexts */
    while (sp_n > 0) {
        int t = sp_n - 1;
        if (sp > stk[t].sp || (!stk[t].irq && pc == stk[t].ret && sp >= stk[t].sp))
            sp_n--;
        else
            break;
    }
    /* interrupt entry */
    for (int l = 1; l <= 7; l++) {
        if (pc24 == irq_vec[l] && (sr & 0x0700)) {
            if (sp_n == STK) { memmove(stk, stk + 1, sizeof stk[0] * (STK - 1)); sp_n--; }
            stk[sp_n].irq = (uint8_t)l; stk[sp_n].sp = sp; stk[sp_n].ret = 0; stk[sp_n].key = 0;
            sp_n++;
            n_irq_entries[l]++;
            break;
        }
    }

    /* attribute this instruction */
    n_instr++;
    if (kind == 0) rom_pg[page]++; else if (kind == 1) ram_pg[page]++; else oth_pg[page]++;
    prev_ctx_len = 0;
    int irq_lvl = 0;
    uint64_t stamp = n_instr;
    for (int t = sp_n - 1; t >= 0; t--) {
        if (stk[t].irq) { irq_lvl = stk[t].irq; break; }
        uint16_t k = stk[t].key & 0xFFF;
        if (t_stamp[k] != stamp) {
            t_stamp[k] = stamp;
            t_incl[k]++;
            prev_ctx[prev_ctx_len++] = k;
        }
    }
    if (!irq_lvl)
        for (int t = 0; t < sp_n; t++) if (stk[t].irq) { irq_lvl = stk[t].irq; break; }
    if (sp_n == 0) { prev_top = -1; n_notrap++; }
    else if (stk[sp_n - 1].irq) { prev_top = -2; n_irq_self++; }
    else { prev_top = stk[sp_n - 1].key & 0xFFF; t_self[prev_top]++; }
    prev_irq = irq_lvl != 0;
    if (prev_irq) { n_irq++; n_irq_lvl[irq_lvl]++; }
    prev_ipl = (sr & 0x0700) != 0;
    if (prev_ipl) n_ipl++;
    prev_kind = kind; prev_page = page;
    prev_rem = m68ki_remaining_cycles;
    prev_valid = 1;

    /* A-line trap: push after attributing the A-line instruction itself */
    if (code && (code[0] & 0xF0) == 0xA0) {
        uint16_t w = (uint16_t)(code[0] << 8 | code[1]);
        uint16_t k = trap_key(w);
        t_calls[k & 0xFFF]++;
        t_word[k & 0xFFF] = w;
        cs_add(k, pc);
        if (sp_n == STK) { memmove(stk, stk + 1, sizeof stk[0] * (STK - 1)); sp_n--; }
        stk[sp_n].irq = 0; stk[sp_n].key = k; stk[sp_n].sp = sp; stk[sp_n].ret = pc + 2;
        sp_n++;
    }
}

/* ---------------- report ---------------- */
static int cmp_desc_idx_vals;
static const uint64_t *sort_vals;
static int cmp_desc(const void *a, const void *b)
{
    uint64_t x = sort_vals[*(const int *)a], y = sort_vals[*(const int *)b];
    (void)cmp_desc_idx_vals;
    return x < y ? 1 : x > y ? -1 : 0;
}
static int *sorted(const uint64_t *v, int n)
{
    int *ix = malloc(sizeof(int) * n);
    for (int i = 0; i < n; i++) ix[i] = i;
    sort_vals = v;
    qsort(ix, n, sizeof(int), cmp_desc);
    return ix;
}
static double pct(uint64_t a, uint64_t b) { return b ? 100.0 * a / b : 0; }

/* top PCs within a page, from the exact per-PC table */
static void top_pcs(FILE *o, const uint32_t *tab, uint32_t base_off, uint32_t disp_base)
{
    uint32_t best[3] = {0}, bpc[3] = {0};
    for (uint32_t i = 0; i < 2048; i++) {
        uint32_t c = tab[(base_off >> 1) + i];
        for (int k = 0; k < 3; k++)
            if (c > best[k]) {
                for (int m = 2; m > k; m--) { best[m] = best[m - 1]; bpc[m] = bpc[m - 1]; }
                best[k] = c; bpc[k] = base_off + 2 * i; break;
            }
    }
    for (int k = 0; k < 3 && best[k]; k++)
        fprintf(o, " %06X(%.1f%%)", disp_base + bpc[k], pct(best[k], n_instr));
}

static void report(FILE *o)
{
    long nf = last_frame - first_frame + 1;
    fprintf(o, "\n=== PROFILE frames %ld-%ld (%ld frames) ===\n", first_frame, last_frame, nf);
    fprintf(o, "instructions %llu (%.0f/frame)  cycles %llu (%.0f/frame, %.2f cyc/instr)\n",
            (unsigned long long)n_instr, nf > 0 ? (double)n_instr / nf : 0,
            (unsigned long long)n_cyc, nf > 0 ? (double)n_cyc / nf : 0,
            n_instr ? (double)n_cyc / n_instr : 0);
#if MAC_ROM_LC_II
    uint32_t rom_disp = 0xA00000;
#else
    uint32_t rom_disp = 0x400000;
#endif

    fprintf(o, "\n-- context --\n");
    fprintf(o, "in interrupt handlers (autovector entry..RTE): %5.1f%% instr  %5.1f%% cyc\n",
            pct(n_irq, n_instr), pct(n_irq_cyc, n_cyc));
    for (int l = 1; l <= 7; l++)
        if (n_irq_entries[l])
            fprintf(o, "   level %d: %llu entries, %5.1f%% instr, %.0f instr/entry (vector $%06X)\n", l,
                    (unsigned long long)n_irq_entries[l], pct(n_irq_lvl[l], n_instr),
                    (double)n_irq_lvl[l] / n_irq_entries[l], (unsigned)irq_vec[l]);
    fprintf(o, "SR interrupt mask >= 1:                          %5.1f%% instr  %5.1f%% cyc\n",
            pct(n_ipl, n_instr), pct(n_ipl_cyc, n_cyc));
    fprintf(o, "outside any trap and interrupt (app/System code): %5.1f%% instr  %5.1f%% cyc\n",
            pct(n_notrap, n_instr), pct(n_notrap_cyc, n_cyc));
    fprintf(o, "interrupt handler, outside traps (self):         %5.1f%% instr  %5.1f%% cyc\n",
            pct(n_irq_self, n_instr), pct(n_irq_self_cyc, n_cyc));

    int *ix = sorted(t_incl, NKEYS);
    fprintf(o, "\n-- A-line traps, inclusive (instructions until return; nested traps count in each) --\n");
    fprintf(o, "  trap   name                    calls   instr/call   incl%%  inclcyc%%   self%%   word  trap-table addr, top call sites\n");
    for (int i = 0; i < prof_top && t_incl[ix[i]]; i++) {
        int k = ix[i];
        uint16_t key = 0xA000 | k;
        fprintf(o, "  $%04X  %-22s %7llu %11.0f  %6.1f  %7.1f  %6.1f", key, trap_name(key),
                (unsigned long long)t_calls[k], t_calls[k] ? (double)t_incl[k] / t_calls[k] : 0,
                pct(t_incl[k], n_instr), pct(t_incl_cyc[k], n_cyc), pct(t_self[k], n_instr));
#if MAC_ROM_LC_II
        uint32_t ta = (key & 0x0800) ? 0xE00 + 4 * (key & 0x3FF) : 0x400 + 4 * (key & 0xFF);
#else   /* SE/Classic dispatcher: 512 toolbox entries, number & $1FF */
        uint32_t ta = (key & 0x0800) ? 0xE00 + 4 * (key & 0x1FF) : 0x400 + 4 * (key & 0xFF);
#endif
        uint32_t tv = ram ? ((uint32_t)ram[ta] << 24 | ram[ta + 1] << 16 | ram[ta + 2] << 8 | ram[ta + 3]) : 0;
        fprintf(o, "  %04X  ->%08X  from", t_word[k], tv);
        cs_print(o, key);
        fprintf(o, "\n");
    }
    free(ix);
    ix = sorted(t_self, NKEYS);
    fprintf(o, "\n-- A-line traps, self (innermost trap only; excludes nested traps) --\n");
    fprintf(o, "  trap   name                    calls    self%%  selfcyc%%\n");
    for (int i = 0; i < prof_top && t_self[ix[i]]; i++) {
        int k = ix[i];
        uint16_t key = 0xA000 | k;
        fprintf(o, "  $%04X  %-22s %7llu  %6.1f  %7.1f\n", key, trap_name(key),
                (unsigned long long)t_calls[k], pct(t_self[k], n_instr), pct(t_self_cyc[k], n_cyc));
    }
    free(ix);
    ix = sorted(t_calls, NKEYS);
    fprintf(o, "\n-- most-called traps --\n ");
    for (int i = 0; i < 20 && t_calls[ix[i]]; i++)
        fprintf(o, " %s $%04X %.1f/frame;", trap_name(0xA000 | ix[i]), 0xA000 | ix[i],
                nf > 0 ? (double)t_calls[ix[i]] / nf : 0);
    fprintf(o, "\n");
    free(ix);

    /* regions: merge into one list: id = kind*8192 + page */
    enum { NR = ROM_PAGES + RAM_PAGES + OTH_PAGES };
    static uint64_t rv[NR];
    for (int i = 0; i < ROM_PAGES; i++) rv[i] = rom_pg[i];
    for (int i = 0; i < RAM_PAGES; i++) rv[ROM_PAGES + i] = ram_pg[i];
    for (int i = 0; i < OTH_PAGES; i++) rv[ROM_PAGES + RAM_PAGES + i] = oth_pg[i];
    uint64_t tot_rom = 0, tot_ram = 0, tot_oth = 0;
    for (int i = 0; i < ROM_PAGES; i++) tot_rom += rom_pg[i];
    for (int i = 0; i < RAM_PAGES; i++) tot_ram += ram_pg[i];
    for (int i = 0; i < OTH_PAGES; i++) tot_oth += oth_pg[i];
    fprintf(o, "\n-- code regions (4 KB pages) --  ROM %.1f%%  RAM %.1f%%  other %.1f%%\n",
            pct(tot_rom, n_instr), pct(tot_ram, n_instr), pct(tot_oth, n_instr));
    fprintf(o, "  region              instr%%   cyc%%   hottest PCs (share of all instr)\n");
    ix = sorted(rv, NR);
    for (int i = 0; i < prof_top && rv[ix[i]]; i++) {
        int r = ix[i];
        if (r < ROM_PAGES) {
            fprintf(o, "  ROM %06X-%06X  %6.1f %6.1f  ", rom_disp + (r << 12), rom_disp + (r << 12) + 0xFFF,
                    pct(rom_pg[r], n_instr), pct(rom_pg_cyc[r], n_cyc));
            top_pcs(o, rom_pc, (uint32_t)r << 12, rom_disp);
        } else if (r < ROM_PAGES + RAM_PAGES) {
            int p = r - ROM_PAGES;
            fprintf(o, "  RAM %06X-%06X  %6.1f %6.1f  ", p << 12, (p << 12) + 0xFFF,
                    pct(ram_pg[p], n_instr), pct(ram_pg_cyc[p], n_cyc));
            top_pcs(o, ram_pc, (uint32_t)p << 12, 0);
        } else {
            int p = r - ROM_PAGES - RAM_PAGES;
            fprintf(o, "  OTH %06X-%06X  %6.1f %6.1f", p << 12, (p << 12) + 0xFFF,
                    pct(oth_pg[p], n_instr), pct(oth_pg_cyc[p], n_cyc));
        }
        fprintf(o, "\n");
    }
    free(ix);
    fprintf(o, "=== END PROFILE ===\n");
}

void prof_report(void)
{
    if (!prof_enabled || !n_instr) return;
    report(stderr);
    if (prof_out) {
        FILE *f = fopen(prof_out, "w");
        if (f) { report(f); fclose(f); }
        else perror(prof_out);
    }
}
#endif /* HOST_TRACE */
