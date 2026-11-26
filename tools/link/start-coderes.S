; Startup code for 68k code resources linked by codelink.py
; --code-resource (for example a screen saver module, an INIT or a
; WDEF).
;
; The owner of the code resource loads it into a handle and calls its
; first byte, here. The image was linked at address 0, and the handle can
; move between two calls. Thus each call compares the load address with
; the address of the last relocation. When the two are different, it adds
; the difference to every absolute pointer listed in the relocation table,
; and flushes the instruction cache. Then it jumps to main with the stack
; and the registers of the caller, so that main gets the arguments of the
; caller as they are (Pascal or C calling convention) and returns to the
; caller.

	.text
	.globl	__start
__start:
	movem.l	%d0-%d2/%a0-%a1, -(%sp)
	lea	(__start, %pc), %a0          ; load address of the image
	move.l	%a0, %d1
	lea	(__coderes_base, %pc), %a1
	sub.l	(%a1), %d1                   ; difference from the last base
	beq	4f
	move.l	%a0, (%a1)                   ; the new base
	move.l	#__reloc_table, %d0          ; offset of the table (absolute)
	move.l	%d0, %a1
	adda.l	%a0, %a1
	move.l	(%a1)+, %d0                  ; number of entries
	bra	2f
1:	move.l	(%a1)+, %d2                  ; offset of a 32-bit pointer
	add.l	%d1, (0, %a0, %d2.l)
2:	sub.l	#1, %d0
	bpl	1b
	; See start.S: call _FlushCodeCache only where it is implemented.
	move.w	0x28E, %d0                   ; ROM85
	bmi	4f
	move.l	#0xA89F, %d0                 ; _Unimplemented (Toolbox trap)
	.short	0xA746                       ; _GetToolTrapAddress
	move.l	%a0, %a1
	move.l	#0xA0BD, %d0
	.short	0xA346                       ; _GetOSTrapAddress
	move.l	%a0, %d2
	cmp.l	%a1, %d2
	beq	4f
	.short	0xA0BD                       ; _FlushCodeCache
4:	movem.l	(%sp)+, %d0-%d2/%a0-%a1
	jmp	main

	.data
	.p2align 2
; The address that the pointers of the image are relative to. The image
; was linked at 0.
__coderes_base:
	.long	0
