;Copies two page frames
;Parameters
;	dst = physical address of the destination frame
;	src = physical address of the source frame
global copy_frame
copy_frame:
	push ebp
	mov ebp,esp
	push esi
	push edi
	
	mov edi, [ebp+8]
	mov esi, [ebp+12]

	;We are going to copy 4096 bytes = 1024 doublewords
	mov ecx, 1024
	
	;Let no one interrupt us because this disables paging
	pushfd
	cli

	;We disable paging to copy the frames
	mov eax,cr0
	and eax,0x7FFFFFFF	
	mov cr0, eax
	
	rep movsd

	;We enable paging again
	mov eax,cr0
	or eax,0x80000000
	mov cr0,eax	
	popfd
	
	pop edi
	pop esi
	pop ebp
	ret
