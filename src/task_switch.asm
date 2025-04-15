section .data

offset: 	dd 0
selector: 	dw 0

section .text

global tss_task_switch
tss_task_switch:
	push ebp
	mov ebp,esp
	mov ax, [ebp+8]
	mov [selector],ax
	jmp far [offset]
	pop ebp
	ret
