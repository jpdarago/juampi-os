section .text

global my_memcpy
global _memsetb
global _memsetw
global _strlen

;extern void *memcpy(unsigned char *dest, const unsigned char *src, int count);
my_memcpy:
	push ebp
	mov ebp,esp
	push esi
	push edi
	mov edi, [ebp+8]
	mov esi, [ebp+12]
	mov ecx, [ebp+16]
	mov eax, ecx
	shr ecx, 2
	repnz movsd
	and eax, 0x3	
	mov ecx, eax
	repnz movsb
	pop edi
	pop esi
	pop ebp
	ret

;extern void memsetb(unsigned char *dest, unsigned char val, int count);
_memsetb:
	push ebp
	mov ebp,esp
	push edi
	mov edi, [ebp+8]
	mov al,  [ebp+12]
	mov ah, al
	mov dx, ax
	mov ecx, [ebp+16]
	bswap eax
	mov ax, dx
	bswap eax
	mov edx, ecx
	shr ecx, 2
	repnz stosd
	and edx, 0x3	
	mov ecx, edx
	repnz stosb
	pop edi
	pop ebp
	ret

;extern void memsetw(unsigned short *dest, unsigned short val, int count);
_memsetw:
	push ebp
	mov ebp,esp
	push edi
	mov edi, [ebp+8]
	mov ax,  [ebp+12]
	mov dx, ax
	mov ecx, [ebp+16]
	bswap eax
	mov ax, dx
	bswap eax
	mov edx, ecx
	shr ecx, 2
	repnz stosd
	and edx, 0x3	
	mov ecx, edx
	repnz stosw
	pop edi
	pop ebp
	ret

;extern void strlen(const char *str);
_strlen:
	push ebp
	mov ebp, esp
	push edi
	mov edi, [ebp+8]
	mov edx, edi
	;Cheat magico para que ecx nunca sea cero (Si, es Intel).	
	xor ecx, ecx
	not ecx
	xor eax, eax
	repne scasb
	sub edi,edx
	mov eax,edi
	pop edi	
	pop ebp
	ret
	
;extern void strcat(const char *dst, const char *src);
