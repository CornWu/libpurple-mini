/* MySpaceIM Protocol Plugin, header file
 *
 * Copyright (C) 2007, Jeff Connelly <jeff2@soc.pidgin.im>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111-1301  USA
 */

#ifndef _MYSPACE_USER_H
#define _MYSPACE_USER_H

/* Hold ephemeral information about buddies, for proto_data of PurpleBuddy. */
/* GHashTable? */
typedef struct _MsimUser
{
	PurpleBuddy *buddy;
	guint client_cv;
	gchar *client_info;
	guint age;
	gchar *gender;
	gchar *location;
	guint total_friends;
	gchar *headline;
	gchar *display_name;
	/* Note: uid is in &buddy->node (set_blist_node_int), since it never changes */
	gchar *username;
	gchar *band_name, *song_name;
	gchar *image_url;
	guint last_image_updated;
} MsimUser;

/* Callback function pointer type for when a user's information is received, 
 * initiated from a user lookup. */
typedef void (*MSIM_USER_LOOKUP_CB)(MsimSession *session, MsimMessage *userinfo, gpointer data);

MsimUser *msim_get_user_from_buddy(PurpleBuddy *buddy);
MsimUser *msim_find_user(MsimSession *session, const gchar *username);
void msim_append_user_info(MsimSession *session, PurpleNotifyUserInfo *user_info, MsimUser *user, gboolean full);
gboolean msim_store_user_info(MsimSession *session, MsimMessage *msg, MsimUser *user);
gboolean msim_is_userid(const gchar *user);
gboolean msim_is_email(const gchar *user);
void msim_lookup_user(MsimSession *session, const gchar *user, MSIM_USER_LOOKUP_CB cb, gpointer data);

#endif /* !_MYSPACE_USER_H */
