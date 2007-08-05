/*
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#ifndef _BONJOUR_MDNS_INTERFACE
#define _BONJOUR_MDNS_INTERFACE

#include "mdns_types.h"
#include "buddy.h"

gboolean _mdns_init_session(BonjourDnsSd *data);

gboolean _mdns_publish(BonjourDnsSd *data, PublishType type);

gboolean _mdns_browse(BonjourDnsSd *data);

guint _mdns_register_to_mainloop(BonjourDnsSd *data);

void _mdns_stop(BonjourDnsSd *data);

void _mdns_init_buddy(BonjourBuddy *buddy);

void _mdns_delete_buddy(BonjourBuddy *buddy);

/* This doesn't quite belong here, but there really isn't any shared functionality */
void bonjour_dns_sd_retrieve_buddy_icon(BonjourBuddy* buddy);

#endif