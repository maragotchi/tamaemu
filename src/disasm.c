#include "emu.h"
#include <stdlib.h>
#include <string.h>

/* --syms accepts "<hex-address> <name>" lines and ignores '#' comments. */
typedef struct { uint32_t addr; char name[48]; } Sym;
static Sym  *g_syms;
static int   g_nsyms;
/* Do not attribute a distant address to the previous known function. */
#define SYM_MAX_SPAN 0x800u

static int sym_cmp(const void *a, const void *b)
{
    uint32_t x = ((const Sym *)a)->addr, y = ((const Sym *)b)->addr;
    return x < y ? -1 : x > y ? 1 : 0;
}

int disasm_syms_load(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "[syms] could not open %s\n", path); return 0; }
    int cap = 256, n = 0;
    Sym *v = (Sym *)malloc((size_t)cap * sizeof *v);
    if (!v) { fclose(f); return 0; }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        unsigned long a; char nm[48];
        if (sscanf(line, "%lx %47s", &a, nm) != 2) continue;   /* leave room for NUL */
        if (n == cap) {
            Sym *nv = (Sym *)realloc(v, (size_t)(cap * 2) * sizeof *v);
            if (!nv) break;
            v = nv; cap *= 2;
        }
        v[n].addr = (uint32_t)a;
        snprintf(v[n].name, sizeof v[n].name, "%s", nm);
        n++;
    }
    fclose(f);
    free(g_syms);
    qsort(v, (size_t)n, sizeof *v, sym_cmp);
    g_syms = v; g_nsyms = n;
    fprintf(stderr, "[syms] loaded %d symbols from %s\n", n, path);
    return n;
}

/* Rotating buffers so two disasm_sym_for() calls in one printf don't collide. */
const char *disasm_sym_for(uint32_t pc)
{
    static char buf[4][80];
    static int  turn;
    if (!g_nsyms) return "";
    int lo = 0, hi = g_nsyms - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_syms[mid].addr <= pc) { best = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    if (best < 0) return "";
    uint32_t off = pc - g_syms[best].addr;
    if (off > SYM_MAX_SPAN) return "";
    char *b = buf[turn++ & 3];
    if (off) snprintf(b, sizeof buf[0], "%s+0x%x", g_syms[best].name, (unsigned)off);
    else     snprintf(b, sizeof buf[0], "%s", g_syms[best].name);
    return b;
}

static uint32_t sext(uint32_t v, int bits)
{
    uint32_t m = 1u << (bits - 1);
    return (v ^ m) - m;
}

static const char *sdname(int i)
{
    static const char *n[16] = {"%psr","%sp","%alr","%ahr","%?4","%?5","%?6","%?7",
                                "%ttbr","%?9","%idir","%dbbr","%?12","%?13","%?14","%pc"};
    return n[i & 15];
}

/* Returns the number of bytes consumed, including ext prefixes. */
int disasm_one(Emu *e, uint32_t pc, char *out, size_t osz)
{
    uint32_t start = pc;
    uint16_t hw = mem_read16(e, pc);
    int ext_n = 0; uint32_t ext0 = 0, ext1 = 0;
    while ((hw >> 13) == 6) {
        if (ext_n == 0) ext0 = hw & 0x1FFF;
        else if (ext_n == 1) ext1 = hw & 0x1FFF;
        else break;
        ext_n++;
        pc += 2;
        hw = mem_read16(e, pc);
    }
    uint32_t iaddr = pc;
    int cls = hw >> 13, rd = hw & 0xF, rs = (hw >> 4) & 0xF;
    char b[96] = "?";

#define P(...) snprintf(b, sizeof b, __VA_ARGS__)
    switch (cls) {
    case 0: {
        int op1 = (hw >> 9) & 0xF, d = (hw >> 8) & 1;
        if (op1 <= 3) {
            int cnst = (hw >> 4) & 3;
            if (cnst == 0) {
                int op2 = (hw >> 6) & 3;
                int key = op1 * 8 + d * 4 + op2;
                switch (key) {
                case 0: P("nop"); break;
                case 1: P("slp"); break;
                case 2: P("halt"); break;
                case 8: P("pushn %%r%d", rd); break;
                case 9: P("popn %%r%d", rd); break;
                case 11: P("jpr %%r%d", rd); break;
                case 15: P("jpr.d %%r%d", rd); break;
                case 16: P("brk"); break;
                case 17: P("retd"); break;
                case 18: P("int 0x%x", hw & 3); break;
                case 19: P("reti"); break;
                case 24: P("call %%r%d", rd); break;
                case 25: P("ret"); break;
                case 26: P("jp %%r%d", rd); break;
                case 28: P("call.d %%r%d", rd); break;
                case 29: P("ret.d"); break;
                case 30: P("jp.d %%r%d", rd); break;
                }
            } else if (cnst == 1 && op1 == 0) {
                int op2 = (hw >> 6) & 7;
                switch (op2) {
                case 0: P("push %%r%d", rd); break;
                case 1: P("pop %%r%d", rd); break;
                case 2: P("pushs %s", (hw & 0xF) == 3 ? "%ahr" : "%alr"); break;
                case 3: P("pops %s", (hw & 0xF) == 3 ? "%ahr" : "%alr"); break;
                case 7: P("ld.cf"); break;
                }
            }
        } else {
            static const char *cc[12] = {"jrgt","jrge","jrlt","jrle","jrugt","jruge",
                                         "jrult","jrule","jreq","jrne","call","jp"};
            uint32_t u8 = hw & 0xFF, target;
            const char *pre = ext_n == 2 ? "x" : ext_n == 1 ? "s" : "";
            if (ext_n == 0)      target = iaddr + sext(u8, 8) * 2;
            else if (ext_n == 1) target = iaddr + ((sext(ext0, 13) << 9) | (u8 << 1));
            else                 target = iaddr + (((ext0 & 0x1ff8) << 19) | (ext1 << 9) | (u8 << 1));
            P("%s%s%s 0x%x", pre, cc[op1 - 4], d ? ".d" : "", target);
        }
        break;
    }
    case 1: {
        int op1 = (hw >> 10) & 7, op2 = (hw >> 8) & 3;
        static const char *sh[8] = {"srl","sll","sra","sla","rr","rl","?","?"};
        static const char *alu[8] = {"add","sub","cmp","ld.w","and","or","xor","not"};
        static const char *ldsz[5] = {"ld.b","ld.ub","ld.h","ld.uh","ld.w"};
        if (op2 == 3) { P("%s %%r%d,0x%x", sh[op1 <= 5 ? op1 : 6], rd, ((hw >> 4) & 0xF) + 16); break; }
        if (op2 == 2) {
            if (ext_n) {
                uint32_t imm = ext_n == 1 ? ext0 : ((ext0 << 13) | ext1);
                P("x%s %%r%d,%%r%d,0x%x", alu[op1], rd, rs, imm);
            } else if (op1 == 3) P("ld.w %%r%d,%%r%d", rd, rs);
            else if (op1 == 7)   P("not %%r%d,%%r%d", rd, rs);
            else                 P("%s %%r%d,%%r%d", alu[op1], rd, rs);
            break;
        }
        {
            char addr[40];
            if (ext_n) {
                uint32_t imm = ext_n == 1 ? ext0 : ((ext0 << 13) | ext1);
                snprintf(addr, sizeof addr, "[%%r%d+0x%x]", rs, imm);
            } else
                snprintf(addr, sizeof addr, op2 == 1 ? "[%%r%d]+" : "[%%r%d]", rs);
            const char *xp = ext_n ? "x" : "";
            if (op1 <= 4) P("%s%s %%r%d,%s", xp, ldsz[op1], rd, addr);
            else {
                static const char *stsz[3] = {"ld.b","ld.h","ld.w"};
                P("%s%s %s,%%r%d", xp, stsz[op1 - 5], addr, rd);
            }
        }
        break;
    }
    case 2: {
        int op1 = (hw >> 10) & 7;
        uint32_t imm6 = (hw >> 4) & 0x3F, off;
        static const uint8_t scale[8] = {1,1,2,2,4,1,2,4};
        static const char *nm[8] = {"ld.b","ld.ub","ld.h","ld.uh","ld.w","ld.b","ld.h","ld.w"};
        /* Keep the offset unscaled to match Ghidra output. */
        if (ext_n == 0) { off = imm6; (void)scale; }
        else if (ext_n == 1) off = (ext0 << 6) | imm6;
        else off = (ext0 << 19) | (ext1 << 6) | imm6;
        const char *xp = ext_n ? "x" : "";
        if (op1 <= 4) P("%s%s %%r%d,[%%sp+0x%x]", xp, nm[op1], rd, off);
        else          P("%s%s [%%sp+0x%x],%%r%d", xp, nm[op1], off, rd);
        break;
    }
    case 3: {
        int op1 = (hw >> 10) & 7;
        uint32_t imm6 = (hw >> 4) & 0x3F, imm;
        static const char *nm[8] = {"add","sub","cmp","ld.w","and","or","xor","not"};
        if (ext_n == 0) imm = (op1 <= 1) ? imm6 : sext(imm6, 6);
        else if (ext_n == 1) { imm = (ext0 << 6) | imm6; if (op1 > 1) imm = sext(imm, 19); }
        else imm = (ext0 << 19) | (ext1 << 6) | imm6;
        P("%s%s %%r%d,0x%x", ext_n ? "x" : "", nm[op1], rd, imm);
        break;
    }
    case 4: {
        int op1 = (hw >> 10) & 7, op2 = (hw >> 8) & 3;
        static const char *sh[8] = {"?","?","srl","sll","sra","sla","rr","rl"};
        if (op1 == 0) { P("add %%sp,0x%x", hw & 0x3FF); break; }   /* displayed raw; executed x4 */
        if (op1 == 1) { P("sub %%sp,0x%x", hw & 0x3FF); break; }
        if (op2 == 2 && op1 == 4) { P("swap %%r%d,%%r%d", rd, rs); break; }
        if (op2 == 2 && op1 == 6) { P("swaph %%r%d,%%r%d", rd, rs); break; }
        if (op2 == 0) P("%s %%r%d,0x%x", sh[op1], rd, (hw >> 4) & 0xF);
        else          P("%s %%r%d,%%r%d", sh[op1], rd, rs);
        break;
    }
    case 5: {
        int op1 = (hw >> 10) & 7, op2 = (hw >> 8) & 3;
        switch (op1 * 4 + op2) {
        case 0: P("ld.w %s,%%r%d", sdname(hw & 0xF), rs); break;
        case 1: P("ld.b %%r%d,%%r%d", rd, rs); break;
        case 2: P("mlt.h %%r%d,%%r%d", rd, rs); break;
        case 4: P("ld.w %%r%d,%s", rd, sdname(rs)); break;
        case 5: P("ld.ub %%r%d,%%r%d", rd, rs); break;
        case 6: P("mltu.h %%r%d,%%r%d", rd, rs); break;
        case 8: case 12: case 16: case 20: {
            static const char *bops[4] = {"btst","bclr","bset","bnot"};
            const char *nm = bops[op1 - 2];
            if (ext_n) {
                uint32_t imm = ext_n == 1 ? ext0 : ((ext0 << 13) | ext1);
                P("x%s [%%r%d+0x%x],0x%x", nm, rs, imm, hw & 7);
            } else P("%s [%%r%d],0x%x", nm, rs, hw & 7);
            break; }
        case 9: P("ld.h %%r%d,%%r%d", rd, rs); break;
        case 10: P("mlt.w %%r%d,%%r%d", rd, rs); break;
        case 13: P("ld.uh %%r%d,%%r%d", rd, rs); break;
        case 14: P("mltu.w %%r%d,%%r%d", rd, rs); break;
        case 17: P("ld.c %%r%d,%d", rd, (hw >> 4) & 0xF); break;
        case 21: P("ld.c %d,%%r%d", (hw >> 4) & 0xF, rd); break;
        case 24: P("adc %%r%d,%%r%d", rd, rs); break;
        case 28: P("sbc %%r%d,%%r%d", rd, rs); break;
        case 31: {
            int op3 = (hw >> 6) & 3, imm5 = hw & 0x1F;
            if (op3 == 0) P("do.c 0x%x", hw & 0x3F);
            else P("psr%s 0x%x", op3 == 1 ? "set" : "clr", imm5);
            break; }
        }
        break;
    }
    case 6:
        P("ext 0x%x", hw & 0x1FFF);
        /* Used only when viewing an ext halfword by itself. */
        break;
    }
#undef P
    snprintf(out, osz, "%s", b);
    return (int)(pc + 2 - start);
}
