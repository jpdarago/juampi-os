#ifndef __CMOS_H
#define __CMOS_H

#include <types.h>

typedef struct {
	uint second;
	uint minute;
	uint hour;
	uint day;
	uint month;
	uint year;	
} date;

void get_current_date(date * d);

#endif
