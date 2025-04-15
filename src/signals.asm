section .__signal_handlers

%include "syscalls_codes.inc"

global enter_coma
enter_coma:
	mov eax, SYSCALL_COMA
	int 0x80
	ret

global die_on_signal
die_on_signal:
	mov eax, SYSCALL_EXIT
	int 0x80	
	ret

global ignore_signal
ignore_signal:
	mov eax, SYSCALL_CLEAR_SIGNAL
	mov ebx, [esp+4]
	int 0x80
	ret

global signal_trampoline
signal_trampoline:
	push ebp
	mov ebp, esp
	pushad
	mov eax, [ebp+4]
	push dword eax
	mov eax, [ebp+8]
	call eax
	add esp,4
	popad
	pop ebp
	add esp,8
	ret
