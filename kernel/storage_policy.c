/* Storage boot policy helpers (M4.4 E.1) - host-testable, no kernel deps. */
#include "kernel.h"

int storage_part_probe_fat32(const struct part_info *p, int qemu_fallback_pass, int dev_is_qemu) {
    if (!p || !p->valid) return 0;
    if (p->gpt_type == 1 || p->gpt_type == 2) return 1; /* Basic Data or ESP: probe BPB */
    if (p->type == 0x0C || p->type == 0x0B) return 1;
    if (qemu_fallback_pass && dev_is_qemu && p->type == 0x83) return 1;
    return 0;
}

int storage_root_can_select(int qualifies_arcroot, int is_esp, int qemu_fallback_pass, int dev_is_qemu) {
    if (is_esp) return 0;
    if (qemu_fallback_pass) return dev_is_qemu ? 1 : 0;
    return qualifies_arcroot ? 1 : 0;
}

int storage_is_esp(const struct part_info *p) {
    return (p && p->valid && p->gpt_type == 2) ? 1 : 0;
}

void ata_ident_string(char *dst, int dst_len, const uint16_t *src_words, int word_count) {
    if (!dst || dst_len <= 0) return;
    int out = 0;
    for (int i = 0; i < word_count && out < dst_len - 1; i++) {
        uint16_t w = src_words ? src_words[i] : 0;
        char c0 = (char)((w >> 8) & 0xFFu);
        char c1 = (char)(w & 0xFFu);
        if (out < dst_len - 1) dst[out++] = c0;
        if (out < dst_len - 1) dst[out++] = c1;
    }
    dst[out] = '\0';
    for (int i = out - 1; i >= 0; i--) {
        if (dst[i] == ' ') dst[i] = '\0';
        else break;
    }
}

void ata_identify_model(char *dst, int dst_len, const uint16_t words[256]) {
    if (!dst || dst_len <= 0) { return; }
    if (!words) { dst[0] = '\0'; return; }
    ata_ident_string(dst, dst_len, &words[27], 20);
}

int storage_model_is_qemu(const char *model) {
    if (!model) return 0;
    return (model[0] == 'Q' && model[1] == 'E' &&
            model[2] == 'M' && model[3] == 'U') ? 1 : 0;
}
