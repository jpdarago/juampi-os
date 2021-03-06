section .text

global my_memcpy
global my_memset

my_memcpy:
	push ebp
	mov ebp, esp
	push esi
	push edi
	
	mov ecx, [ebp+16]
	mov esi, [ebp+12]
	mov edi, [ebp+8]

	mov edx, ecx
	sar ecx, 2
	
	repe movsd
	
	mov ecx, edx
	and ecx, 3	
	mov edx, 0

.loop:
	cmp edx, ecx
	je .fin
	mov al, [esi+edx]
	mov [edi+edx],eax
	inc edx
	jmp .loop

.fin:
	mov eax, edi
	
	pop edi
	pop esi
	pop ebp
	ret

my_memset:
	push ebp
	mov ebp, esp
	push esi
	push edi
	
	mov eax, 0
	mov ecx, [ebp+16]
	mov al,  [ebp+12]
	mov edi, [ebp +8] 

	;Copiamos el byte mas bajo
	;de al a todos los de eax
	mov edx, eax
	shl eax, 8
	or eax, edx
	shl eax, 8
	or eax, edx
	shl eax, 8
	or eax, edx

	mov edx, ecx
	sar ecx, 2

	repnz stosd

	mov ecx, edx
	and ecx, 3	
	mov edx, 0

.loop:
	cmp edx, ecx
	je .fin
	mov [edi+edx],al
	inc edx
	jmp .loop

.fin:
	mov eax, edi

	pop edi
	pop esi
	pop ebp
	ret
