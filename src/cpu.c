/* S1C33 PE interpreter implemented with reference to GMMan's SLEIGH definition:
 * https://github.com/GMMan/s1c33_sleigh
 */

 #include "emu.h"
#include <stdlib.h>
#include <string.h>

static uint32_t sext(uint32_t v, int bits)
{
    uint32_t m = 1u << (bits - 1);
    return (v ^ m) - m;
}

/* ---- flag helpers (s1c33_macros.sinc) ---- */
static void result_flags(Emu *e, uint32_t r)
{
    e->f_n = (r >> 31) & 1;
    e->f_z = (r == 0);
}

static uint8_t scarry(uint32_t a, uint32_t b)
{
    uint32_t s = a + b;
    return ((~(a ^ b)) & (a ^ s)) >> 31;
}

static uint8_t sborrow(uint32_t a, uint32_t b)
{
    uint32_t d = a - b;
    return ((a ^ b) & (a ^ d)) >> 31;
}

static uint32_t add_op(Emu *e, uint32_t a, uint32_t b)
{
    e->f_v = scarry(a, b);
    e->f_c = (a + b) < a;
    uint32_t r = a + b;
    result_flags(e, r);
    return r;
}

static uint32_t adc_op(Emu *e, uint32_t a, uint32_t b)
{
    uint32_t cz = e->f_c, s = a + b;
    e->f_v = scarry(a, b) ^ scarry(s, cz);
    e->f_c = ((a + b) < a) || ((s + cz) < s);
    uint32_t r = s + cz;
    result_flags(e, r);
    return r;
}

static uint32_t sub_op(Emu *e, uint32_t a, uint32_t b)
{
    e->f_v = sborrow(a, b);
    e->f_c = a < b;
    uint32_t r = a - b;
    result_flags(e, r);
    return r;
}

static uint32_t sbc_op(Emu *e, uint32_t a, uint32_t b)
{
    uint32_t cz = e->f_c, d = a - b;
    e->f_v = sborrow(a, b) || sborrow(d, cz);
    e->f_c = (a < b) || (d < cz);
    uint32_t r = d - cz;
    result_flags(e, r);
    return r;
}

static void cmp_op(Emu *e, uint32_t a, uint32_t b)
{
    e->f_v = sborrow(a, b);
    e->f_c = a < b;
    result_flags(e, a - b);
}

static uint32_t log_flags(Emu *e, uint32_t r)
{
    result_flags(e, r);
    e->f_v = 0;
    return r;
}

static uint32_t sl_op(Emu *e, uint32_t a, uint32_t n)
{
    n &= 31;
    uint32_t r = a << n;
    result_flags(e, r);
    return r;
}

static uint32_t srl_op(Emu *e, uint32_t a, uint32_t n)
{
    n &= 31;
    uint32_t r = a >> n;
    result_flags(e, r);
    return r;
}

static uint32_t sra_op(Emu *e, uint32_t a, uint32_t n)
{
    n &= 31;
    uint32_t r = (uint32_t)((int32_t)a >> n);
    result_flags(e, r);
    return r;
}

static uint32_t rr_op(Emu *e, uint32_t a, uint32_t n)
{
    n &= 31;
    uint32_t r = n ? ((a >> n) | (a << (32 - n))) : a;
    result_flags(e, r);
    return r;
}

static uint32_t rl_op(Emu *e, uint32_t a, uint32_t n)
{
    n &= 31;
    uint32_t r = n ? ((a << n) | (a >> (32 - n))) : a;
    result_flags(e, r);
    return r;
}

/* ---- PSR pack/unpack (bit0 N,1 Z,2 V,3 C,4 IE, 8-11 IL) ---- */
static uint32_t psr_pack(Emu *e)
{
    return (e->psr_rest & ~0xF1Fu) | e->f_n | (e->f_z << 1) | (e->f_v << 2) |
           (e->f_c << 3) | (e->f_ie << 4) | ((uint32_t)(e->il & 0xF) << 8);
}

static void psr_unpack(Emu *e, uint32_t v)
{
    e->f_n = v & 1;
    e->f_z = (v >> 1) & 1;
    e->f_v = (v >> 2) & 1;
    e->f_c = (v >> 3) & 1;
    e->f_ie = (v >> 4) & 1;
    e->il = (v >> 8) & 0xF;
    e->psr_rest = v;
}

static void push32(Emu *e, uint32_t v)
{
    e->sp -= 4;
    mem_write32(e, e->sp, v);
}

static uint32_t pop32(Emu *e)
{
    uint32_t v = mem_read32(e, e->sp);
    e->sp += 4;
    return v;
}

static void stop(Emu *e, uint32_t iaddr, const char *why, uint32_t detail)
{
    e->stopped = true;
    snprintf(e->stop_reason, sizeof e->stop_reason, "%s (detail %x) at pc=%08x cyc=%llu",
             why, detail, iaddr, (unsigned long long)e->cycles);
}

/* Trap instruction fetches outside ROM and RAM. Unmapped reads return zero,
 * which otherwise turns a wild jump into an endless stream of valid NOPs. */
static int pc_mapped(Emu *e, uint32_t a)
{
    const DeviceProfile *d = &e->dev;
    return a - d->rom_base   < d->rom_size   || a - d->a0ram_base  < d->a0ram_size ||
           a - d->ivram_base < d->ivram_size || a - d->dstram_base < d->dstram_size;
}

/* Trace calls entering watched ranges and their nested calls until return. */
static int call_in_range(Emu *e, uint32_t t)
{
    for (int i = 0; i < e->call_nranges; i++)
        if (t >= e->call_lo[i] && t <= e->call_hi[i]) return 1;
    return 0;
}

static void trace_call(Emu *e, uint32_t target, uint32_t from)
{
    if (!e->call_nranges || e->call_budget <= 0) return;
    int inside = e->call_depth > 0;
    if (!inside && !call_in_range(e, target)) return;
    /* S1C33 arguments are r6-r9. r4/r5 are scratch and r0-r3 are callee-saved, so
     * printing those as arguments gives structured-looking nonsense. */
    fprintf(stderr, "[call%c] %*s-> %08x %-28s args r6=%08x r7=%08x r8=%08x r9=%08x"
                    " (from %08x %s)\n",
            e->core_id ? 'B' : 'A', e->call_depth * 2, "",
            target, disasm_sym_for(target),
            e->r[6], e->r[7], e->r[8], e->r[9], from, disasm_sym_for(from));
    e->call_depth++;
    e->call_budget--;
}

/* Trace indirect jumps into watched ranges; jump-table state machines may not
 * have call references. Jumps do not change call depth. */
static void trace_jump(Emu *e, uint32_t target, uint32_t from)
{
    if (!e->call_nranges || e->call_budget <= 0) return;
    if (!call_in_range(e, target)) return;
    fprintf(stderr, "[jump%c] %*s=> %08x %-28s (from %08x %s)\n",
            e->core_id ? 'B' : 'A', e->call_depth * 2, "",
            target, disasm_sym_for(target), from, disasm_sym_for(from));
    e->call_budget--;
}

static void trace_ret(Emu *e, uint32_t to)
{
    if (!e->call_nranges || e->call_depth <= 0) return;
    e->call_depth--;
    if (e->call_budget <= 0) return;
    fprintf(stderr, "[call%c] %*s<- ret to %08x %s\n",
            e->core_id ? 'B' : 'A', e->call_depth * 2, "", to, disasm_sym_for(to));
    e->call_budget--;
}

static void bad_pc(Emu *e)
{
    char c = e->core_id ? 'B' : 'A';
    fprintf(stderr, "[cpu%c] BAD PC %08x - fetch outside all mapped memory, cyc=%llu\n",
            c, e->pc, (unsigned long long)e->cycles);
    fprintf(stderr, "[cpu%c] last 64 instructions, oldest first (the jump origin is the tail):\n", c);
    for (int i = 0; i < 64; i++) {
        if ((i & 7) == 0) fprintf(stderr, "[cpu%c]  ", c);
        fprintf(stderr, " %08x", e->trace_ring[(e->trace_ring_pos - 64 + i) & TRACE_RING_M]);
        if ((i & 7) == 7) fprintf(stderr, "\n");
    }
    fprintf(stderr, "[cpu%c] jumped from %08x %s\n", c,
            e->trace_ring[(e->trace_ring_pos - 1) & TRACE_RING_M],
            disasm_sym_for(e->trace_ring[(e->trace_ring_pos - 1) & TRACE_RING_M]));
    fprintf(stderr, "[cpu%c] sp=%08x stack:", c, e->sp);
    for (int i = 0; i < 8; i++)
        fprintf(stderr, " %08x", mem_read32(e, e->sp + (uint32_t)i * 4));
    fprintf(stderr, "\n");
    stop(e, e->pc, "fetch from unmapped memory", e->pc);
}

/* special register file for class 5 moves: sd=(0,3) / ss=(4,7) index */
static uint32_t sreg_read(Emu *e, int idx, uint32_t iaddr)
{
    switch (idx) {
    case 0: return psr_pack(e);
    case 1: return e->sp;
    case 2: return e->alr;
    case 3: return e->ahr;
    case 8: return e->ttbr;
    case 10: return e->idir;
    case 11: return e->dbbr;
    case 15: return iaddr;
    default: stop(e, iaddr, "read of invalid special reg", idx); return 0;
    }
}

static void sreg_write(Emu *e, int idx, uint32_t v, uint32_t iaddr)
{
    switch (idx) {
    case 0: psr_unpack(e, v); break;
    case 1: e->sp = v; break;
    case 2: e->alr = v; break;
    case 3: e->ahr = v; break;
    case 8: e->ttbr = v; break;
    case 10: e->idir = v; break;
    case 11: e->dbbr = v; break;
    case 15: e->pc = v; break;   /* ld.w %pc,%rs acts as a jump */
    default: stop(e, iaddr, "write of invalid special reg", idx); break;
    }
}

/* conditional branch predicates, c0_op1 4..13 */
static int cond_true(Emu *e, int op)
{
    switch (op) {
    case 4:  return !e->f_z && e->f_n == e->f_v;        /* jrgt */
    case 5:  return e->f_n == e->f_v;                   /* jrge */
    case 6:  return e->f_n != e->f_v;                   /* jrlt */
    case 7:  return e->f_z || e->f_n != e->f_v;         /* jrle */
    case 8:  return !e->f_z && !e->f_c;                 /* jrugt */
    case 9:  return !e->f_c;                            /* jruge */
    case 10: return e->f_c;                             /* jrult */
    case 11: return e->f_z || e->f_c;                   /* jrule */
    case 12: return e->f_z;                             /* jreq */
    case 13: return !e->f_z;                            /* jrne */
    default: return 1;                                  /* call/jp */
    }
}

static void exec_insn(Emu *e, uint16_t hw, uint32_t iaddr, int in_slot);

/* execute exactly one instruction (ext prefixes folded) at e->pc; no irq check */
static void step_one(Emu *e, int in_slot)
{
    e->pc &= 0x0FFFFFFFu;   /* 28-bit address bus */
    if (!pc_mapped(e, e->pc)) { bad_pc(e); return; }
    uint16_t hw = mem_read16(e, e->pc);
    int guard = 0;
    while ((hw >> 13) == 6) {                     /* class 6 = ext prefix */
        if (in_slot && guard == 0)
            fprintf(stderr, "[cpu] warning: ext prefix in delay slot at %08x\n", e->pc);
        if (e->ext_count < 2) e->ext_val[e->ext_count] = hw & 0x1FFF;
        e->ext_count++;
        e->pc += 2; e->cycles++;
        hw = mem_read16(e, e->pc);
        if (++guard > 2) { stop(e, e->pc, "more than two ext prefixes", hw); return; }
    }
    uint32_t iaddr = e->pc;
    e->trace_ring[e->trace_ring_pos++ & TRACE_RING_M] = iaddr;
    e->pc += 2; e->cycles++;
    exec_insn(e, hw, iaddr, in_slot);
}

/* run the delay slot of a .d instruction */
static void run_slot(Emu *e)
{
    if (e->stopped) return;
    step_one(e, 1);
}

static void exec_insn(Emu *e, uint16_t hw, uint32_t iaddr, int in_slot)
{
    int cls = hw >> 13;
    int rd = hw & 0xF, rs = (hw >> 4) & 0xF;
    int ext_n = e->ext_count > 2 ? 2 : e->ext_count;
    uint32_t ext0 = e->ext_val[0], ext1 = e->ext_val[1];
    e->ext_count = 0;

    switch (cls) {

    case 0: {
        int op1 = (hw >> 9) & 0xF, d = (hw >> 8) & 1;
        if (op1 <= 3) {
            int cnst = (hw >> 4) & 3;
            if (cnst == 0) {
                int op2 = (hw >> 6) & 3;
                switch (op1 * 8 + d * 4 + op2) {
                case 0: /* nop */ break;
                case 1: /* slp  */ {
                    static int force_wake = -1;
                    if (force_wake < 0) force_wake = getenv("TAMAEMU_SLPWAKE") != NULL;
                    e->halted = true;
                    /* OSC-stabilization auto-wake only when the CMU Clock Option
                     * register is armed for it (boot/PLL flow writes 0x3E00 before
                     * slp). A normal idle slp sleeps until a real interrupt. */
                    e->halt_hard = !(e->cmu.opt & 0x3E00) && !force_wake;
                    e->wake_at = e->cycles + 20000;
                    break; }
                case 2: /* halt */ e->halted = true; e->halt_hard = true; break;
                case 8: { /* pushn rd */ for (int i = rd; i >= 0; i--) push32(e, e->r[i]); break; }
                case 9: { /* popn rd  */ for (int i = 0; i <= rd; i++) e->r[i] = pop32(e); break; }
                case 11: /* jpr rd   */ e->pc = iaddr + e->r[rd];
                    trace_jump(e, e->pc, iaddr); break;
                case 15: { /* jpr.d  */ uint32_t t = iaddr + e->r[rd]; run_slot(e); e->pc = t;
                    trace_jump(e, t, iaddr); break; }
                case 16: /* brk  */ stop(e, iaddr, "brk", 0); break;
                case 17: /* retd */ stop(e, iaddr, "retd (debug) unsupported", 0); break;
                case 18: { /* int imm2 */
                    uint32_t vec = 12 + (hw & 3);   /* software exceptions int0-3 */
                    fprintf(stderr, "[cpu] int %d at %08x -> vector %u\n", hw & 3, iaddr, vec);
                    push32(e, e->pc);
                    push32(e, psr_pack(e));
                    e->f_ie = 0;
                    e->pc = mem_read32(e, e->ttbr + vec * 4);
                    break; }
                case 19: { /* reti */ psr_unpack(e, pop32(e)); e->pc = pop32(e);
                    trace_ret(e, e->pc); break; }
                case 24: /* call rd */ push32(e, e->pc); trace_call(e, e->r[rd], iaddr);
                    e->pc = e->r[rd]; break;
                case 25: /* ret     */ e->pc = pop32(e); trace_ret(e, e->pc); break;
                case 26: /* jp rd   */ e->pc = e->r[rd]; trace_jump(e, e->pc, iaddr); break;
                case 28: { /* call.d rd */ uint32_t t = e->r[rd]; run_slot(e); push32(e, e->pc);
                    trace_call(e, t, iaddr); e->pc = t; break; }
                case 29: { /* ret.d */ run_slot(e); e->pc = pop32(e); trace_ret(e, e->pc); break; }
                case 30: { /* jp.d rd */ uint32_t t = e->r[rd]; run_slot(e); e->pc = t;
                    trace_jump(e, t, iaddr); break; }
                default: stop(e, iaddr, "bad class0 group", hw); break;
                }
            } else if (cnst == 1 && op1 == 0) {
                int op2 = (hw >> 6) & 7;
                switch (op2) {
                case 0: push32(e, e->r[rd]); break;
                case 1: e->r[rd] = pop32(e); break;
                case 2: /* pushs */
                    if ((hw & 0xF) == 2) push32(e, e->alr);
                    else if ((hw & 0xF) == 3) { push32(e, e->ahr); push32(e, e->alr); }
                    else stop(e, iaddr, "pushs bad sx", hw);
                    break;
                case 3: /* pops */
                    if ((hw & 0xF) == 2) e->alr = pop32(e);
                    else if ((hw & 0xF) == 3) { e->alr = pop32(e); e->ahr = pop32(e); }
                    else stop(e, iaddr, "pops bad sx", hw);
                    break;
                case 7: /* ld.cf: coprocessor flags - none present */
                    e->f_n = e->f_z = e->f_v = e->f_c = 0; break;
                default: stop(e, iaddr, "bad class0 const1", hw); break;
                }
            } else stop(e, iaddr, "bad class0 encoding", hw);
        } else {
            /* rel8 branches, c0_op1 4..15 */
            uint32_t u8 = hw & 0xFF;
            uint32_t target;
            if (ext_n == 0)      target = iaddr + sext(u8, 8) * 2;
            else if (ext_n == 1) target = iaddr + ((sext(ext0, 13) << 9) | (u8 << 1));
            else                 target = iaddr + (((ext0 & 0x1ff8) << 19) | (ext1 << 9) | (u8 << 1));
            int taken = cond_true(e, op1);
            if (in_slot) { fprintf(stderr, "[cpu] warning: branch in delay slot at %08x\n", iaddr); return; }
            if (op1 == 14) {            /* call / call.d */
                if (d) { run_slot(e); push32(e, e->pc); trace_call(e, target, iaddr); e->pc = target; }
                else   { push32(e, e->pc); trace_call(e, target, iaddr); e->pc = target; }
            } else if (op1 == 15) {     /* jp / jp.d */
                if (d) { run_slot(e); }
                e->pc = target;
            } else {                    /* conditional */
                if (d) { run_slot(e); }
                if (taken) e->pc = target;
            }
            break;
        }
        break;
    }

    case 1: {
        int op1 = (hw >> 10) & 7, op2 = (hw >> 8) & 3;
        if (op2 == 3) {  /* shifts by DoubleImm5 = imm4+16 */
            uint32_t n = ((hw >> 4) & 0xF) + 16;
            switch (op1) {
            case 0: e->r[rd] = srl_op(e, e->r[rd], n); break;
            case 1: e->r[rd] = sl_op (e, e->r[rd], n); break;
            case 2: e->r[rd] = sra_op(e, e->r[rd], n); break;
            case 3: e->r[rd] = sl_op (e, e->r[rd], n); break;  /* sla */
            case 4: e->r[rd] = rr_op (e, e->r[rd], n); break;
            case 5: e->r[rd] = rl_op (e, e->r[rd], n); break;
            default: stop(e, iaddr, "bad class1 shift", hw); break;
            }
            break;
        }
        if (op2 == 2) {  /* reg-reg ALU; with ext -> rd = rs OP imm */
            uint32_t imm = (ext_n == 1) ? ext0 : ((ext0 << 13) | ext1);
            switch (op1) {
            case 0: e->r[rd] = ext_n ? add_op(e, e->r[rs], imm) : add_op(e, e->r[rd], e->r[rs]); break;
            case 1: e->r[rd] = ext_n ? sub_op(e, e->r[rs], imm) : sub_op(e, e->r[rd], e->r[rs]); break;
            case 2: if (ext_n) cmp_op(e, e->r[rs], imm); else cmp_op(e, e->r[rd], e->r[rs]); break;
            case 3: if (ext_n) { stop(e, iaddr, "ext on ld.w rd,rs", hw); break; }
                    e->r[rd] = e->r[rs]; break;
            case 4: e->r[rd] = ext_n ? log_flags(e, e->r[rs] & imm) : log_flags(e, e->r[rd] & e->r[rs]); break;
            case 5: e->r[rd] = ext_n ? log_flags(e, e->r[rs] | imm) : log_flags(e, e->r[rd] | e->r[rs]); break;
            case 6: e->r[rd] = ext_n ? log_flags(e, e->r[rs] ^ imm) : log_flags(e, e->r[rd] ^ e->r[rs]); break;
            case 7: if (ext_n) { stop(e, iaddr, "ext on not rd,rs", hw); break; }
                    e->r[rd] = log_flags(e, ~e->r[rs]); break;
            }
            break;
        }
        /* op2 0/1: memory ops. ext (op2==0 only) -> [rs+imm], no post-inc */
        {
            uint32_t addr = e->r[rs];
            int postinc = (op2 == 1);
            if (ext_n) {
                if (postinc) { stop(e, iaddr, "ext on post-inc load", hw); break; }
                addr += (ext_n == 1) ? ext0 : ((ext0 << 13) | ext1);
            }
            switch (op1) {
            case 0: e->r[rd] = sext(mem_read8(e, addr), 8);  if (postinc) e->r[rs] += 1; break;
            case 1: e->r[rd] = mem_read8(e, addr);           if (postinc) e->r[rs] += 1; break;
            case 2: e->r[rd] = sext(mem_read16(e, addr), 16); if (postinc) e->r[rs] += 2; break;
            case 3: e->r[rd] = mem_read16(e, addr);          if (postinc) e->r[rs] += 2; break;
            case 4: e->r[rd] = mem_read32(e, addr);          if (postinc) e->r[rs] += 4; break;
            case 5: mem_write8 (e, addr, (uint8_t)e->r[rd]);  if (postinc) e->r[rs] += 1; break;
            case 6: mem_write16(e, addr, (uint16_t)e->r[rd]); if (postinc) e->r[rs] += 2; break;
            case 7: mem_write32(e, addr, e->r[rd]);          if (postinc) e->r[rs] += 4; break;
            }
            break;
        }
    }

    case 2: {  /* SP-relative; base form scales by size, ext form does not */
        int op1 = (hw >> 10) & 7;
        uint32_t imm6 = (hw >> 4) & 0x3F;
        uint32_t off;
        if (ext_n == 0) {
            static const uint8_t scale[8] = {1,1,2,2,4,1,2,4};
            off = imm6 * scale[op1];
        } else if (ext_n == 1) off = (ext0 << 6) | imm6;
        else off = (ext0 << 19) | (ext1 << 6) | imm6;
        uint32_t addr = e->sp + off;
        switch (op1) {
        case 0: e->r[rd] = sext(mem_read8(e, addr), 8); break;
        case 1: e->r[rd] = mem_read8(e, addr); break;
        case 2: e->r[rd] = sext(mem_read16(e, addr), 16); break;
        case 3: e->r[rd] = mem_read16(e, addr); break;
        case 4: e->r[rd] = mem_read32(e, addr); break;
        case 5: mem_write8 (e, addr, (uint8_t)e->r[rd]); break;
        case 6: mem_write16(e, addr, (uint16_t)e->r[rd]); break;
        case 7: mem_write32(e, addr, e->r[rd]); break;
        }
        break;
    }

    case 3: {  /* imm6 ALU */
        int op1 = (hw >> 10) & 7;
        uint32_t imm6 = (hw >> 4) & 0x3F;
        uint32_t imm;
        if (ext_n == 0)
            imm = (op1 == 0 || op1 == 1) ? imm6 : sext(imm6, 6);
        else if (ext_n == 1) {
            imm = (ext0 << 6) | imm6;
            if (!(op1 == 0 || op1 == 1)) imm = sext(imm, 19);
        } else
            imm = (ext0 << 19) | (ext1 << 6) | imm6;
        switch (op1) {
        case 0: e->r[rd] = add_op(e, e->r[rd], imm); break;
        case 1: e->r[rd] = sub_op(e, e->r[rd], imm); break;
        case 2: cmp_op(e, e->r[rd], imm); break;
        case 3: e->r[rd] = imm; break;                       /* ld.w / xld.w imm */
        case 4: e->r[rd] = log_flags(e, e->r[rd] & imm); break;
        case 5: e->r[rd] = log_flags(e, e->r[rd] | imm); break;
        case 6: e->r[rd] = log_flags(e, e->r[rd] ^ imm); break;
        case 7: e->r[rd] = log_flags(e, ~imm); break;
        }
        break;
    }

    case 4: {
        int op1 = (hw >> 10) & 7, op2 = (hw >> 8) & 3;
        uint32_t imm4 = (hw >> 4) & 0xF;
        if (op1 == 0) { e->sp += (uint32_t)(hw & 0x3FF) * 4; break; }
        if (op1 == 1) { e->sp -= (uint32_t)(hw & 0x3FF) * 4; break; }
        uint32_t amt = (op2 == 0) ? imm4 : e->r[rs];
        switch (op1) {
        case 2: e->r[rd] = srl_op(e, e->r[rd], amt); break;
        case 3: e->r[rd] = sl_op (e, e->r[rd], amt); break;
        case 4:
            if (op2 == 2) { uint32_t v = e->r[rs];
                e->r[rd] = (v << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | (v >> 24); }
            else e->r[rd] = sra_op(e, e->r[rd], amt);
            break;
        case 5: e->r[rd] = sl_op(e, e->r[rd], amt); break;   /* sla */
        case 6:
            if (op2 == 2) { uint32_t v = e->r[rs];           /* swaph */
                e->r[rd] = ((v & 0x00FF0000) << 8) | ((v >> 8) & 0x00FF0000) |
                           ((v & 0x000000FF) << 8) | ((v >> 8) & 0x000000FF); }
            else e->r[rd] = rr_op(e, e->r[rd], amt);
            break;
        case 7: e->r[rd] = rl_op(e, e->r[rd], amt); break;
        }
        break;
    }

    case 5: {
        int op1 = (hw >> 10) & 7, op2 = (hw >> 8) & 3;
        switch (op1 * 4 + op2) {
        case 0*4+0: sreg_write(e, hw & 0xF, e->r[rs], iaddr); break;       /* ld.w %sd,%rs */
        case 0*4+1: e->r[rd] = sext(e->r[rs] & 0xFF, 8); break;            /* ld.b rd,rs */
        case 0*4+2: e->alr = (uint32_t)((int32_t)sext(e->r[rd] & 0xFFFF, 16) *
                                        (int32_t)sext(e->r[rs] & 0xFFFF, 16)); break; /* mlt.h */
        case 1*4+0: e->r[rd] = sreg_read(e, rs, iaddr); break;             /* ld.w rd,%ss */
        case 1*4+1: e->r[rd] = e->r[rs] & 0xFF; break;                     /* ld.ub */
        case 1*4+2: e->alr = (e->r[rd] & 0xFFFF) * (e->r[rs] & 0xFFFF); break; /* mltu.h */
        case 2*4+0: case 3*4+0: case 4*4+0: case 5*4+0: {                  /* bit ops */
            uint32_t addr = e->r[rs];
            if (ext_n) addr += (ext_n == 1) ? ext0 : ((ext0 << 13) | ext1);
            int pos = hw & 7;
            uint8_t b = mem_read8(e, addr);
            switch (op1) {
            case 2: e->f_z = ((b >> pos) & 1) == 0; break;                 /* btst */
            case 3: mem_write8(e, addr, b & ~(1u << pos)); break;          /* bclr */
            case 4: mem_write8(e, addr, b |  (1u << pos)); break;          /* bset */
            case 5: mem_write8(e, addr, b ^  (1u << pos)); break;          /* bnot */
            }
            break; }
        case 2*4+1: e->r[rd] = sext(e->r[rs] & 0xFFFF, 16); break;         /* ld.h */
        case 2*4+2: { int64_t p = (int64_t)(int32_t)e->r[rd] * (int32_t)e->r[rs];
                      e->alr = (uint32_t)p; e->ahr = (uint32_t)((uint64_t)p >> 32); break; } /* mlt.w */
        case 3*4+1: e->r[rd] = e->r[rs] & 0xFFFF; break;                   /* ld.uh */
        case 3*4+2: { uint64_t p = (uint64_t)e->r[rd] * e->r[rs];
                      e->alr = (uint32_t)p; e->ahr = (uint32_t)(p >> 32); break; }           /* mltu.w */
        case 4*4+1: case 5*4+1:                                            /* ld.c: coprocessor */
            fprintf(stderr, "[cpu] coprocessor ld.c at %08x ignored\n", iaddr); break;
        case 6*4+0: e->r[rd] = adc_op(e, e->r[rd], e->r[rs]); break;
        case 7*4+0: e->r[rd] = sbc_op(e, e->r[rd], e->r[rs]); break;
        case 7*4+3: {  /* class 5 (2): do.c / psrset / psrclr */
            int op3 = (hw >> 6) & 3, imm5 = hw & 0x1F;
            if (op3 == 0) { fprintf(stderr, "[cpu] do.c at %08x ignored\n", iaddr); break; }
            if (op3 == 1 || op3 == 2) {
                int val = (op3 == 1);
                switch (imm5) {
                case 0: e->f_n = val; break;
                case 1: e->f_z = val; break;
                case 2: e->f_v = val; break;
                case 3: e->f_c = val; break;
                case 4: e->f_ie = val; break;
                default: fprintf(stderr, "[cpu] psr%s bit %d at %08x ignored\n",
                                 val ? "set" : "clr", imm5, iaddr); break;
                }
                break;
            }
            stop(e, iaddr, "bad class5(2)", hw); break; }
        default: stop(e, iaddr, "bad class5 encoding", hw); break;
        }
        break;
    }

    default:
        stop(e, iaddr, "unreachable class", hw);
        break;
    }
    (void)in_slot;
}

void cpu_reset(Emu *e)
{
    memset(e->r, 0, sizeof e->r);
    e->sp = 0; e->alr = e->ahr = 0;
    e->ttbr = e->dev.ttbr_reset;
    e->idir = e->dbbr = 0;
    e->f_n = e->f_z = e->f_v = e->f_c = e->f_ie = 0;
    e->il = 0; e->psr_rest = 0;
    e->ext_count = 0;
    e->halted = false; e->halt_hard = false;
    e->stopped = false;
    if (e->dev.nfc_pn512) pn512_power_on(e);
    e->pc = mem_read32(e, e->ttbr);   /* reset vector */
}

void cpu_step(Emu *e)
{
    if (e->stopped) return;
    /* Hold the NFC field for the complete touch routine. */
    if (e->dev.bingo_open_pc && e->auto_touch && !e->halted) {
        if (e->pc == e->dev.bingo_open_pc && !e->nfc_probe_at) {
            pn512_probe_set(e, 1);
            e->nfc_probe_until = e->cycles +
                (uint64_t)((e->cmu.mclk_hz > 0 ? e->cmu.mclk_hz
                                               : e->dev.osc3_hz) * 120.0);
        } else if (e->nfc_probe_at &&
                   (e->pc == e->dev.bingo_done_pc[0] ||
                    e->pc == e->dev.bingo_done_pc[1])) {
            pn512_probe_set(e, 0);
        }
    }
    /* Skip PC-hook work when no hooks are registered. */
    if (e->n_pc_hook && !e->halted) {
        for (int i = 0; i < e->n_pc_hook; i++) {
            if (e->pc != e->pc_hook[i]) continue;
            /* r4/r5 may hold useful loop state despite being scratch registers. */
            fprintf(stderr, "[pc%c] %-16s %08x t=%.3fs  r0=%08x r1=%08x r2=%08x r3=%08x"
                            " r4=%08x r5=%08x r6=%08x r7=%08x r8=%08x r9=%08x"
                            " sp=%08x ra=%08x\n",
                    e->core_id ? 'B' : 'A', e->pc_hook_name[i], e->pc, e->emu_secs,
                    e->r[0], e->r[1], e->r[2], e->r[3],
                    e->r[4], e->r[5],
                    e->r[6], e->r[7], e->r[8], e->r[9],
                    e->sp, mem_read32(e, e->sp));
            /* Collapse repeated PCs so wait loops do not erase the earlier path. */
            if (e->pc_hook_ring[i]) {
                char c = e->core_id ? 'B' : 'A';
                uint32_t shown[320];
                int nshown = 0;
                fprintf(stderr, "[pt%c] path in, newest first (loops collapsed):\n", c);
                for (int k = 1; k <= TRACE_RING_N && nshown < 320; k++) {
                    uint32_t a = e->trace_ring[(e->trace_ring_pos - k) & TRACE_RING_M];
                    if (!a) continue;
                    int dup = 0;
                    for (int j = 0; j < nshown; j++) if (shown[j] == a) { dup = 1; break; }
                    if (dup) continue;
                    shown[nshown++] = a;
                    if ((nshown & 3) == 1) fprintf(stderr, "[pt%c] ", c);
                    fprintf(stderr, " %08x %-24s", a, disasm_sym_for(a));
                    if ((nshown & 3) == 0) fprintf(stderr, "\n");
                }
                fprintf(stderr, "\n");
            }
        }
    }
    /* Trace the path from session completion to the result screen. */
    if (e->post_ir_trace > 0 && !e->halted) {
        uint32_t b = e->pc & 0xFFFFFFF0u;
        if (b != e->post_ir_last) {
            e->post_ir_last = b;
            e->post_ir_trace--;
            fprintf(stderr, "[irpost%c] %08x %s\n", e->core_id ? 'B' : 'A', b, disasm_sym_for(b));
        }
    }
    if (e->halted) {
        e->cycles += 16;
        if (e->pending_irq || e->wake_req || (!e->halt_hard && e->cycles >= e->wake_at)) {
            e->halted = false;
            e->halt_wakes++;
        } else
            return;
    }
    /* interrupt accept point (never between ext and target: ext consumed inside step_one) */
    if (e->pending_irq && e->f_ie && e->pending_level > e->il) {
        if (e->pending_irq < 80) e->irq_taken[e->pending_irq]++;
        uint32_t vec = e->pending_irq;
        push32(e, e->pc);
        push32(e, psr_pack(e));
        e->f_ie = 0;
        e->il = e->pending_level;
        e->pc = mem_read32(e, e->ttbr + vec * 4);
        e->pending_irq = 0;
    }
    step_one(e, 0);
}
