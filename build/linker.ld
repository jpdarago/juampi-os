ENTRY(loader)
SECTIONS
{
	. = 0x00100000;

	.__mbHeader : {
		*(.__mbHeader)
	}

	.text ALIGN (0x1000) :
	{
		*(.text)

		__TEXT_END__ = .;
	}

	.__signal_handlers ALIGN(0x1000):
	{
		signal_handlers_start = .;
		*(.__signal_handlers)
		signal_handlers_end = .;
	}

	.rodata ALIGN (0x1000) :
	{
		*(.rodata*)

		__RODATA_END__ = .;
	}

	
	.data ALIGN (0x1000) :
	{
		*(.data)

		__DATA_END__ = .;
	}

	.bss :
	{
		sbss = .;
		*(COMMON)
		*(.bss)
		ebss = .;
	}

	kernel_end = .; 
}
