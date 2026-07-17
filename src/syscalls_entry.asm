%include "mode_switch.inc"

extern syscalls_entry_point

;Handler for interrupt 0x80, the entry
;point of the system calls.
global _isr0x80
_isr0x80:
	pushad
	SAVEINTTRACE GEN_REGS_SIZEOF	
	
	KSPACESWITCH

	;We re-enable interrupts
	sti
		
	call syscalls_entry_point	
	USPACESWITCH	

	popad
	iretd
