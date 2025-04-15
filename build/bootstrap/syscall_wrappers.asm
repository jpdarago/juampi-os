section .text

%include "syscalls_codes.inc"

global 	fork,exit,scrn_print
global	wait4,sleep,kill,signal
global	open,read,write,close,exec

wait4:
	push ebp
	mov ebp, esp	
	pushad
	mov ebx,[ebp+8]
	mov eax,SYSCALL_WAIT
	int 0x80
	popad
	pop ebp
	ret

;void scrn_print(int row, int col, char * message)
scrn_print:
	push ebp
	mov ebp,esp	
	push ebx	
	mov ebx,[ebp+8]
	mov ecx,[ebp+12]	
	mov edx,[ebp+16]
	mov eax,SYSCALL_SCRN_PRINT
	int 0x80
	pop ebx	
	pop ebp
	ret

fork:
	mov eax,SYSCALL_FORK
	int 0x80
	ret 

exit:
	mov eax,SYSCALL_EXIT
	int 0x80
	ret

sleep:
	mov eax,SYSCALL_SLEEP
	int 0x80
	ret

;int do_kill(int pid, int signal)
kill:
	push ebp
	mov ebp, esp
	push ebx
	mov	ebx, [ebp+8]
	mov ecx, [ebp+12]
	mov eax, SYSCALL_KILL	
	int 0x80
	pop ebx
	pop ebp
	ret

;int open(char * pathname, uint flags)
open:
	push ebp
	mov ebp,esp
	push ebx
	mov ebx, [ebp+8]
	mov ecx, [ebp+12]
	mov eax, SYSCALL_OPEN
	int 0x80
	pop ebx
	pop ebp
	ret

read:
	push ebp
	mov ebp, esp
	push ebx
	mov ebx, [ebp+8]
	mov ecx, [ebp+12]
	mov edx, [ebp+16]
	mov eax, SYSCALL_READ
	int 0x80
	pop ebx
	pop ebp
	ret

write:
	push ebp
	mov ebp, esp
	push ebx	
	mov ebx, [ebp+8]
	mov ecx, [ebp+12]
	mov edx, [ebp+16]
	mov eax, SYSCALL_WRITE
	int 0x80
	pop ebx	
	pop ebp
	ret
	
close:
	push ebp
	mov ebp, esp
	push ebx
	mov ebx, [ebp+8]
	mov eax, SYSCALL_CLOSE
	int 0x80
	pop ebx
	pop ebp
	ret	

exec:
	push ebp
	mov ebp, esp
	push ebx
	;Aca llama a la syscall
	mov ebx, [ebp+8]
	mov ecx, [ebp+12]
	mov eax, SYSCALL_EXEC
	int 0x80
	pop ebx
	pop ebp
	ret	
