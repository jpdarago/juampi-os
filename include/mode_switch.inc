extern switch_kernel_mode
extern switch_user_mode

%define GEN_REGS_SIZEOF 8*4

%macro SAVEINTTRACE 1 
	mov esi, esp
	add esi, %1
%endmacro

%macro KSPACESWITCH 0
	;We hand the processor over to the kernel's control
	mov bx, ds
	mov ax, 0x10 ;Kernel data segment
	mov ds,ax
	mov es,ax
	mov fs,ax
	mov gs,ax
	
	call switch_kernel_mode
%endmacro

%assign _jmp_i 0
%macro USPACESWITCH 0
	;We return the processor to how it was before
	
	cmp bx, 0x23
	jne .was_kernel %+ _jmp_i
	
	push esi
	call switch_user_mode
	add esp, 4

.was_kernel %+ _jmp_i %+:
	mov ds, bx
	mov es, bx
	mov fs, bx
	mov gs, bx

%assign _jmp_i _jmp_i+1
%endmacro
