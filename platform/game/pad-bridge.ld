ENTRY(pm_pad_bridge)
SECTIONS {
 . = 0;
 .text : { KEEP(*(.text.entry)) *(.text*) *(.rodata*) }
 .data : { *(.data*) *(.bss*) *(COMMON) }
 /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
 ASSERT(SIZEOF(.text) > 0 && SIZEOF(.text) <= 8192, "Pad bridge exceeds code reservation")
 ASSERT(SIZEOF(.data) == 0, "Pad bridge must keep mutable state in its control block")
}
