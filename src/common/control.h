/**
 * Sailfish OS Factory Snapshot Update
 * Copyright (C) 2015 Jolla Ltd.
 * Contact: Thomas Perl <thomas.perl@jolla.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 **/

#ifndef SFMF_CONTROL_H
#define SFMF_CONTROL_H

void sfmf_control_init(); // registers on the bus, exits the process if that fails
int sfmf_control_process(); // returns 1 if the application should quit
void sfmf_control_set_progress(const char *target, int progress); // sends out a progress signal on the bus
void sfmf_control_close(); // deregisters on the bus

#endif /* SFMF_CONTROL_H */
