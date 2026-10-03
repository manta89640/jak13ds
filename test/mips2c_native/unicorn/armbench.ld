/* (AI-assisted) armbench.cpp: everything at 0x100000 (armbench.py maps 8 MB there) */
ENTRY(_start)
SECTIONS
{
  . = 0x00100000;
  .text : { *(.text.start) *(.text .text.*) *(.glue_7) *(.glue_7t) }
  .rodata : { *(.rodata .rodata.*) }
  .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) }
  .ARM.exidx : { __exidx_start = .; *(.ARM.exidx* .gnu.linkonce.armexidx.*) __exidx_end = .; }
  .init_array : { __init_array_start = .; KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array)) __init_array_end = .; }
  .fini_array : { KEEP(*(.fini_array*)) }
  .data : { *(.data .data.*) }
  .bss : { __bss_start__ = .; *(.bss .bss.*) *(COMMON) __bss_end__ = .; }
  . = ALIGN(16);
  end = .;
  _end = .;
  __end__ = .;
}
