ENTRY(_start)
SECTIONS {
    . = 0x10000;                              /* 对应 layout::CODE.start */
    .text   : { *(.text._start) *(.text*) }   /* _start 放最前面 */
    .rodata : { *(.rodata*) }                 /* 字符串常量 */
    .data   : { *(.data*) }
    .bss    : { *(.bss*) *(COMMON) }
}