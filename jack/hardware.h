/*
    Copyright (C) 2001 Paul Davis
    
    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

    $Id$
*/

#ifndef __jack_hardware_h__
#define __jack_hardware_h__

#include <jack/types.h>

struct _jack_hardware;

typedef void (*JackHardwareReleaseFunction)(struct _jack_hardware *);
typedef int (*JackHardwareSetInputMonitorMaskFunction)(struct _jack_hardware *, unsigned long);
typedef int (*JackHardwareChangeSampleClockFunction)(struct _jack_hardware *, SampleClockMode);

typedef struct _jack_hardware {

    unsigned long capabilities;
    unsigned long input_monitor_mask;

    JackHardwareChangeSampleClockFunction change_sample_clock;
    JackHardwareSetInputMonitorMaskFunction set_input_monitor_mask;
    JackHardwareReleaseFunction release;

    void *private;

} jack_hardware_t;

jack_hardware_t * jack_hardware_new ();

#endif /* __jack_hardware_h__ */