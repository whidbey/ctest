/*----------------------------------------------------------------
This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License version 2 as
published by the Free Software Foundation.

Midas Zhou
-----------------------------------------------------------------*/

#ifndef __EGI_TOUCH_H__
#define __EGI_TOUCH_H__

#include "egi.h"
#include "egi_fbgeom.h"
#include <stdbool.h>

bool egi_touch_getdata(EGI_TOUCH_DATA *data);
EGI_TOUCH_DATA egi_touch_peekdata(void);
int egi_touch_peekdx(void);
enum egi_touch_status egi_touch_peekstatus(void);
void egi_touch_loopread(void); /* for thread func */


#endif
