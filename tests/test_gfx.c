#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gfx.h"

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

static uint32_t fb[256 * 256];

static void reset_fb(uint32_t w, uint32_t h, uint32_t color) {
    for (uint32_t i = 0; i < w * h; i++) fb[i] = color;
    gfx_test_set_fb(fb, w, h);
    gfx_dirty_reset();
}

static void test_fill_clip(void) {
    printf("\n=== Test 1: clip and fill ===\n");
    reset_fb(10, 10, 0xFF000000u);
    gfx_fill_rect(-2, -2, 5, 5, 0xFFFFFFFFu);
    CHECK(fb[0] == 0xFFFFFFFFu, "top-left clipped pixel");
    CHECK(fb[2 * 10 + 2] == 0xFFFFFFFFu, "inside clipped fill");
    CHECK(fb[3 * 10 + 3] == 0xFF000000u, "outside clipped fill");
    gfx_fill_rect(8, 8, 5, 5, 0xFFFF0000u);
    CHECK(fb[9 * 10 + 9] == 0xFFFF0000u, "bottom-right clipped");
    CHECK(fb[7 * 10 + 7] == 0xFF000000u, "untouched pixel");
    gfx_fill_rect(3, 3, 0, 5, 0xFF00FF00u);
    CHECK(fb[3 * 10 + 3] == 0xFF000000u, "zero-width no-op");
}

static void test_alpha_blend_math(void) {
    printf("\n=== Test 2: alpha blend math (premultiplied source-over) ===\n");
    /* Opaque source */
    CHECK(gfx_blend_premul(0xFFFF0000u, 0xFF00FF00u) == 0xFFFF0000u, "opaque src");
    /* Transparent source */
    CHECK(gfx_blend_premul(0x0000FF00u, 0xFF112233u) == 0xFF112233u, "transparent src");
    /* 50% premultiplied white over black -> gray with alpha */
    uint32_t half_white = 0x80808080u; /* a=128, premul rgb=128 */
    uint32_t over_black = gfx_blend_premul(half_white, 0xFF000000u);
    uint32_t r = (over_black >> 16) & 0xFFu;
    uint32_t a = (over_black >> 24) & 0xFFu;
    CHECK(a == 255, "50%% over opaque alpha=%u", (unsigned)a);
    CHECK(r >= 127 && r <= 129, "50%% over black r=%u", (unsigned)r);
    /* Partial over partial */
    uint32_t src = 0x40804020u; /* a=64, r=128,g=64,b=32 premul */
    uint32_t dst = 0x80FF8000u; /* a=128, premul rgb */
    uint32_t out = gfx_blend_premul(src, dst);
    CHECK(((out >> 24) & 0xFFu) >= 64, "partial alpha increased");
}

static void test_dirty_rect_merge(void) {
    printf("\n=== Test 3: dirty-rect merge ===\n");
    reset_fb(100, 100, 0);
    gfx_mark_dirty(10, 10, 10, 10);
    gfx_mark_dirty(15, 15, 10, 10); /* overlaps */
    int count = 0, full = 0;
    const struct gfx_rect* rects = gfx_dirty_rects(&count, &full);
    CHECK(!full, "not full");
    CHECK(count == 1, "merged to 1 rect, got %d", count);
    if (count >= 1) {
        CHECK(rects[0].x == 10 && rects[0].y == 10, "merged x,y");
        CHECK(rects[0].w == 15 && rects[0].h == 15, "merged w,h");
    }
    gfx_dirty_reset();
    gfx_mark_dirty(0, 0, 5, 5);
    gfx_mark_dirty(5, 0, 5, 5); /* adjacent */
    rects = gfx_dirty_rects(&count, &full);
    CHECK(count == 1, "adjacent merged to 1, got %d", count);
    if (count >= 1) CHECK(rects[0].w == 10, "adjacent width");
    gfx_dirty_reset();
    /* Non-overlapping */
    gfx_mark_dirty(0, 0, 2, 2);
    gfx_mark_dirty(50, 50, 2, 2);
    rects = gfx_dirty_rects(&count, &full);
    CHECK(count == 2, "separate rects=%d", count);
    gfx_dirty_reset();
    /* Overflow to full */
    for (int i = 0; i < 80; i++) gfx_mark_dirty((i % 16) * 4, (i / 16) * 4, 1, 1);
    rects = gfx_dirty_rects(&count, &full);
    CHECK(full == 1, "overflow sets full");
    (void)rects;
}

static void test_font_coverage_blend(void) {
    printf("\n=== Test 4: font coverage blend ===\n");
    reset_fb(64, 64, 0xFFFFFFFFu); /* white background */
    const struct gfx_font* font = gfx_font_get(18);
    CHECK(font != NULL, "font 18 exists");
    if (!font) return;
    CHECK(font->size == 18, "font size 18");
    int adv = gfx_draw_char(font, 2, 2, 'A', 0xFF000000u); /* black text */
    CHECK(adv > 0, "advance > 0");
    /* Find any pixel that became darker than white but not fully black - coverage blend */
    int found_partial = 0;
    int found_dark = 0;
    for (int i = 0; i < 64 * 64; i++) {
        uint32_t p = fb[i];
        uint32_t r = (p >> 16) & 0xFFu;
        if (r < 255) found_dark = 1;
        if (r > 0 && r < 255) found_partial = 1;
    }
    CHECK(found_dark, "glyph drew dark pixels");
    CHECK(found_partial, "glyph has anti-aliased coverage pixels");
    /* Text width */
    int w = gfx_text_width(font, "Hi");
    CHECK(w > 0, "text width > 0");
}

static void test_fill_rect_alpha(void) {
    printf("\n=== Test 5: premultiplied fill_rect_alpha ===\n");
    reset_fb(4, 4, 0xFF000000u);
    gfx_fill_rect_alpha(0, 0, 4, 4, 0x80800000u); /* premul: a=128, r=128 */
    uint32_t p = fb[0];
    uint32_t a = (p >> 24) & 0xFFu;
    uint32_t r = (p >> 16) & 0xFFu;
    CHECK(a == 255, "alpha over opaque dst a=%u", (unsigned)a);
    CHECK(r >= 127 && r <= 129, "premul red blended r=%u", (unsigned)r);
}

int main(void) {
    printf("ARC OS gfx test (real kernel/gfx.c)\n");
    test_fill_clip();
    test_alpha_blend_math();
    test_dirty_rect_merge();
    test_font_coverage_blend();
    test_fill_rect_alpha();
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
