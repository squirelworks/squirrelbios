/* SquirrelBIOS make test: the head of a Linux-style s390x boot image.
 * Linked at 0x10000 and given 64 KB of zeros in front, so the file
 * belongs at address 0: "S390EP" at 0x10008, the parameter area at
 * 0x10400 (initrd address and size at +8 and +16, the command line size
 * at +0x30) and the command line at 0x10480. Original code. */
        .text
        .globl _image
_image:
        j       _start
        .org    _image + 8
        .ascii  "S390EP"
        .org    _image + 0x400
        .globl  image_parm
image_parm:
        .quad   0               /* IPL device */
        .quad   0               /* initrd address, set by the firmware */
        .quad   0               /* initrd size */
        .quad   0, 0            /* old memory */
        .quad   0               /* version string */
        .quad   0x200           /* command line size */
        .org    _image + 0x480
        .globl  image_cmdline
image_cmdline:
        .asciz  "squirrel default"
        .org    _image + 0x680
