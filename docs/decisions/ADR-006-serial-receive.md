# ADR-006: Interrupt-driven COM1 receive, 14-byte FIFO trigger, TCP for scripted tests

## Status
Accepted

## Date
2026-11-20

## Context

The ring-3 shell reads input through `kbd_getchar()`. Initially that only
consulted the PS/2 keyboard ring. To make the OS usable headlessly, a COM1
polling fallback was added, but a pasted command longer than the 16-byte UART
FIFO lost characters (`touch runtime.txt` arrived as `runtime.t`). Polling also
made input latency depend on how often the shell called `read`.

The first fix moved COM1 receive to IRQ4, drained the FIFO into a 64-byte
software ring, and set the FIFO trigger to 1 byte. That still lost almost every
byte of a piped line: `help\n` produced only `h`. QEMU's interrupt trace showed
a single vector `0x24` delivery, and the serial trace showed the handler
reading `h` and clearing `LSR.DR`; the remaining bytes never entered the UART.

QEMU 11.1's Windows stdio backend explains the loss. When stdin is a pipe,
`chardev/char-win-stdio.c` reads one byte at a time and only forwards it if
`qemu_chr_be_can_write()` is non-zero; otherwise the byte is discarded. The
16550 model's `serial_can_receive()` advertises fewer bytes as the FIFO fills
and returns zero exactly when the FIFO count reaches the programmed trigger.
With a 1-byte trigger, capacity is zero after the first byte, so the next
keystroke is dropped before the guest can run its interrupt handler.

This is a limitation of the Windows stdio backend, not of the guest driver.
On real hardware at 115200 baud, bytes arrive roughly 87 µs apart and the IRQ
handler drains the FIFO long before it overflows.

## Decision

Use interrupt-driven COM1 receive:

- Enable the UART receive interrupt (`IER.RDI`), route IRQ4/GSI4 to vector 36,
  and install an IDT gate for that vector.
- Program the IOAPIC redirection entry for COM1 as **level-triggered** (and
  apply the ACPI ISO polarity if present). The 16550 keeps its interrupt line
  asserted while data remains in the FIFO; edge-triggered delivery can lose
  bytes that arrive while the handler is draining.
- Drain the UART FIFO into a 64-byte software ring in the IRQ handler, translate
  CR to LF, acknowledge the LAPIC (and the legacy PIC for compatibility), and
  let `kbd_getchar()` fall back to `serial_poll()` after the PS/2 ring is empty.
- Program the FIFO with `FCR = 0xC7`: FIFO enabled, both FIFOs cleared, and a
  14-byte receive trigger. A full burst costs one interrupt; short lines are
  delivered by the 16550 receive-timeout interrupt (four character times).
- Use QEMU's TCP serial backend for automated tests:
  `-serial tcp:127.0.0.1:<port>,server=on,wait=on`. TCP applies backpressure:
  QEMU stops reading the socket when the UART cannot accept data, so bytes are
  buffered rather than dropped. `wait=on` holds the VM until the test connects,
  so no boot output is missed.
- Keep `-serial stdio` / `-nographic` for manual interactive use. Human typing
  is slow enough that the drop path is not reached.

## Alternatives Considered

### Poll the UART from `kbd_getchar()`
- Pros: No IDT/IOAPIC work; simplest driver.
- Cons: The guest is too slow to drain a host-side burst; anything beyond the
  16-byte FIFO is lost. Input latency also depends on the shell's read loop.
- Rejected: It cannot survive pasted commands.

### Keep the 1-byte FIFO trigger
- Pros: Lowest possible interrupt latency; each byte raises an interrupt.
- Cons: On QEMU's Windows stdio backend, advertised capacity is zero after every
  byte, so all but the first byte of a piped line are discarded. It also
  generates one interrupt per byte.
- Rejected: It fails the scripted test and wastes interrupts.

### Pace the test input with sleeps
- Pros: Works with stdio without changing the driver.
- Cons: Masks the burst behavior, makes tests slow and timing-dependent, and
  would still fail if a command were pasted quickly.
- Rejected: Tests should exercise the driver, not avoid it.

### Use the legacy PIC instead of the IOAPIC
- Pros: Fewer moving parts during early boot.
- Cons: The kernel already switches to the IOAPIC and masks the PIC; mixing the
  two would make routing inconsistent.
- Rejected: The IOAPIC is the active interrupt controller.

### Add RTS/CTS hardware flow control
- Pros: The real fix for a slow guest on a real UART.
- Cons: QEMU's Windows stdio backend does not model it; no current hardware
  target needs it.
- Deferred: Revisit for real-hardware serial at higher baud rates.

## Consequences

- Scripted shell input is reliable even when a whole command arrives at once.
- A 14-byte burst costs one interrupt; short lines cost one timeout interrupt.
- The COM1 route is level-triggered, matching the 16550's asserted-until-drained
  behavior, so bytes arriving during the handler are not lost.
- The automated boot test no longer uses `-serial stdio`; it uses a loopback TCP
  chardev. Manual runs still use stdio and remain interactive.
- There is no hardware flow control. At 115200 with a 64-byte software ring this
  is fine for interactive use; extreme bursts on real hardware could still
  overrun the UART.
- The serial trace and QEMU source behavior are documented here so a future
  "why TCP?" question does not lead to re-enabling a lossy test path.
