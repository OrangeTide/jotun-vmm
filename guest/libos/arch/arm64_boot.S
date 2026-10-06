/* arm64_boot.S : the library OS entry.
 *
 * The host sets PC here, SP_EL1 to the top of this vcpu's stack, and X0 to the
 * vcpu id. X0 is already the first argument, so this stub just calls kmain.
 * kmain does not return; park if it ever does. */

    .text
    .global _start
_start:
    bl kmain                    /* x0 (vcpu id) is arg0 */
1:
    wfe
    b 1b

    .section .note.GNU-stack, "", %progbits
