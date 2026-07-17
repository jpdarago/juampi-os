%include "mode_switch.inc"

;General handler
extern irq_common_handler;

;Interrupt handlers
;Macro that generates an interrupt handler. What it does is push the interrupt code
;before jumping to the common handler.
%macro IRQ 2
global _irq %+ %1
_irq %+ %1 %+ :
	pushfd
	cli
	push dword %2
	jmp _irq_common
%endmacro

IRQ 0, 32
IRQ 1, 33
IRQ 2, 34
IRQ 3, 35
IRQ 4, 36
IRQ 5, 37
IRQ 6, 38
IRQ 7, 39
IRQ 8, 40
IRQ 9, 41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

;Stub common to all exceptions.
_irq_common:
	pushad
	
	SAVEINTTRACE GEN_REGS_SIZEOF+8 
	KSPACESWITCH
	call irq_common_handler
	
	USPACESWITCH 

	popad
	add esp,4
	popfd
	iretd
