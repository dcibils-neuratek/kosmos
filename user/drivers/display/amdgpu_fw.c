/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The CP's and RLC's firmware in the image, when the build carries it
 * (`amdgpu_fw.h`). Each file is between two labels in .rodata; nothing is
 * weak, so a build without the files has no symbols to resolve at all.
 */

#include "amdgpu_fw.h"

#ifdef AMDGPU_FW_DIR

#define FW(sym, file)                                                   \
    ".balign 4\n"                                                       \
    ".global " sym "\n" sym ":\n"                                       \
    ".incbin \"" AMDGPU_FW_DIR "/" file "\"\n"                          \
    ".global " sym "_end\n" sym "_end:\n"

__asm__(".section .rodata\n"
        FW("amdgpu_fw_tahiti_pfp", "tahiti_pfp.bin")
        FW("amdgpu_fw_tahiti_me", "tahiti_me.bin")
        FW("amdgpu_fw_tahiti_ce", "tahiti_ce.bin")
        FW("amdgpu_fw_tahiti_rlc", "tahiti_rlc.bin")
        FW("amdgpu_fw_pitcairn_pfp", "pitcairn_pfp.bin")
        FW("amdgpu_fw_pitcairn_me", "pitcairn_me.bin")
        FW("amdgpu_fw_pitcairn_ce", "pitcairn_ce.bin")
        FW("amdgpu_fw_pitcairn_rlc", "pitcairn_rlc.bin")
        ".previous\n");

extern const uint8_t amdgpu_fw_tahiti_pfp[], amdgpu_fw_tahiti_pfp_end[];
extern const uint8_t amdgpu_fw_tahiti_me[], amdgpu_fw_tahiti_me_end[];
extern const uint8_t amdgpu_fw_tahiti_ce[], amdgpu_fw_tahiti_ce_end[];
extern const uint8_t amdgpu_fw_tahiti_rlc[], amdgpu_fw_tahiti_rlc_end[];
extern const uint8_t amdgpu_fw_pitcairn_pfp[], amdgpu_fw_pitcairn_pfp_end[];
extern const uint8_t amdgpu_fw_pitcairn_me[], amdgpu_fw_pitcairn_me_end[];
extern const uint8_t amdgpu_fw_pitcairn_ce[], amdgpu_fw_pitcairn_ce_end[];
extern const uint8_t amdgpu_fw_pitcairn_rlc[], amdgpu_fw_pitcairn_rlc_end[];

bool amdgpu_fw_image(enum amdgpu_family family, enum amdgpu_fw_kind kind,
                     const uint8_t **bytes, size_t *len)
{
    static const uint8_t *const tahiti[4][2] = {
        { amdgpu_fw_tahiti_pfp, amdgpu_fw_tahiti_pfp_end },
        { amdgpu_fw_tahiti_me, amdgpu_fw_tahiti_me_end },
        { amdgpu_fw_tahiti_ce, amdgpu_fw_tahiti_ce_end },
        { amdgpu_fw_tahiti_rlc, amdgpu_fw_tahiti_rlc_end },
    };
    static const uint8_t *const pitcairn[4][2] = {
        { amdgpu_fw_pitcairn_pfp, amdgpu_fw_pitcairn_pfp_end },
        { amdgpu_fw_pitcairn_me, amdgpu_fw_pitcairn_me_end },
        { amdgpu_fw_pitcairn_ce, amdgpu_fw_pitcairn_ce_end },
        { amdgpu_fw_pitcairn_rlc, amdgpu_fw_pitcairn_rlc_end },
    };
    const uint8_t *const (*t)[2];

    if ((unsigned)kind > AMDGPU_FW_RLC) {
        return false;
    }

    t = family == AMDGPU_TAHITI ? tahiti : family == AMDGPU_PITCAIRN ? pitcairn : NULL;
    if (t == NULL) {
        return false;
    }

    *bytes = t[kind][0];
    *len = (size_t)(t[kind][1] - t[kind][0]);
    return true;
}

#else

bool amdgpu_fw_image(enum amdgpu_family family, enum amdgpu_fw_kind kind,
                     const uint8_t **bytes, size_t *len)
{
    (void)family;
    (void)kind;
    (void)bytes;
    (void)len;
    return false;
}

#endif
