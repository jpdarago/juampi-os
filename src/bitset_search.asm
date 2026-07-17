section .text

global bitset_search

;Searches for a free bit in the bitmap.
;The bitmap structure is in bitmap.c
bitset_search:
	push ebp
	mov ebp,esp
	push ebx
	push edi

	mov eax, [ebp+8]
	;In ecx we put the size of the bitset
	mov ecx, [eax+4]
	;In edi we put the start address of the bitset
	mov edi, [eax]
	;We look for an empty bit in the bitmap
	;For that we look for the first double word
	;that does not have all its bits set
	mov eax, 0xFFFFFFFF	
	;In ebx we copy this initial address to offset from it
	mov ebx, edi
	;We scan until we run out of doublewords or a
	;double word is not zero
	repe scasd
	;If ecx is zero we stop because we went past the map
	cmp ecx, 0
	;If we went past the map, we return 0xFFFFFFF
	;which in two's complement is equal to -1
	je .end
	;They are not equal, there is an index that is not zero
	;in the doubleword found in esi.
	;We look for which index is not zero.
	;We are shifted one dword because we overshot
	sub edi,4
	mov ecx, [edi]
	mov eax, edi
	not ecx
	sub eax, ebx
	bsf edx,ecx
	;We get the offset from the base in eax and the index in
	;that double word in edx. We still need to combine them.
	;We multiply by eight because the offset is in bytes
	shl eax, 3
	;We add the offset to the bit index in the doubleword
	add eax, edx
.end:
	pop edi
	pop ebx
	pop ebp
	ret
