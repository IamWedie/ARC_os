# ADR-004: Run without SSE/MMX until the kernel saves FPU state

## Status
Accepted

## Date
2026-11-18

## Context

The first successful long-mode boot stopped silently right after
`E64:segdone`, before the kernel banner. Disassembly showed that Clang
auto-vectorized `terminal_clear` into SSE2 instructions:

```
movd   %ecx,%xmm0
pshuflw $0x0,%xmm0,%xmm0
pshufd $0x44,%xmm0,%xmm0
movdqu %xmm0,(%rax,%rcx,2)
```

The boot path never sets `CR4.OSFXSR`/`OSXMMEXCPT`, so the first `movd`
raises `#UD`. No IDT exists yet at that point, which turns the fault into a
silent triple fault.

`-mgeneral-regs-only` did not prevent the vectorization on x86 and has been
removed from the kernel flags.

## Decision

Compile all kernel and ring-3 code with `-mno-mmx -mno-sse -mno-sse2
-mno-avx`, and enforce it in the build: after linking, `objdump -d` scans the
code sections and the build fails if `%xmm`, `%ymm`, `%zmm`, or `%mm` appears.

`.rodata` is a separate output section so string data cannot be mistaken for
instructions by that scan.

## Alternatives Considered

### Enable SSE in the boot path
- Pros: Keeps compiler vectorization available.
- Cons: Interrupts and context switches do not save XMM state today, so a
  preempted thread would lose live vector registers; requires `FXSAVE`/
  `FXRSTOR` in the switch path and `CR0`/`CR4` setup.
- Deferred: correct once the scheduler owns full FPU context switching.

### Rely on `-mgeneral-regs-only`
- Pros: One flag, no extra scan.
- Cons: Demonstrably ineffective for this Clang target; the kernel still
  contained SSE2 and crashed.
- Rejected: It does not provide the guarantee the kernel needs.

### Disable optimization (`-O0`)
- Pros: Avoids vectorization.
- Cons: Loses performance and does not prove future code is vector-free.
- Rejected: The build-time scan is a stronger invariant.

## Consequences

- Kernel code cannot use floating point or SIMD until FPU context switching
  is implemented.
- A compiler upgrade that reintroduces vector code fails the build with the
  offending instructions printed, instead of producing an unbootable image.
- Enabling SSE later requires: set `CR0.MP`, clear `CR0.EM`/`TS`, set
  `CR4.OSFXSR`/`OSXMMEXCPT`, and add `FXSAVE`/`FXRSTOR` to the context switch.
