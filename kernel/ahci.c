/*
 * AHCI (SATA) controller driver - M4.4 (sub-step A: detection/capabilities/handoff).
 *
 * A-scope: map BAR5 (ABAR) uncached MMIO, reserve in PMM, read CAP/GHC/PI/VS,
 * perform BIOS/OS handoff (BOHC) when supported, set GHC.AE via RMW, and log
 * capabilities. No DMA command engine, no interrupts, no port start.
 * Failures are non-fatal; ATA PIO remains the fallback.
 */
#include "kernel.h"

#define AHCI_MMIO_MAP_SIZE   0x2000u   /* 8KB covers HBA + port headers */

#define AHCI_REG_CAP         0x00u
#define AHCI_REG_GHC         0x04u
#define AHCI_REG_IS          0x08u
#define AHCI_REG_PI          0x0Cu
#define AHCI_REG_VS          0x10u
#define AHCI_REG_CAP2        0x24u
#define AHCI_REG_BOHC        0x28u

#define AHCI_GHC_AE          (1u << 31)
#define AHCI_GHC_IE          (1u << 1)
#define AHCI_GHC_HR          (1u << 0)

/* BOHC/CAP2 bit defines live in kernel.h for host tests. */

int ahci_bar_is_mmio32(uint32_t bar) {
    if (bar == 0 || bar == 0xFFFFFFFFu) return 0;
    if (bar & 0x1u) return 0;           /* I/O space */
    if ((bar & 0x6u) != 0x0u) return 0; /* must be 32-bit memory BAR */
    return 1;
}

uint32_t ahci_bar_base(uint32_t bar) {
    return bar & ~0xFu;
}

uint32_t ahci_cap_np(uint32_t cap) {
    return (cap & 0x1Fu) + 1u; /* NP is number of ports minus 1 */
}

int ahci_cap_s64a(uint32_t cap) {
    return (cap & (1u << 31)) ? 1 : 0;
}

uint32_t ahci_pi_popcount(uint32_t pi) {
    uint32_t n = 0;
    while (pi) { n += pi & 1u; pi >>= 1; }
    return n;
}

int ahci_bohc_run(ahci_bohc_state_t *r,
                  int (*wait_bos_clear)(ahci_bohc_state_t *r, uint32_t timeout_ms, uint32_t *elapsed_ms),
                  int (*wait_bb_clear)(ahci_bohc_state_t *r, uint32_t timeout_ms, uint32_t *elapsed_ms),
                  uint32_t *elapsed_ms) {
    uint32_t elapsed = 0;
    if (elapsed_ms) *elapsed_ms = 0;
    if (!r) return AHCI_BOHC_UNSUPPORTED;
    if ((r->cap2 & AHCI_CAP2_BOH) == 0) return AHCI_BOHC_UNSUPPORTED;

    if ((r->bohc & AHCI_BOHC_BOS) == 0) {
        /* BIOS does not own semaphore; ensure OS ownership bit is set. */
        r->bohc |= AHCI_BOHC_OOS;
        if (elapsed_ms) *elapsed_ms = elapsed;
        return AHCI_BOHC_ALREADY_OWNED;
    }

    /* Request OS ownership. */
    r->bohc |= AHCI_BOHC_OOS;
    if (wait_bos_clear) {
        uint32_t e = 0;
        int ok = wait_bos_clear(r, 2000u, &e);
        elapsed += e;
        if (!ok) { if (elapsed_ms) *elapsed_ms = elapsed; return AHCI_BOHC_BB_TIMEOUT; }
    } else {
        /* No wait callback: cannot complete handoff safely. */
        if (elapsed_ms) *elapsed_ms = elapsed;
        return AHCI_BOHC_BB_TIMEOUT;
    }

    if (r->bohc & AHCI_BOHC_BB) {
        uint32_t e = 0;
        int ok = wait_bb_clear ? wait_bb_clear(r, 2000u, &e) : 0;
        elapsed += e;
        if (!ok) { if (elapsed_ms) *elapsed_ms = elapsed; return AHCI_BOHC_BB_TIMEOUT; }
    }

    if (elapsed_ms) *elapsed_ms = elapsed;
    return AHCI_BOHC_SUCCESS;
}

uint32_t ahci_ssts_det(uint32_t ssts) { return ssts & 0xFu; }
uint32_t ahci_ssts_ipm(uint32_t ssts) { return (ssts >> 8) & 0xFu; }

int ahci_sig_is_sata(uint32_t sig) { return sig == AHCI_SIG_ATA; }

const char* ahci_sig_name(uint32_t sig) {
    if (sig == AHCI_SIG_ATA) return "sata";
    if (sig == AHCI_SIG_ATAPI) return "atapi";
    if (sig == AHCI_SIG_SEMB) return "semb";
    if (sig == AHCI_SIG_PM) return "pm";
    return "unknown";
}

/*
 * Per-port DMA page layout (4 KiB):
 *   0x0000 - 0x03FF : command list (32 * 32 = 1024), 1K aligned
 *   0x0400 - 0x04FF : FIS receive area (256), 256 aligned
 *   0x0500 - 0x057F : command table header / CFIS+ACMD+res (128), 128 aligned
 *   0x0580 - 0x05FF : PRDT entries (8 * 16 = 128)  => total CT 256
 */
#define AHCI_CL_OFF      0x0000u
#define AHCI_CL_SIZE     0x0400u
#define AHCI_FIS_OFF     0x0400u
#define AHCI_FIS_SIZE    0x0100u
#define AHCI_CT_OFF      0x0500u
#define AHCI_CT_SIZE     0x0100u
#define AHCI_PRDT_OFF    0x0580u
#define AHCI_PRDT_ENTRIES 8u
#define AHCI_PRDT_ENTRY_SIZE 16u

int ahci_port_layout_ok(uint64_t page_phys) {
    if ((page_phys & 0xFFFu) != 0) return 0;
    if ((AHCI_CL_OFF & 0x3FFu) != 0) return 0;
    if ((AHCI_FIS_OFF & 0xFFu) != 0) return 0;
    if ((AHCI_CT_OFF & 0x7Fu) != 0) return 0;
    if (AHCI_CL_OFF + AHCI_CL_SIZE > 0x1000u) return 0;
    if (AHCI_FIS_OFF + AHCI_FIS_SIZE > 0x1000u) return 0;
    if (AHCI_CT_OFF + AHCI_CT_SIZE > 0x1000u) return 0;
    if (AHCI_PRDT_OFF + AHCI_PRDT_ENTRIES * AHCI_PRDT_ENTRY_SIZE > AHCI_CT_OFF + AHCI_CT_SIZE) return 0;
    if (AHCI_PRDT_ENTRIES < 8u) return 0;
    return 1;
}

int ahci_port_stop_sim(ahci_fake_port_t *p,
                       int (*wait_clear)(ahci_fake_port_t *p, uint32_t mask, uint32_t timeout_ms, uint32_t *elapsed_ms),
                       uint32_t *elapsed_ms) {
    uint32_t elapsed = 0;
    if (elapsed_ms) *elapsed_ms = 0;
    if (!p || !wait_clear) return 0;

    p->cmd &= ~AHCI_PORT_CMD_ST;
    {
        uint32_t e = 0;
        int ok = wait_clear(p, AHCI_PORT_CMD_CR, 500u, &e);
        elapsed += e;
        if (!ok) { if (elapsed_ms) *elapsed_ms = elapsed; return 0; }
    }

    p->cmd &= ~AHCI_PORT_CMD_FRE;
    {
        uint32_t e = 0;
        int ok = wait_clear(p, AHCI_PORT_CMD_FR, 500u, &e);
        elapsed += e;
        if (!ok) { if (elapsed_ms) *elapsed_ms = elapsed; return 0; }
    }

    if (elapsed_ms) *elapsed_ms = elapsed;
    return 1;
}

int ahci_port_start_sim(ahci_fake_port_t *p,
                        int (*wait_tfd_ready)(ahci_fake_port_t *p, uint32_t timeout_ms, uint32_t *elapsed_ms),
                        uint32_t *elapsed_ms) {
    uint32_t elapsed = 0;
    if (elapsed_ms) *elapsed_ms = 0;
    if (!p) return 0;

    p->cmd |= AHCI_PORT_CMD_FRE;
    if (wait_tfd_ready) {
        uint32_t e = 0;
        int ok = wait_tfd_ready(p, 1000u, &e);
        elapsed += e;
        if (!ok) { if (elapsed_ms) *elapsed_ms = elapsed; return 0; }
    }
    if ((p->tfd & (AHCI_PORT_TFD_BSY | AHCI_PORT_TFD_DRQ)) != 0) {
        if (elapsed_ms) *elapsed_ms = elapsed;
        return 0;
    }
    p->cmd |= AHCI_PORT_CMD_ST;
    if (elapsed_ms) *elapsed_ms = elapsed;
    return 1;
}

void ahci_build_h2d_identify(uint8_t fis[20]) {
    for (int i = 0; i < 20; i++) fis[i] = 0;
    fis[0] = AHCI_FIS_TYPE_REG_H2D;
    fis[1] = 0x80; /* C bit = 1 (command register update) */
    fis[2] = ATA_CMD_IDENTIFY;
    /* device, LBA, count, features all zero for IDENTIFY */
}

void ahci_build_cmd_header(ahci_cmd_header_t *h, uint32_t ctba, uint32_t ctbau) {
    if (!h) return;
    h->dw0 = 5u | (1u << 16); /* CFL=5 DWORDs, W=0, PRDTL=1 */
    h->prdbc = 0;
    h->ctba = ctba;
    h->ctbau = ctbau;
    for (int i = 0; i < 4; i++) h->rsv[i] = 0;
}

void ahci_build_prdt(ahci_prdt_entry_t *p, uint64_t dba, uint32_t byte_count) {
    if (!p) return;
    p->dba = (uint32_t)(dba & 0xFFFFFFFFu);
    p->dbau = (uint32_t)(dba >> 32);
    p->rsv = 0;
    /* DBC field is byte count minus 1 in bits 21:0, I bit 31 clear */
    p->dbc_i = (byte_count ? (byte_count - 1u) : 0u) & 0x003FFFFFu;
}

/* ATA identify string parsing shared via ata_ident_string() in storage_policy.c. */

int ahci_identify_parse(const uint16_t words[256], ahci_identify_result_t *out) {
    if (!words || !out) return 0;
    for (int i = 0; i < 41; i++) out->model[i] = 0;
    for (int i = 0; i < 21; i++) out->serial[i] = 0;
    for (int i = 0; i < 9; i++) out->firmware[i] = 0;
    out->sectors = 0;
    out->lba48 = 0;
    out->sector_size = 512;
    out->phys_sector_mult = 1;
    out->flush_supported = 0;
    out->flush_ext_supported = 0;
    out->rw_dma_ext_supported = 0;
    out->lba48_assumed_enabled = 0;
    out->supported = 0;

    ata_ident_string(out->model, 41, &words[27], 20);
    ata_ident_string(out->serial, 21, &words[10], 10);
    ata_ident_string(out->firmware, 9, &words[23], 4);

    uint16_t w83 = words[83];
    uint16_t w86 = words[86];
    uint16_t w106 = words[106];

    /* Word 83 validity: bit 14 must be 1 and bit 15 must be 0. */
    int w83_valid = ((w83 & (1u << 14)) != 0) && ((w83 & (1u << 15)) == 0);
    /* LBA48 supported: word 83 bit 10 set and word 83 valid. */
    int lba48_supported = w83_valid && ((w83 & (1u << 10)) != 0);
    /*
     * Word 86 validity follows IDENTIFY data word validity rule: bit 14 set,
     * bit 15 clear. LBA48 enabled is word 86 bit 10 when word 86 is valid.
     * LBA48 is usable only when both supported (word 83) and enabled (word 86).
     */
    int w86_valid = ((w86 & (1u << 14)) != 0) && ((w86 & (1u << 15)) == 0);
    int lba48_enabled = w86_valid && ((w86 & (1u << 10)) != 0);
    /*
     * LBA48 usable when supported and enabled. If word 86 is not valid, the
     * enabled bit is not reliable; treat supported as usable in that case.
     */
    out->lba48_assumed_enabled = (lba48_supported && !w86_valid) ? 1 : 0;
    out->lba48 = (lba48_supported && (lba48_enabled || !w86_valid)) ? 1 : 0;

    if (lba48_supported) {
        uint64_t s = (uint64_t)words[100]
                   | ((uint64_t)words[101] << 16)
                   | ((uint64_t)words[102] << 32)
                   | ((uint64_t)words[103] << 48);
        out->sectors = s;
    } else {
        uint64_t s = (uint64_t)words[60] | ((uint64_t)words[61] << 16);
        out->sectors = s;
    }
    if (out->sectors == 0) return 0;

    /* Logical sector size: word 106 valid if bit14 set, bit15 clear. */
    int w106_valid = ((w106 & (1u << 14)) != 0) && ((w106 & (1u << 15)) == 0);
    if (w106_valid && (w106 & (1u << 12))) {
        uint32_t sz = ((uint32_t)words[117] | ((uint32_t)words[118] << 16)) * 2u;
        if (sz != 0) out->sector_size = sz;
    } else {
        out->sector_size = 512;
    }
    if (w106_valid) {
        if (w106 & (1u << 13)) {
            uint32_t mult = (uint32_t)(w106 & 0xFu);
            out->phys_sector_mult = mult ? (1u << mult) : 1u;
        } else {
            out->phys_sector_mult = 1u;
        }
    }

    out->flush_supported = (w83_valid && ((w83 & (1u << 12)) != 0 || (w83 & (1u << 13)) != 0)) ? 1 : 0;
    out->flush_ext_supported = (w83_valid && ((w83 & (1u << 13)) != 0)) ? 1 : 0;
    out->rw_dma_ext_supported = (out->lba48 && out->flush_ext_supported) ? 1 : 0;

    if (out->sector_size != 512u) {
        out->supported = 0;
        return 1; /* parsed but unsupported */
    }
    out->supported = 1;
    return 1;
}

int ahci_identify_poll_sim(ahci_fake_port_t *p,
                           int (*step)(ahci_fake_port_t *p, uint32_t iter),
                           uint32_t max_iters,
                           uint32_t *prdbc_out) {
    if (!p) return 0;
    if (prdbc_out) *prdbc_out = 0;
    for (uint32_t i = 0; i < max_iters; i++) {
        if (step) step(p, i);
        if ((p->is & AHCI_PORT_IS_TFES) || (p->tfd & AHCI_PORT_TFD_ERR)) {
            return -1;
        }
        if ((p->ci & 1u) == 0) {
            if (prdbc_out) *prdbc_out = 512;
            p->is = 0;
            return 1;
        }
    }
    return 0;
}

int ahci_cmd_poll_sim(ahci_fake_port_t *p,
                      int (*step)(ahci_fake_port_t *p, uint32_t iter),
                      uint32_t max_iters,
                      uint32_t expected_prdbc,
                      uint32_t *prdbc_out) {
    if (!p) return 0;
    if (prdbc_out) *prdbc_out = 0;
    for (uint32_t i = 0; i < max_iters; i++) {
        if (step) step(p, i);
        if ((p->is & AHCI_PORT_IS_TFES) || (p->tfd & AHCI_PORT_TFD_ERR)) {
            return -1;
        }
        if ((p->ci & 1u) == 0) {
            if (prdbc_out) *prdbc_out = expected_prdbc;
            p->is = 0;
            return 1;
        }
    }
    return 0;
}

int ahci_writes_enabled = 0;

void ahci_build_h2d_rw(uint8_t fis[20], uint8_t cmd, uint64_t lba, uint16_t count) {
    for (int i = 0; i < 20; i++) fis[i] = 0;
    fis[0] = AHCI_FIS_TYPE_REG_H2D;
    fis[1] = 0x80; /* C=1 */
    fis[2] = cmd;
    fis[3] = 0;    /* features low */
    fis[4] = (uint8_t)(lba & 0xFFu);          /* LBA0 */
    fis[5] = (uint8_t)((lba >> 8) & 0xFFu);   /* LBA1 */
    fis[6] = (uint8_t)((lba >> 16) & 0xFFu);  /* LBA2 */
    fis[7] = 0x40; /* device: LBA mode */
    fis[8]  = (uint8_t)((lba >> 24) & 0xFFu); /* LBA3 */
    fis[9]  = (uint8_t)((lba >> 32) & 0xFFu); /* LBA4 */
    fis[10] = (uint8_t)((lba >> 40) & 0xFFu); /* LBA5 */
    fis[11] = 0;   /* features high */
    fis[12] = (uint8_t)(count & 0xFFu);       /* count low */
    fis[13] = (uint8_t)((count >> 8) & 0xFFu);/* count high */
    fis[14] = 0;   /* ICC */
    fis[15] = 0;   /* control */
    /* 16-19 zero */
}

void ahci_build_h2d_flush(uint8_t fis[20]) {
    for (int i = 0; i < 20; i++) fis[i] = 0;
    fis[0] = AHCI_FIS_TYPE_REG_H2D;
    fis[1] = 0x80; /* C=1 */
    fis[2] = ATA_CMD_FLUSH_CACHE_EXT;
    fis[7] = 0x40; /* device LBA mode, all else zero */
}

void ahci_build_cmd_header_ex(ahci_cmd_header_t *h, uint16_t prdtl, int write, uint32_t ctba, uint32_t ctbau) {
    if (!h) return;
    h->dw0 = 5u; /* CFL=5 DWORDs */
    if (write) h->dw0 |= (1u << 6); /* W */
    h->dw0 |= ((uint32_t)prdtl & 0xFFFFu) << 16;
    h->prdbc = 0;
    h->ctba = ctba;
    h->ctbau = ctbau;
    for (int i = 0; i < 4; i++) h->rsv[i] = 0;
}

int ahci_prdt_build(ahci_prdt_entry_t *prdt, uint32_t max_entries, uint64_t phys_addr, uint32_t bytes) {
    if (!prdt || max_entries == 0) return -1;
    if (bytes == 0) return 0;
    if ((phys_addr & 1u) != 0) return -1; /* DBA word-aligned minimum */
    if ((bytes & 1u) != 0) return -1;      /* byte count even */
    uint32_t n = 0;
    uint64_t addr = phys_addr;
    uint32_t rem = bytes;
    while (rem > 0) {
        if (n >= max_entries) return -1;
        uint32_t chunk = rem;
        /* Max 4 MiB per PRDT entry: DBC field is bytes-1 in 22 bits => max 0x400000 bytes. */
        if (chunk > 0x400000u) chunk = 0x400000u;
        prdt[n].dba = (uint32_t)(addr & 0xFFFFFFFFu);
        prdt[n].dbau = (uint32_t)(addr >> 32);
        prdt[n].rsv = 0;
        prdt[n].dbc_i = ((chunk - 1u) & 0x003FFFFFu); /* I=0 */
        addr += chunk;
        rem -= chunk;
        n++;
    }
    return (int)n;
}

int ahci_prdt_build_from_va(ahci_prdt_entry_t *prdt, uint32_t max_entries,
                            ahci_phys_lookup_fn lookup, void *ctx,
                            uint64_t va, uint32_t bytes) {
    if (!prdt || max_entries == 0 || !lookup) return -1;
    if (bytes == 0) return 0;
    if ((va & 1u) != 0) return -1;       /* word-aligned */
    if ((bytes & 1u) != 0) return -1;      /* even byte count */
    uint32_t n = 0;
    uint64_t cur_va = va;
    uint32_t rem = bytes;
    uint64_t entry_phys = 0;
    uint32_t entry_len = 0;

    while (rem > 0) {
        uint32_t page_off = (uint32_t)(cur_va & 0xFFFu);
        uint32_t page_chunk = 0x1000u - page_off;
        if (page_chunk > rem) page_chunk = rem;
        int ok = 0;
        uint64_t phys = lookup(ctx, cur_va, &ok);
        if (!ok) return -1;
        if ((phys & 1u) != 0) return -1;

        if (entry_len == 0) {
            entry_phys = phys;
            entry_len = page_chunk;
        } else if (phys == entry_phys + (uint64_t)entry_len && entry_len + page_chunk <= 0x400000u) {
            entry_len += page_chunk;
        } else {
            if (n >= max_entries) return -1;
            prdt[n].dba = (uint32_t)(entry_phys & 0xFFFFFFFFu);
            prdt[n].dbau = (uint32_t)(entry_phys >> 32);
            prdt[n].rsv = 0;
            prdt[n].dbc_i = ((entry_len - 1u) & 0x003FFFFFu);
            n++;
            entry_phys = phys;
            entry_len = page_chunk;
        }
        cur_va += page_chunk;
        rem -= page_chunk;
    }
    if (entry_len > 0) {
        if (n >= max_entries) return -1;
        prdt[n].dba = (uint32_t)(entry_phys & 0xFFFFFFFFu);
        prdt[n].dbau = (uint32_t)(entry_phys >> 32);
        prdt[n].rsv = 0;
        prdt[n].dbc_i = ((entry_len - 1u) & 0x003FFFFFu);
        n++;
    }
    return (int)n;
}

int ahci_validate_rw_args(const ahci_port_state_t *ps, uint64_t lba, uint32_t count, const void *buf) {
    if (!ps) return AHCI_ERR_BADARG;
    if (!ps->started || !ps->identify_ok || ps->failed) return AHCI_ERR_UNSUPPORTED;
    if (!buf) return AHCI_ERR_BADARG;
    if (count == 0 || count > AHCI_MAX_SECTORS) return AHCI_ERR_BADARG;
    if (!ps->rw_dma_ext_supported) return AHCI_ERR_UNSUPPORTED;
    uint64_t end = lba + (uint64_t)count;
    if (end < lba) return AHCI_ERR_RANGE; /* overflow */
    if (end > ps->sectors) return AHCI_ERR_RANGE;
    return AHCI_OK;
}

int ahci_validate_flush_args(const ahci_port_state_t *ps) {
    if (!ps) return AHCI_ERR_BADARG;
    if (!ps->started || !ps->identify_ok || ps->failed) return AHCI_ERR_UNSUPPORTED;
    if (!ps->flush_ext_supported) return AHCI_ERR_UNSUPPORTED;
    return AHCI_OK;
}

int ahci_error_from_status(uint32_t is, uint32_t tfd) {
    if (is & AHCI_PORT_IS_TFES) return AHCI_ERR_DEV;
    if (tfd & AHCI_PORT_TFD_ERR) return AHCI_ERR_DEV;
    if (is & (1u << 29)) return AHCI_ERR_DEV; /* HBFS */
    if (is & (1u << 28)) return AHCI_ERR_DEV; /* HBDS */
    if (is & (1u << 27)) return AHCI_ERR_DEV; /* IFS */
    if (is & (1u << 26)) return AHCI_ERR_DEV; /* INFS */
    return AHCI_OK;
}

int ahci_walk_virt_to_phys(const uint64_t *pml4, uint64_t va, uint64_t *pa_out) {
    if (!pml4 || !pa_out) return -1;
    uint64_t pml4e = pml4[(va >> 39) & 0x1FFu];
    if (!(pml4e & PT_PG_PRESENT)) return -1;
    const uint64_t *pdpt = (const uint64_t *)(uintptr_t)(pml4e & 0x000FFFFFFFFFF000ULL);

    uint64_t pdpte = pdpt[(va >> 30) & 0x1FFu];
    if (!(pdpte & PT_PG_PRESENT)) return -1;
    if (pdpte & PT_PG_PSE) {
        *pa_out = (pdpte & 0x000FFFFFC0000000ULL) | (va & 0x3FFFFFFFu);
        return 0;
    }
    const uint64_t *pd = (const uint64_t *)(uintptr_t)(pdpte & 0x000FFFFFFFFFF000ULL);

    uint64_t pde = pd[(va >> 21) & 0x1FFu];
    if (!(pde & PT_PG_PRESENT)) return -1;
    if (pde & PT_PG_PSE) {
        *pa_out = (pde & 0x000FFFFFFFE00000ULL) | (va & 0x1FFFFFu);
        return 0;
    }
    const uint64_t *pt = (const uint64_t *)(uintptr_t)(pde & 0x000FFFFFFFFFF000ULL);

    uint64_t pte = pt[(va >> 12) & 0x1FFu];
    if (!(pte & PT_PG_PRESENT)) return -1;
    *pa_out = (pte & 0x000FFFFFFFFFF000ULL) | (va & 0xFFFu);
    return 0;
}

int ahci_port_can_register(int identify_ok, int started, uint32_t sector_size, int rw_dma_ext_supported, int failed) {
    if (failed) return 0;
    if (!identify_ok || !started) return 0;
    if (sector_size != 512u) return 0;
    if (!rw_dma_ext_supported) return 0;
    return 1;
}

int ahci_write_policy_writable(const char *model, int is_root, int has_flush_ext, int arc_ahci_write_real) {
    if (!is_root) return 0;
    if (!has_flush_ext) return 0;
    if (model && model[0] == 'Q' && model[1] == 'E' && model[2] == 'M' && model[3] == 'U')
        return 1;
    if (arc_ahci_write_real) return 1;
    return 0;
}

int ahci_split_rw(uint64_t total_sectors, uint64_t lba, uint32_t count,
                  int (*do_chunk)(void *ctx, uint64_t lba, uint32_t count, uint32_t done), void *ctx,
                  uint32_t *done_out) {
    if (done_out) *done_out = 0;
    if (count == 0) return AHCI_ERR_BADARG;
    uint64_t end = lba + (uint64_t)count;
    if (end < lba || end > total_sectors) return AHCI_ERR_RANGE;
    uint32_t done = 0;
    while (done < count) {
        uint32_t chunk = count - done;
        if (chunk > AHCI_MAX_SECTORS) chunk = AHCI_MAX_SECTORS;
        int r = do_chunk ? do_chunk(ctx, lba + done, chunk, done) : AHCI_OK;
        if (r != AHCI_OK) {
            if (done_out) *done_out = done;
            return r;
        }
        done += chunk;
    }
    if (done_out) *done_out = done;
    return AHCI_OK;
}

#ifndef AHCI_HOST_TEST

#define AHCI_MAX_PORTS 32u

#define AHCI_PORT_CLB   0x00u
#define AHCI_PORT_CLBU  0x04u
#define AHCI_PORT_FB    0x08u
#define AHCI_PORT_FBU   0x0Cu
#define AHCI_PORT_IS    0x10u
#define AHCI_PORT_IE    0x14u
#define AHCI_PORT_CMD   0x18u
#define AHCI_PORT_TFD   0x20u
#define AHCI_PORT_SIG   0x24u
#define AHCI_PORT_SSTS  0x28u
#define AHCI_PORT_SERR  0x30u
#define AHCI_PORT_CI    0x38u

static int g_ahci_present;
static int g_ahci_s64a;
static uint32_t g_ahci_bar5;
static uint8_t g_ahci_irq;
static volatile uint32_t* g_ahci_abar;
static ahci_port_state_t g_ahci_ports[AHCI_MAX_PORTS];
#ifndef AHCI_HOST_TEST
static struct blockdev g_ahci_bdevs[AHCI_MAX_PORTS];
#endif

int ahci_present(void) { return g_ahci_present; }
uint32_t ahci_bar5(void) { return g_ahci_bar5; }

static void ahci_barrier(void) {
    __asm__ volatile("mfence" ::: "memory");
}

static uint32_t ahci_rd32(uint32_t off) {
    return g_ahci_abar[off / 4u];
}

static void ahci_wr32(uint32_t off, uint32_t v) {
    g_ahci_abar[off / 4u] = v;
    (void)ahci_rd32(off); /* flush posted write */
}

static uint32_t ahci_port_off(uint32_t port, uint32_t reg) {
    return 0x100u + port * 0x80u + reg;
}

static uint32_t ahci_prd32(uint32_t port, uint32_t reg) {
    return ahci_rd32(ahci_port_off(port, reg));
}

static void ahci_pwr32(uint32_t port, uint32_t reg, uint32_t v) {
    ahci_wr32(ahci_port_off(port, reg), v);
}

static int ahci_large_page_covered(uint64_t base, uint64_t len) {
    uint64_t root_pa = vmm_master_root();
    const uint64_t *root = (const uint64_t *)(uintptr_t)root_pa;
    uint64_t start = base & ~0xFFFULL;
    uint64_t end = (base + len - 1u) & ~0xFFFULL;
    for (uint64_t va = start; va <= end; va += 0x1000u) {
        uint64_t pml4i = (va >> 39) & 0x1FFu;
        uint64_t pdpti = (va >> 30) & 0x1FFu;
        uint64_t pdi   = (va >> 21) & 0x1FFu;
        uint64_t pml4e = root[pml4i];
        if (!(pml4e & PT_PG_PRESENT)) continue;
        if (pml4e & PT_PG_PSE) return 1;
        const uint64_t *pdpt = (const uint64_t *)(uintptr_t)(pml4e & 0x000FFFFFFFFFF000ULL);
        uint64_t pdpte = pdpt[pdpti];
        if (!(pdpte & PT_PG_PRESENT)) continue;
        if (pdpte & PT_PG_PSE) return 1;
        const uint64_t *pd = (const uint64_t *)(uintptr_t)(pdpte & 0x000FFFFFFFFFF000ULL);
        uint64_t pde = pd[pdi];
        if ((pde & PT_PG_PRESENT) && (pde & PT_PG_PSE)) return 1;
    }
    return 0;
}

static void ahci_delay_1ms(void) {
    /* PIT channel 0 one-shot ~1ms (1193 counts @ 1193182Hz). */
    outb(0x43, 0x30);
    outb(0x40, 1193 & 0xFF);
    outb(0x40, (1193 >> 8) & 0xFF);
    for (;;) {
        outb(0x43, 0x00);
        uint16_t lo = inb(0x40);
        uint16_t hi = inb(0x40);
        uint16_t count = (uint16_t)(lo | (hi << 8));
        if (count == 0) break;
        __asm__ volatile("pause");
    }
}

static int ahci_wait_mask_clear(uint32_t reg_off, uint32_t mask, uint32_t timeout_ms, uint32_t *elapsed_ms) {
    uint32_t elapsed = 0;
    while ((ahci_rd32(reg_off) & mask) != 0) {
        if (elapsed >= timeout_ms) { if (elapsed_ms) *elapsed_ms = elapsed; return 0; }
        ahci_delay_1ms();
        elapsed++;
    }
    if (elapsed_ms) *elapsed_ms = elapsed;
    return 1;
}

static int ahci_wait_port_mask_clear(uint32_t port, uint32_t reg, uint32_t mask, uint32_t timeout_ms, uint32_t *elapsed_ms) {
    uint32_t elapsed = 0;
    while ((ahci_prd32(port, reg) & mask) != 0) {
        if (elapsed >= timeout_ms) { if (elapsed_ms) *elapsed_ms = elapsed; return 0; }
        ahci_delay_1ms();
        elapsed++;
    }
    if (elapsed_ms) *elapsed_ms = elapsed;
    return 1;
}

static int ahci_wait_port_tfd_ready(uint32_t port, uint32_t timeout_ms, uint32_t *elapsed_ms) {
    uint32_t elapsed = 0;
    while ((ahci_prd32(port, AHCI_PORT_TFD) & (AHCI_PORT_TFD_BSY | AHCI_PORT_TFD_DRQ)) != 0) {
        if (elapsed >= timeout_ms) { if (elapsed_ms) *elapsed_ms = elapsed; return 0; }
        ahci_delay_1ms();
        elapsed++;
    }
    if (elapsed_ms) *elapsed_ms = elapsed;
    return 1;
}

static int ahci_bohc_wait_bos(ahci_bohc_state_t *r, uint32_t timeout_ms, uint32_t *elapsed_ms) {
    int ok = ahci_wait_mask_clear(AHCI_REG_BOHC, AHCI_BOHC_BOS, timeout_ms, elapsed_ms);
    if (r) r->bohc = ahci_rd32(AHCI_REG_BOHC);
    return ok;
}

static int ahci_bohc_wait_bb(ahci_bohc_state_t *r, uint32_t timeout_ms, uint32_t *elapsed_ms) {
    int ok = ahci_wait_mask_clear(AHCI_REG_BOHC, AHCI_BOHC_BB, timeout_ms, elapsed_ms);
    if (r) r->bohc = ahci_rd32(AHCI_REG_BOHC);
    return ok;
}

static void ahci_log_caps(uint32_t cap, uint32_t vs, uint32_t pi) {
    terminal_putstring("AHCI: version ");
    terminal_print_int((vs >> 16) & 0xFFFFu);
    terminal_putstring(".");
    terminal_print_int(vs & 0xFFFFu);
    terminal_putstring(" ports=");
    terminal_print_int(ahci_cap_np(cap));
    terminal_putstring(" pi=");
    terminal_print_hex(pi);
    terminal_putstring(" s64a=");
    terminal_print_int((uint64_t)ahci_cap_s64a(cap));
    terminal_putstring("\n");
}

static int ahci_port_stop(uint32_t port) {
    uint32_t cmd = ahci_prd32(port, AHCI_PORT_CMD);
    if (cmd & AHCI_PORT_CMD_ST) {
        cmd &= ~AHCI_PORT_CMD_ST;
        ahci_barrier();
        ahci_pwr32(port, AHCI_PORT_CMD, cmd);
        (void)ahci_prd32(port, AHCI_PORT_CMD);
        if (!ahci_wait_port_mask_clear(port, AHCI_PORT_CMD, AHCI_PORT_CMD_CR, 500u, 0)) {
            terminal_putstring("AHCI: port ");
            terminal_print_int(port);
            terminal_putstring(" stop timeout (CR)\n");
            return 0;
        }
    }
    cmd = ahci_prd32(port, AHCI_PORT_CMD);
    if (cmd & AHCI_PORT_CMD_FRE) {
        cmd &= ~AHCI_PORT_CMD_FRE;
        ahci_barrier();
        ahci_pwr32(port, AHCI_PORT_CMD, cmd);
        (void)ahci_prd32(port, AHCI_PORT_CMD);
        if (!ahci_wait_port_mask_clear(port, AHCI_PORT_CMD, AHCI_PORT_CMD_FR, 500u, 0)) {
            terminal_putstring("AHCI: port ");
            terminal_print_int(port);
            terminal_putstring(" stop timeout (FR)\n");
            return 0;
        }
    }
    return 1;
}

static int ahci_port_start(uint32_t port) {
    uint32_t cmd = ahci_prd32(port, AHCI_PORT_CMD);
    cmd |= AHCI_PORT_CMD_FRE;
    ahci_barrier();
    ahci_pwr32(port, AHCI_PORT_CMD, cmd);
    (void)ahci_prd32(port, AHCI_PORT_CMD);

    if (!ahci_wait_port_tfd_ready(port, 1000u, 0)) {
        terminal_putstring("AHCI: port ");
        terminal_print_int(port);
        terminal_putstring(" start timeout (BSY/DRQ)\n");
        return 0;
    }

    cmd = ahci_prd32(port, AHCI_PORT_CMD);
    cmd |= AHCI_PORT_CMD_ST;
    ahci_barrier();
    ahci_pwr32(port, AHCI_PORT_CMD, cmd);
    (void)ahci_prd32(port, AHCI_PORT_CMD);
    return 1;
}

static int ahci_port_identify(uint32_t port) {
    ahci_port_state_t *ps = &g_ahci_ports[port];
    ps->identify_ok = 0;
    ps->sectors = 0;
    ps->lba48 = 0;
    ps->sector_size = 512;
    ps->flush_supported = 0;
    ps->busy = 0;

    if (!ps->started || ps->clb == 0 || ps->ctba == 0) return 0;
    if (ps->busy) return 0;
    ps->busy = 1;

    /* Wait for BSY/DRQ clear before issuing. */
    if (!ahci_wait_port_tfd_ready(port, 1000u, 0)) {
        terminal_putstring("AHCI: port ");
        terminal_print_int(port);
        terminal_putstring(" IDENTIFY timeout waiting BSY/DRQ clear\n");
        ps->busy = 0;
        return 0;
    }

    /* Clear PxIS and PxSERR before issuing. */
    ahci_pwr32(port, AHCI_PORT_SERR, 0xFFFFFFFFu);
    ahci_pwr32(port, AHCI_PORT_IS, 0xFFFFFFFFu);

    volatile uint8_t *page = (volatile uint8_t *)(uintptr_t)(ps->clb - AHCI_CL_OFF);
    volatile ahci_cmd_header_t *hdr = (volatile ahci_cmd_header_t *)(uintptr_t)ps->clb;
    volatile uint8_t *ct = (volatile uint8_t *)(uintptr_t)ps->ctba;
    volatile ahci_prdt_entry_t *prdt = (volatile ahci_prdt_entry_t *)(uintptr_t)(ps->ctba + 0x80u);
    uint64_t id_buf_phys = (ps->clb - AHCI_CL_OFF) + 0x800u;

    /* Zero IDENTIFY buffer (512 bytes) in DMA page. */
    for (uint32_t i = 0; i < 512u; i++) page[0x800u + i] = 0;

    /* Zero command table (CFIS 64 + ACMD 16 + reserved 48 + PRDT 8*16). */
    for (uint32_t i = 0; i < AHCI_CT_SIZE; i++) ct[i] = 0;

    uint8_t fis[20];
    ahci_build_h2d_identify(fis);
    for (int i = 0; i < 20; i++) ct[i] = fis[i];

    ahci_prdt_entry_t prdt_e;
    ahci_build_prdt(&prdt_e, id_buf_phys, 512u);
    prdt[0].dba = prdt_e.dba;
    prdt[0].dbau = prdt_e.dbau;
    prdt[0].rsv = prdt_e.rsv;
    prdt[0].dbc_i = prdt_e.dbc_i;

    ahci_cmd_header_t h;
    ahci_build_cmd_header(&h, (uint32_t)(ps->ctba & 0xFFFFFFFFu), (uint32_t)(ps->ctba >> 32));
    hdr[0].dw0 = h.dw0;
    hdr[0].prdbc = 0;
    hdr[0].ctba = h.ctba;
    hdr[0].ctbau = h.ctbau;
    hdr[0].rsv[0] = 0; hdr[0].rsv[1] = 0; hdr[0].rsv[2] = 0; hdr[0].rsv[3] = 0;

    /* Issue slot 0. */
    ahci_barrier();
    ahci_pwr32(port, AHCI_PORT_CI, 1u);

    /* Poll for completion. */
    uint32_t elapsed = 0;
    int ok = 0;
    while (elapsed < 1000u) {
        uint32_t is = ahci_prd32(port, AHCI_PORT_IS);
        uint32_t tfd = ahci_prd32(port, AHCI_PORT_TFD);
        if ((is & AHCI_PORT_IS_TFES) || (tfd & AHCI_PORT_TFD_ERR)) {
            terminal_putstring("AHCI: port ");
            terminal_print_int(port);
            terminal_putstring(" IDENTIFY error is=");
            terminal_print_hex(is);
            terminal_putstring(" tfd=");
            terminal_print_hex(tfd);
            terminal_putstring("\n");
            ahci_pwr32(port, AHCI_PORT_IS, 0xFFFFFFFFu);
            ps->busy = 0;
            return 0;
        }
        if ((ahci_prd32(port, AHCI_PORT_CI) & 1u) == 0) { ok = 1; break; }
        ahci_delay_1ms();
        elapsed++;
    }
    if (!ok) {
        terminal_putstring("AHCI: port ");
        terminal_print_int(port);
        terminal_putstring(" IDENTIFY timeout\n");
        ahci_pwr32(port, AHCI_PORT_IS, 0xFFFFFFFFu);
        ps->busy = 0;
        return 0;
    }

    ahci_pwr32(port, AHCI_PORT_IS, 0xFFFFFFFFu);
    uint32_t prdbc = hdr[0].prdbc;
    if (prdbc != 512u) {
        terminal_putstring("AHCI: port ");
        terminal_print_int(port);
        terminal_putstring(" IDENTIFY bad PRDBC=");
        terminal_print_int(prdbc);
        terminal_putstring("\n");
        ps->busy = 0;
        return 0;
    }

    uint16_t words[256];
    for (int i = 0; i < 256; i++) {
        uint16_t lo = (uint16_t)page[0x800u + i * 2u];
        uint16_t hi = (uint16_t)page[0x800u + i * 2u + 1u];
        words[i] = (uint16_t)(lo | (hi << 8));
    }

    ahci_identify_result_t r;
    if (!ahci_identify_parse(words, &r)) {
        terminal_putstring("AHCI: port ");
        terminal_print_int(port);
        terminal_putstring(" IDENTIFY parse failed\n");
        ps->busy = 0;
        return 0;
    }
    if (r.lba48_assumed_enabled) {
        terminal_putstring("AHCI: port ");
        terminal_print_int(port);
        terminal_putstring(" LBA48 supported but word86 invalid, assuming enabled\n");
    }
    if (!r.supported) {
        terminal_putstring("AHCI: port ");
        terminal_print_int(port);
        terminal_putstring(" unsupported sector size=");
        terminal_print_int(r.sector_size);
        terminal_putstring("\n");
        ps->busy = 0;
        return 0;
    }

    /* Store in per-port state. */
    for (int i = 0; i < 41; i++) ps->model[i] = r.model[i];
    for (int i = 0; i < 21; i++) ps->serial[i] = r.serial[i];
    for (int i = 0; i < 9; i++) ps->firmware[i] = r.firmware[i];
    ps->sectors = r.sectors;
    ps->lba48 = r.lba48;
    ps->sector_size = r.sector_size;
    ps->flush_supported = r.flush_supported;
    ps->flush_ext_supported = r.flush_ext_supported;
    ps->rw_dma_ext_supported = r.rw_dma_ext_supported;
    ps->identify_ok = 1;
    ps->busy = 0;

    terminal_putstring("AHCI: port ");
    terminal_print_int(port);
    terminal_putstring(" model=\"");
    terminal_putstring(ps->model);
    terminal_putstring("\" sectors=");
    terminal_print_int(ps->sectors);
    terminal_putstring(" lba48=");
    terminal_print_int((uint64_t)ps->lba48);
    terminal_putstring(" ss=");
    terminal_print_int(ps->sector_size);
    terminal_putstring(" flush=");
    terminal_print_int((uint64_t)ps->flush_supported);
    terminal_putstring("\n");
    if (!ps->rw_dma_ext_supported) {
        terminal_putstring("AHCI: port ");
        terminal_print_int(port);
        terminal_putstring(" RW DMA EXT unsupported (lba48=");
        terminal_print_int((uint64_t)ps->lba48);
        terminal_putstring(" flush_ext=");
        terminal_print_int((uint64_t)ps->flush_ext_supported);
        terminal_putstring(")\n");
    }
    return 1;
}

static void ahci_probe_ports(uint32_t pi, int s64a) {
    for (uint32_t p = 0; p < AHCI_MAX_PORTS; p++) {
        if ((pi & (1u << p)) == 0) continue;

        uint32_t ssts = ahci_prd32(p, AHCI_PORT_SSTS);
        uint32_t det = ahci_ssts_det(ssts);
        uint32_t ipm = ahci_ssts_ipm(ssts);
        if (det != 3u || ipm != 1u) {
            terminal_putstring("AHCI: port ");
            terminal_print_int(p);
            terminal_putstring(" skipped (det=");
            terminal_print_int(det);
            terminal_putstring(" ipm=");
            terminal_print_int(ipm);
            terminal_putstring(")\n");
            continue;
        }

        uint32_t sig = ahci_prd32(p, AHCI_PORT_SIG);
        if (!ahci_sig_is_sata(sig)) {
            terminal_putstring("AHCI: port ");
            terminal_print_int(p);
            terminal_putstring(" skipped (sig=");
            terminal_print_hex(sig);
            terminal_putstring(" ");
            terminal_putstring(ahci_sig_name(sig));
            terminal_putstring(")\n");
            continue;
        }

        if (!ahci_port_stop(p)) {
            continue;
        }

        void *dma_page = pmm_alloc_pages_below4g(1);
        if (!dma_page) {
            terminal_putstring("AHCI: port ");
            terminal_print_int(p);
            terminal_putstring(" DMA alloc failed\n");
            continue;
        }
        uint64_t page_phys = (uint64_t)(uintptr_t)dma_page;
        if (!s64a && (page_phys + 0x1000u) > 0x100000000ULL) {
            terminal_putstring("AHCI: port ");
            terminal_print_int(p);
            terminal_putstring(" DMA not below 4G\n");
            pmm_free_pages(dma_page, 1);
            continue;
        }
        if (!ahci_port_layout_ok(page_phys)) {
            terminal_putstring("AHCI: port ");
            terminal_print_int(p);
            terminal_putstring(" bad DMA layout\n");
            pmm_free_pages(dma_page, 1);
            continue;
        }

        /* Identity-map DMA page if needed (safe if already mapped). */
        vmm_map_page(page_phys, page_phys, PT_PG_RW | PT_PG_PCD | PT_PG_PWT);
        memset(dma_page, 0, 4096);

        uint64_t clb = page_phys + AHCI_CL_OFF;
        uint64_t fb  = page_phys + AHCI_FIS_OFF;
        uint64_t ct  = page_phys + AHCI_CT_OFF;

        ahci_pwr32(p, AHCI_PORT_CLB, (uint32_t)(clb & 0xFFFFFFFFu));
        ahci_pwr32(p, AHCI_PORT_CLBU, (uint32_t)(clb >> 32));
        ahci_pwr32(p, AHCI_PORT_FB, (uint32_t)(fb & 0xFFFFFFFFu));
        ahci_pwr32(p, AHCI_PORT_FBU, (uint32_t)(fb >> 32));

        /* Clear errors/interrupt status; keep interrupts disabled. */
        ahci_pwr32(p, AHCI_PORT_SERR, 0xFFFFFFFFu);
        ahci_pwr32(p, AHCI_PORT_IS, 0xFFFFFFFFu);
        ahci_pwr32(p, AHCI_PORT_IE, 0u);

        if (!ahci_port_start(p)) {
            g_ahci_ports[p].started = 0;
            pmm_free_pages(dma_page, 1);
            continue;
        }

        g_ahci_ports[p].port = (int)p;
        g_ahci_ports[p].sig = sig;
        g_ahci_ports[p].clb = clb;
        g_ahci_ports[p].fb = fb;
        g_ahci_ports[p].ctba = ct;
        g_ahci_ports[p].started = 1;
        g_ahci_ports[p].busy = 0;
        g_ahci_ports[p].identify_ok = 0;

        terminal_putstring("AHCI: port ");
        terminal_print_int(p);
        terminal_putstring(" sig=");
        terminal_print_hex(sig);
        terminal_putstring(" sata started\n");

        (void)ahci_port_identify(p);
    }
}

#ifdef AHCI_SELFTEST
static void ahci_selftest_pattern(uint8_t *buf, uint32_t bytes, uint64_t lba) {
    for (uint32_t i = 0; i < bytes; i++) {
        buf[i] = (uint8_t)(((uint64_t)i * 131u + lba * 17u + 0x5Au) & 0xFFu);
    }
}

static int ahci_selftest_cmp(const uint8_t *a, const uint8_t *b, uint32_t bytes, uint32_t *off_out) {
    for (uint32_t i = 0; i < bytes; i++) {
        if (a[i] != b[i]) { if (off_out) *off_out = i; return -1; }
    }
    return 0;
}

static void ahci_selftest(void) {
    int port = -1;
    for (uint32_t p = 0; p < AHCI_MAX_PORTS; p++) {
        if (g_ahci_ports[p].identify_ok && strcmp(g_ahci_ports[p].serial, "ARCSCRATCH") == 0) {
            port = (int)p;
            break;
        }
    }
    if (port < 0) {
        terminal_putstring("AHCI: selftest skipped (no ARCSCRATCH disk)\n");
        return;
    }

    ahci_port_state_t *ps = &g_ahci_ports[port];
    if (!ps->rw_dma_ext_supported) {
        terminal_putstring("AHCI: selftest FAIL unsupported scratch disk\n");
        return;
    }

    /* Argument checks without touching hardware. */
    uint8_t tmp[512];
    memset(tmp, 0, sizeof(tmp));
    if (ahci_read(port, 0, 0, tmp) != AHCI_ERR_BADARG) { terminal_putstring("AHCI: selftest FAIL count0 read\n"); return; }
    if (ahci_read(port, ps->sectors, 1, tmp) != AHCI_ERR_RANGE) { terminal_putstring("AHCI: selftest FAIL oob read\n"); return; }
    int old_we = ahci_writes_enabled;
    ahci_writes_enabled = 1;
    if (ahci_write(port, 0, 0, tmp) != AHCI_ERR_BADARG) { terminal_putstring("AHCI: selftest FAIL count0 write\n"); ahci_writes_enabled = old_we; return; }
    if (ahci_write(port, ps->sectors, 1, tmp) != AHCI_ERR_RANGE) { terminal_putstring("AHCI: selftest FAIL oob write\n"); ahci_writes_enabled = old_we; return; }
    ahci_writes_enabled = old_we;

    uint64_t mid = ps->sectors / 2;
    uint64_t last = ps->sectors - 1;
    uint64_t lbas[5];
    lbas[0] = 0; lbas[1] = 1; lbas[2] = 127 < ps->sectors ? 127 : last; lbas[3] = mid; lbas[4] = last;

    static uint8_t wbuf[64 * 512];
    static uint8_t rbuf[64 * 512];
    static uint8_t pat[512];

    ahci_writes_enabled = 1;
    for (int i = 0; i < 5; i++) {
        ahci_selftest_pattern(pat, 512, lbas[i]);
        int wrc = ahci_write(port, lbas[i], 1, pat);
        if (wrc != AHCI_OK) {
            terminal_putstring("AHCI: selftest FAIL write lba=");
            terminal_print_int(lbas[i]);
            terminal_putstring(" rc=");
            if (wrc < 0) { terminal_putstring("-"); terminal_print_int((uint64_t)(-wrc)); }
            else terminal_print_int((uint64_t)wrc);
            terminal_putstring("\n");
            ahci_writes_enabled = 0; return;
        }
    }

    /* 64-sector block at LBA 2 (if fits) else LBA 0 */
    uint64_t blk_lba = (ps->sectors >= 66) ? 2 : 0;
    for (uint32_t s = 0; s < 64; s++) ahci_selftest_pattern(wbuf + s * 512, 512, blk_lba + s);
    if (ahci_write(port, blk_lba, 64, wbuf) != AHCI_OK) {
        terminal_putstring("AHCI: selftest FAIL write 64-block\n");
        ahci_writes_enabled = 0; return;
    }

    if (ahci_flush(port) != AHCI_OK) {
        terminal_putstring("AHCI: selftest FAIL flush\n");
        ahci_writes_enabled = 0; return;
    }
    ahci_writes_enabled = 0;

    /* Read back and compare single sectors */
    for (int i = 0; i < 5; i++) {
        uint64_t l = lbas[i];
        /* skip ones overwritten by 64-block if overlapping */
        if (l >= blk_lba && l < blk_lba + 64) continue;
        ahci_selftest_pattern(pat, 512, l);
        memset(rbuf, 0, 512);
        if (ahci_read(port, l, 1, rbuf) != AHCI_OK) {
            terminal_putstring("AHCI: selftest FAIL read lba=");
            terminal_print_int(l); terminal_putstring("\n"); return;
        }
        uint32_t off = 0;
        if (ahci_selftest_cmp(pat, rbuf, 512, &off) != 0) {
            terminal_putstring("AHCI: selftest FAIL mismatch lba=");
            terminal_print_int(l);
            terminal_putstring(" off=");
            terminal_print_int(off);
            terminal_putstring("\n"); return;
        }
    }

    /* Read back 64-block */
    memset(rbuf, 0, sizeof(rbuf));
    if (ahci_read(port, blk_lba, 64, rbuf) != AHCI_OK) {
        terminal_putstring("AHCI: selftest FAIL read 64-block\n"); return;
    }
    uint32_t off = 0;
    if (ahci_selftest_cmp(wbuf, rbuf, 64 * 512, &off) != 0) {
        terminal_putstring("AHCI: selftest FAIL mismatch 64-block off=");
        terminal_print_int(off);
        terminal_putstring("\n"); return;
    }

    terminal_putstring("AHCI: selftest PASS\n");
}
#endif

int ahci_dev_model_is_qemu(const char *name) {
    if (!name) return 0;
    if (!(name[0]=='a' && name[1]=='h' && name[2]=='c' && name[3]=='i')) return 0;
    uint32_t p = 0;
    for (int i = 4; name[i]; i++) {
        if (name[i] < '0' || name[i] > '9') return 0;
        p = p * 10u + (uint32_t)(name[i] - '0');
    }
    if (p >= AHCI_MAX_PORTS) return 0;
    return storage_model_is_qemu(g_ahci_ports[p].model);
}

#ifdef AHCI_HOST_TEST
void ahci_test_set_port_model(int port, const char *model) {
    if (port < 0 || port >= (int)AHCI_MAX_PORTS) return;
    int i = 0;
    for (; model && model[i] && i < 40; i++) g_ahci_ports[port].model[i] = model[i];
    g_ahci_ports[port].model[i] = '\0';
}
#endif

#ifndef AHCI_HOST_TEST
static void ahci_register_blockdevs(void);
#endif

void ahci_init(void) {
    g_ahci_present = 0;
    g_ahci_s64a = 0;
    g_ahci_bar5 = 0;
    g_ahci_irq = 0;
    g_ahci_abar = 0;
    for (uint32_t i = 0; i < AHCI_MAX_PORTS; i++) {
        g_ahci_ports[i].port = (int)i;
        g_ahci_ports[i].sig = 0;
        g_ahci_ports[i].clb = 0;
        g_ahci_ports[i].fb = 0;
        g_ahci_ports[i].ctba = 0;
        g_ahci_ports[i].started = 0;
        g_ahci_ports[i].busy = 0;
        g_ahci_ports[i].failed = 0;
        g_ahci_ports[i].identify_ok = 0;
        g_ahci_ports[i].sectors = 0;
        g_ahci_ports[i].lba48 = 0;
        g_ahci_ports[i].sector_size = 512;
        g_ahci_ports[i].flush_supported = 0;
        g_ahci_ports[i].flush_ext_supported = 0;
        g_ahci_ports[i].rw_dma_ext_supported = 0;
        g_ahci_ports[i].writable = 0;
        g_ahci_ports[i].bd_registered = 0;
        g_ahci_ports[i].bd_failed_logged = 0;
        g_ahci_ports[i].bd_name[0] = '\0';
        g_ahci_ports[i].model[0] = '\0';
        g_ahci_ports[i].serial[0] = '\0';
        g_ahci_ports[i].firmware[0] = '\0';
    }

    const struct pci_device* dev = pci_find_class(0x01, 0x06, 0);
    if (!dev) {
        terminal_putstring("AHCI: no controller detected\n");
        return;
    }

    uint32_t bar5 = dev->bar[5];
    if (!ahci_bar_is_mmio32(bar5)) {
        terminal_putstring("AHCI: BAR5 invalid/not MMIO32, using ATA PIO fallback\n");
        return;
    }
    uint32_t abar = ahci_bar_base(bar5);

    /* Reserve MMIO range in PMM so it is never allocated as RAM. */
    pmm_reserve_range(abar, AHCI_MMIO_MAP_SIZE);

    if (ahci_large_page_covered(abar, AHCI_MMIO_MAP_SIZE)) {
        terminal_putstring("AHCI: ABAR covered by large-page mapping, using ATA PIO fallback\n");
        return;
    }

    /* Map ABAR uncached (PCD+PWT), identity VA==PA. */
    for (uint32_t off = 0; off < AHCI_MMIO_MAP_SIZE; off += 0x1000u) {
        vmm_map_page((uint64_t)(abar + off), (uint64_t)(abar + off),
                     PT_PG_RW | PT_PG_PCD | PT_PG_PWT);
    }
    g_ahci_abar = (volatile uint32_t*)(uintptr_t)abar;

    /* Enable PCI Memory Space + Bus Master (DMA needed in B). */
    uint32_t cmd = pci_config_read32(dev->bus, dev->slot, dev->func, 0x04);
    cmd |= (1u << 1); /* MEM space */
    cmd |= (1u << 2); /* Bus Master */
    pci_config_write32(dev->bus, dev->slot, dev->func, 0x04, cmd);

    uint32_t cap = ahci_rd32(AHCI_REG_CAP);
    uint32_t vs  = ahci_rd32(AHCI_REG_VS);
    uint32_t pi  = ahci_rd32(AHCI_REG_PI);
    if (cap == 0xFFFFFFFFu || vs == 0xFFFFFFFFu) {
        terminal_putstring("AHCI: CAP/VS reads 0xFFFFFFFF, no device; ATA PIO fallback\n");
        g_ahci_abar = 0;
        return;
    }

    /* GHC.AE via read-modify-write. Leave GHC.IE=0, do not touch GHC.HR. */
    uint32_t ghc = ahci_rd32(AHCI_REG_GHC);
    ghc |= AHCI_GHC_AE;
    ghc &= ~AHCI_GHC_IE;
    ahci_wr32(AHCI_REG_GHC, ghc);

    uint32_t cap2 = ahci_rd32(AHCI_REG_CAP2);
    uint32_t bohc = ahci_rd32(AHCI_REG_BOHC);
    if ((cap2 & AHCI_CAP2_BOH) && (bohc & AHCI_BOHC_BOS)) {
        ahci_wr32(AHCI_REG_BOHC, bohc | AHCI_BOHC_OOS);
        bohc = ahci_rd32(AHCI_REG_BOHC);
    }
    ahci_bohc_state_t bohc_state = { .cap2 = cap2, .bohc = bohc };
    uint32_t bohc_elapsed = 0;
    int bohc_res = ahci_bohc_run(&bohc_state, ahci_bohc_wait_bos, ahci_bohc_wait_bb, &bohc_elapsed);
    if (bohc_res == AHCI_BOHC_SUCCESS || bohc_res == AHCI_BOHC_ALREADY_OWNED) {
        uint32_t bohc_now = ahci_rd32(AHCI_REG_BOHC);
        ahci_wr32(AHCI_REG_BOHC, bohc_now | AHCI_BOHC_OOS);
    }
    terminal_putstring("AHCI: BOHC ");
    if (bohc_res == AHCI_BOHC_UNSUPPORTED) terminal_putstring("unsupported");
    else if (bohc_res == AHCI_BOHC_ALREADY_OWNED) terminal_putstring("already-owned");
    else if (bohc_res == AHCI_BOHC_SUCCESS) terminal_putstring("success");
    else terminal_putstring("bb-timeout");
    terminal_putstring(" elapsed=");
    terminal_print_int(bohc_elapsed);
    terminal_putstring("ms\n");

    terminal_putstring("AHCI: controller detected vendor=");
    terminal_print_hex(dev->vendor);
    terminal_putstring(" device=");
    terminal_print_hex(dev->device);
    terminal_putstring(" bar5=");
    terminal_print_hex(abar);
    terminal_putstring(" irq=");
    terminal_print_int(dev->irq_line);
    terminal_putstring("\n");
    ahci_log_caps(cap, vs, pi);

    if (pi == 0) {
        terminal_putstring("AHCI: PI==0, no implemented ports; ATA PIO fallback\n");
        g_ahci_abar = 0;
        return;
    }

    g_ahci_present = 1;
    g_ahci_s64a = ahci_cap_s64a(cap);
    g_ahci_bar5 = abar;
    g_ahci_irq = dev->irq_line;

    ahci_probe_ports(pi, g_ahci_s64a);

#ifdef AHCI_SELFTEST
    ahci_selftest();
#endif
    ahci_register_blockdevs();
}

static uint64_t ahci_kernel_virt_to_phys(void *ctx, uint64_t va, int *ok) {
    (void)ctx;
    if (ok) *ok = 0;
    uint64_t root_pa = vmm_master_root();
    uint64_t *table = (uint64_t *)(uintptr_t)root_pa;
    if (!table) return 0;

    uint64_t pml4e = table[(va >> 39) & 0x1FFu];
    if (!(pml4e & PT_PG_PRESENT)) return 0;
    uint64_t *pdpt = (uint64_t *)(uintptr_t)(pml4e & 0x000FFFFFFFFFF000ULL);

    uint64_t pdpte = pdpt[(va >> 30) & 0x1FFu];
    if (!(pdpte & PT_PG_PRESENT)) return 0;
    if (pdpte & PT_PG_PSE) {
        if (ok) *ok = 1;
        return (pdpte & 0x000FFFFFC0000000ULL) | (va & 0x3FFFFFFFu);
    }
    uint64_t *pd = (uint64_t *)(uintptr_t)(pdpte & 0x000FFFFFFFFFF000ULL);

    uint64_t pde = pd[(va >> 21) & 0x1FFu];
    if (!(pde & PT_PG_PRESENT)) return 0;
    if (pde & PT_PG_PSE) {
        if (ok) *ok = 1;
        return (pde & 0x000FFFFFFFE00000ULL) | (va & 0x1FFFFFu);
    }
    uint64_t *pt = (uint64_t *)(uintptr_t)(pde & 0x000FFFFFFFFFF000ULL);

    uint64_t pte = pt[(va >> 12) & 0x1FFu];
    if (!(pte & PT_PG_PRESENT)) return 0;
    if (ok) *ok = 1;
    return (pte & 0x000FFFFFFFFFF000ULL) | (va & 0xFFFu);
}

static void ahci_log_cmd_error(uint32_t port, const char *op, uint32_t is, uint32_t tfd, uint32_t serr) {
    terminal_putstring("AHCI: port ");
    terminal_print_int(port);
    terminal_putstring(" ");
    terminal_putstring(op);
    terminal_putstring(" error is=");
    terminal_print_hex(is);
    terminal_putstring(" tfd=");
    terminal_print_hex(tfd);
    terminal_putstring(" serr=");
    terminal_print_hex(serr);
    terminal_putstring("\n");
}

static int ahci_port_wait_ready(uint32_t port, uint32_t timeout_ms) {
    return ahci_wait_port_tfd_ready(port, timeout_ms, 0);
}

static int ahci_issue_cmd(uint32_t port, uint8_t cmd, uint64_t lba, uint32_t count,
                          void *buf, int write, uint32_t timeout_ms) {
    ahci_port_state_t *ps = &g_ahci_ports[port];
    if (!ps->started || !ps->identify_ok || ps->failed) return AHCI_ERR_UNSUPPORTED;
    if (ps->busy) return AHCI_ERR_BUSY;
    ps->busy = 1;

    if (!ahci_port_wait_ready(port, 1000u)) {
        terminal_putstring("AHCI: port ");
        terminal_print_int(port);
        terminal_putstring(" cmd timeout waiting BSY/DRQ\n");
        ps->failed = 1;
        ps->busy = 0;
        return AHCI_ERR_TIMEOUT;
    }

    ahci_pwr32(port, AHCI_PORT_SERR, 0xFFFFFFFFu);
    ahci_pwr32(port, AHCI_PORT_IS, 0xFFFFFFFFu);

    volatile uint8_t *dma_page = (volatile uint8_t *)(uintptr_t)(ps->clb - AHCI_CL_OFF);
    volatile ahci_cmd_header_t *hdr = (volatile ahci_cmd_header_t *)(uintptr_t)ps->clb;
    volatile uint8_t *ct = (volatile uint8_t *)(uintptr_t)ps->ctba;
    volatile ahci_prdt_entry_t *prdt = (volatile ahci_prdt_entry_t *)(uintptr_t)(ps->ctba + 0x80u);

    for (uint32_t i = 0; i < AHCI_CT_SIZE; i++) ct[i] = 0;

    uint8_t fis[20];
    if (cmd == ATA_CMD_FLUSH_CACHE_EXT) {
        ahci_build_h2d_flush(fis);
    } else {
        ahci_build_h2d_rw(fis, cmd, lba, (uint16_t)count);
    }
    for (int i = 0; i < 20; i++) ct[i] = fis[i];

    uint16_t prdtl = 0;
    if (cmd != ATA_CMD_FLUSH_CACHE_EXT) {
        ahci_prdt_entry_t tmp[AHCI_PRDT_ENTRIES];
        int n = ahci_prdt_build_from_va(tmp, AHCI_PRDT_ENTRIES, ahci_kernel_virt_to_phys, 0,
                                        (uint64_t)(uintptr_t)buf, count * 512u);
        if (n <= 0) {
            terminal_putstring("AHCI: port ");
            terminal_print_int(port);
            terminal_putstring(" PRDT build failed n=");
            if (n < 0) { terminal_putstring("-"); terminal_print_int((uint64_t)(-n)); }
            else terminal_print_int((uint64_t)n);
            terminal_putstring(" bytes=");
            terminal_print_int(count * 512u);
            terminal_putstring("\n");
            ps->busy = 0;
            return AHCI_ERR_BADARG;
        }
        prdtl = (uint16_t)n;
        for (int i = 0; i < n; i++) {
            if (!g_ahci_s64a && tmp[i].dbau != 0) {
                ps->busy = 0;
                return AHCI_ERR_UNSUPPORTED;
            }
            prdt[i].dba = tmp[i].dba;
            prdt[i].dbau = tmp[i].dbau;
            prdt[i].rsv = tmp[i].rsv;
            prdt[i].dbc_i = tmp[i].dbc_i;
        }
    }

    ahci_cmd_header_t h;
    ahci_build_cmd_header_ex(&h, prdtl, write, (uint32_t)(ps->ctba & 0xFFFFFFFFu), (uint32_t)(ps->ctba >> 32));
    hdr[0].dw0 = h.dw0;
    hdr[0].prdbc = 0;
    hdr[0].ctba = h.ctba;
    hdr[0].ctbau = h.ctbau;
    hdr[0].rsv[0] = 0; hdr[0].rsv[1] = 0; hdr[0].rsv[2] = 0; hdr[0].rsv[3] = 0;
    (void)dma_page;

    ahci_barrier();
    ahci_pwr32(port, AHCI_PORT_CI, 1u);

    uint32_t elapsed = 0;
    int done = 0;
    while (elapsed < timeout_ms) {
        uint32_t is = ahci_prd32(port, AHCI_PORT_IS);
        uint32_t tfd = ahci_prd32(port, AHCI_PORT_TFD);
        uint32_t serr = ahci_prd32(port, AHCI_PORT_SERR);
        if ((is & (AHCI_PORT_IS_TFES | (1u<<29) | (1u<<28) | (1u<<27) | (1u<<26))) || (tfd & AHCI_PORT_TFD_ERR)) {
            ahci_log_cmd_error(port, (cmd == ATA_CMD_FLUSH_CACHE_EXT) ? "FLUSH" : (write ? "WRITE" : "READ"), is, tfd, serr);
            ahci_pwr32(port, AHCI_PORT_IS, 0xFFFFFFFFu);
            ps->failed = 1;
            ps->busy = 0;
            return AHCI_ERR_DEV;
        }
        if ((ahci_prd32(port, AHCI_PORT_CI) & 1u) == 0) { done = 1; break; }
        ahci_delay_1ms();
        elapsed++;
        if (elapsed == 1000u) {
            terminal_putstring("AHCI: port ");
            terminal_print_int(port);
            terminal_putstring(" cmd slow (>1s)\n");
        }
    }
    if (!done) {
        terminal_putstring("AHCI: port ");
        terminal_print_int(port);
        terminal_putstring(" cmd timeout\n");
        ahci_pwr32(port, AHCI_PORT_IS, 0xFFFFFFFFu);
        ps->failed = 1;
        ps->busy = 0;
        return AHCI_ERR_TIMEOUT;
    }

    ahci_pwr32(port, AHCI_PORT_IS, 0xFFFFFFFFu);
    if (cmd != ATA_CMD_FLUSH_CACHE_EXT) {
        uint32_t expected = count * 512u;
        uint32_t prdbc = hdr[0].prdbc;
        if (prdbc != expected) {
            terminal_putstring("AHCI: port ");
            terminal_print_int(port);
            terminal_putstring(" bad PRDBC=");
            terminal_print_int(prdbc);
            terminal_putstring(" expected=");
            terminal_print_int(expected);
            terminal_putstring("\n");
            ps->failed = 1;
            ps->busy = 0;
            return AHCI_ERR_DEV;
        }
    }

    ps->busy = 0;
    return AHCI_OK;
}

int ahci_read(uint32_t port, uint64_t lba, uint32_t count, void *buf) {
    if (port >= AHCI_MAX_PORTS) return AHCI_ERR_BADARG;
    ahci_port_state_t *ps = &g_ahci_ports[port];
    int v = ahci_validate_rw_args(ps, lba, count, buf);
    if (v != AHCI_OK) return v;
    if (ps->busy) return AHCI_ERR_BUSY;

    uint32_t remaining = count;
    uint64_t cur_lba = lba;
    uint8_t *cur_buf = (uint8_t *)buf;
    while (remaining > 0) {
        uint32_t chunk = remaining;
        if (chunk > AHCI_MAX_SECTORS) chunk = AHCI_MAX_SECTORS;
        /* Reduce chunk until PRDT fits in 8 entries */
        for (;;) {
            ahci_prdt_entry_t tmp[AHCI_PRDT_ENTRIES];
            int n = ahci_prdt_build_from_va(tmp, AHCI_PRDT_ENTRIES, ahci_kernel_virt_to_phys, 0,
                                            (uint64_t)(uintptr_t)cur_buf, chunk * 512u);
            if (n > 0) break;
            if (chunk <= 1) return AHCI_ERR_BADARG;
            chunk /= 2;
        }
        int r = ahci_issue_cmd(port, ATA_CMD_READ_DMA_EXT, cur_lba, chunk, cur_buf, 0, 5000u);
        if (r != AHCI_OK) return r;
        cur_lba += chunk;
        cur_buf += chunk * 512u;
        remaining -= chunk;
    }
    return AHCI_OK;
}

static int ahci_port_is_scratch(uint32_t port) {
    if (port >= AHCI_MAX_PORTS) return 0;
    const char *s = g_ahci_ports[port].serial;
    return (s[0]=='A' && s[1]=='R' && s[2]=='C' && s[3]=='S' && s[4]=='C' &&
            s[5]=='R' && s[6]=='A' && s[7]=='T' && s[8]=='C' && s[9]=='H' && s[10]=='\0');
}

int ahci_write(uint32_t port, uint64_t lba, uint32_t count, const void *buf) {
    if (port >= AHCI_MAX_PORTS) return AHCI_ERR_BADARG;
    ahci_port_state_t *ps = &g_ahci_ports[port];
    /* Production writes require per-disk writable flag. Selftest override
     * (ahci_writes_enabled) is allowed only for the ARCSCRATCH disk. */
    if (!ps->writable && !(ahci_writes_enabled && ahci_port_is_scratch(port)))
        return AHCI_ERR_UNSUPPORTED;
    int v = ahci_validate_rw_args(ps, lba, count, buf);
    if (v != AHCI_OK) return v;
    if (ps->busy) return AHCI_ERR_BUSY;

    uint32_t remaining = count;
    uint64_t cur_lba = lba;
    uint8_t *cur_buf = (uint8_t *)(uintptr_t)buf;
    while (remaining > 0) {
        uint32_t chunk = remaining;
        if (chunk > AHCI_MAX_SECTORS) chunk = AHCI_MAX_SECTORS;
        for (;;) {
            ahci_prdt_entry_t tmp[AHCI_PRDT_ENTRIES];
            int n = ahci_prdt_build_from_va(tmp, AHCI_PRDT_ENTRIES, ahci_kernel_virt_to_phys, 0,
                                            (uint64_t)(uintptr_t)cur_buf, chunk * 512u);
            if (n > 0) break;
            if (chunk <= 1) return AHCI_ERR_BADARG;
            chunk /= 2;
        }
        int r = ahci_issue_cmd(port, ATA_CMD_WRITE_DMA_EXT, cur_lba, chunk, cur_buf, 1, 5000u);
        if (r != AHCI_OK) return r;
        cur_lba += chunk;
        cur_buf += chunk * 512u;
        remaining -= chunk;
    }
    return AHCI_OK;
}

int ahci_flush(uint32_t port) {
    if (port >= AHCI_MAX_PORTS) return AHCI_ERR_BADARG;
    ahci_port_state_t *ps = &g_ahci_ports[port];
    int v = ahci_validate_flush_args(ps);
    if (v != AHCI_OK) return v;
    if (ps->busy) return AHCI_ERR_BUSY;
    return ahci_issue_cmd(port, ATA_CMD_FLUSH_CACHE_EXT, 0, 0, 0, 0, 30000u);
}

static void ahci_bd_log_fail_once(uint32_t port) {
    ahci_port_state_t *ps = &g_ahci_ports[port];
    if (ps->bd_failed_logged) return;
    ps->bd_failed_logged = 1;
    terminal_putstring("AHCI: ahci");
    terminal_print_int(port);
    terminal_putstring(" I/O failed, port marked unusable\n");
}

static int ahci_bd_read(struct blockdev *dev, uint32_t lba, uint32_t count, void *buf) {
    uint32_t port = (uint32_t)(uintptr_t)dev->ctx;
    if (port >= AHCI_MAX_PORTS || !dev) return -1;
    ahci_port_state_t *ps = &g_ahci_ports[port];
    if (ps->failed) { ahci_bd_log_fail_once(port); return -1; }
    if (count == 0 || !buf) return -1;
    uint64_t end = (uint64_t)lba + (uint64_t)count;
    if (end < lba || end > ps->sectors) return -1;

    uint32_t done = 0;
    while (done < count) {
        uint32_t chunk = count - done;
        if (chunk > AHCI_MAX_SECTORS) chunk = AHCI_MAX_SECTORS;
        int r = ahci_read(port, (uint64_t)lba + done, chunk, (uint8_t *)buf + (size_t)done * 512u);
        if (r != AHCI_OK) {
            terminal_putstring("AHCI: ahci");
            terminal_print_int(port);
            terminal_putstring(" read error lba=");
            terminal_print_int(lba + done);
            terminal_putstring(" done=");
            terminal_print_int(done);
            terminal_putstring("/");
            terminal_print_int(count);
            terminal_putstring(" rc=");
            if (r < 0) { terminal_putstring("-"); terminal_print_int((uint64_t)(-r)); }
            else terminal_print_int((uint64_t)r);
            terminal_putstring("\n");
            if (ps->failed) ahci_bd_log_fail_once(port);
            return -1;
        }
        done += chunk;
    }
    return 0;
}

static int ahci_bd_write(struct blockdev *dev, uint32_t lba, uint32_t count, const void *buf) {
    uint32_t port = (uint32_t)(uintptr_t)dev->ctx;
    if (port >= AHCI_MAX_PORTS || !dev) return -1;
    ahci_port_state_t *ps = &g_ahci_ports[port];
    if (dev->read_only || !ps->writable) return -1;
    if (ps->failed) { ahci_bd_log_fail_once(port); return -1; }
    if (count == 0 || !buf) return -1;
    uint64_t end = (uint64_t)lba + (uint64_t)count;
    if (end < lba || end > ps->sectors) return -1;

    uint32_t done = 0;
    while (done < count) {
        uint32_t chunk = count - done;
        if (chunk > AHCI_MAX_SECTORS) chunk = AHCI_MAX_SECTORS;
        int r = ahci_write(port, (uint64_t)lba + done, chunk, (const uint8_t *)buf + (size_t)done * 512u);
        if (r != AHCI_OK) {
            terminal_putstring("AHCI: ahci");
            terminal_print_int(port);
            terminal_putstring(" write error lba=");
            terminal_print_int(lba + done);
            terminal_putstring(" done=");
            terminal_print_int(done);
            terminal_putstring("/");
            terminal_print_int(count);
            terminal_putstring(" rc=");
            if (r < 0) { terminal_putstring("-"); terminal_print_int((uint64_t)(-r)); }
            else terminal_print_int((uint64_t)r);
            terminal_putstring("\n");
            if (ps->failed) ahci_bd_log_fail_once(port);
            return -1;
        }
        done += chunk;
    }
    return 0;
}

static int ahci_bd_flush(struct blockdev *dev) {
    uint32_t port = (uint32_t)(uintptr_t)dev->ctx;
    if (port >= AHCI_MAX_PORTS || !dev) return -1;
    ahci_port_state_t *ps = &g_ahci_ports[port];
    if (ps->failed) { ahci_bd_log_fail_once(port); return -1; }
    if (!ps->flush_ext_supported) {
        /* Read-only no-flush devices have no dirty write-back data to flush. */
        return dev->read_only ? 0 : -1;
    }
    int r = ahci_flush(port);
    if (r != AHCI_OK) {
        if (ps->failed) ahci_bd_log_fail_once(port);
        return -1;
    }
    return 0;
}

static void ahci_register_blockdevs(void) {
    for (uint32_t p = 0; p < AHCI_MAX_PORTS; p++) {
        ahci_port_state_t *ps = &g_ahci_ports[p];
        ps->bd_registered = 0;
        ps->writable = 0;
        ps->bd_failed_logged = 0;
        ps->bd_name[0] = 'a'; ps->bd_name[1] = 'h'; ps->bd_name[2] = 'c'; ps->bd_name[3] = 'i';
        /* port number: support 0-31, but names ahci0..ahci9, ahci10.. */
        if (p < 10) { ps->bd_name[4] = (char)('0' + p); ps->bd_name[5] = '\0'; }
        else { ps->bd_name[4] = (char)('0' + (p / 10)); ps->bd_name[5] = (char)('0' + (p % 10)); ps->bd_name[6] = '\0'; }

        if (!ahci_port_can_register(ps->identify_ok, ps->started, ps->sector_size, ps->rw_dma_ext_supported, ps->failed))
            continue;

        struct blockdev *bd = &g_ahci_bdevs[p];
        memset(bd, 0, sizeof(*bd));
        bd->name = ps->bd_name;
        bd->sector_count = (ps->sectors > 0xFFFFFFFFULL) ? 0xFFFFFFFFu : (uint32_t)ps->sectors;
        bd->read = ahci_bd_read;
        bd->write = ahci_bd_write;
        bd->flush = ahci_bd_flush;
        bd->ctx = (void *)(uintptr_t)p;
        bd->read_only = ps->flush_ext_supported ? 0 : 1;
        if (bd->read_only) {
            terminal_putstring("AHCI: port ");
            terminal_print_int(p);
            terminal_putstring(" read-only (no FLUSH CACHE EXT)\n");
        }
        if (blockdev_register(bd) >= 0) {
            ps->bd_registered = 1;
        }
    }
}

/* moved above AHCI_HOST_TEST guard for host testing */

void ahci_apply_write_policy(const char *root_name) {
#ifdef ARC_AHCI_WRITE_REAL
    int write_real = 1;
#else
    int write_real = 0;
#endif
    for (uint32_t p = 0; p < AHCI_MAX_PORTS; p++) {
        ahci_port_state_t *ps = &g_ahci_ports[p];
        if (!ps->bd_registered) continue;
        struct blockdev *bd = &g_ahci_bdevs[p];
        int is_root = (root_name && bd->name && strcmp(root_name, bd->name) == 0);
        int writable = ahci_write_policy_writable(ps->model, is_root, ps->flush_ext_supported, write_real);
        ps->writable = writable;
        bd->read_only = writable ? 0 : 1;
        if (!writable) {
            if (is_root) {
                terminal_putstring("AHCI: port ");
                terminal_print_int(p);
                terminal_putstring(" read-only (real hardware, ARC_AHCI_WRITE_REAL not set)\n");
            }
        }
    }
}

#endif /* AHCI_HOST_TEST */
