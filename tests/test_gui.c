#include <stdint.h>
#include <stdio.h>

#include "mouse.h"

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

int main(void) {
    printf("ARC OS gui test (kernel gui/mouse)\n");
    test_mouse_decode();
    test_mouse_clamp_accel();
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
