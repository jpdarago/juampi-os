section .text

global syscall_fork
extern do_fork

;System call de fork. Dado que necesitamos hacer magia negra
;con la tss para que salte al lugar correcto, lo mas 
;conveniente era droppear a assembly y punto.

%define EAX_POS 7*4
syscall_fork:
	push ebp
	mov ebp, esp
	push esi	

	;No me interrumpan que esto es RECONTRA critico	
	pushfd
	cli
		
	mov esi, [ebp+8]

	;Le pasamos el eip de a donde queremos que salte
	push task_ret
	;Le pasamos los flags que tenemos actualmente
	pushfd	
	;Pasamos todos los registros generales para generar una copia
	pushad
	;Le decimos donde esta la pila de kernel asi la copia
	;El esp - 4 es porque al pushear el esp la pila queda corrida
	;entonces en realidad no es esp el valor final sino esp - 4 por
	;push
	lea eax, [esp-4]
	push eax

	call do_fork

;En este lugar deberia aparecer la tarea nueva
task_ret:
	;Restauramos la pila 
	add esp, 4+4+8*4+4
	
	;Ahora tenemos que toquetear el eax con el valor 
	;que obtuvimos (o con el que llegamos) aca. struct gen_regs
	;termina con eax asi que este copy debiera ser correcto
	mov [esi+EAX_POS], eax
	
	;Ahora si restauramos
	popfd

	pop esi
	pop ebp
	ret
