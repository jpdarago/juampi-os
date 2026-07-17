section .text

global syscall_fork
extern do_fork

;Fork system call. Since we need to do black magic
;with the tss so that it jumps to the correct place, the most
;convenient thing was to drop down to assembly, period.

%define EAX_POS 7*4
syscall_fork:
	push ebp
	mov ebp, esp
	push esi	

	;Don't interrupt me, this is SUPER critical
	pushfd
	cli
		
	mov esi, [ebp+8]

	;We pass it the eip of where we want it to jump to
	push task_ret
	;We pass it the flags we currently have
	pushfd	
	;We pass all the general registers to generate a copy
	pushad
	;We tell it where the kernel stack is so it copies it
	;The esp - 4 is because when pushing esp the stack gets shifted
	;so the final value is not really esp but esp - 4 due to
	;push
	lea eax, [esp-4]
	push eax

	call do_fork

;At this place the new task should appear
task_ret:
	;We restore the stack
	add esp, 4+4+8*4+4
	
	;Now we have to tweak eax with the value
	;we obtained (or with the one we arrived with) here. struct gen_regs
	;ends with eax so this copy should be correct
	mov [esi+EAX_POS], eax
	
	;Now we do restore
	popfd

	pop esi
	pop ebp
	ret
