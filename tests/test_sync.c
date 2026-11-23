/* Host sync/flush contract tests (M4.10a). */
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

static int g_flush_count;
static int g_flush_fail;
static int g_sync_hook_count;
static int g_sync_hook_fail;

static int fake_flush(struct blockdev* dev) {
    (void)dev;
    g_flush_count++;
    return g_flush_fail ? -1 : 0;
}

static int fake_sync_hook(void* ctx) {
    (void)ctx;
    g_sync_hook_count++;
    return g_sync_hook_fail ? -1 : 0;
}

static struct blockdev g_fake_dev = {
    .name = "fake0",
    .sector_count = 128,
    .read = 0,
    .write = 0,
    .flush = fake_flush,
    .ctx = 0
};

static const struct vfs_ops g_fake_ops = {
    .open = 0, .read = 0, .write = 0, .size = 0, .truncate = 0,
    .close_slot = 0, .add_ref = 0, .release = 0,
    .stat = 0, .readdir = 0, .mkdir = 0, .unlink = 0, .rename = 0,
    .sync = fake_sync_hook
};

static void reset_state(void) {
    g_flush_count = 0;
    g_flush_fail = 0;
    g_sync_hook_count = 0;
    g_sync_hook_fail = 0;
}

static void test_sync_calls_flush_once(void) {
    reset_state();
    CHECK(vfs_mount("/", &g_fake_ops, 0, &g_fake_dev) == 0, "mount fake fs");
    int rc = vfs_sync("/");
    CHECK(rc == 0, "vfs_sync returns success");
    CHECK(g_sync_hook_count == 1, "sync hook called once");
    CHECK(g_flush_count == 1, "blockdev flush called exactly once");
}

static void test_sync_propagates_flush_error(void) {
    reset_state();
    g_flush_fail = 1;
    CHECK(vfs_mount("/", &g_fake_ops, 0, &g_fake_dev) == 0, "mount fake fs for flush error");
    int rc = vfs_sync("/");
    CHECK(rc == -1, "vfs_sync propagates flush error");
    CHECK(g_sync_hook_count == 1, "sync hook called before flush error");
    CHECK(g_flush_count == 1, "flush attempted exactly once on error");
}

static void test_sync_propagates_hook_error(void) {
    reset_state();
    g_sync_hook_fail = 1;
    CHECK(vfs_mount("/", &g_fake_ops, 0, &g_fake_dev) == 0, "mount fake fs for hook error");
    int rc = vfs_sync("/");
    CHECK(rc == -1, "vfs_sync propagates sync hook error");
    CHECK(g_sync_hook_count == 1, "sync hook attempted once");
    CHECK(g_flush_count == 0, "flush not called when sync hook fails");
}

static void test_sync_all_propagates_error(void) {
    reset_state();
    g_flush_fail = 1;
    CHECK(vfs_mount("/", &g_fake_ops, 0, &g_fake_dev) == 0, "mount fake fs for sync_all error");
    int rc = vfs_sync_all();
    CHECK(rc == -1, "vfs_sync_all propagates flush error");
    CHECK(g_flush_count == 1, "sync_all flush attempted once");
}

static void test_sync_missing_mount(void) {
    reset_state();
    int rc = vfs_sync("/nope");
    CHECK(rc == -1, "vfs_sync on missing mount fails");
    CHECK(g_flush_count == 0, "missing mount does not flush");
}

int main(void) {
    test_sync_calls_flush_once();
    test_sync_propagates_flush_error();
    test_sync_propagates_hook_error();
    test_sync_all_propagates_error();
    test_sync_missing_mount();
    printf("sync tests: %d/%d passed\n", pass_count, pass_count + fail_count);
    return fail_count ? 1 : 0;
}
