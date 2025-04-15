%include "mode_switch.inc"

;Funcion de manejo de excepciones
extern exception_handlers

;Handlers de excepciones.
%assign ir_index 0
%rep 20
global _isr %+ ir_index
_isr %+ ir_index %+ :
	%if ir_index != 8 && (ir_index < 10 || ir_index > 14)
	;Si la excepcion no pushea un error code, pusheamos uno propio
	push dword 0xFFFFFFFF
	%endif	

	pushad
	;Asumimos que preserva convencion C y que
	;por lo tanto no toca edi
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

	;Movemos el procesador a modo kernel en
	;segmentos tambien. Asumimos que el 
	;handler que llamamos sigue la convencion C
	;y por lo tanto preserva ebx
	KSPACESWITCH

	;Ahora hay que conseguir el handler 
	mov eax, exception_handlers
	mov ecx, ir_index
	mov eax, [eax+4*ecx]

	;Apagamos interrupciones: esto es critico
	;Asumimos que el handler sigue convencion C
	;y por lo tanto no toca edi
	pushfd
	pop edi
	cli

	call eax

	;Restauramos interrupciones	
	push edi
	popfd

	add esp, 44
	
	;Restauramos los registros de segmento
	;de datos del ebx preservado
	USPACESWITCH

	popad
	add esp, 4 
	iret
	%assign ir_index ir_index+1
%endrep
