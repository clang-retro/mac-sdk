; Startup code for 68k applications linked by codelink.py.
;
; The Segment Loader loads CODE 1 (the whole program: code, data and
; zeroed bss) into the heap and jumps here. The program was linked at
; address 0, so first add our load address to every absolute pointer
; listed in the relocation table, flush the instruction cache (where
; there is one), then call
; main and quit to the Finder.

	.text
	.globl	__start
__start:
	lea	(__start, %pc), %a0          ; load address of the image
	move.l	%a0, %d1
	move.l	#__reloc_table, %d0          ; offset of the table (absolute)
	move.l	%d0, %a1
	adda.l	%a0, %a1
	move.l	(%a1)+, %d0                  ; number of entries
	bra	2f
1:	move.l	(%a1)+, %d2                  ; offset of a 32-bit pointer
	add.l	%d1, (0, %a0, %d2.l)
2:	sub.l	#1, %d0
	bpl	1b
	; _FlushCodeCache only exists on systems that know 68020 caches.
	; On System 6 on a 68000 Mac it is unimplemented, so call it only
	; if its address differs from that of _Unimplemented. The 64K ROM
	; (ROM85 negative) has neither a cache nor _GetOSTrapAddress.
	move.w	0x28E, %d0                   ; ROM85
	bmi	3f
	move.l	#0xA89F, %d0                 ; _Unimplemented (Toolbox trap)
	.short	0xA746                       ; _GetToolTrapAddress
	move.l	%a0, %a1
	move.l	#0xA0BD, %d0
	.short	0xA346                       ; _GetOSTrapAddress
	move.l	%a0, %d2
	cmp.l	%a1, %d2
	beq	3f
	.short	0xA0BD                       ; _FlushCodeCache
3:	jsr	main
	.short	0xA9F4                       ; _ExitToShell
