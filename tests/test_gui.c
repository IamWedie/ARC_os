#include <stdint.h>
#include <stdio.h>

#include "mouse.h"
#include "compositor.h"

static int tests_passed;
static int tests_failed;

#define CHECK(cond, ...)                                      \
    do {                                                      \
        if (cond) {                                           \
            tests_passed++;                                   \
        } else {                                              \
            tests_failed++;                                   \
            printf("  FAIL: ");                               \
            printf(__VA_ARGS__);                              \
            printf("\n");                                     \
        }                                                     \
    } while (0)

static void test_mouse_decode(void) {
    printf("\n=== Test 1: mouse decode ===\n");
    struct mouse_packet p;
    uint8_t raw[3] = {0x08, 10, 5}; /* no buttons, dx=10, dy=5 up */
    CHECK(mouse_decode_packet(raw, &p), "valid packet");
    CHECK(p.dx == 10, "dx=%d", p.dx);
    CHECK(p.dy == -5, "dy inverted=%d", p.dy); /* screen Y down */
    CHECK(p.buttons == 0, "no buttons");

    uint8_t left[3] = {0x09, 0, 0}; /* left button */
    CHECK(mouse_decode_packet(left, &p) && p.buttons == MOUSE_BTN_LEFT, "left button");

    uint8_t neg[3] = {0x18, 0xF6, 0x0A}; /* dx negative (-10), dy positive (10, screen down) */
    CHECK(mouse_decode_packet(neg, &p), "negative decode");
    CHECK(p.dx == -10, "neg dx=%d", p.dx);
    CHECK(p.dy == -10, "neg dy inverted=%d", p.dy);

    uint8_t bad[3] = {0x00, 1, 1}; /* missing always-1 bit */
    CHECK(!mouse_decode_packet(bad, &p), "reject missing always-1");

    uint8_t ovf[3] = {0xC8, 1, 1}; /* overflow */
    CHECK(!mouse_decode_packet(ovf, &p), "reject overflow");
}

static void test_mouse_clamp_accel(void) {
    printf("\n=== Test 2: mouse clamp/accel ===\n");
    CHECK(mouse_clamp(-5, 0, 1919) == 0, "clamp low");
    CHECK(mouse_clamp(5000, 0, 1919) == 1919, "clamp high");
    CHECK(mouse_clamp(100, 0, 1919) == 100, "clamp mid");
    CHECK(mouse_accel(1) == 1, "accel small");
    CHECK(mouse_accel(20) == 40, "accel medium");
    CHECK(mouse_accel(40) == 120, "accel large");
    CHECK(mouse_accel(-20) == -40, "accel negative");
}

static void test_comp_zorder_hit(void) {
    printf("\n=== Test 3: compositor z-order/hit-test ===\n");
    struct compositor c;
    comp_init(&c);
    uint32_t s1[10*10], s2[10*10], s3[10*10];
    struct comp_window* w1 = comp_create_window(&c, 0, 0, 10, 10, s1);
    struct comp_window* w2 = comp_create_window(&c, 5, 5, 10, 10, s2);
    struct comp_window* w3 = comp_create_window(&c, 20, 20, 10, 10, s3);
    CHECK(w1 && w2 && w3, "created 3 windows");
    CHECK(c.zcount == 3, "zcount 3");
    CHECK(comp_hit_test_id(&c, 7, 7) == w2->id, "topmost hit w2");
    CHECK(comp_hit_test_id(&c, 2, 2) == w1->id, "hit w1");
    CHECK(comp_hit_test_id(&c, 25, 25) == w3->id, "hit w3");
    CHECK(comp_hit_test_id(&c, 100, 100) == 0, "miss returns 0");
    comp_bring_to_front(&c, w1->id);
    CHECK(comp_hit_test_id(&c, 7, 7) == w1->id, "bring-to-front changes hit");
    CHECK(c.zorder[c.zcount - 1] == comp_window_index(&c, w1->id), "w1 at top");
    comp_destroy_window(&c, w2->id);
    CHECK(c.zcount == 2, "destroy reduced zcount");
    CHECK(comp_hit_test_id(&c, 7, 7) == w1->id, "hit after destroy");
    CHECK(comp_find(&c, w2->id) == 0, "destroyed not found");
}

static void test_comp_composite(void) {
    printf("\n=== Test 4: compositor composite ===\n");
    struct compositor c;
    comp_init(&c);
    uint32_t s1[4*4];
    uint32_t s2[4*4];
    for (int i = 0; i < 16; i++) { s1[i] = 0xFFFF0000u; s2[i] = 0x8000FF00u; } /* opaque red, 50% premul green */
    struct comp_window* w1 = comp_create_window(&c, 0, 0, 4, 4, s1);
    struct comp_window* w2 = comp_create_window(&c, 2, 2, 4, 4, s2);
    CHECK(w1 && w2, "windows created");
    uint32_t fbpx[8*8];
    for (int i = 0; i < 64; i++) fbpx[i] = 0xFF000000u;
    struct gfx_fb fb = {8, 8, 32, 32, fbpx};
    struct gfx_rect dmg = {0, 0, 8, 8};
    comp_composite(&c, &fb, &dmg);
    CHECK(fbpx[0] == 0xFFFF0000u, "w1 opaque pixel");
    CHECK(fbpx[3*8+3] != 0xFFFF0000u && fbpx[3*8+3] != 0xFF000000u, "overlap blended");
    /* topmost w2 at (2,2) should be blended over w1; check alpha result is opaque */
    uint32_t p = fbpx[2*8+2];
    CHECK(((p >> 24) & 0xFFu) == 255, "blended pixel opaque");
    uint32_t g = (p >> 8) & 0xFFu;
    CHECK(g > 0, "green channel present from w2");
}

int main(void) {
    printf("ARC OS gui test (kernel gui/mouse)\n");
    test_mouse_decode();
    test_mouse_clamp_accel();
    test_comp_zorder_hit();
    test_comp_composite();
    printf("\n=== Results ===\n");
    printf("  passed: %d\n", tests_passed);
    printf("  failed: %d\n", tests_failed);
    if (tests_failed) {
        printf("\n  SOME TESTS FAILED\n");
        return 1;
    }
    printf("\n  ALL TESTS PASSED\n");
    return 0;
}
