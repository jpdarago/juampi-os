#include <hdd.h>
#include <exception.h>
#include <scrn.h>

static uchar ata_read_stable(void)
{
    uchar status;
    //Se lee el registro 5 veces para generar un
    //delay de 400 ns de acuerdo a la especificacion
    status = inb(ATA_PRIMARY_COMSTAT);
    status = inb(ATA_PRIMARY_COMSTAT);
    status = inb(ATA_PRIMARY_COMSTAT);
    status = inb(ATA_PRIMARY_COMSTAT);
    status = inb(ATA_PRIMARY_COMSTAT);
    return status;
}

static void ata_reset(void)
{
    uchar status;
    outb(ATA_PRIMARY_DEVCONTROL,ATA_CTRL_SRST);
    status = ata_read_stable();
    if(status & ATA_STATUS_BSY) {
        kernel_panic("ERROR: ATA RESET FALLO\n");
    }
    outb(ATA_PRIMARY_DEVCONTROL,0x0);
    status = ata_read_stable();
    if(status & ATA_STATUS_BSY) {
        kernel_panic("ERROR: ATA RESET FALLO\n");
    }
}

static void hdd_setup_lba(uint lba_address, uint sectors, uchar command)
{
    inb(ATA_PRIMARY_ERR);
    while((inb(ATA_PRIMARY_COMSTAT) & ATA_STATUS_BSY) == ATA_STATUS_BSY) ;
    while((inb(ATA_PRIMARY_COMSTAT) & ATA_STATUS_RDY) != ATA_STATUS_RDY) ;

    outb(ATA_PRIMARY_DEVCONTROL,0x0A);
    outb(ATA_PRIMARY_ERR,0x00);
    outb(ATA_PRIMARY_SECTORCOUNT,sectors);
    outb(ATA_PRIMARY_LBALOW,(uchar)lba_address);
    outb(ATA_PRIMARY_LBAMID,(uchar)(lba_address >> 8));
    outb(ATA_PRIMARY_LBAHIGH,(uchar)(lba_address >> 16));
    outb(ATA_PRIMARY_DRIVE,0xE0 | ((lba_address >> 24) & 0x0F));
    outb(ATA_PRIMARY_COMSTAT,command);
}

static void hdd_ata_wait(void)
{
    uchar status;
    while(((status = inb(ATA_PRIMARY_COMSTAT)) & ATA_STATUS_BSY) == ATA_STATUS_BSY) ;
    return;
}

void hdd_read(uint lba_address, uint sectors, void* _buffer)
{
    ushort* buffer = _buffer;
    hdd_setup_lba(lba_address,sectors,ATA_COMM_READ_LBA28);
    uint eflags = irq_cli();
    uint i,j,buffer_pos = 0;
    for(i = 0; i < sectors; i++) {
        hdd_ata_wait();
        for(j = 0; j < 256; j++) {
            buffer[buffer_pos] = inw(ATA_PRIMARY_DATA);
            buffer_pos++;
        }
        ata_read_stable();
    }
    irq_sti(eflags);
}

void hdd_write(uint lba_address, uint sectors, void* _buffer)
{
    ushort* buffer = _buffer;
    hdd_setup_lba(lba_address,sectors,ATA_COMM_WRITE_LBA28);
    uint i,j,buffer_pos = 0;
    uint eflags = irq_cli();
    for(i = 0; i < sectors; i++) {
        hdd_ata_wait();
        for(j = 0; j < 256; j++) {
            outw(ATA_PRIMARY_DATA,buffer[buffer_pos]);
            buffer_pos++;
        }

        hdd_ata_wait();
        outb(ATA_PRIMARY_COMSTAT,ATA_COMM_FLUSHCACHE);
    }
    hdd_ata_wait();
    irq_sti(eflags);
}

void hdd_init()
{
    //Resetear los discos
    ata_reset();
    outb(ATA_PRIMARY_LBALOW,0xAA);
    if(inb(ATA_PRIMARY_LBALOW) == 0xAA) {
        scrn_printf("HAY DISCO ATA MASTER!\n");
    } else {
        kernel_panic("No se ha detectado un disco.");
    }
}
