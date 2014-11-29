#include <cmos.h>
#include <ports.h>
#include <utils.h>
#include <irq.h>
#include <scrn.h>

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

bool update_in_progress(void)
{
    outb(CMOS_ADDR, 0x0A);
    return (inb(CMOS_DATA) & 0x80);
}

uchar get_rtc_register(uchar reg)
{
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

void fetch_data(date * d)
{
    d->second   = get_rtc_register(0x00);
    d->minute   = get_rtc_register(0x02);
    d->hour     = get_rtc_register(0x04);
    d->day      = get_rtc_register(0x07);
    d->month    = get_rtc_register(0x08);
    //Precaucion: Algunas maquinas no tienen
    //registro de century y a veces no lo tienen
    //en el registro 0x32 del CMOS. Habria que
    //leerlo de la ACPI table.
    d->year     = 100*get_rtc_register(0x32)
                  + get_rtc_register(0x09);
}

void get_current_date(date * d)
{
    uint eflags = irq_cli();
    date tmp;

    while(update_in_progress()) ;
    fetch_data(d);

    do {
        memcpy(&tmp,d,sizeof(date));
        while(update_in_progress()) ;
        fetch_data(d);
    } while(memcmp(&tmp,d,sizeof(date)));

    uchar breg = get_rtc_register(0x0B);
    if(!(breg & 0x04)) {
        d->second   = (d->second & 0xF) + ((d->second / 16)*10);
        d->minute   = (d->minute & 0xF) + ((d->minute / 16)*10);
        d->hour     = ((d->hour & 0xF) + (((d->hour & 0x70) / 16)*10 ))
                      | (d->hour & 0x80);
        d->day      = (d->day & 0xF) + (d->day / 16)*10;
        d->month    = (d->month & 0xF) + (d->month / 16)*10;
        d->year     = (d->year & 0xF) + (d->year / 16)*10;
    }

    if(!(breg & 0x2) && (d->hour & 0x80)) {
        d->hour = ((d->hour & 0x7F) + 12) % 24;
    }

    irq_sti(eflags);
}
