section .rodata

msg: db "Hola mundo de la syscall!",0

section .text

global test_print_syscall
test_print_syscall:
	push ebp
	mov ebp,esp
	
	mov eax, 0
	mov ebx, 18
	mov ecx, 0	
	mov edx, msg
	int 0x80

	pop ebp
	ret

