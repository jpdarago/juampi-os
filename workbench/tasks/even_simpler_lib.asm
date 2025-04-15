global _start

extern sqr

section .bss

array2:	
	resb 1024

section .data

array:	dd 1
	dd 2
	dd 3
	dd 4

section .text

_start:
	lea ebx,[array]
	mov ecx,4
	mov eax,0
.calcula:
	mov edx,[ebx]
	push eax
	push edx
	call sqr
	add esp,4
	mov edx, eax
	pop eax
	add eax, 4
	add ebx,4
	loop .calcula
.fin:
	mov eax, 1
	mov ebx,0
	int 0x80
