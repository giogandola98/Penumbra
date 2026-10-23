/*
 * Bare-metal ARMv5 self-test for ci/qemu/plugins/wakeups.c (QEMU -M versatilepb, arm926).
 *
 * Runs N_WFI idle cycles. Each cycle: WFI with IRQs masked (the core still wakes on a pending IRQ),
 * unmask so exactly one SP804 timer IRQ is taken, mask again, burn a fixed busy loop.
 * Masking around the WFI makes the count deterministic (one WFI, one IRQ per cycle) whatever the
 * host speed. The program exits through semihosting: exit code 0 iff its own IRQ counter == N_WFI.
 * The plugin must then report wfi == irq == N_WFI.
 */
        .equ N_WFI,      200
        .equ BUSY_ITERS, 1000            @ 2 instructions per iteration
        .equ TIMER0,     0x101E2000      @ SP804, 1 MHz, VIC line 4
        .equ VIC,        0x10140000      @ PL190
        .equ MODE_IRQ,   0xD2            @ IRQ mode, I+F masked
        .equ MODE_SVC,   0xD3            @ SVC mode, I+F masked
        .equ SYS_WRITE0, 0x04
        .equ SYS_EXIT,   0x18
        .equ ADP_Stopped_ApplicationExit, 0x20026
        .equ ADP_Stopped_InternalError,   0x20024

        .section .vectors, "ax"
        .arm
        .global _start
_start: b       reset                   @ 0x00
        b       .                       @ 0x04 undef
        b       .                       @ 0x08 swi
        b       .                       @ 0x0C prefetch abort
        b       .                       @ 0x10 data abort
        b       .                       @ 0x14
        b       irq_handler             @ 0x18 IRQ: the plugin counts executions of this address
        b       .                       @ 0x1C FIQ

        .text
reset:
        msr     cpsr_c, #MODE_IRQ
        ldr     sp, =irq_stack_top
        msr     cpsr_c, #MODE_SVC
        ldr     sp, =svc_stack_top

        ldr     r0, =VIC
        mov     r1, #0
        str     r1, [r0, #0x0C]         @ VICIntSelect: all IRQ, no FIQ
        mov     r1, #(1 << 4)
        str     r1, [r0, #0x10]         @ VICIntEnable: timer 0/1

        ldr     r0, =TIMER0
        ldr     r1, =1000               @ 1 ms period
        str     r1, [r0, #0x00]         @ Load
        mov     r1, #0xE2               @ enable | periodic | int enable | 32-bit
        str     r1, [r0, #0x08]         @ Control

        ldr     r4, =N_WFI
loop:
        mcr     p15, 0, r0, c7, c0, 4   @ WFI (ARM926/946 encoding used by DryOS)
        mrs     r0, cpsr
        bic     r0, r0, #0x80
        msr     cpsr_c, r0              @ unmask: the pending timer IRQ is taken here
        orr     r0, r0, #0x80
        msr     cpsr_c, r0              @ mask again
        ldr     r1, =BUSY_ITERS
1:      subs    r1, r1, #1
        bne     1b
        subs    r4, r4, #1
        bne     loop

        ldr     r0, =TIMER0
        mov     r1, #0
        str     r1, [r0, #0x08]         @ stop timer

        mov     r0, #SYS_WRITE0
        ldr     r1, =msg_done
        svc     0x123456

        ldr     r2, =irq_count
        ldr     r2, [r2]
        ldr     r3, =N_WFI
        cmp     r2, r3
        ldreq   r1, =ADP_Stopped_ApplicationExit
        ldrne   r1, =ADP_Stopped_InternalError
        mov     r0, #SYS_EXIT
        svc     0x123456
        b       .

irq_handler:
        push    {r0, r1}
        ldr     r0, =TIMER0
        str     r0, [r0, #0x0C]         @ IntClr (any value)
        ldr     r0, =irq_count
        ldr     r1, [r0]
        add     r1, r1, #1
        str     r1, [r0]
        pop     {r0, r1}
        subs    pc, lr, #4

        .section .rodata
msg_done:
        .asciz  "wfi_test: done\n"

        .bss
        .align 3
irq_count:
        .word   0
        .space  1024
irq_stack_top:
        .space  1024
svc_stack_top:
