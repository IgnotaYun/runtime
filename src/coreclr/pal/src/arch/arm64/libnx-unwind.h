// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
//
// Native DWARF CFI unwinder for libnx, shared between the PAL (libnxstubs.cpp) and
// the standalone register-restore test.
//
// libnx links libgcc, which owns the .eh_frame tables and exports _Unwind_Find_FDE,
// but it does not link libunwind and has neither ucontext nor dl_iterate_phdr.
// LibnxVirtualUnwindCfi evaluates the CFA program of the FDE that libgcc reports for
// the current PC, restoring the caller's general, callee-saved and NEON registers
// exactly as the Unix libunwind path does. A frame whose CFI cannot be decoded is
// reported as a failure and the context is left untouched (no frame-pointer
// heuristic, no fabricated progress).
//
// LibnxUnwindContext mirrors the fields of the aarch64 PAL CONTEXT the unwinder
// touches, so the PAL can call it with a plain cast.
#ifndef LIBNX_UNWIND_H
#define LIBNX_UNWIND_H

#include <stdint.h>
#include <string.h>

struct LibnxUnwindContext
{
    uint32_t ContextFlags;
    uint32_t Cpsr;
    uint64_t X[29];
    uint64_t Fp, Lr, Sp, Pc;
    struct { uint64_t Low, High; } V[32];
};

struct LibnxEhBases { void *tbase; void *dbase; void *func; };
extern "C" const void *LibnxFindFde(void *pc, struct LibnxEhBases *bases) __asm__("_Unwind_Find_FDE");

namespace libnxcfi
{
    enum
    {
        LIBNX_CFI_UNDEF = 0,
        LIBNX_CFI_SAME,
        LIBNX_CFI_OFFSET,
        LIBNX_CFI_VAL_OFFSET,
        LIBNX_CFI_REGISTER,
        LIBNX_CFI_UNSUPPORTED
    };

    enum
    {
        LIBNX_DW_SP = 31,
        LIBNX_DW_RA = 30,
        LIBNX_DW_V0 = 64,
        LIBNX_DW_V31 = 95,
        LIBNX_DW_COUNT = 128
    };

    struct LibnxRule { int kind; long long offset; unsigned reg; };
    struct LibnxState
    {
        bool cfaIsReg;
        unsigned cfaReg;
        long long cfaOff;
        LibnxRule rules[LIBNX_DW_COUNT];
    };
    struct LibnxCur { const uint8_t *p; const uint8_t *end; };

    static bool LnxRead(LibnxCur &c, int n, uint64_t *out)
    {
        if (c.p + n > c.end) return false;
        uint64_t v = 0;
        for (int i = 0; i < n; i++) v |= (uint64_t)c.p[i] << (8 * i);
        c.p += n; *out = v; return true;
    }

    static bool LnxUleb(LibnxCur &c, uint64_t *out)
    {
        uint64_t v = 0; int shift = 0;
        while (c.p < c.end)
        {
            uint8_t b = *c.p++;
            v |= (uint64_t)(b & 0x7f) << shift;
            if (!(b & 0x80)) { *out = v; return true; }
            shift += 7;
            if (shift >= 64) return false;
        }
        return false;
    }

    static bool LnxSleb(LibnxCur &c, long long *out)
    {
        long long v = 0; int shift = 0; uint8_t b = 0;
        while (c.p < c.end)
        {
            b = *c.p++;
            v |= (long long)(b & 0x7f) << shift;
            shift += 7;
            if (!(b & 0x80)) break;
            if (shift >= 64) return false;
        }
        if (shift < 64 && (b & 0x40)) v |= -(1LL << shift);
        *out = v; return true;
    }

    // Reads one DW_EH_PE encoded pointer. `datarel` is the base for datarel encodings.
    // Indirect (0x80) is tolerated and left un-dereferenced: it only appears for
    // personality/LSDA pointers we never consume, never for the FDE location.
    static bool LnxEncoded(LibnxCur &c, unsigned enc, uint64_t datarel, uint64_t *out)
    {
        uint64_t field = (uint64_t)(uintptr_t)c.p;
        uint64_t raw = 0;
        switch (enc & 0x0f)
        {
            case 0x00: if (!LnxRead(c, 8, &raw)) return false; break;
            case 0x01: if (!LnxUleb(c, &raw)) return false; break;
            case 0x02: { uint64_t t; if (!LnxRead(c, 2, &t)) return false; raw = (uint64_t)(long long)(int16_t)t; break; }
            case 0x03: { uint64_t t; if (!LnxRead(c, 4, &t)) return false; raw = (uint64_t)(long long)(int32_t)t; break; }
            case 0x04: if (!LnxRead(c, 8, &raw)) return false; break;
            case 0x09: { long long s; if (!LnxSleb(c, &s)) return false; raw = (uint64_t)s; break; }
            case 0x0a: { uint64_t t; if (!LnxRead(c, 2, &t)) return false; raw = (uint64_t)(long long)(int16_t)t; break; }
            case 0x0b: { uint64_t t; if (!LnxRead(c, 4, &t)) return false; raw = (uint64_t)(long long)(int32_t)t; break; }
            case 0x0c: if (!LnxRead(c, 8, &raw)) return false; break;
            default: return false;
        }
        switch (enc & 0x70)
        {
            case 0x00: break;
            case 0x10: raw += field; break;   // pcrel
            case 0x30: raw += datarel; break; // datarel
            default: return false;            // aligned/textrel/funcrel are never emitted for aarch64 CFI
        }
        *out = raw; return true;
    }

    static uint64_t LnxGetReg(const LibnxUnwindContext *context, unsigned reg)
    {
        if (reg <= 28) return (uint64_t)context->X[reg];
        if (reg == 29) return (uint64_t)context->Fp;
        if (reg == 30) return (uint64_t)context->Lr;
        if (reg == 31) return (uint64_t)context->Sp;
        return 0;
    }

    // Executes a CFI instruction stream. With stopAtPc, only rows whose start
    // location is <= pc are applied (the FDE body, starting at startLoc); otherwise
    // the whole stream runs (the CIE initial instructions, startLoc == 0).
    static bool LnxExecCfi(LibnxCur &c, const uint8_t *end, uint64_t pc, bool stopAtPc, uint64_t startLoc,
                           unsigned char fdeEnc, int codeAlign, long long dataAlign,
                           const LibnxState &initial, LibnxState &state)
    {
        uint64_t loc = startLoc;
        LibnxState save[8];
        int depth = 0;

        while (c.p < end)
        {
            uint8_t op = *c.p++;
            if (op & 0xc0)
            {
                switch (op & 0xc0)
                {
                    case 0x40: // advance_loc
                    {
                        uint64_t delta = (uint64_t)(op & 0x3f) * (uint64_t)codeAlign;
                        if (stopAtPc && loc + delta > pc) return true;
                        loc += delta;
                        break;
                    }
                    case 0x80: // offset: the register is encoded in the opcode, one ULEB operand
                    {
                        unsigned reg = op & 0x3f;
                        uint64_t off;
                        if (!LnxUleb(c, &off) || reg >= LIBNX_DW_COUNT) return false;
                        state.rules[reg].kind = LIBNX_CFI_OFFSET;
                        state.rules[reg].offset = (long long)off * dataAlign;
                        break;
                    }
                    default: // restore
                    {
                        unsigned reg = op & 0x3f;
                        if (reg >= LIBNX_DW_COUNT) return false;
                        state.rules[reg] = initial.rules[reg];
                        break;
                    }
                }
                continue;
            }
            switch (op)
            {
                case 0x00: break; // nop
                case 0x01: { uint64_t a; if (!LnxEncoded(c, fdeEnc, 0, &a)) return false; loc = a; break; } // set_loc
                case 0x02: { if (c.p >= end) return false; uint64_t d = (uint64_t)*c.p++ * (uint64_t)codeAlign; if (stopAtPc && loc + d > pc) return true; loc += d; break; }
                case 0x03: { uint64_t t; if (!LnxRead(c, 2, &t)) return false; uint64_t d = t * (uint64_t)codeAlign; if (stopAtPc && loc + d > pc) return true; loc += d; break; }
                case 0x04: { uint64_t t; if (!LnxRead(c, 4, &t)) return false; uint64_t d = t * (uint64_t)codeAlign; if (stopAtPc && loc + d > pc) return true; loc += d; break; }
                case 0x05: { uint64_t reg, off; if (!LnxUleb(c, &reg) || !LnxUleb(c, &off) || reg >= LIBNX_DW_COUNT) return false; state.rules[reg].kind = LIBNX_CFI_OFFSET; state.rules[reg].offset = (long long)off * dataAlign; break; }
                case 0x06: { uint64_t reg; if (!LnxUleb(c, &reg) || reg >= LIBNX_DW_COUNT) return false; state.rules[reg] = initial.rules[reg]; break; }
                case 0x07: { uint64_t reg; if (!LnxUleb(c, &reg) || reg >= LIBNX_DW_COUNT) return false; state.rules[reg].kind = LIBNX_CFI_UNDEF; break; }
                case 0x08: { uint64_t reg; if (!LnxUleb(c, &reg) || reg >= LIBNX_DW_COUNT) return false; state.rules[reg].kind = LIBNX_CFI_SAME; break; }
                case 0x09: { uint64_t r1, r2; if (!LnxUleb(c, &r1) || !LnxUleb(c, &r2) || r1 >= LIBNX_DW_COUNT) return false; state.rules[r1].kind = LIBNX_CFI_REGISTER; state.rules[r1].reg = (unsigned)r2; break; }
                case 0x0a: { if (depth >= 8) return false; save[depth++] = state; break; }
                case 0x0b: { if (depth <= 0) return false; state = save[--depth]; break; }
                case 0x0c: { uint64_t reg, off; if (!LnxUleb(c, &reg) || !LnxUleb(c, &off)) return false; state.cfaIsReg = true; state.cfaReg = (unsigned)reg; state.cfaOff = (long long)off; break; }
                case 0x0d: { uint64_t reg; if (!LnxUleb(c, &reg)) return false; state.cfaIsReg = true; state.cfaReg = (unsigned)reg; break; }
                case 0x0e: { uint64_t off; if (!LnxUleb(c, &off)) return false; state.cfaIsReg = true; state.cfaOff = (long long)off; break; }
                case 0x0f: { uint64_t reg, len; if (!LnxUleb(c, &reg) || !LnxUleb(c, &len) || reg >= LIBNX_DW_COUNT || c.p + len > end) return false; c.p += len; state.rules[reg].kind = LIBNX_CFI_UNSUPPORTED; break; } // def_cfa_expression
                case 0x10: { uint64_t reg, len; if (!LnxUleb(c, &reg) || !LnxUleb(c, &len) || reg >= LIBNX_DW_COUNT || c.p + len > end) return false; c.p += len; state.rules[reg].kind = LIBNX_CFI_UNSUPPORTED; break; } // expression
                case 0x11: { uint64_t reg; long long off; if (!LnxUleb(c, &reg) || !LnxSleb(c, &off) || reg >= LIBNX_DW_COUNT) return false; state.rules[reg].kind = LIBNX_CFI_OFFSET; state.rules[reg].offset = off * dataAlign; break; } // offset_extended_sf
                case 0x12: { uint64_t reg; long long off; if (!LnxUleb(c, &reg) || !LnxSleb(c, &off)) return false; state.cfaIsReg = true; state.cfaReg = (unsigned)reg; state.cfaOff = off * dataAlign; break; } // def_cfa_sf
                case 0x13: { long long off; if (!LnxSleb(c, &off)) return false; state.cfaIsReg = true; state.cfaOff = off * dataAlign; break; } // def_cfa_offset_sf
                case 0x14: { uint64_t reg, off; if (!LnxUleb(c, &reg) || !LnxUleb(c, &off) || reg >= LIBNX_DW_COUNT) return false; state.rules[reg].kind = LIBNX_CFI_VAL_OFFSET; state.rules[reg].offset = (long long)off * dataAlign; break; } // val_offset
                case 0x15: { uint64_t reg; long long off; if (!LnxUleb(c, &reg) || !LnxSleb(c, &off) || reg >= LIBNX_DW_COUNT) return false; state.rules[reg].kind = LIBNX_CFI_VAL_OFFSET; state.rules[reg].offset = off * dataAlign; break; } // val_offset_sf
                case 0x16: { uint64_t reg, len; if (!LnxUleb(c, &reg) || !LnxUleb(c, &len) || reg >= LIBNX_DW_COUNT || c.p + len > end) return false; c.p += len; state.rules[reg].kind = LIBNX_CFI_UNSUPPORTED; break; } // val_expression
                default: return false;
            }
        }
        return true;
    }

    static bool LnxReadRule(const LibnxUnwindContext *context, const LibnxState &state, uint64_t cfa, unsigned reg, uint64_t *out)
    {
        const LibnxRule &r = state.rules[reg];
        switch (r.kind)
        {
            case LIBNX_CFI_UNDEF: *out = 0; return true;
            case LIBNX_CFI_SAME: *out = LnxGetReg(context, reg); return true;
            case LIBNX_CFI_OFFSET: memcpy(out, (const void *)(uintptr_t)(cfa + (uint64_t)r.offset), sizeof(uint64_t)); return true;
            case LIBNX_CFI_VAL_OFFSET: *out = cfa + (uint64_t)r.offset; return true;
            case LIBNX_CFI_REGISTER: *out = LnxGetReg(context, r.reg); return true;
            default: return false;
        }
    }

    inline bool LibnxVirtualUnwindCfi(LibnxUnwindContext *context)
    {
        struct LibnxEhBases bases = { nullptr, nullptr, nullptr };
        const uint8_t *fde = (const uint8_t *)LibnxFindFde((void *)(uintptr_t)context->Pc, &bases);
        if (fde == nullptr) return false;

        uint32_t l32; memcpy(&l32, fde, 4);
        uint64_t len; int lenSize;
        if (l32 == 0xffffffffu) { uint64_t l64; memcpy(&l64, fde + 4, 8); len = l64; lenSize = 12; }
        else { len = l32; lenSize = 4; }
        if (len == 0) return false;
        const uint8_t *fdeEnd = fde + lenSize + len;

        uint32_t cieDelta; memcpy(&cieDelta, fde + lenSize, 4);
        if (cieDelta == 0) return false;
        const uint8_t *cie = (fde + lenSize) - cieDelta;
        if (cie >= fde) return false;

        uint32_t cl; memcpy(&cl, cie, 4);
        const uint8_t *cieEnd = cie + 4 + cl;

        LibnxCur cc; cc.p = cie + 4; cc.end = cieEnd;
        uint64_t cieId; if (!LnxRead(cc, 4, &cieId) || cieId != 0) return false;
        uint64_t version; if (!LnxRead(cc, 1, &version) || version == 0 || version > 4) return false;
        const char *aug = (const char *)cc.p;
        while (cc.p < cc.end && *cc.p) cc.p++;
        if (cc.p >= cc.end) return false;
        cc.p++;
        if (version >= 4) { if (cc.p + 2 > cc.end) return false; cc.p += 2; }
        uint64_t codeAlignU, raRegU; long long dataAlign;
        if (!LnxUleb(cc, &codeAlignU) || !LnxSleb(cc, &dataAlign)) return false;
        if (version == 1) { uint64_t t; if (!LnxRead(cc, 1, &t)) return false; raRegU = t; }
        else if (!LnxUleb(cc, &raRegU)) return false;
        if (codeAlignU == 0 || raRegU >= LIBNX_DW_COUNT) return false;

        unsigned char fdeEnc = 0x00;
        bool hasZ = (aug[0] == 'z');
        if (hasZ)
        {
            uint64_t augLen;
            if (!LnxUleb(cc, &augLen)) return false;
            if (cc.p + augLen > cc.end) return false;
            LibnxCur ac; ac.p = cc.p; ac.end = cc.p + augLen;
            for (const char *ch = aug + 1; *ch; ++ch)
            {
                if (*ch == 'R') { if (ac.p >= ac.end) return false; fdeEnc = *ac.p++; }
                else if (*ch == 'L') { if (ac.p >= ac.end) return false; ac.p++; }
                else if (*ch == 'P') { if (ac.p >= ac.end) return false; unsigned char enc = *ac.p++; uint64_t d; if (!LnxEncoded(ac, enc, 0, &d)) return false; }
                else if (*ch == 'S') { }
                else return false; // unknown augmentation data layout
            }
            cc.p += augLen;
        }
        else if (aug[0] != '\0') return false;

        LibnxState state;
        state.cfaIsReg = false; state.cfaReg = LIBNX_DW_SP; state.cfaOff = 0;
        // DWARF starts every register undefined, but the AAPCS64 callee-saved set
        // (x19-x30 and d8-d15) is preserved across a call even when a frame does not
        // save it, so default it to same_value rather than dropping the value.
        for (unsigned i = 0; i < LIBNX_DW_COUNT; i++)
        {
            bool calleeSaved = (i >= 19 && i <= 30) || (i >= LIBNX_DW_V0 + 8 && i <= LIBNX_DW_V0 + 15);
            state.rules[i].kind = calleeSaved ? LIBNX_CFI_SAME : LIBNX_CFI_UNDEF;
            state.rules[i].offset = 0;
            state.rules[i].reg = 0;
        }
        LibnxCur ci; ci.p = cc.p; ci.end = cieEnd;
        if (!LnxExecCfi(ci, cieEnd, 0, false, 0, fdeEnc, (int)codeAlignU, dataAlign, state, state)) return false;
        LibnxState initial = state;

        LibnxCur fc; fc.p = fde + lenSize + 4; fc.end = fdeEnd;
        uint64_t initialLoc, range;
        if (!LnxEncoded(fc, fdeEnc, 0, &initialLoc)) return false;
        if (!LnxEncoded(fc, (unsigned char)(fdeEnc & 0x0f), 0, &range)) return false;
        if (hasZ)
        {
            // A z-augmented FDE carries its own augmentation-data-length field (and the
            // LSDA pointer inside it) before the CFI instructions.
            uint64_t augDataLen;
            if (!LnxUleb(fc, &augDataLen)) return false;
            if (fc.p + augDataLen > fdeEnd) return false;
            fc.p += augDataLen;
        }
        if ((uint64_t)context->Pc < initialLoc || (uint64_t)context->Pc >= initialLoc + range) return false;
        if (!LnxExecCfi(fc, fdeEnd, (uint64_t)context->Pc, true, initialLoc, fdeEnc, (int)codeAlignU, dataAlign, initial, state)) return false;
        if (!state.cfaIsReg) return false;

        uint64_t cfa = LnxGetReg(context, state.cfaReg) + (uint64_t)state.cfaOff;
        if (cfa == 0) return false;

        uint64_t newX[29], newFp, newLr, newPc;
        for (unsigned i = 0; i <= 28; i++) if (!LnxReadRule(context, state, cfa, i, &newX[i])) return false;
        if (!LnxReadRule(context, state, cfa, 29, &newFp)) return false;
        if (!LnxReadRule(context, state, cfa, 30, &newLr)) return false;
        if (!LnxReadRule(context, state, cfa, (unsigned)raRegU, &newPc)) return false;

        uint8_t newV[32][16];
        for (unsigned v = 0; v < 32; v++)
        {
            const LibnxRule &rule = state.rules[LIBNX_DW_V0 + v];
            switch (rule.kind)
            {
                case LIBNX_CFI_UNDEF: memset(newV[v], 0, 16); break;
                case LIBNX_CFI_SAME: memcpy(newV[v], &context->V[v], 16); break;
                case LIBNX_CFI_OFFSET: memcpy(newV[v], (const void *)(uintptr_t)(cfa + (uint64_t)rule.offset), 16); break;
                case LIBNX_CFI_REGISTER:
                    if (rule.reg >= LIBNX_DW_V0 && rule.reg <= LIBNX_DW_V31) memcpy(newV[v], &context->V[rule.reg - LIBNX_DW_V0], 16);
                    else return false;
                    break;
                default: return false;
            }
        }

        for (unsigned i = 0; i <= 28; i++) context->X[i] = newX[i];
        context->Fp = newFp;
        context->Lr = newLr;
        context->Pc = newPc;
        context->Sp = cfa;
        for (unsigned v = 0; v < 32; v++) memcpy(&context->V[v], newV[v], 16);
        return true;
    }
}

#endif // LIBNX_UNWIND_H
