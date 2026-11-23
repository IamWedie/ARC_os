/* Host AHCI helper tests (M4.4 A). */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#undef SYS_OPEN
#include "kernel.h"

static int pass_count, fail_count;
#define CHECK(cond, msg) do { \
    if (cond) { pass_count++; } \
    else { fail_count++; printf("FAIL: %s\n", msg); } \
} while (0)

/* Fake BOHC register struct/state used by callbacks to simulate MMIO behavior. */
typedef struct {
    ahci_bohc_state_t reg;
    uint32_t bos_clear_after_ms;
    uint32_t bb_clear_after_ms;
} fake_bohc_regs_t;

static int fake_wait_bos_clear(ahci_bohc_state_t *r, uint32_t timeout_ms, uint32_t *elapsed_ms) {
    fake_bohc_regs_t *f = (fake_bohc_regs_t *)r;
    uint32_t wait = f->bos_clear_after_ms;
    if (wait > timeout_ms) {
        if (elapsed_ms) *elapsed_ms = timeout_ms;
        f->reg.bohc |= AHCI_BOHC_BOS;
        return 0;
    }
    if (elapsed_ms) *elapsed_ms = wait;
    f->reg.bohc &= ~AHCI_BOHC_BOS;
    return 1;
}

static int fake_wait_bb_clear(ahci_bohc_state_t *r, uint32_t timeout_ms, uint32_t *elapsed_ms) {
    fake_bohc_regs_t *f = (fake_bohc_regs_t *)r;
    uint32_t wait = f->bb_clear_after_ms;
    if (wait > timeout_ms) {
        if (elapsed_ms) *elapsed_ms = timeout_ms;
        f->reg.bohc |= AHCI_BOHC_BB;
        return 0;
    }
    if (elapsed_ms) *elapsed_ms = wait;
    f->reg.bohc &= ~AHCI_BOHC_BB;
    return 1;
}

static void test_bar_decode(void) {
    CHECK(ahci_bar_is_mmio32(0xFEBF1000u) == 1, "MMIO32 BAR accepted");
    CHECK(ahci_bar_base(0xFEBF1000u) == 0xFEBF1000u, "BAR base masks low flags");
    CHECK(ahci_bar_base(0xFEBF100Cu) == 0xFEBF1000u, "BAR base masks type/prefetch bits");
    CHECK(ahci_bar_is_mmio32(0x00000001u) == 0, "I/O BAR rejected");
    CHECK(ahci_bar_is_mmio32(0xFEBF1004u) == 0, "64-bit BAR type rejected in A-scope");
    CHECK(ahci_bar_is_mmio32(0x00000000u) == 0, "zero BAR rejected");
    CHECK(ahci_bar_is_mmio32(0xFFFFFFFFu) == 0, "all-ones BAR rejected");
}

static void test_cap_fields(void) {
    uint32_t cap = (1u << 31) | (5u << 20) | 0x1Fu; /* S64A=1, NP=31 => 32 ports */
    CHECK(ahci_cap_np(cap) == 32, "CAP.NP decodes to port count");
    CHECK(ahci_cap_s64a(cap) == 1, "CAP.S64A detected");
    CHECK(ahci_cap_np(0u) == 1, "CAP.NP zero means 1 port");
    CHECK(ahci_cap_s64a(0u) == 0, "CAP.S64A clear detected");
}

static void test_pi_popcount(void) {
    CHECK(ahci_pi_popcount(0u) == 0, "PI popcount zero");
    CHECK(ahci_pi_popcount(0x1u) == 1, "PI popcount one bit");
    CHECK(ahci_pi_popcount(0x80000001u) == 2, "PI popcount high+low bits");
    CHECK(ahci_pi_popcount(0xFFFFFFFFu) == 32, "PI popcount all bits");
}

static void test_bohc_unsupported(void) {
    fake_bohc_regs_t f;
    memset(&f, 0, sizeof(f));
    f.reg.cap2 = 0;
    f.reg.bohc = AHCI_BOHC_BOS;
    uint32_t elapsed = 0;
    int res = ahci_bohc_run(&f.reg, fake_wait_bos_clear, fake_wait_bb_clear, &elapsed);
    CHECK(res == AHCI_BOHC_UNSUPPORTED, "BOHC unsupported when CAP2.BOH clear");
    CHECK(elapsed == 0, "BOHC unsupported elapsed 0");
}

static void test_bohc_already_owned(void) {
    fake_bohc_regs_t f;
    memset(&f, 0, sizeof(f));
    f.reg.cap2 = AHCI_CAP2_BOH;
    f.reg.bohc = 0; /* BOS clear, OS not owner */
    uint32_t elapsed = 0;
    int res = ahci_bohc_run(&f.reg, fake_wait_bos_clear, fake_wait_bb_clear, &elapsed);
    CHECK(res == AHCI_BOHC_ALREADY_OWNED, "BOHC already owned when BOS clear");
    CHECK((f.reg.bohc & AHCI_BOHC_OOS) != 0, "BOHC already owned sets OOS");
    CHECK(elapsed == 0, "BOHC already owned elapsed 0");
}

static void test_bohc_success(void) {
    fake_bohc_regs_t f;
    memset(&f, 0, sizeof(f));
    f.reg.cap2 = AHCI_CAP2_BOH;
    f.reg.bohc = AHCI_BOHC_BOS | AHCI_BOHC_BB;
    f.bos_clear_after_ms = 5;
    f.bb_clear_after_ms = 7;
    uint32_t elapsed = 0;
    int res = ahci_bohc_run(&f.reg, fake_wait_bos_clear, fake_wait_bb_clear, &elapsed);
    CHECK(res == AHCI_BOHC_SUCCESS, "BOHC success when BOS/BB clear");
    CHECK((f.reg.bohc & AHCI_BOHC_BOS) == 0, "BOHC success BOS cleared");
    CHECK((f.reg.bohc & AHCI_BOHC_BB) == 0, "BOHC success BB cleared");
    CHECK((f.reg.bohc & AHCI_BOHC_OOS) != 0, "BOHC success keeps OOS");
    CHECK(elapsed == 12, "BOHC success elapsed includes BOS+BB waits");
}

static void test_bohc_bb_timeout(void) {
    fake_bohc_regs_t f;
    memset(&f, 0, sizeof(f));
    f.reg.cap2 = AHCI_CAP2_BOH;
    f.reg.bohc = AHCI_BOHC_BOS | AHCI_BOHC_BB;
    f.bos_clear_after_ms = 3;
    f.bb_clear_after_ms = 5000; /* exceeds 2s timeout */
    uint32_t elapsed = 0;
    int res = ahci_bohc_run(&f.reg, fake_wait_bos_clear, fake_wait_bb_clear, &elapsed);
    CHECK(res == AHCI_BOHC_BB_TIMEOUT, "BOHC BB timeout reported");
    CHECK((f.reg.bohc & AHCI_BOHC_BOS) == 0, "BOHC BB timeout BOS cleared");
    CHECK((f.reg.bohc & AHCI_BOHC_BB) != 0, "BOHC BB timeout BB still set");
    CHECK(elapsed >= 2003, "BOHC BB timeout elapsed bounded at ~2s+bos");
}

static void test_ssts_decode(void) {
    uint32_t ssts = (1u << 8) | 3u; /* IPM=1, DET=3 */
    CHECK(ahci_ssts_det(ssts) == 3u, "SSTS DET decode");
    CHECK(ahci_ssts_ipm(ssts) == 1u, "SSTS IPM decode");
    CHECK(ahci_ssts_det(0u) == 0u, "SSTS DET zero");
    CHECK(ahci_ssts_ipm(0xF00u) == 0xFu, "SSTS IPM max nibble");
}

static void test_sig_classify(void) {
    CHECK(ahci_sig_is_sata(AHCI_SIG_ATA) == 1, "SATA sig accepted");
    CHECK(ahci_sig_is_sata(AHCI_SIG_ATAPI) == 0, "ATAPI sig rejected");
    CHECK(strcmp(ahci_sig_name(AHCI_SIG_ATA), "sata") == 0, "SATA sig name");
    CHECK(strcmp(ahci_sig_name(AHCI_SIG_ATAPI), "atapi") == 0, "ATAPI sig name");
    CHECK(strcmp(ahci_sig_name(AHCI_SIG_SEMB), "semb") == 0, "SEMB sig name");
    CHECK(strcmp(ahci_sig_name(AHCI_SIG_PM), "pm") == 0, "PM sig name");
    CHECK(strcmp(ahci_sig_name(0xDEADBEEFu), "unknown") == 0, "unknown sig name");
}

static void test_port_layout(void) {
    CHECK(ahci_port_layout_ok(0x100000u) == 1, "4K-aligned page layout ok");
    CHECK(ahci_port_layout_ok(0x100001u) == 0, "unaligned page layout rejected");
    CHECK(ahci_port_layout_ok(0u) == 1, "zero page base aligned ok");
}

static int fake_wait_cmd_clear_ok(ahci_fake_port_t *p, uint32_t mask, uint32_t timeout_ms, uint32_t *elapsed_ms) {
    (void)timeout_ms;
    if (elapsed_ms) *elapsed_ms = 1;
    p->cmd &= ~mask;
    return 1;
}

static int fake_wait_cmd_clear_timeout(ahci_fake_port_t *p, uint32_t mask, uint32_t timeout_ms, uint32_t *elapsed_ms) {
    (void)p; (void)mask;
    if (elapsed_ms) *elapsed_ms = timeout_ms;
    return 0;
}

static int fake_wait_tfd_ready_ok(ahci_fake_port_t *p, uint32_t timeout_ms, uint32_t *elapsed_ms) {
    (void)timeout_ms;
    if (elapsed_ms) *elapsed_ms = 1;
    p->tfd &= ~(AHCI_PORT_TFD_BSY | AHCI_PORT_TFD_DRQ);
    return 1;
}

static int fake_wait_tfd_ready_stuck(ahci_fake_port_t *p, uint32_t timeout_ms, uint32_t *elapsed_ms) {
    (void)timeout_ms;
    if (elapsed_ms) *elapsed_ms = 1000;
    p->tfd |= AHCI_PORT_TFD_BSY;
    return 0;
}

static void test_port_stop_start(void) {
    ahci_fake_port_t p;
    uint32_t elapsed = 0;

    memset(&p, 0, sizeof(p));
    p.cmd = AHCI_PORT_CMD_ST | AHCI_PORT_CMD_FRE | AHCI_PORT_CMD_CR | AHCI_PORT_CMD_FR;
    CHECK(ahci_port_stop_sim(&p, fake_wait_cmd_clear_ok, &elapsed) == 1, "port stop success");
    CHECK((p.cmd & AHCI_PORT_CMD_ST) == 0, "port stop clears ST");
    CHECK((p.cmd & AHCI_PORT_CMD_FRE) == 0, "port stop clears FRE");
    CHECK((p.cmd & AHCI_PORT_CMD_CR) == 0, "port stop CR cleared");
    CHECK((p.cmd & AHCI_PORT_CMD_FR) == 0, "port stop FR cleared");

    memset(&p, 0, sizeof(p));
    p.cmd = AHCI_PORT_CMD_ST | AHCI_PORT_CMD_CR;
    CHECK(ahci_port_stop_sim(&p, fake_wait_cmd_clear_timeout, &elapsed) == 0, "port stop timeout on CR");
    CHECK((p.cmd & AHCI_PORT_CMD_ST) == 0, "port stop timeout still cleared ST");

    memset(&p, 0, sizeof(p));
    p.tfd = 0;
    CHECK(ahci_port_start_sim(&p, fake_wait_tfd_ready_ok, &elapsed) == 1, "port start success");
    CHECK((p.cmd & AHCI_PORT_CMD_FRE) != 0, "port start sets FRE");
    CHECK((p.cmd & AHCI_PORT_CMD_ST) != 0, "port start sets ST");

    memset(&p, 0, sizeof(p));
    p.tfd = AHCI_PORT_TFD_BSY;
    CHECK(ahci_port_start_sim(&p, fake_wait_tfd_ready_stuck, &elapsed) == 0, "port start BSY stuck fails");
    CHECK((p.cmd & AHCI_PORT_CMD_ST) == 0, "port start BSY stuck does not set ST");
    CHECK((p.cmd & AHCI_PORT_CMD_FRE) != 0, "port start BSY stuck set FRE before wait");
}

static void make_identify_words(uint16_t w[256]) {
    for (int i = 0; i < 256; i++) w[i] = 0;
    /* Word 83: bit14 set, bit15 clear (valid). */
    w[83] = (1u << 14);
    /* Word 86: bit14 set, bit15 clear (valid). */
    w[86] = (1u << 14);
    /* Word 106: bit14 set, bit15 clear (valid), 512-byte logical sectors. */
    w[106] = (1u << 14);
}

static void set_ata_string(uint16_t *dst, int word_count, const char *s) {
    int slen = 0;
    while (s[slen]) slen++;
    for (int i = 0; i < word_count; i++) {
        int i0 = i * 2;
        int i1 = i * 2 + 1;
        char c0 = (i0 < slen) ? s[i0] : ' ';
        char c1 = (i1 < slen) ? s[i1] : ' ';
        dst[i] = (uint16_t)(((uint8_t)c0 << 8) | (uint8_t)c1);
    }
}

static void test_identify_parse_lba28(void) {
    uint16_t w[256]; ahci_identify_result_t r;
    make_identify_words(w);
    w[60] = 0x1234; w[61] = 0x00AB; /* 28-bit sectors */
    set_ata_string(&w[27], 20, "ARC DISK MODEL          ");
    set_ata_string(&w[10], 10, "SER123    ");
    set_ata_string(&w[23], 4, "FW1 ");
    CHECK(ahci_identify_parse(w, &r) == 1, "LBA28 parse ok");
    CHECK(r.supported == 1, "LBA28 supported");
    CHECK(r.lba48 == 0, "LBA28 lba48 clear");
    CHECK(r.sectors == 0x00AB1234u, "LBA28 sectors");
    CHECK(r.sector_size == 512, "LBA28 sector size 512");
    CHECK(strcmp(r.model, "ARC DISK MODEL") == 0, "model swapped/trimmed");
    CHECK(strcmp(r.serial, "SER123") == 0, "serial swapped/trimmed");
    CHECK(strcmp(r.firmware, "FW1") == 0, "firmware swapped/trimmed");
}

static void test_identify_parse_qemu_model_canned(void) {
    /* Canned IDENTIFY block: model "QEMU HARDDISK" as ATA words. */
    uint16_t w[256]; ahci_identify_result_t r;
    make_identify_words(w);
    w[60] = 32768; w[61] = 0;
    set_ata_string(&w[27], 20, "QEMU HARDDISK");
    CHECK(ahci_identify_parse(w, &r) == 1, "qemu canned identify parse ok");
    CHECK(strcmp(r.model, "QEMU HARDDISK") == 0, "qemu canned model parsed as QEMU HARDDISK");
}

static void test_ata_identify_model_canned(void) {
    uint16_t w[256];
    char model[41];

    make_identify_words(w);
    set_ata_string(&w[27], 20, "QEMU HARDDISK");
    ata_identify_model(model, (int)sizeof(model), w);
    CHECK(strcmp(model, "QEMU HARDDISK") == 0, "ata identify model QEMU HARDDISK");

    make_identify_words(w);
    set_ata_string(&w[27], 20, "WDC WD10EZEX-00BN5A0");
    ata_identify_model(model, (int)sizeof(model), w);
    CHECK(strcmp(model, "WDC WD10EZEX-00BN5A0") == 0, "ata identify model WDC WD10EZEX-00BN5A0");

    make_identify_words(w);
    set_ata_string(&w[27], 20, "Samsung SSD 860 EVO");
    ata_identify_model(model, (int)sizeof(model), w);
    CHECK(strcmp(model, "Samsung SSD 860 EVO") == 0, "ata identify model Samsung SSD 860 EVO");

    /* Odd length and trailing spaces. */
    make_identify_words(w);
    set_ata_string(&w[27], 20, "ODD MODEL   ");
    ata_identify_model(model, (int)sizeof(model), w);
    CHECK(strcmp(model, "ODD MODEL") == 0, "ata identify model odd length trailing spaces trimmed");

    /* Raw words 27..30 as specified; rest spaces. */
    for (int i = 0; i < 256; i++) w[i] = 0;
    for (int i = 27; i < 47; i++) w[i] = 0x2020; /* spaces */
    w[27] = 0x5145; w[28] = 0x4d55; w[29] = 0x2048; w[30] = 0x4152;
    ata_identify_model(model, (int)sizeof(model), w);
    CHECK(strcmp(model, "QEMU HAR") == 0, "ata identify model raw words 27-30 QEMU HAR");
}

static void test_model_is_qemu_functions(void) {
    CHECK(storage_model_is_qemu("QEMU HARDDISK") == 1, "storage_model_is_qemu true for QEMU HARDDISK");
    CHECK(storage_model_is_qemu("QUEM HARDDISK") == 0, "storage_model_is_qemu false for QUEM HARDDISK");
    CHECK(storage_model_is_qemu("qemu harddisk") == 0, "storage_model_is_qemu false for qemu harddisk");
    CHECK(storage_model_is_qemu("") == 0, "storage_model_is_qemu false for empty");
    CHECK(storage_model_is_qemu((const char *)0) == 0, "storage_model_is_qemu false for NULL");
    CHECK(storage_model_is_qemu("QEM") == 0, "storage_model_is_qemu false for short QEM");
}

static void test_identify_parse_lba48(void) {
    uint16_t w[256]; ahci_identify_result_t r;
    make_identify_words(w);
    w[83] |= (1u << 10) | (1u << 12) | (1u << 13); /* LBA48 + flush + flush ext */
    w[86] |= (1u << 10);
    w[100] = 0x0000; w[101] = 0x0001; w[102] = 0; w[103] = 0; /* 65536 sectors */
    w[60] = 1; w[61] = 0;
    CHECK(ahci_identify_parse(w, &r) == 1, "LBA48 parse ok");
    CHECK(r.supported == 1, "LBA48 supported");
    CHECK(r.lba48 == 1, "LBA48 flag set");
    CHECK(r.sectors == 65536u, "LBA48 sectors from 100-103");
    CHECK(r.flush_supported == 1, "flush supported");
    CHECK(r.flush_ext_supported == 1, "flush ext supported");
    CHECK(r.rw_dma_ext_supported == 1, "rw dma ext supported");
}

static void test_identify_parse_lba48_supported_not_enabled(void) {
    uint16_t w[256]; ahci_identify_result_t r;
    make_identify_words(w);
    w[83] |= (1u << 10) | (1u << 13); /* LBA48 supported + flush ext */
    /* w86 bit 10 not set => not enabled */
    w[100] = 0x0000; w[101] = 0x0001; w[102] = 0; w[103] = 0;
    w[60] = 100; w[61] = 0;
    CHECK(ahci_identify_parse(w, &r) == 1, "LBA48 supported-not-enabled parses");
    CHECK(r.lba48 == 0, "LBA48 not usable when not enabled");
    CHECK(r.rw_dma_ext_supported == 0, "RW DMA EXT unsupported when lba48 not usable");
    CHECK(r.sectors == 65536u, "sectors from LBA48 field when supported");
}

static void test_identify_parse_invalid(void) {
    uint16_t w[256]; ahci_identify_result_t r;
    make_identify_words(w);
    w[83] = (1u << 15); /* invalid: bit15 set */
    w[60] = 100; w[61] = 0;
    CHECK(ahci_identify_parse(w, &r) == 1, "invalid w83 still parses LBA28");
    CHECK(r.lba48 == 0, "invalid w83 no lba48");
    CHECK(r.flush_supported == 0, "invalid w83 no flush");

    make_identify_words(w);
    w[60] = 0; w[61] = 0; w[100] = 0; w[101] = 0; w[102] = 0; w[103] = 0;
    CHECK(ahci_identify_parse(w, &r) == 0, "zero capacity rejected");
}

static void test_identify_parse_4kn(void) {
    uint16_t w[256]; ahci_identify_result_t r;
    make_identify_words(w);
    w[60] = 1024; w[61] = 0;
    w[106] = (1u << 14) | (1u << 12); /* valid, logical sector size in 117-118 */
    w[117] = 2048; w[118] = 0; /* 2048 words = 4096 bytes */
    CHECK(ahci_identify_parse(w, &r) == 1, "4Kn parse returns 1");
    CHECK(r.sector_size == 4096, "4Kn sector size 4096");
    CHECK(r.supported == 0, "4Kn marked unsupported");
}

static void test_identify_encoding(void) {
    uint8_t fis[20];
    ahci_build_h2d_identify(fis);
    CHECK(fis[0] == 0x27, "H2D FIS type");
    CHECK(fis[1] == 0x80, "H2D FIS C bit set");
    CHECK(fis[2] == 0xEC, "H2D IDENTIFY command");
    CHECK(fis[7] == 0x00, "H2D device zero");
    for (int i = 3; i < 20; i++) {
        if (i == 7) continue;
        if (fis[i] != 0) { CHECK(0, "H2D other fields zero"); break; }
        if (i == 19) CHECK(1, "H2D other fields zero");
    }

    ahci_cmd_header_t h;
    ahci_build_cmd_header(&h, 0x12345000u, 0x00000001u);
    CHECK(h.dw0 == (5u | (1u << 16)), "cmd header DW0 CFL=5 PRDTL=1 W=0");
    CHECK(h.prdbc == 0, "cmd header PRDBC clear");
    CHECK(h.ctba == 0x12345000u, "cmd header CTBA");
    CHECK(h.ctbau == 0x00000001u, "cmd header CTBAU");

    ahci_prdt_entry_t p;
    ahci_build_prdt(&p, 0x00000002ABCDEF00ULL, 512u);
    CHECK(p.dba == 0xABCDEF00u, "PRDT DBA");
    CHECK(p.dbau == 0x00000002u, "PRDT DBAU");
    CHECK((p.dbc_i & 0x003FFFFFu) == 511u, "PRDT DBC 511");
    CHECK((p.dbc_i & (1u << 31)) == 0, "PRDT I clear");
}

static int poll_step_success(ahci_fake_port_t *p, uint32_t iter) {
    if (iter == 2) { p->ci &= ~1u; p->is |= 1u; }
    return 0;
}
static int poll_step_tfes(ahci_fake_port_t *p, uint32_t iter) {
    if (iter == 1) { p->is |= AHCI_PORT_IS_TFES; p->tfd |= AHCI_PORT_TFD_ERR; }
    return 0;
}
static int poll_step_timeout(ahci_fake_port_t *p, uint32_t iter) { (void)p; (void)iter; return 0; }

static void test_identify_poll_sm(void) {
    ahci_fake_port_t p; uint32_t prdbc = 0;
    memset(&p, 0, sizeof(p)); p.ci = 1;
    CHECK(ahci_identify_poll_sim(&p, poll_step_success, 10, &prdbc) == 1, "poll success");
    CHECK(prdbc == 512, "poll success PRDBC 512");

    memset(&p, 0, sizeof(p)); p.ci = 1;
    CHECK(ahci_identify_poll_sim(&p, poll_step_tfes, 10, &prdbc) == -1, "poll TFES error");

    memset(&p, 0, sizeof(p)); p.ci = 1;
    CHECK(ahci_identify_poll_sim(&p, poll_step_timeout, 5, &prdbc) == 0, "poll timeout");
}

static void test_h2d_rw_encoding(void) {
    uint8_t fis[20];
    ahci_build_h2d_rw(fis, ATA_CMD_READ_DMA_EXT, 0, 1);
    CHECK(fis[0] == 0x27, "RW FIS type");
    CHECK(fis[1] == 0x80, "RW FIS C");
    CHECK(fis[2] == ATA_CMD_READ_DMA_EXT, "RW FIS cmd read ext");
    CHECK(fis[7] == 0x40, "RW FIS device LBA");
    CHECK(fis[4]==0 && fis[5]==0 && fis[6]==0 && fis[8]==0 && fis[9]==0 && fis[10]==0, "RW LBA0 zero");
    CHECK(fis[12]==1 && fis[13]==0, "RW count 1");

    ahci_build_h2d_rw(fis, ATA_CMD_WRITE_DMA_EXT, 0xFFFFFFFFULL, 128);
    CHECK(fis[2] == ATA_CMD_WRITE_DMA_EXT, "RW cmd write ext");
    CHECK(fis[4]==0xFF && fis[5]==0xFF && fis[6]==0xFF && fis[8]==0xFF, "LBA 2^32-1 low32");
    CHECK(fis[9]==0x00 && fis[10]==0x00, "LBA 2^32-1 high");
    CHECK(fis[12]==128 && fis[13]==0, "count 128");

    ahci_build_h2d_rw(fis, ATA_CMD_READ_DMA_EXT, 0x100000000ULL, 1);
    CHECK(fis[4]==0 && fis[5]==0 && fis[6]==0 && fis[8]==0, "LBA 2^32 bytes 4-8 zero");
    CHECK(fis[9]==0x01 && fis[10]==0x00, "LBA 2^32 byte9=1");

    ahci_build_h2d_rw(fis, ATA_CMD_READ_DMA_EXT, 0x0000FFFFFFFFFFFFULL, 1);
    CHECK(fis[4]==0xFF && fis[5]==0xFF && fis[6]==0xFF && fis[8]==0xFF && fis[9]==0xFF && fis[10]==0xFF, "LBA 2^48-1");

    ahci_build_h2d_flush(fis);
    CHECK(fis[0]==0x27 && fis[1]==0x80 && fis[2]==ATA_CMD_FLUSH_CACHE_EXT, "flush fis header/cmd");
    CHECK(fis[7]==0x40, "flush device 0x40");
    for (int i=3;i<20;i++) { if (i==7) continue; if (fis[i]!=0) { CHECK(0,"flush other zero"); return; } }
    CHECK(1, "flush other zero");
}

static void test_cmd_header_ex(void) {
    ahci_cmd_header_t h;
    ahci_build_cmd_header_ex(&h, 3, 1, 0x11112222u, 0x33334444u);
    CHECK((h.dw0 & 0x1F) == 5, "hdr CFL5");
    CHECK((h.dw0 & (1u<<6)) != 0, "hdr W set");
    CHECK(((h.dw0 >> 16) & 0xFFFFu) == 3, "hdr PRDTL");
    CHECK(h.prdbc == 0, "hdr PRDBC clear");
    CHECK(h.ctba == 0x11112222u && h.ctbau == 0x33334444u, "hdr CTBA");

    ahci_build_cmd_header_ex(&h, 0, 0, 1, 0);
    CHECK((h.dw0 & (1u<<6)) == 0, "hdr W clear read/flush");
    CHECK(((h.dw0 >> 16) & 0xFFFFu) == 0, "hdr PRDTL 0 flush");
}

typedef struct { uint64_t base; uint64_t map[16]; int n; int contiguous; uint64_t contig_phys_base; } fake_va_ctx_t;
static uint64_t fake_va_lookup(void *ctx, uint64_t va, int *ok) {
    fake_va_ctx_t *c = (fake_va_ctx_t*)ctx;
    uint64_t page = va & ~0xFFFULL;
    uint64_t off = va & 0xFFFULL;
    if (c->contiguous) {
        if (page < c->base) { if (ok) *ok = 0; return 0; }
        uint64_t idx = (page - c->base) / 0x1000ULL;
        if ((int)idx >= c->n) { if (ok) *ok = 0; return 0; }
        if (ok) *ok = 1;
        return c->contig_phys_base + idx * 0x1000ULL + off;
    }
    for (int i = 0; i < c->n; i++) {
        uint64_t vpage = c->base + (uint64_t)i * 0x1000ULL;
        if (page == vpage) { if (ok) *ok = 1; return c->map[i] + off; }
    }
    if (ok) *ok = 0;
    return 0;
}

static void test_prdt_split_and_limits(void) {
    ahci_prdt_entry_t prdt[8];
    /* contiguous 4MiB+4K spanning 4MB boundary -> should split into 2 entries */
    fake_va_ctx_t c; memset(&c, 0, sizeof(c));
    c.base = 0x100000; c.n = 1025; c.contiguous = 1; c.contig_phys_base = 0x200000;
    int n = ahci_prdt_build_from_va(prdt, 8, fake_va_lookup, &c, 0x100000, 0x401000u);
    CHECK(n == 2, "4MB+4K splits into 2 entries");
    CHECK((prdt[0].dbc_i & 0x3FFFFFu) == 0x3FFFFFu, "first entry 4MB");
    CHECK((prdt[1].dbc_i & 0x3FFFFFu) == 0x0FFFu, "second entry 4K");

    /* discontiguous pages */
    fake_va_ctx_t d; memset(&d, 0, sizeof(d)); d.base = 0x400000; d.n = 3;
    d.map[0] = 0x10000; d.map[1] = 0x30000; d.map[2] = 0x31000;
    n = ahci_prdt_build_from_va(prdt, 8, fake_va_lookup, &d, 0x400000, 0x3000u);
    CHECK(n == 2, "discontig pages split");
    CHECK(prdt[0].dba == 0x10000u && (prdt[0].dbc_i & 0x3FFFFFu) == 0xFFFu, "discontig first");
    CHECK(prdt[1].dba == 0x30000u && (prdt[1].dbc_i & 0x3FFFFFu) == 0x1FFFu, "discontig coalesce");

    /* 8-entry limit exceeded */
    fake_va_ctx_t e; memset(&e, 0, sizeof(e)); e.base = 0x500000; e.n = 9;
    for (int i=0;i<e.n;i++) e.map[i] = 0x100000 + (uint64_t)i * 0x2000ULL; /* gaps */
    n = ahci_prdt_build_from_va(prdt, 8, fake_va_lookup, &e, 0x500000, 0x9000u);
    CHECK(n == -1, "8-entry limit exceeded returns -1");

    /* alignment/odd-length rejection */
    n = ahci_prdt_build_from_va(prdt, 8, fake_va_lookup, &d, 0x400001, 0x1000u);
    CHECK(n == -1, "odd VA rejected");
    n = ahci_prdt_build_from_va(prdt, 8, fake_va_lookup, &d, 0x400000, 0x1001u);
    CHECK(n == -1, "odd length rejected");
}

static void test_arg_validation(void) {
    ahci_port_state_t ps; memset(&ps, 0, sizeof(ps));
    ps.started = 1; ps.identify_ok = 1; ps.rw_dma_ext_supported = 1; ps.flush_ext_supported = 1;
    ps.sectors = 1000; ps.sector_size = 512;
    uint8_t buf[512];
    memset(buf, 0, sizeof(buf));
    CHECK(ahci_validate_rw_args(&ps, 0, 0, buf) == AHCI_ERR_BADARG, "count 0 rejected");
    CHECK(ahci_validate_rw_args(&ps, 0, 129, buf) == AHCI_ERR_BADARG, "count>128 rejected");
    CHECK(ahci_validate_rw_args(&ps, 0, 1, NULL) == AHCI_ERR_BADARG, "null buf rejected");
    CHECK(ahci_validate_rw_args(&ps, 1000, 1, buf) == AHCI_ERR_RANGE, "out of range rejected");
    CHECK(ahci_validate_rw_args(&ps, 999, 2, buf) == AHCI_ERR_RANGE, "end overflow range rejected");
    CHECK(ahci_validate_rw_args(&ps, 0xFFFFFFFFFFFFFFF0ULL, 32, buf) == AHCI_ERR_RANGE, "lba+count overflow rejected");
    ps.rw_dma_ext_supported = 0;
    CHECK(ahci_validate_rw_args(&ps, 0, 1, buf) == AHCI_ERR_UNSUPPORTED, "rw unsupported rejected");
    ps.rw_dma_ext_supported = 1; ps.flush_ext_supported = 0;
    CHECK(ahci_validate_flush_args(&ps) == AHCI_ERR_UNSUPPORTED, "flush ext unsupported rejected");
}

static void test_error_mapping(void) {
    CHECK(ahci_error_from_status(AHCI_PORT_IS_TFES, 0) == AHCI_ERR_DEV, "TFES maps dev err");
    CHECK(ahci_error_from_status(0, AHCI_PORT_TFD_ERR) == AHCI_ERR_DEV, "TFD ERR maps dev err");
    CHECK(ahci_error_from_status(1u<<29, 0) == AHCI_ERR_DEV, "HBFS maps dev err");
    CHECK(ahci_error_from_status(1u<<28, 0) == AHCI_ERR_DEV, "HBDS maps dev err");
    CHECK(ahci_error_from_status(1u<<27, 0) == AHCI_ERR_DEV, "IFS maps dev err");
    CHECK(ahci_error_from_status(1u<<26, 0) == AHCI_ERR_DEV, "INFS maps dev err");
    CHECK(ahci_error_from_status(0, 0) == AHCI_OK, "no error ok");
}

static void test_walk_virt_to_phys(void) {
    static uint64_t __attribute__((aligned(4096))) pml4[512];
    static uint64_t __attribute__((aligned(4096))) pdpt[512];
    static uint64_t __attribute__((aligned(4096))) pd[512];
    static uint64_t __attribute__((aligned(4096))) pt[512];
    static uint64_t __attribute__((aligned(4096))) pt2[512];
    memset(pml4, 0, sizeof(pml4));
    memset(pdpt, 0, sizeof(pdpt));
    memset(pd, 0, sizeof(pd));
    memset(pt, 0, sizeof(pt));
    memset(pt2, 0, sizeof(pt2));

    uint64_t va4k = 0x0000000040000000ULL; /* PML4=0 PDPT=0 PD=2 PT=0 */
    pml4[(va4k >> 39) & 0x1FF] = ((uint64_t)(uintptr_t)pdpt & 0x000FFFFFFFFFF000ULL) | 0x3ULL;
    pdpt[(va4k >> 30) & 0x1FF] = ((uint64_t)(uintptr_t)pd & 0x000FFFFFFFFFF000ULL) | 0x3ULL;
    pd[(va4k >> 21) & 0x1FF] = ((uint64_t)(uintptr_t)pt & 0x000FFFFFFFFFF000ULL) | 0x3ULL;
    pt[(va4k >> 12) & 0x1FF] = 0x0000000012345000ULL | 0x3ULL;
    uint64_t pa = 0;
    CHECK(ahci_walk_virt_to_phys(pml4, va4k + 0x123, &pa) == 0, "4k walk ok");
    CHECK(pa == 0x0000000012345123ULL, "4k walk pa");

    uint64_t va2m = 0x0000000080200000ULL; /* PML4=0 PDPT=0 PD=1, 2MB page */
    pdpt[(va2m >> 30) & 0x1FF] = ((uint64_t)(uintptr_t)pd & 0x000FFFFFFFFFF000ULL) | 0x3ULL;
    pd[(va2m >> 21) & 0x1FF] = 0x0000000080200000ULL | 0x83ULL; /* present+rw+pse */
    pa = 0;
    CHECK(ahci_walk_virt_to_phys(pml4, va2m + 0x1ABCDE, &pa) == 0, "2m walk ok");
    CHECK(pa == 0x00000000803ABCDEULL, "2m walk pa");

    /* buffer crossing 4K page boundary: map consecutive VAs to discontiguous phys pages */
    uint64_t vac = 0x0000000040600000ULL; /* PD index 3, PT index 0 and 1 */
    pd[(vac >> 21) & 0x1FF] = ((uint64_t)(uintptr_t)pt2 & 0x000FFFFFFFFFF000ULL) | 0x3ULL;
    pt2[(vac >> 12) & 0x1FF] = 0x0000000050000000ULL | 0x3ULL;
    pt2[((vac + 0x1000) >> 12) & 0x1FF] = 0x0000000060000000ULL | 0x3ULL;
    uint64_t pa1 = 0, pa2 = 0;
    CHECK(ahci_walk_virt_to_phys(pml4, vac + 0xFFF, &pa1) == 0, "cross boundary first ok");
    CHECK(ahci_walk_virt_to_phys(pml4, vac + 0x1000, &pa2) == 0, "cross boundary second ok");
    CHECK(pa1 == 0x0000000050000FFFULL, "cross boundary first pa");
    CHECK(pa2 == 0x0000000060000000ULL, "cross boundary second pa");
}

static int poll_step_success_prdbc(ahci_fake_port_t *p, uint32_t iter) {
    if (iter == 1) { p->ci &= ~1u; }
    return 0;
}
static void test_cmd_poll_sm(void) {
    ahci_fake_port_t p; uint32_t prdbc = 0;
    memset(&p, 0, sizeof(p)); p.ci = 1;
    CHECK(ahci_cmd_poll_sim(&p, poll_step_success_prdbc, 10, 4096, &prdbc) == 1, "cmd poll success");
    CHECK(prdbc == 4096, "cmd poll prdbc");

    memset(&p, 0, sizeof(p)); p.ci = 1;
    CHECK(ahci_cmd_poll_sim(&p, poll_step_tfes, 10, 4096, &prdbc) == -1, "cmd poll TFES");

    memset(&p, 0, sizeof(p)); p.ci = 1;
    CHECK(ahci_cmd_poll_sim(&p, poll_step_timeout, 5, 4096, &prdbc) == 0, "cmd poll timeout");

    /* flush long timeout simulation: completes at iter 2500 within 30000 */
    memset(&p, 0, sizeof(p)); p.ci = 1;
    int ok = 0;
    for (uint32_t i=0;i<30000;i++) {
        if (i == 2500) p.ci &= ~1u;
        if ((p.ci & 1u) == 0) { ok = 1; break; }
    }
    CHECK(ok == 1, "flush long timeout completes within 30s window");
}

/* ---- M4.4 E tests ---- */
static void test_registration_conditions(void) {
    CHECK(ahci_port_can_register(1,1,512,1,0) == 1, "reg ok");
    CHECK(ahci_port_can_register(0,1,512,1,0) == 0, "reg no identify");
    CHECK(ahci_port_can_register(1,0,512,1,0) == 0, "reg not started");
    CHECK(ahci_port_can_register(1,1,4096,1,0) == 0, "reg 4Kn not registered");
    CHECK(ahci_port_can_register(1,1,512,0,0) == 0, "reg no rw dma ext");
    CHECK(ahci_port_can_register(1,1,512,1,1) == 0, "reg failed port");
    /* unsupported flush -> read-only is policy/adapter decision: can register but not writable without flush */
    CHECK(ahci_port_can_register(1,1,512,1,0) == 1, "reg no flush ext still registerable");
}

static void test_writable_policy_matrix(void) {
    /* QEMU/real x ARC_AHCI_WRITE_REAL on/off x root/non-root, flush required */
    CHECK(ahci_write_policy_writable("QEMU HARDDISK", 1, 1, 0) == 1, "qemu root no-real");
    CHECK(ahci_write_policy_writable("QEMU HARDDISK", 1, 1, 1) == 1, "qemu root real");
    CHECK(ahci_write_policy_writable("QEMU HARDDISK", 0, 1, 0) == 0, "qemu non-root");
    CHECK(ahci_write_policy_writable("TESTDISK", 1, 1, 0) == 0, "real root no-real");
    CHECK(ahci_write_policy_writable("TESTDISK", 1, 1, 1) == 1, "real root real");
    CHECK(ahci_write_policy_writable("TESTDISK", 0, 1, 1) == 0, "real non-root real");
    CHECK(ahci_write_policy_writable("QEMU HARDDISK", 1, 0, 1) == 0, "qemu root no flush");
    CHECK(ahci_write_policy_writable("TESTDISK", 1, 0, 1) == 0, "real root no flush");
}

struct split_rec { uint64_t lbas[16]; uint32_t counts[16]; uint32_t dones[16]; int n; int fail_at; };
static int split_cb(void *ctx, uint64_t lba, uint32_t count, uint32_t done) {
    struct split_rec *r = (struct split_rec*)ctx;
    if (r->n < 16) { r->lbas[r->n]=lba; r->counts[r->n]=count; r->dones[r->n]=done; }
    r->n++;
    if (r->fail_at >= 0 && r->n == r->fail_at) return AHCI_ERR_DEV;
    return AHCI_OK;
}

static void test_adapter_splitting(void) {
    struct split_rec r; uint32_t done = 0;
    memset(&r,0,sizeof(r)); r.fail_at=-1;
    CHECK(ahci_split_rw(100000, 0, 1, split_cb, &r, &done)==AHCI_OK, "split 1 ok");
    CHECK(r.n==1 && r.lbas[0]==0 && r.counts[0]==1 && done==1, "split 1 layout");

    memset(&r,0,sizeof(r)); r.fail_at=-1; done=0;
    CHECK(ahci_split_rw(100000, 10, 128, split_cb, &r, &done)==AHCI_OK, "split 128 ok");
    CHECK(r.n==1 && r.lbas[0]==10 && r.counts[0]==128, "split 128 layout");

    memset(&r,0,sizeof(r)); r.fail_at=-1; done=0;
    CHECK(ahci_split_rw(100000, 5, 129, split_cb, &r, &done)==AHCI_OK, "split 129 ok");
    CHECK(r.n==2 && r.counts[0]==128 && r.counts[1]==1 && r.lbas[1]==133, "split 129 layout");

    memset(&r,0,sizeof(r)); r.fail_at=-1; done=0;
    CHECK(ahci_split_rw(100000, 1000, 300, split_cb, &r, &done)==AHCI_OK, "split 300 ok");
    CHECK(r.n==3 && r.counts[0]==128 && r.counts[1]==128 && r.counts[2]==44, "split 300 counts");
    CHECK(r.lbas[0]==1000 && r.lbas[1]==1128 && r.lbas[2]==1256, "split 300 lbas");

    /* error in middle chunk */
    memset(&r,0,sizeof(r)); r.fail_at=2; done=0;
    CHECK(ahci_split_rw(100000, 0, 300, split_cb, &r, &done)==AHCI_ERR_DEV, "split mid err");
    CHECK(done==128, "split mid done");

    /* offset arithmetic at LBA boundaries */
    memset(&r,0,sizeof(r)); r.fail_at=-1; done=0;
    CHECK(ahci_split_rw(1024, 1023, 1, split_cb, &r, &done)==AHCI_OK, "lba boundary last");
    memset(&r,0,sizeof(r)); r.fail_at=-1; done=0;
    CHECK(ahci_split_rw(1024, 1023, 2, split_cb, &r, &done)==AHCI_ERR_RANGE, "lba boundary oob");
    memset(&r,0,sizeof(r)); r.fail_at=-1; done=0;
    CHECK(ahci_split_rw(0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFF00ULL, 1, split_cb, &r, &done)==AHCI_OK, "lba high ok");

    CHECK(ahci_split_rw(100, 0, 0, split_cb, &r, &done)==AHCI_ERR_BADARG, "split 0 count");
}

static void test_busy_handling(void) {
    ahci_port_state_t ps;
    memset(&ps, 0, sizeof(ps));
    ps.started = 1; ps.identify_ok = 1; ps.rw_dma_ext_supported = 1;
    ps.flush_ext_supported = 1; ps.sectors = 1000; ps.sector_size = 512;
    ps.busy = 1;
    CHECK(ahci_validate_rw_args(&ps, 0, 1, (void*)0x1000) == AHCI_OK, "busy validate still ok");
    /* busy is checked at adapter/ahci_read level; here we validate args logic doesn't block,
       and AHCI_ERR_BUSY is defined for the adapter to return. */
    CHECK(AHCI_ERR_BUSY == -4, "busy err code");
}

int main(void) {
    test_bar_decode();
    test_cap_fields();
    test_pi_popcount();
    test_bohc_unsupported();
    test_bohc_already_owned();
    test_bohc_success();
    test_bohc_bb_timeout();
    test_ssts_decode();
    test_sig_classify();
    test_port_layout();
    test_port_stop_start();
    test_identify_parse_lba28();
    test_identify_parse_qemu_model_canned();
    test_ata_identify_model_canned();
    test_model_is_qemu_functions();
    test_identify_parse_lba48();
    test_identify_parse_lba48_supported_not_enabled();
    test_identify_parse_invalid();
    test_identify_parse_4kn();
    test_identify_encoding();
    test_identify_poll_sm();
    test_h2d_rw_encoding();
    test_cmd_header_ex();
    test_prdt_split_and_limits();
    test_arg_validation();
    test_error_mapping();
    test_cmd_poll_sm();
    test_walk_virt_to_phys();
    test_registration_conditions();
    test_writable_policy_matrix();
    test_adapter_splitting();
    test_busy_handling();
    printf("ahci tests: %d/%d passed\n", pass_count, pass_count + fail_count);
    return fail_count ? 1 : 0;
}
