section .text

global bitset_search

;Busca un bit libre en el bitmap.
;La estructura del bitmap esta en bitmap.c
bitset_search:
	push ebp
	mov ebp,esp
	push ebx
	push edi

	mov eax, [ebp+8]
	;En ecx ponemos el tamaño del bitset
	mov ecx, [eax+4]
	;En edi podemos la direccion de inicio del bitset
	mov edi, [eax]
	;Buscamos un bit vacio en el bitmap
	;Para eso buscamos la primer double word
	;que no tenga todos los bits prendidos
	mov eax, 0xFFFFFFFF	
	;En ebx copiamos esta direccion inicial para offsetear
	mov ebx, edi
	;Escaneamos hasta que no tengamos doublewords o una
	;double word no sea cero
	repe scasd
	;Si ecx es cero paramos porque nos fuimos del mapa
	cmp ecx, 0
	;Si nos fuimos del mapa, devolvemos 0xFFFFFFF
	;que en complemento a dos es igual a -1
	je .end
	;No son iguales, hay un indice que no es cero
	;en la doubleword encontrada en esi.
	;Buscamos que indice no es cero.
	;Estamos corridos un dword porque nos pasamos
	sub edi,4
	mov ecx, [edi]
	mov eax, edi
	not ecx
	sub eax, ebx
	bsf edx,ecx
	;Conseguimos el offset de la base en eax y el indice en
	;esa double word en edx. Falta unirlos.
	;Multiplicamos por ocho porque el offset es en bytes
	shl eax, 3
	;Agregamos el offset al indice del bit en la doubleword
	add eax, edx
.end:
	pop edi
	pop ebx
	pop ebp
	ret
