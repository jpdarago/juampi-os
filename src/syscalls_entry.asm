%include "mode_switch.inc"

extern syscalls_entry_point

;Handler de interrupcion 0x80, el punto de 
;entrada de las llamadas a sistema.
global _isr0x80
_isr0x80:
	pushad
	SAVEINTTRACE GEN_REGS_SIZEOF	
	
	KSPACESWITCH

	;Volvemos a habilitar interrupcciones
	sti
		
	call syscalls_entry_point	
	USPACESWITCH	

	popad
	iretd
