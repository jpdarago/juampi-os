;The idea is to boot with GRUB.
global loader                          
extern kmain                            ; kmain is the entry point to the real kernel, it is in a separate file
 
;GRUB multiboot header
MODULEALIGN equ  1<<0                   ; align loaded modules on page boundaries
MEMINFO     equ  1<<1                   ; provide memory map
FLAGS       equ  MODULEALIGN | MEMINFO  ; this is the Multiboot 'flag' field
MAGIC       equ    0x1BADB002           ; 'magic number' lets bootloader find the header
CHECKSUM    equ -(MAGIC + FLAGS)        ; checksum required

section .__mbHeader 
;AOUT KLUDGE 
align 4
	dd MAGIC
	dd FLAGS
	dd CHECKSUM
 
section .text

;The genuine loader code
loader:
	mov  esp, stack + STACKSIZE         ; We set the stack
	mov  ebp, stack + STACKSIZE	    ; We set the bottom of the stack

	push eax                            ; We push the magic number.
	push ebx                            ; We push the information. This is useful for example to obtain how much ram we have.

	cli
	call kmain                          ; We call the real kernel
	
	cli
.hang:
	hlt                                 ; Halt in case kmain does not work (it shouldn't).
	jmp  .hang
 

section .bss

;Stack
;We reserve an initial stack space, 64K
STACKSIZE equ 0x10000                    
align 4
stack:
	resb STACKSIZE                      ; We reserve the amount of stack we want
