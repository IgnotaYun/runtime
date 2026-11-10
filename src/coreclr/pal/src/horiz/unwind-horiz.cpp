// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// DWARF CFI unwinder for horiz (arm64). See unwind-horiz.h.

#include "unwind-horiz.h"

#include <stddef.h>
#include <string.h>

struct HorizEhBases
{
    void* tbase;
    void* dbase;
    void* func;
};

// libgcc's FDE lookup over the .eh_frame of the application.
extern "C" const void* _Unwind_Find_FDE(void* pc, HorizEhBases* bases);

namespace horizcfi
{
    // DWARF register numbers for arm64
    const unsigned DwarfSp = 31;
    const unsigned DwarfV0 = 64;
    const unsigned DwarfV31 = 95;
    const unsigned DwarfRegisterCount = 128;

    enum RuleKind
    {
        RuleUndefined,
        RuleSameValue,
        RuleOffset,
        RuleValOffset,
        RuleRegister,
        RuleUnsupported
    };

    struct Rule
    {
        RuleKind kind;
        int64_t offset;
        unsigned reg;
    };

    enum CfaKind
    {
        CfaUndefined,
        CfaRegisterOffset,
        CfaExpression
    };

    struct State
    {
        CfaKind cfaKind;
        unsigned cfaReg;
        int64_t cfaOffset;
        Rule rules[DwarfRegisterCount];
    };

    struct Cursor
    {
        const uint8_t* p;
        const uint8_t* end;
    };

    struct CieInfo
    {
        uint64_t codeAlign;
        int64_t dataAlign;
        unsigned returnAddressReg;
        uint8_t fdeEncoding;
        bool hasAugmentationData;
    };

    static bool ReadFixed(Cursor& c, int size, uint64_t* out)
    {
        if (c.end - c.p < size)
            return false;

        uint64_t value = 0;
        for (int i = 0; i < size; i++)
            value |= (uint64_t)c.p[i] << (8 * i);

        c.p += size;
        *out = value;
        return true;
    }

    static bool ReadUleb(Cursor& c, uint64_t* out)
    {
        uint64_t value = 0;
        for (int shift = 0; c.p < c.end && shift < 64; shift += 7)
        {
            uint8_t b = *c.p++;
            value |= (uint64_t)(b & 0x7f) << shift;
            if ((b & 0x80) == 0)
            {
                *out = value;
                return true;
            }
        }
        return false;
    }

    static bool ReadSleb(Cursor& c, int64_t* out)
    {
        uint64_t value = 0;
        for (int shift = 0; c.p < c.end && shift < 64; )
        {
            uint8_t b = *c.p++;
            value |= (uint64_t)(b & 0x7f) << shift;
            shift += 7;
            if ((b & 0x80) == 0)
            {
                if (shift < 64 && (b & 0x40) != 0)
                    value |= ~(uint64_t)0 << shift;

                *out = (int64_t)value;
                return true;
            }
        }
        return false;
    }

    // Reads a DW_EH_PE encoded pointer. Indirect pointers (DW_EH_PE_indirect)
    // are not dereferenced; they are only used for the personality routine,
    // which the unwinder skips.
    static bool ReadEncoded(Cursor& c, uint8_t encoding, uint64_t* out)
    {
        uint64_t fieldAddress = (uint64_t)(uintptr_t)c.p;
        uint64_t value;
        uint64_t raw;
        int64_t signedValue;

        switch (encoding & 0x0f)
        {
            case 0x00: // absptr
            case 0x04: // udata8
            case 0x0c: // sdata8
                if (!ReadFixed(c, 8, &value))
                    return false;
                break;
            case 0x01: // uleb128
                if (!ReadUleb(c, &value))
                    return false;
                break;
            case 0x02: // udata2
                if (!ReadFixed(c, 2, &value))
                    return false;
                break;
            case 0x03: // udata4
                if (!ReadFixed(c, 4, &value))
                    return false;
                break;
            case 0x09: // sleb128
                if (!ReadSleb(c, &signedValue))
                    return false;
                value = (uint64_t)signedValue;
                break;
            case 0x0a: // sdata2
                if (!ReadFixed(c, 2, &raw))
                    return false;
                value = (uint64_t)(int64_t)(int16_t)raw;
                break;
            case 0x0b: // sdata4
                if (!ReadFixed(c, 4, &raw))
                    return false;
                value = (uint64_t)(int64_t)(int32_t)raw;
                break;
            default:
                return false;
        }

        switch (encoding & 0x70)
        {
            case 0x00: // absolute
                break;
            case 0x10: // pcrel
                value += fieldAddress;
                break;
            default: // textrel, datarel and funcrel are not used on arm64
                return false;
        }

        *out = value;
        return true;
    }

    static bool GetRegister(const HorizUnwindContext* context, unsigned reg, uint64_t* out)
    {
        if (reg <= 28)
            *out = context->X[reg];
        else if (reg == 29)
            *out = context->Fp;
        else if (reg == 30)
            *out = context->Lr;
        else if (reg == DwarfSp)
            *out = context->Sp;
        else
            return false;

        return true;
    }

    static bool SetRule(State& state, uint64_t reg, RuleKind kind, int64_t offset = 0, unsigned otherReg = 0)
    {
        if (reg >= DwarfRegisterCount)
            return false;

        state.rules[reg].kind = kind;
        state.rules[reg].offset = offset;
        state.rules[reg].reg = otherReg;
        return true;
    }

    static bool SkipBlock(Cursor& c)
    {
        uint64_t length;
        if (!ReadUleb(c, &length) || (uint64_t)(c.end - c.p) < length)
            return false;

        c.p += length;
        return true;
    }

    // Runs CFI instructions. When stopAtPc is set (the FDE instructions), only
    // the rows that start at or before pc are applied; otherwise (the CIE
    // initial instructions) the whole program runs.
    static bool ExecuteCfi(Cursor& c, uint64_t pc, bool stopAtPc, uint64_t location,
                           const CieInfo& cie, const State& initial, State& state)
    {
        const int MaxRememberedStates = 8;
        State remembered[MaxRememberedStates];
        int rememberedCount = 0;

        while (c.p < c.end)
        {
            uint8_t op = *c.p++;
            uint64_t advance = 0;
            uint64_t reg;
            uint64_t operand;
            int64_t signedOperand;

            switch (op & 0xc0)
            {
                case 0x40: // DW_CFA_advance_loc
                    advance = (uint64_t)(op & 0x3f) * cie.codeAlign;
                    break;
                case 0x80: // DW_CFA_offset
                    if (!ReadUleb(c, &operand) || !SetRule(state, op & 0x3f, RuleOffset, (int64_t)operand * cie.dataAlign))
                        return false;
                    continue;
                case 0xc0: // DW_CFA_restore
                    state.rules[op & 0x3f] = initial.rules[op & 0x3f];
                    continue;
                default:
                    switch (op)
                    {
                        case 0x00: // DW_CFA_nop
                            continue;
                        case 0x01: // DW_CFA_set_loc
                            if (!ReadEncoded(c, cie.fdeEncoding, &operand))
                                return false;
                            if (stopAtPc && operand > pc)
                                return true;
                            location = operand;
                            continue;
                        case 0x02: // DW_CFA_advance_loc1
                        case 0x03: // DW_CFA_advance_loc2
                        case 0x04: // DW_CFA_advance_loc4
                            if (!ReadFixed(c, op == 0x02 ? 1 : op == 0x03 ? 2 : 4, &operand))
                                return false;
                            advance = operand * cie.codeAlign;
                            break;
                        case 0x05: // DW_CFA_offset_extended
                            if (!ReadUleb(c, &reg) || !ReadUleb(c, &operand) || !SetRule(state, reg, RuleOffset, (int64_t)operand * cie.dataAlign))
                                return false;
                            continue;
                        case 0x06: // DW_CFA_restore_extended
                            if (!ReadUleb(c, &reg) || reg >= DwarfRegisterCount)
                                return false;
                            state.rules[reg] = initial.rules[reg];
                            continue;
                        case 0x07: // DW_CFA_undefined
                            if (!ReadUleb(c, &reg) || !SetRule(state, reg, RuleUndefined))
                                return false;
                            continue;
                        case 0x08: // DW_CFA_same_value
                            if (!ReadUleb(c, &reg) || !SetRule(state, reg, RuleSameValue))
                                return false;
                            continue;
                        case 0x09: // DW_CFA_register
                            if (!ReadUleb(c, &reg) || !ReadUleb(c, &operand) || operand >= DwarfRegisterCount ||
                                !SetRule(state, reg, RuleRegister, 0, (unsigned)operand))
                                return false;
                            continue;
                        case 0x0a: // DW_CFA_remember_state
                            if (rememberedCount == MaxRememberedStates)
                                return false;
                            remembered[rememberedCount++] = state;
                            continue;
                        case 0x0b: // DW_CFA_restore_state
                            if (rememberedCount == 0)
                                return false;
                            state = remembered[--rememberedCount];
                            continue;
                        case 0x0c: // DW_CFA_def_cfa
                            if (!ReadUleb(c, &reg) || !ReadUleb(c, &operand))
                                return false;
                            state.cfaKind = CfaRegisterOffset;
                            state.cfaReg = (unsigned)reg;
                            state.cfaOffset = (int64_t)operand;
                            continue;
                        case 0x0d: // DW_CFA_def_cfa_register
                            if (!ReadUleb(c, &reg))
                                return false;
                            state.cfaKind = CfaRegisterOffset;
                            state.cfaReg = (unsigned)reg;
                            continue;
                        case 0x0e: // DW_CFA_def_cfa_offset
                            if (!ReadUleb(c, &operand))
                                return false;
                            state.cfaOffset = (int64_t)operand;
                            continue;
                        case 0x0f: // DW_CFA_def_cfa_expression
                            if (!SkipBlock(c))
                                return false;
                            state.cfaKind = CfaExpression;
                            continue;
                        case 0x10: // DW_CFA_expression
                        case 0x16: // DW_CFA_val_expression
                            if (!ReadUleb(c, &reg) || !SkipBlock(c) || !SetRule(state, reg, RuleUnsupported))
                                return false;
                            continue;
                        case 0x11: // DW_CFA_offset_extended_sf
                            if (!ReadUleb(c, &reg) || !ReadSleb(c, &signedOperand) || !SetRule(state, reg, RuleOffset, signedOperand * cie.dataAlign))
                                return false;
                            continue;
                        case 0x12: // DW_CFA_def_cfa_sf
                            if (!ReadUleb(c, &reg) || !ReadSleb(c, &signedOperand))
                                return false;
                            state.cfaKind = CfaRegisterOffset;
                            state.cfaReg = (unsigned)reg;
                            state.cfaOffset = signedOperand * cie.dataAlign;
                            continue;
                        case 0x13: // DW_CFA_def_cfa_offset_sf
                            if (!ReadSleb(c, &signedOperand))
                                return false;
                            state.cfaOffset = signedOperand * cie.dataAlign;
                            continue;
                        case 0x14: // DW_CFA_val_offset
                            if (!ReadUleb(c, &reg) || !ReadUleb(c, &operand) || !SetRule(state, reg, RuleValOffset, (int64_t)operand * cie.dataAlign))
                                return false;
                            continue;
                        case 0x15: // DW_CFA_val_offset_sf
                            if (!ReadUleb(c, &reg) || !ReadSleb(c, &signedOperand) || !SetRule(state, reg, RuleValOffset, signedOperand * cie.dataAlign))
                                return false;
                            continue;
                        case 0x2e: // DW_CFA_GNU_args_size
                            if (!ReadUleb(c, &operand))
                                return false;
                            continue;
                        default:
                            // Includes DW_CFA_AARCH64_negate_ra_state: return
                            // addresses signed with pointer authentication are
                            // not supported.
                            return false;
                    }
            }

            if (stopAtPc && location + advance > pc)
                return true;

            location += advance;
        }

        return true;
    }

    // Reads the length of a CIE or FDE and returns the end of its contents.
    static const uint8_t* ReadEntryLength(const uint8_t* entry, const uint8_t** contents)
    {
        uint32_t length;
        memcpy(&length, entry, sizeof(length));

        // 64-bit DWARF entries are not emitted for .eh_frame
        if (length == 0 || length == 0xffffffff)
            return nullptr;

        *contents = entry + sizeof(length);
        return *contents + length;
    }

    static bool ParseCie(const uint8_t* cie, CieInfo* info, Cursor* instructions)
    {
        const uint8_t* contents;
        const uint8_t* end = ReadEntryLength(cie, &contents);
        if (end == nullptr)
            return false;

        Cursor c = { contents, end };
        uint64_t value;

        if (!ReadFixed(c, 4, &value) || value != 0) // CIE id
            return false;

        if (!ReadFixed(c, 1, &value) || (value != 1 && value != 3 && value != 4)) // version
            return false;
        uint64_t version = value;

        const char* augmentation = (const char*)c.p;
        while (c.p < c.end && *c.p != '\0')
            c.p++;
        if (c.p == c.end)
            return false;
        c.p++;

        if (version == 4)
        {
            // address_size and segment_selector_size
            if (c.end - c.p < 2)
                return false;
            c.p += 2;
        }

        if (!ReadUleb(c, &info->codeAlign) || info->codeAlign == 0 || !ReadSleb(c, &info->dataAlign))
            return false;

        if (version == 1 ? !ReadFixed(c, 1, &value) : !ReadUleb(c, &value))
            return false;
        if (value >= DwarfRegisterCount)
            return false;
        info->returnAddressReg = (unsigned)value;

        info->fdeEncoding = 0; // DW_EH_PE_absptr
        info->hasAugmentationData = augmentation[0] == 'z';
        if (info->hasAugmentationData)
        {
            uint64_t length;
            if (!ReadUleb(c, &length) || (uint64_t)(c.end - c.p) < length)
                return false;

            Cursor data = { c.p, c.p + length };
            for (const char* a = augmentation + 1; *a != '\0'; a++)
            {
                switch (*a)
                {
                    case 'R': // FDE pointer encoding
                        if (!ReadFixed(data, 1, &value))
                            return false;
                        info->fdeEncoding = (uint8_t)value;
                        break;
                    case 'L': // LSDA encoding
                        if (!ReadFixed(data, 1, &value))
                            return false;
                        break;
                    case 'P': // personality routine
                        if (!ReadFixed(data, 1, &value) || !ReadEncoded(data, (uint8_t)value, &value))
                            return false;
                        break;
                    case 'S': // signal frame
                    case 'B': // arm64 B-key pointer authentication
                        break;
                    default:
                        return false;
                }
            }
            c.p += length;
        }
        else if (augmentation[0] != '\0')
        {
            return false;
        }

        *instructions = c;
        return true;
    }

    // Reads the caller's value of an integer register.
    static bool RecoverRegister(const HorizUnwindContext* context, const State& state, uint64_t cfa, unsigned reg, uint64_t* out)
    {
        const Rule& rule = state.rules[reg];
        switch (rule.kind)
        {
            case RuleUndefined:
                *out = 0;
                return true;
            case RuleSameValue:
                return GetRegister(context, reg, out);
            case RuleOffset:
                memcpy(out, (const void*)(uintptr_t)(cfa + rule.offset), sizeof(uint64_t));
                return true;
            case RuleValOffset:
                *out = cfa + rule.offset;
                return true;
            case RuleRegister:
                return GetRegister(context, rule.reg, out);
            default:
                return false;
        }
    }

    bool HorizVirtualUnwindCfi(HorizUnwindContext* context, bool exactPc, HorizUnwindSavedLocations* savedLocations)
    {
        // A return address points after the call, which may be the first
        // instruction of the next function or of a different CFI row.
        uint64_t pc = exactPc ? context->Pc : context->Pc - 1;

        HorizEhBases bases = { nullptr, nullptr, nullptr };
        const uint8_t* fde = (const uint8_t*)_Unwind_Find_FDE((void*)(uintptr_t)pc, &bases);
        if (fde == nullptr)
            return false;

        const uint8_t* fdeContents;
        const uint8_t* fdeEnd = ReadEntryLength(fde, &fdeContents);
        if (fdeEnd == nullptr)
            return false;

        uint32_t ciePointer;
        memcpy(&ciePointer, fdeContents, sizeof(ciePointer));
        if (ciePointer == 0 || ciePointer > (uintptr_t)fdeContents)
            return false;

        CieInfo cie;
        Cursor cieInstructions;
        if (!ParseCie(fdeContents - ciePointer, &cie, &cieInstructions))
            return false;

        State state;
        state.cfaKind = CfaUndefined;
        state.cfaReg = DwarfSp;
        state.cfaOffset = 0;

        // DWARF starts every register undefined, but the callee-saved registers
        // (x19-x30 and d8-d15) keep their value across a call even when a frame
        // does not save them.
        for (unsigned i = 0; i < DwarfRegisterCount; i++)
        {
            bool calleeSaved = (i >= 19 && i <= 30) || (i >= DwarfV0 + 8 && i <= DwarfV0 + 15);
            SetRule(state, i, calleeSaved ? RuleSameValue : RuleUndefined);
        }

        if (!ExecuteCfi(cieInstructions, 0, false, 0, cie, state, state))
            return false;

        State initial = state;

        Cursor c = { fdeContents + sizeof(ciePointer), fdeEnd };
        uint64_t start;
        uint64_t range;
        if (!ReadEncoded(c, cie.fdeEncoding, &start) || !ReadEncoded(c, cie.fdeEncoding & 0x0f, &range))
            return false;

        if (cie.hasAugmentationData && !SkipBlock(c))
            return false;

        if (pc < start || pc - start >= range)
            return false;

        if (!ExecuteCfi(c, pc, true, start, cie, initial, state))
            return false;

        if (state.cfaKind != CfaRegisterOffset)
            return false;

        uint64_t cfa;
        if (!GetRegister(context, state.cfaReg, &cfa))
            return false;
        cfa += state.cfaOffset;

        uint64_t x[29];
        uint64_t fp;
        uint64_t lr;
        uint64_t returnAddress;
        for (unsigned i = 0; i <= 28; i++)
        {
            if (!RecoverRegister(context, state, cfa, i, &x[i]))
                return false;
        }

        if (!RecoverRegister(context, state, cfa, 29, &fp) ||
            !RecoverRegister(context, state, cfa, 30, &lr) ||
            !RecoverRegister(context, state, cfa, cie.returnAddressReg, &returnAddress))
            return false;

        // Only the low 64 bits of v8-v15 are preserved across calls, and frames
        // save just those (as d8-d15).
        uint64_t v[32][2];
        for (unsigned i = 0; i < 32; i++)
        {
            const Rule& rule = state.rules[DwarfV0 + i];
            switch (rule.kind)
            {
                case RuleUndefined:
                    v[i][0] = 0;
                    v[i][1] = 0;
                    break;
                case RuleSameValue:
                    v[i][0] = context->V[i].Low;
                    v[i][1] = context->V[i].High;
                    break;
                case RuleOffset:
                    memcpy(&v[i][0], (const void*)(uintptr_t)(cfa + rule.offset), sizeof(uint64_t));
                    v[i][1] = 0;
                    break;
                case RuleRegister:
                    if (rule.reg < DwarfV0 || rule.reg > DwarfV31)
                        return false;
                    v[i][0] = context->V[rule.reg - DwarfV0].Low;
                    v[i][1] = context->V[rule.reg - DwarfV0].High;
                    break;
                default:
                    return false;
            }
        }

        if (savedLocations != nullptr)
        {
            for (unsigned i = 19; i <= 30; i++)
            {
                if (state.rules[i].kind != RuleOffset)
                    continue;

                uint64_t* location = (uint64_t*)(uintptr_t)(cfa + state.rules[i].offset);
                if (i == 29)
                    savedLocations->Fp = location;
                else if (i == 30)
                    savedLocations->Lr = location;
                else
                    savedLocations->X19ToX28[i - 19] = location;
            }

            for (unsigned i = 8; i <= 15; i++)
            {
                if (state.rules[DwarfV0 + i].kind == RuleOffset)
                    savedLocations->D8ToD15[i - 8] = (uint64_t*)(uintptr_t)(cfa + state.rules[DwarfV0 + i].offset);
            }
        }

        for (unsigned i = 0; i <= 28; i++)
            context->X[i] = x[i];

        context->Fp = fp;
        context->Lr = lr;
        context->Sp = cfa;
        context->Pc = returnAddress;

        for (unsigned i = 0; i < 32; i++)
        {
            context->V[i].Low = v[i][0];
            context->V[i].High = v[i][1];
        }

        return true;
    }
}
