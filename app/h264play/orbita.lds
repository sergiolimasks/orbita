ENTRY(_start)
SECTIONS
{
	. = 0x80000000;
	.text : {  *(.except*); *(.pagetable); pagetable_end = .; *(.init*); *(.fini*);
		/* orbita v0.21.29: nada do app nos primeiros 256 KB (área do hypervisor;
		 * em alguns consoles a página 0x3b000 chega estragada = tela vermelha
		 * 0x700 no 1o código que passa por ela). Só os vetores do libxenon. */
		. = . + 0x3e000;   /* múltiplo de 4 KB: o código fica com o mesmo alinhamento de antes (cache), só deslocado */
		*(.text*); }
	.elfldr : { elfldr_start = .;*(.elfldr); elfldr_end = .;}
	
	.data : { *(.data*) }
	.sdata : { *(.sdata*) }
	.rodata : { *(.rodata*)}

	.ctors : { __CTOR_LIST__ = .; *(.ctors); __CTOR_END__ = .; }
	.dtors : {__DTOR_LIST__ = .; *(.dtors); __DTOR_END__ = .; }

	.eh_frame_hdr : { *(.eh_frame_hdr) }
	.eh_frame ALIGN(0x4): { PROVIDE (__eh_frame_start = .); KEEP (*(.eh_frame)) LONG (0); }
	.gcc_except_table :  { *(.gcc_except_table .gcc_except_table.*) }

	bss_start = .;
	.bss : { *(.bss.beginning.lower); *(.bss.beginning.upper); *(.bss*) }
	.sbss : { *(.sbss .sbss.*) }
	bss_end = .;

	heap_begin = .;
	. = 0x9E000000;
	__libc_stack_end = . ;
	heap_end = .;
	. = 0xa0000000;
}
