ENTRY(pm_game_gate)
SECTIONS {
  . = 0;
  .text : { KEEP(*(.text.entry)) *(.text*) *(.rodata*) }
  .data : { *(.data*) *(.bss*) *(COMMON) }
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
  ASSERT(SIZEOF(.text) > 0 && SIZEOF(.text) <= 4096, "Game callback exceeds its code reservation")
  ASSERT(SIZEOF(.data) == 0, "Game callback must keep all mutable state in its control block")
}
