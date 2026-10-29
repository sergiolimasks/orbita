ENTRY(_start)
SECTIONS
{
	. = 0x80000000;
	.text : {  *(.except*); *(.pagetable); pagetable_end = .; *(.init*); *(.fini*);
		/* orbita v0.21.29: nada do app nos primeiros 256 KB (área do hypervisor;
		 * em alguns consoles a página 0x3b000 chega estragada = tela vermelha
		 * 0x700 no 1o código que passa por ela). Só os vetores do libxenon. */
		/* orbita v0.21.45: o vão vai até 4 MB. Em alguns consoles (app aberto com o sistema
		 * já rodando?) chegam pacotes de rede gravados em 0x1d7000-0x1df000, que era código
		 * do decoder (tela vermelha 0x700 ao abrir um jogo). O xc_holewatch.c vigia o vão. */
		/* 0x3fe000 = o vão antigo (0x3e000) + 3,75 MB: múltiplo de 4 KB, o código mantém o
		 * alinhamento da 0.21.44 (cache; com o vão "redondo" o áudio engasgou) */
		orbita_hole_start = .;
		. = . + 0x3fe000;
		orbita_hole_end = .;
		*(.text*); orbita_text_end = .; }
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
