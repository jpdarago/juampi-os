#ifndef __HDD_H
#define __HDD_H

#include <types.h>
#include <utils.h>
#include <scrn.h>
#include <ports.h>

//Constantes utiles

#define ATA_SECTSIZE    512L

#define ATA_PRIMARY_BASE    0x1F0
#define ATA_PRIMARY_DEVCONTROL  0x3F6

#define ATA_PRIMARY_DATA    (0x0+ATA_PRIMARY_BASE)
#define ATA_PRIMARY_ERR     (0x1+ATA_PRIMARY_BASE)
#define ATA_PRIMARY_SECTORCOUNT (0x2+ATA_PRIMARY_BASE)
#define ATA_PRIMARY_LBALOW  (0x3+ATA_PRIMARY_BASE)
#define ATA_PRIMARY_LBAMID  (0x4+ATA_PRIMARY_BASE)
#define ATA_PRIMARY_LBAHIGH (0x5+ATA_PRIMARY_BASE)
#define ATA_PRIMARY_DRIVE   (0x6+ATA_PRIMARY_BASE)
#define ATA_PRIMARY_COMSTAT (0x7+ATA_PRIMARY_BASE)

#define ATA_STATUS_ERR  (1 << 0)
#define ATA_STATUS_DRQ  (1 << 3)
#define ATA_STATUS_SRV  (1 << 4)
#define ATA_STATUS_DF   (1 << 5)
#define ATA_STATUS_RDY  (1 << 6)
#define ATA_STATUS_BSY  (1 << 7)

#define ATA_CTRL_NIEN   (1 << 1)
#define ATA_CTRL_SRST   (1 << 2)
#define ATA_CTRL_HOB    (1 << 7)

#define ATA_DEV_PATAPI  0x1
#define ATA_DEV_SATAPI  0x2
#define ATA_DEV_PATA    0x3
#define ATA_DEV_SATA    0x4
#define ATA_DEV_UNKNOWN 0x5

#define ATA_COMM_READ_LBA28 0x20
#define ATA_COMM_WRITE_LBA28    0x30
#define ATA_COMM_FLUSHCACHE     0xE7

//Inicializar y detectar discos
void hdd_init(void);
//Leer (usando LBA 28) una cantidad de bytes de un disco a un buffer
void hdd_read(uint, uint32, void *);
//Escribir (usando LBA 28) una cantidad de bytes de un disco a un buffer
void hdd_write(uint,uint32, void *);

#endif
