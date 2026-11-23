/* Host tests for M4.4 E.1 storage boot policy helpers. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#undef SYS_OPEN
#include "kernel.h"

static int pass_count, fail_count;
#define CHECK(cond, msg) do { if (cond) { pass_count++; } else { fail_count++; printf("FAIL: %s\n", msg); } } while(0)

static void test_esp_skipped(void) {
    struct part_info esp; memset(&esp, 0, sizeof(esp));
    esp.valid = 1; esp.gpt_type = 2; esp.type = 0xEF; esp.lba_start = 2048; esp.sector_count = 1024;
    CHECK(storage_is_esp(&esp) == 1, "esp detected");
    CHECK(storage_root_can_select(1, 1, 0, 1) == 0, "esp skipped pass0 qemu");
    CHECK(storage_root_can_select(1, 1, 0, 0) == 0, "esp skipped pass0 real");
    CHECK(storage_root_can_select(1, 1, 1, 1) == 0, "esp skipped qemu fallback");
}

static void test_qemu_fallback(void) {
    CHECK(storage_root_can_select(0, 0, 1, 1) == 1, "qemu fallback selects qemu");
    CHECK(storage_root_can_select(0, 0, 1, 0) == 0, "qemu fallback rejects real");
    CHECK(storage_root_can_select(1, 0, 0, 0) == 1, "arcroot qualifies real");
    CHECK(storage_root_can_select(0, 0, 0, 1) == 0, "no arcroot no fallback pass0");
}

static void test_two_qualifying_first_wins(void) {
    struct part_info parts[2];
    memset(parts, 0, sizeof(parts));
    parts[0].valid = 1; parts[0].type = 0x0C; parts[0].lba_start = 2048; parts[0].sector_count = 1000;
    parts[1].valid = 1; parts[1].type = 0x0C; parts[1].lba_start = 4096; parts[1].sector_count = 1000;
    int qual[2] = {1, 1};
    int selected = -1;
    for (int i = 0; i < 2; i++) {
        int is_esp = storage_is_esp(&parts[i]);
        if (storage_root_can_select(qual[i], is_esp, 0, 0)) { selected = i; break; }
    }
    CHECK(selected == 0, "two qualifying first wins");
}

static void test_probe_policy(void) {
    struct part_info p; memset(&p, 0, sizeof(p));
    p.valid = 1; p.gpt_type = 1; CHECK(storage_part_probe_fat32(&p, 0, 0) == 1, "probe gpt basic data");
    memset(&p, 0, sizeof(p)); p.valid = 1; p.gpt_type = 2; CHECK(storage_part_probe_fat32(&p, 0, 0) == 1, "probe gpt esp");
    memset(&p, 0, sizeof(p)); p.valid = 1; p.type = 0x0C; CHECK(storage_part_probe_fat32(&p, 0, 0) == 1, "probe mbr fat32");
    memset(&p, 0, sizeof(p)); p.valid = 1; p.type = 0x83; CHECK(storage_part_probe_fat32(&p, 0, 0) == 0, "no probe linux pass0");
    CHECK(storage_part_probe_fat32(&p, 1, 1) == 1, "probe linux qemu fallback");
    CHECK(storage_part_probe_fat32(&p, 1, 0) == 0, "no probe linux real fallback");
}

int main(void) {
    test_esp_skipped();
    test_qemu_fallback();
    test_two_qualifying_first_wins();
    test_probe_policy();
    printf("storage policy tests: %d/%d passed\n", pass_count, pass_count + fail_count);
    return fail_count ? 1 : 0;
}
