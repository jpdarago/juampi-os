;Copia dos marcos de pagina
;Parametros
;	dst = direccion fisica del marco destino
;	src = direccion fisica del marco origen
global copy_frame
copy_frame:
	push ebp
	mov ebp,esp
	push esi
	push edi
	
	mov edi, [ebp+8]
	mov esi, [ebp+12]

	;Vamos a copiar 4096 bytes = 1024 doublewords
	mov ecx, 1024
	
	;Que no nos interrumpan porque esto deshabilita paginacion
	pushfd
	cli

	;Deshabilitamos paginacion para copiar los frames
	mov eax,cr0
	and eax,0x7FFFFFFF	
	mov cr0, eax
	
	rep movsd

	;Habilitamos paginacion de nuevo
	mov eax,cr0
	or eax,0x80000000
	mov cr0,eax	
	popfd
	
	pop edi
	pop esi
	pop ebp
	ret
