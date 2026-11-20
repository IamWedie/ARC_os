# Per-Process Address Spaces Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give every user process its own CR3-loadable page-table root with shared kernel mappings, so a later `fork` cannot overwrite its parent.

**Architecture:** A pure, host-testable 4-level page-table core (`kernel/pagetable.c`) is driven by injected allocators. `kernel/vmm.c` keeps the master kernel table and all inline assembly (invlpg, CR3). `kernel/address_space.c` builds per-process roots from PMM frames and the pure core. The scheduler loads the right root on real switches; reap tears it down. The shell loader maps into its private root.

**Tech Stack:** Zig `cc` cross-compiler (`x86_64-freestanding`, `-mno-sse`, AT&T asm), MinGW-w64 host gcc for tests, native Windows `python` build/boot scripts, QEMU via `tools/boot_test.py`.

**Spec:** `docs/specs/process-address-space-design.md`

## Global Constraints

- Full gate command is exactly `cmd /c tests\run_all.bat`; host suite is `cmd /c tests\run_tests.bat`. Batch files are run through `cmd /c`, never PowerShell `&&`.
- Host gcc is `C:\msys64\ucrt64\bin\gcc.exe` with `-O2 -Wall -Wextra -std=gnu11 -I kernel`.
- Every commit must be buildable: `python tools\build_kernel.py --quiet` must pass `NO-SSE`, ELF verification, and multiboot verification.
- Do not change: syscall numbers, fd semantics, MLFQ policy/counters, `USER_CODE_VA`/`USER_STACK_VA`, the embedded shell image, or the QEMU gate script in `tests/run_all.bat`.
- Uniprocessor only. No PCID, no SMP, no COW, no page-fault changes.
- Commit each task only after its tests and the applicable gate are green.

## Review Focus

These are the conditions the spec implies that no happy-path test covers. Each is pinned to a task below.

1. A user mapping requested below PML4 slot 1 (i.e. in shared kernel slot 0) must be rejected, never written into a shared kernel table — pinned in Task 4 (`test_map_user_rejects_kernel_va`).
2. A 2 MiB `PSE` entry found during a walk must be refused without corrupting the entry — pinned in Task 1 (`test_large_page_refused`).
3. Allocator exhaustion mid-walk must leave a usable root and no orphaned tables — pinned in Task 1 (`test_alloc_failure`).
4. CR3 selection for PID 0, an out-of-range PID, or a process with no private root must load the master root — pinned in Task 5 (`test_cr3_selection`).
5. Double `as_destroy()`/reap must be an idempotent no-op and must not double-free — pinned in Task 4 (`test_destroy_idempotent`) and Task 5 (`test_kill_reap` assertion).

---

### Task 1: Pure page-table core — map, lookup, unmap, count

**Files:**
- Create: `kernel/pagetable.c`
- Modify: `kernel/kernel.h`
- Create: `tests/pt_stub.c`, `tests/test_pagetable.c`
- Modify: `tests/run_tests.bat`

**Interfaces:**
- Consumes: nothing new beyond `kernel.h` types.
- Produces: `PT_PG_PRESENT`, `PT_PG_RW`, `PT_PG_USER`, `PT_PG_PSE`, `PT_USER_SLOT_FIRST 1` (only PML4 slot 0 is shared kernel; both user VAs are slot 1), `pt_ops_t`, `pt_map()`, `pt_lookup()`, `pt_unmap()`, `pt_count_user_pages()`.

- [ ] **Step 1: Write the failing test**

`tests/pt_stub.c` provides a 4 MiB arena of zeroed 4096-byte pages plus `pt_stub_root()` (a zeroed root page), `pt_stub_reset()`, `pt_stub_alloc_count()`, `pt_stub_freed_count()`, `pt_stub_ops`, and `pt_stub_fail_after(int n)` (n = -1 disables failure). `free` increments `pt_stub_freed_count()`.

`tests/test_pagetable.c` uses the repo's `CHECK` macro and defines:

```c
static void test_map_and_lookup(void) {
    pt_stub_reset();
    uint64_t* root = pt_stub_root();             /* zeroed 4096-byte page */
    CHECK(pt_map(root, USER_CODE_VA, 0x1000000,
                 PT_PG_USER | PT_PG_RW, &pt_stub_ops) == 0, "map failed");
    uint64_t* leaf = pt_lookup(root, USER_CODE_VA);
    CHECK(leaf != NULL, "leaf missing");
    CHECK(*leaf == (0x1000000 | PT_PG_PRESENT | PT_PG_USER | PT_PG_RW),
          "leaf wrong: 0x%llx", (unsigned long long)*leaf);
    CHECK(pt_count_user_pages(root) == 1, "page count != 1");
}
static void test_intermediate_reuse(void) {
    pt_stub_reset();
    uint64_t* root = pt_stub_root();
    pt_map(root, USER_CODE_VA,      0x1000000, PT_PG_USER | PT_PG_RW, &pt_stub_ops);
    pt_map(root, USER_CODE_VA+4096, 0x1001000, PT_PG_USER | PT_PG_RW, &pt_stub_ops);
    CHECK(pt_stub_alloc_count() == 3, "expected 3 tables, got %d",
          pt_stub_alloc_count());               /* PDPT + PD + PT */
    CHECK(pt_count_user_pages(root) == 2, "page count != 2");
}
static void test_unmap(void) {
    /* after a map: pt_unmap(root, USER_CODE_VA) == 0; leaf == 0; count == 0 */
}
static void test_large_page_refused(void) {
    /* preset PDPT entry with PT_PG_PSE; pt_map returns -1; entry unchanged */
}
static void test_alloc_failure(void) {
    pt_stub_fail_after(2);                       /* third allocation fails */
    CHECK(pt_map(root, USER_CODE_VA, 0x1000000, PT_PG_USER, &pt_stub_ops) == -1,
          "alloc failure not reported");
    pt_stub_fail_after(-1);
    CHECK(pt_map(root, USER_CODE_VA, 0x1000000, PT_PG_USER, &pt_stub_ops) == 0,
          "root unusable after partial failure");
}
```

Add a `[7/8]` block to `tests/run_tests.bat` compiling `tests\test_pagetable.c tests\pt_stub.c kernel\pagetable.c` with the standard flags and running `tests\test_pagetable.exe`.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmd /c tests\run_tests.bat`
Expected: FAIL at `test_pagetable.c` compile with undefined `pt_map`/`pt_ops_t`.

- [ ] **Step 3: Implement the core**

In `kernel/pagetable.c` write `pt_map()` to walk PML4→PDPT→PD→PT using indices `(v >> 39|30|21|12) & 0x1FF`, allocate a zeroed page via `ops->alloc` whenever a level is absent, use `flags ? flags : PT_PG_PRESENT|PT_PG_RW` for every new entry, mask addresses with `0x000FFFFFFFFFF000ULL`, refuse an existing `PT_PG_PSE` entry by returning `-1`, and write the leaf last. `pt_lookup()` returns `NULL` on any absent level or large page. `pt_unmap()` zeroes the leaf and returns `0`, or `-1` if no leaf exists. `pt_count_user_pages()` counts present leaves in slots >= `PT_USER_SLOT_FIRST`. Add the constants, `pt_ops_t`, and prototypes to `kernel/kernel.h`.

- [ ] **Step 4: Run test to verify it passes**

Run: `cmd /c tests\run_tests.bat`
Expected: new suite PASS; existing four suites unchanged (slab 2269, scheduler 343, fd 194, fs-slot 38), 0 failures.

- [ ] **Step 5: Commit**

```bash
git add kernel/pagetable.c kernel/kernel.h tests/pt_stub.c tests/test_pagetable.c tests/run_tests.bat
git commit -m "feat: add pure host-testable page-table walk core"
```

---

### Task 2: Pure core — clone shared slots and tear down user tables

**Files:**
- Modify: `kernel/pagetable.c`, `tests/test_pagetable.c`
- Modify: `kernel/kernel.h`

**Interfaces:**
- Consumes: `pt_ops_t`, `PT_USER_SLOT_FIRST`, all Task 1 functions.
- Produces: `pt_clone_shared()`, `pt_free_user_tables()`.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_pagetable.c`:

```c
static void test_clone_shared(void) {
    /* master slot 0 = 0xABC000|PT_PG_PRESENT|PT_PG_RW; slot 1 = sentinel */
    pt_clone_shared(master, child);
    CHECK(child[0] == master[0], "kernel slot not shared");
    CHECK(child[PT_USER_SLOT_FIRST] == 0, "user slot copied");
    CHECK(child[2] == 0, "unused slot copied");
}
static void test_isolation(void) {
    /* two roots cloned from the same master; map different paddr at USER_CODE_VA */
    CHECK(*pt_lookup(a, USER_CODE_VA) != *pt_lookup(b, USER_CODE_VA),
          "processes share a leaf");
}
static void test_free_user_tables(void) {
    /* USER_CODE_VA and USER_STACK_VA are both slot 1 but different PDPT indices:
       1 PDPT + 2 PD + 2 PT = 5 private tables. Clone slot 0 from master. */
    pt_free_user_tables(root, &pt_stub_ops);
    CHECK(pt_stub_freed_count() == 5, "expected 5 private tables, got %d",
          pt_stub_freed_count());
    CHECK(root[0] == master[0], "shared slot cleared");
    CHECK(pt_count_user_pages(root) == 0, "user leaves remain");
}
static void test_free_idempotent(void) {
    pt_free_user_tables(root, &pt_stub_ops);
    CHECK(pt_stub_freed_count() == 0, "second free released pages");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmd /c tests\run_tests.bat`
Expected: FAIL with undefined `pt_clone_shared`/`pt_free_user_tables`.

- [ ] **Step 3: Implement clone and teardown**

`pt_clone_shared(src, dst)` copies `dst[0] = src[0]` and zeroes `dst[1..511]`. `pt_free_user_tables(root, ops)` recurses only from root slots >= 1: at PDPT/PD levels it recurses into present non-PSE children, then frees the table via `ops->free`; at the PT level it frees without recursing into leaves; it never frees the root and zeroes each user slot after freeing.

- [ ] **Step 4: Run test to verify it passes**

Run: `cmd /c tests\run_tests.bat`
Expected: all suites PASS, 0 failures.

- [ ] **Step 5: Commit**

```bash
git add kernel/pagetable.c kernel/kernel.h tests/test_pagetable.c tests/pt_stub.c
git commit -m "feat: clone kernel slots and reclaim private page tables"
```

---

### Task 3: VMM master glue and hardware switching

**Files:**
- Modify: `kernel/vmm.c`, `kernel/kernel.h`

**Interfaces:**
- Consumes: `pt_map()`, `pt_unmap()`, `pt_ops_t`.
- Produces: `uint64_t vmm_master_root(void)`, `void vmm_invalidate_page(uint64_t vaddr)`, `void vmm_switch_address_space(int pid)`. Also adds `uint64_t pml4_phys` to `process_t`.

- [ ] **Step 1: Record the green baseline**

Run: `cmd /c tests\run_all.bat`
Expected: `QUALITY GATE PASSED`; record check counts, kernel sizes, and `BOOT OK`.

- [ ] **Step 2: Refactor and add hardware glue**

In `kernel/vmm.c`: keep `vmm_pml4`, `vmm_pdpt`, `vmm_pd[]`, and `vmm_init()` unchanged; add a static `vmm_ops` whose `alloc` calls `pmm_alloc_pages(1)` then `memset(...,0,4096)` and whose `free` calls `pmm_free_pages(p,1)`. Replace the walk bodies of `vmm_map_page()` and `vmm_unmap_page()` with `pt_map`/`pt_unmap` on the master root followed by `vmm_invlpg(vaddr)`. Leave `vmm_create_user_mapping()` intact for now — it still delegates to `vmm_map_page()` and keeps the tree buildable until Task 6 migrates its caller. Add `vmm_master_root()` returning the physical address of `vmm_pml4`, `vmm_invalidate_page()` exposing `vmm_invlpg`, and `vmm_switch_address_space(int pid)` loading CR3 with `processes[pid].pml4_phys` when `0 < pid < MAX_PROCESSES && pml4_phys != 0`, otherwise the master root. Update `kernel.h`: add the three new prototypes alongside the existing `vmm_create_user_mapping` prototype and add `uint64_t pml4_phys;` to `process_t` after `stack_size`.

- [ ] **Step 3: Verify no behaviour change**

Run: `cmd /c tests\run_all.bat`
Expected: `QUALITY GATE PASSED` with the same check counts and shell output as baseline; `NO-SSE` still passes.

- [ ] **Step 4: Commit**

```bash
git add kernel/vmm.c kernel/kernel.h
git commit -m "refactor: route master VMM through page-table core and add CR3 switch"
```

---

### Task 4: Per-process address-space service

**Files:**
- Create: `kernel/address_space.c`, `tests/test_address_space.c`
- Modify: `tests/run_tests.bat`

**Interfaces:**
- Consumes: `pt_map`, `pt_clone_shared`, `pt_free_user_tables`, `pt_count_user_pages`, `vmm_master_root`, `vmm_invalidate_page`, `processes[]`, `pmm_alloc_pages`, `pmm_free_pages`.
- Produces: `int as_create(int pid)`, `int as_map_user(int pid, uint64_t vaddr, uint64_t paddr, size_t pages, uint8_t flags)`, `void as_destroy(int pid)`, `uint64_t as_root(int pid)`.

- [ ] **Step 1: Write the failing test**

`tests/test_address_space.c` defines `process_t processes[MAX_PROCESSES]`, a fake master root page whose slot 0 is a sentinel, and stubs for `vmm_master_root()` and `vmm_invalidate_page()`. Tests:

```c
static void test_create_isolated(void) {
    CHECK(as_create(1) == 0, "create 1 failed");
    CHECK(as_create(2) == 0, "create 2 failed");
    CHECK(as_root(1) != 0 && as_root(1) != as_root(2), "roots not private");
    CHECK(((uint64_t*)as_root(1))[0] == master[0], "kernel slot not shared");
    CHECK(((uint64_t*)as_root(1))[PT_USER_SLOT_FIRST] == 0, "user slot not empty");
    CHECK(as_root(0) == 0, "idle root should be master");
    CHECK(as_create(1) == -1, "double create accepted");
}
static void test_map_user_rejects_kernel_va(void) {
    CHECK(as_map_user(1, 0x1000, 0x1000000, 1, PT_PG_RW) == -1,
          "kernel VA accepted");
    CHECK(as_map_user(1, USER_CODE_VA, 0x1000000, 1, PT_PG_RW) == 0,
          "user VA rejected");
}
static void test_destroy_idempotent(void) {
    as_destroy(1);
    CHECK(as_root(1) == 0, "root not cleared");
    as_destroy(1);                               /* must not double-free */
}
static void test_repeated_cycles_net_zero(void) {
    uint64_t before = pmm_stub_used_pages();
    for (int i = 0; i < 8; i++) {
        CHECK(as_create(1) == 0, "cycle %d create failed", i);
        CHECK(as_map_user(1, USER_CODE_VA, 0x1000000, 2, PT_PG_RW) == 0,
              "cycle %d map failed", i);
        as_destroy(1);
    }
    CHECK(pmm_stub_used_pages() == before, "PMM leak: %llu",
          (unsigned long long)pmm_stub_used_pages());
}
```

Add a `[8/9]` block to `tests/run_tests.bat` for this suite.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmd /c tests\run_tests.bat`
Expected: FAIL with undefined `as_create`/`as_root`.

- [ ] **Step 3: Implement the service**

`as_create(pid)` rejects pid <= 0, out-of-range, and existing roots; allocates one zeroed PMM page; calls `pt_clone_shared((uint64_t*)vmm_master_root(), root)`; stores its physical address in `processes[pid].pml4_phys`. `as_root(pid)` returns that field. `as_map_user()` requires a private root, rejects `vaddr >> 39 < PT_USER_SLOT_FIRST`, ORs `PT_PG_USER` into flags, calls `vmm_invalidate_page()` for each mapped VA, and returns `-1` on the first `pt_map` failure. `as_destroy()` calls `pt_free_user_tables()` then frees the root page and zeroes `pml4_phys`.

- [ ] **Step 4: Run tests and full gate**

Run: `cmd /c tests\run_tests.bat` then `cmd /c tests\run_all.bat`
Expected: all host suites PASS; gate `QUALITY GATE PASSED`, `BOOT OK`.

- [ ] **Step 5: Commit**

```bash
git add kernel/address_space.c tests/test_address_space.c tests/run_tests.bat
git commit -m "feat: add per-process address-space service"
```

---

### Task 5: Wire the scheduler to CR3 switching and teardown

**Files:**
- Modify: `kernel/scheduler.c`, `tests/scheduler_stub.c`, `tests/test_scheduler.c`

**Interfaces:**
- Consumes: `vmm_switch_address_space(int)`, `as_destroy(int)`.
- Produces: no new public API.

- [ ] **Step 1: Write the failing test**

In `tests/scheduler_stub.c` add:

```c
int scheduler_stub_cr3_pid = -1;
int as_destroy_calls;
void vmm_switch_address_space(int pid) { scheduler_stub_cr3_pid = pid; }
void as_destroy(int pid) { (void)pid; as_destroy_calls++; }
```

In `tests/test_scheduler.c` add to `test_kill_reap()` after the successful reap:

```c
CHECK(as_destroy_calls == 1, "reap did not destroy address space");
CHECK(scheduler_reap(p1) == -1, "double reap accepted");
CHECK(as_destroy_calls == 1, "failed reap destroyed address space");
```

Add `test_cr3_selection()`:

```c
reset_world();
int p1 = process_create(fake_entry, "cr3");
scheduler_switch(0x1000);
CHECK(scheduler_stub_cr3_pid == p1, "switch did not load child root");
scheduler_kill(p1);
scheduler_switch(0x2000);
CHECK(scheduler_stub_cr3_pid == 0, "fallback did not load master root");
```

Reset both counters in `reset_world()`.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmd /c tests\run_tests.bat`
Expected: FAIL on `as_destroy_calls == 1` and `scheduler_stub_cr3_pid`.

- [ ] **Step 3: Implement scheduler calls**

In `schedule()`: call `vmm_switch_address_space(0)` immediately before setting `current_process = 0` in the idle-fallback branch, and `vmm_switch_address_space(next)` immediately before `current_process = next` in the real-switch branch. Do not call it on the `next == prev` or "stay on current" paths. In `scheduler_reap()`, call `as_destroy(pid)` as the first cleanup action, before freeing `stack_base` and `owned[]`.

- [ ] **Step 4: Run tests and full gate**

Run: `cmd /c tests\run_tests.bat` then `cmd /c tests\run_all.bat`
Expected: all suites PASS; gate green. Since no process has a private root yet, every switch still loads the master — behaviour is unchanged.

- [ ] **Step 5: Commit**

```bash
git add kernel/scheduler.c tests/scheduler_stub.c tests/test_scheduler.c
git commit -m "feat: load per-process CR3 on switch and destroy on reap"
```

---

### Task 6: Move the shell loader onto its private root

**Files:**
- Modify: `kernel/user.c`, `kernel/vmm.c`, `kernel/kernel.h`, `docs/specs/process-address-space-design.md`

**Interfaces:**
- Consumes: `as_create`, `as_map_user`, `PT_PG_RW`, `USER_CODE_VA`, `USER_STACK_VA`.

- [ ] **Step 1: Record the green baseline**

Run: `cmd /c tests\run_all.bat`
Expected: `QUALITY GATE PASSED`. Private roots exist but are unused; the shell still maps through the temporary master-root `vmm_create_user_mapping()` wrapper.

- [ ] **Step 2: Migrate the loader and remove the wrapper**

In `user_process_create_from_blob()`, keep the `preempt_disable()` window. After `process_create_user()` succeeds, call `as_create(pid)`; on failure use the existing kill+reap path. Replace both `vmm_create_user_mapping()` calls with `as_map_user(pid, USER_CODE_VA, (uint64_t)phys, pages, PT_PG_RW)` and `as_map_user(pid, USER_STACK_VA, (uint64_t)stk, USER_STACK_PAGES, PT_PG_RW)`. Keep the existing `ready` flag and error path unchanged. Then delete `vmm_create_user_mapping()` from `kernel/vmm.c` and its prototype from `kernel/kernel.h`.

- [ ] **Step 3: Run the full gate**

Run: `cmd /c tests\run_all.bat`
Expected: `QUALITY GATE PASSED`, `NO-SSE` OK, QEMU reaches `arc> `, `cat fdtest.txt` → `beta`, no `alphabeta`/`betaa`, `BOOT OK`.

- [ ] **Step 4: Record evidence and commit**

Replace the "Verification evidence" section of `docs/specs/process-address-space-design.md` with the actual RED/GREEN counts, build sizes, and QEMU result.

```bash
git add kernel/user.c kernel/vmm.c kernel/kernel.h docs/specs/process-address-space-design.md
git commit -m "feat: map shell into a per-process address space"
```

---

### Task 7: Whole-branch review and final gate

**Files:**
- Any files required by review fixes.

**Interfaces:**
- Consumes: the complete `f127822..HEAD` diff.

- [ ] **Step 1: Dispatch a fresh reviewer**

Use `subagent` with `agent: "general"` to review the full diff against the spec with `code-review-and-quality` rules. Ask specifically about CR3 timing, shared-slot correctness, double-free on the reap path, and host-test fidelity.

- [ ] **Step 2: Fix findings in one wave**

Apply all Critical/Important findings; park deferred items with a documented reason.

- [ ] **Step 3: Re-run the full gate**

Run: `cmd /c tests\run_all.bat`
Expected: `QUALITY GATE PASSED`, 0 failures.

- [ ] **Step 4: Commit and report**

```bash
git add -A
git commit -m "fix: address final address-space review findings"
```

Report the decisions/rulings and ask the human partner for the integration choice. Do not merge or push without that choice.
