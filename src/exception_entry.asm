%include "mode_switch.inc"

;Exception handling function
extern exception_handlers

;Exception handlers.
%assign ir_index 0
%rep 20
global _isr %+ ir_index
_isr %+ ir_index %+ :
	%if ir_index != 8 && (ir_index < 10 || ir_index > 14)
	;If the exception does not push an error code, we push our own
	push dword 0xFFFFFFFF
	%endif	

	pushad
	;We assume it preserves the C convention and that
	;therefore it does not touch edi
	SAVEINTTRACE GEN_REGS_SIZEOF + 4

	push ss
	push gs
	push fs
	push es
	push ds
	push cs

	xor eax, eax
	mov eax, cr4
	push eax
	mov eax, cr3
	push eax
	mov eax, cr2
	push eax
	mov eax, cr0
	push eax
	
	push dword ir_index

	;We move the processor to kernel mode in
	;segments too. We assume that the
	;handler we call follows the C convention
	;and therefore preserves ebx
	KSPACESWITCH

	;Now we need to get the handler
	mov eax, exception_handlers
	mov ecx, ir_index
	mov eax, [eax+4*ecx]

	;We turn off interrupts: this is critical
	;We assume the handler follows the C convention
	;and therefore does not touch edi
	pushfd
	pop edi
	cli

	call eax

	;We restore interrupts
	push edi
	popfd

	add esp, 44
	
	;We restore the data segment registers
	;from the preserved ebx
	USPACESWITCH

	popad
	add esp, 4 
	iret
	%assign ir_index ir_index+1
%endrep
