/* x86_64_boot.S : the library OS entry.
 *
 * The host sets RIP here, RSP to the top of this vcpu's stack, and RDI to the
 * vcpu id. This stub passes the id to kmain as its first argument. The id is
 * why an asm entry is needed: a C entry cannot reliably capture RDI before the
 * prologue clobbers it. kmain does not return; halt if it ever does. */

    .text
    .global _start
_start:
    and $-16, %rsp              /* keep the stack 16-aligned for the call */
    call kmain                  /* rdi (vcpu id) is arg0 */
1:
    hlt
    jmp 1b

    .section .note.GNU-stack, "", @progbits
