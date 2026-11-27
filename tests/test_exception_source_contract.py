#!/usr/bin/env python3
"""Source-level contract checks for the exception stub and diagnostics.

The runtime tests exercise real CPU faults in QEMU; these checks guard the
assembly/C details that hosted tests cannot observe directly: a zero stack
return must never fall through to POPREGS/iretq, every hardware-error-code
vector must use the has_err=1 stub form, the vector-name fallback index must be
inside the table, and all fatal exceptions must reach the centralized,
recursion-safe, allocation-free panic path.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISR = os.path.join(ROOT, "kernel", "isr.S")
EXCEPTIONS = os.path.join(ROOT, "kernel", "exceptions.c")
KERNEL = os.path.join(ROOT, "kernel", "kernel.c")
KERNEL_H = os.path.join(ROOT, "kernel", "kernel.h")
GDT = os.path.join(ROOT, "kernel", "gdt.c")
PANIC = os.path.join(ROOT, "kernel", "panic.c")
POLICY = os.path.join(ROOT, "kernel", "exception_policy.c")


def read(path):
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        return handle.read()


def strip_comments(text):
    return re.sub(r"/\*.*?\*/", "", text, flags=re.S)


def main():
    errors = []

    isr = read(ISR)
    match = re.search(r"\.macro EXC_STUB.*?\.endm", isr, re.S)
    if not match:
        errors.append("kernel/isr.S: EXC_STUB macro not found")
    else:
        body = strip_comments(match.group(0))
        if "jz 1f" in body or "1:\tPOPREGS" in body or "1:  POPREGS" in body:
            errors.append(
                "kernel/isr.S: zero-return fallback resumes the faulting frame")
        safe_return = re.compile(
            r"testq\s+%rax,\s+%rax\s*\n"
            r"\s*jz\s+2f\s*\n"
            r"\s*movq\s+%rax,\s+%rsp\s*\n"
            r"\s*POPREGS\s*\n"
            r"\s*iretq\s*\n"
            r"\s*2:\s*cli\s*\n"
            r"\s*3:\s*hlt\s*\n"
            r"\s*jmp\s+3b")
        if not safe_return.search(body):
            errors.append(
                "kernel/isr.S: expected nonzero-switch/zero-halt sequence")

    # A has_err=0 stub pushes a dummy error code. If the CPU also pushes one,
    # the dummy shifts RIP/CS/RFLAGS out of the exception_frame_t ABI slots.
    err_vectors = (8, 10, 11, 12, 13, 14, 17, 21, 29, 30)
    listed = re.search(r"/\* error-code vectors: ([0-9, ]+) \*/", isr)
    if not listed:
        errors.append("kernel/isr.S: error-code vector comment not found")
    else:
        listed_vectors = [int(n) for n in re.findall(r"\d+", listed.group(1))]
        if listed_vectors != list(err_vectors):
            errors.append(
                "kernel/isr.S: error-code vector comment %s, want %s"
                % (listed_vectors, list(err_vectors)))
    for vec in err_vectors:
        if not re.search(r"^EXC_STUB %d,1\s*$" % vec, isr, re.M):
            errors.append(
                "kernel/isr.S: vector %d must be declared EXC_STUB %d,1"
                % (vec, vec))
    for vec in (21, 29):
        if re.search(r"^EXC_STUB %d,0\s*$" % vec, isr, re.M):
            errors.append(
                "kernel/isr.S: vector %d incorrectly pushes a dummy error code"
                % vec)

    # The IST wiring is deliberately source-checked: the host test exercises
    # the same functions, but the boot-order and stub-form contracts are only
    # visible in the real sources.
    kernel_h = read(KERNEL_H)
    if not re.search(r"#define DOUBLE_FAULT_IST_INDEX\s+1\b", kernel_h):
        errors.append("kernel/kernel.h: DOUBLE_FAULT_IST_INDEX must be 1")
    if not re.search(r"#define DOUBLE_FAULT_VECTOR\s+8\b", kernel_h):
        errors.append("kernel/kernel.h: DOUBLE_FAULT_VECTOR must be 8")

    gdt = read(GDT)
    if not re.search(r"aligned\(4096\)", gdt):
        errors.append("kernel/gdt.c: double-fault stack must be page aligned")
    if not re.search(r"tss\.ist\[DOUBLE_FAULT_IST_INDEX\s*-\s*1\]\s*="
                     r"\s*double_fault_stack_top\(\)", gdt):
        errors.append(
            "kernel/gdt.c: TSS IST1 must point to double_fault_stack_top()")

    exceptions = read(EXCEPTIONS)
    exceptions_code = strip_comments(exceptions)
    if "idt_set_exception_gate((uint8_t)i, exc_stub_table[i])" not in exceptions_code:
        errors.append(
            "kernel/exceptions.c: exception gates must use "
            "idt_set_exception_gate")
    kernel = read(KERNEL)
    if not re.search(r"gdt_init\(\);\s*\n\s*idt_init\(\);", kernel):
        errors.append(
            "kernel/kernel.c: TSS/GDT must be loaded before IDT setup")

    # Exception names now have exactly one owner; fatal and user diagnostics
    # must both consume it instead of keeping separate tables.
    panic = read(PANIC)
    panic_code = strip_comments(panic)
    if "static const char* panic_exception_names[33]" not in panic_code:
        errors.append(
            "kernel/panic.c: panic_exception_names needs a 33rd fallback entry")
    if "panic_exception_name(vec)" not in panic_code:
        errors.append(
            "kernel/panic.c: fatal diagnostics must use the shared name lookup")
    if "panic_exception_name(v)" not in exceptions_code:
        errors.append(
            "kernel/exceptions.c: user diagnostics must use the shared name "
            "lookup")
    if "exc_names" in exceptions_code:
        errors.append(
            "kernel/exceptions.c: duplicate exception-name table still present")

    # One fatal entry point: ring 0 and #DF must reach it; a null frame must
    # reach the same centralized fatal path; the user path must remain intact.
    if "if (exception_frame_is_fatal(f)) kernel_panic_exception(f);" not in exceptions_code:
        errors.append(
            "kernel/exceptions.c: fatal frames must call kernel_panic_exception")
    if 'if (!f) kernel_panic("CPU exception with null frame");' not in exceptions_code:
        errors.append(
            "kernel/exceptions.c: null frames must call kernel_panic")
    if "[User fault]" not in exceptions_code:
        errors.append(
            "kernel/exceptions.c: ring-3 isolation diagnostic was removed")
    if "=== KERNEL EXCEPTION ===" not in panic_code:
        errors.append(
            "kernel/panic.c: preserved === KERNEL EXCEPTION === marker missing")

    policy_code = strip_comments(read(POLICY))
    if not re.search(r"frame->vec\s*&\s*0xFFULL\)\s*==\s*DOUBLE_FAULT_VECTOR",
                     policy_code):
        errors.append(
            "kernel/exception_policy.c: #DF must always be kernel-fatal")

    # Panic-state contract: one guarded entry, immediate CLI, emergency halt
    # on recursive entry, and no return to normal exception handling.
    if not re.search(r"static\s+volatile\s+int\s+panic_state", panic_code):
        errors.append("kernel/panic.c: panic guard must be volatile static state")
    if not re.search(r"if\s*\(\s*!\s*panic_guard_enter\s*\(\)\s*\)\s*"
                     r"panic_emergency_halt\s*\(\s*\)", panic_code):
        errors.append(
            "kernel/panic.c: recursive entry must skip output and emergency halt")
    if panic_code.count("PANIC_CLI();") < 2:
        errors.append(
            "kernel/panic.c: every fatal entry must disable interrupts first")
    if not re.search(r'#define\s+PANIC_CLI\(\)\s+__asm__\s+volatile\(\s*"cli"'
                     r'\s*:::\s*"memory"\s*\)', panic_code):
        errors.append(
            "kernel/panic.c: PANIC_CLI must be a compiler memory barrier")
    if not re.search(r"for\s*\(\s*;\s*;\s*\)\s*__asm__\s*volatile\(\s*"
                     r"\"cli; hlt\"\s*\)", panic_code):
        errors.append("kernel/panic.c: expected deterministic cli/hlt loop")
    if not re.search(r"#define\s+PANIC_NORETURN\s+__attribute__\(\(noreturn\)\)",
                     kernel_h):
        errors.append("kernel/kernel.h: fatal entry points must be noreturn")
    for fn in ("kernel_panic", "kernel_panic_exception"):
        if not re.search(r"void\s+%s\s*\([^)]*\)[^{]*\{.*panic_emergency_halt\(\);"
                         % fn, panic_code, re.S):
            errors.append("kernel/panic.c: %s must never return" % fn)

    forbidden = (r"\bkmalloc\b", r"\bmalloc\b", r"\bscheduler_[a-z_]+\b",
                 r"\bspin_[a-z_]+\b", r"\block_[a-z_]+\b",
                 r"\bterminal_[a-z_]+\b", r"\bgfx_[a-z_]+\b",
                 r"\bmemcpy\b", r"\bstrcpy\b", r"\bstrcat\b",
                 r"\bsprintf\b", r"\bsnprintf\b", r"\bvsnprintf\b")
    for pattern in forbidden:
        if re.search(pattern, panic_code):
            errors.append(
                "kernel/panic.c: fatal path uses forbidden operation %s"
                % pattern)

    # Fault-context policy: bounded lines, explicit stack availability, CR2
    # only for #PF, and IST frames must not be mislabelled as handler RSP.
    if not re.search(r"#define\s+PANIC_LINE_MAX\s+192u", kernel_h):
        errors.append("kernel/kernel.h: PANIC_LINE_MAX must stay bounded")
    if not re.search(r"if\s*\(\s*panic_vector_needs_cr2\(vec\)\s*\)\s*\{"
                     r"[^}]*panic_read_cr2\(\)", panic_code):
        errors.append(
            "kernel/panic.c: CR2 must be read only inside the #PF branch")
    if "panic_format_stack_state(frame, handler_rsp, ist, line, sizeof(line))" not in panic_code:
        errors.append(
            "kernel/panic.c: fatal report must distinguish interrupted and "
            "handler stacks")

    # Vector 8 selects IST delivery, but the positive marker and the handler
    # stack label must both come from an actual DF-stack bounds check.
    if not re.search(r"frame_on_double_fault_stack\(\(uint64_t\)\(uintptr_t\)frame\)",
                     panic_code):
        errors.append(
            "kernel/panic.c: #DF verification must bounds-check the frame "
            "address")
    marker = re.search(
        r"if\s*\(v\s*==\s*DOUBLE_FAULT_VECTOR\)\s*\{\s*"
        r"if\s*\((\w+)\(frame\)\)\s*\{\s*"
        r'PANIC_PUTS\("DOUBLEFAULT frame on dedicated IST stack', panic_code)
    if not marker or marker.group(1) != "panic_df_frame_on_ist_stack":
        errors.append(
            "kernel/panic.c: positive #DF marker must use the bounds-checked "
            "frame verdict, not the vector alone")
    if not re.search(r"if\s*\(panic_df_frame_on_ist_stack\(frame\)\)\s*\{\s*"
                     r'pos\s*=\s*panic_append_cstr\(buf,\s*cap,\s*pos,\s*" \(IST\)"',
                     panic_code):
        errors.append(
            "kernel/panic.c: handler stack must not be labelled IST on the "
            "vector check alone")
    for reg in ("r15", "r14", "r13", "r12", "rbp", "rbx", "r11", "r10",
                "r9", "r8", "rdi", "rsi", "rdx", "rcx", "rax"):
        if '"%s"' % reg not in panic_code:
            errors.append("kernel/panic.c: missing register %s in fatal dump" % reg)

    if errors:
        for error in errors:
            print("FAIL: " + error)
        return 1
    print("exception source contract OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
