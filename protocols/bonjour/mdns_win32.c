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

#include "mdns_win32.h"

#include "debug.h"

void 
_mdns_resolve_host_callback(GSList *hosts, gpointer data, const char *error_message)
{
	ResolveCallbackArgs* args = (ResolveCallbackArgs*)data;

	if (!hosts || !hosts->data)
	{
		purple_debug_error("bonjour", "host resolution - callback error.\n");	
	}
	else
	{
		struct sockaddr_in *addr = (struct sockaddr_in*)g_slist_nth_data(hosts, 1);
		BonjourBuddy* buddy = args->buddy;
		
		buddy->ip = inet_ntoa(addr->sin_addr);
		
		/* finally, set up the continuous txt record watcher, and add the buddy to purple */
	
		if (kDNSServiceErr_NoError == DNSServiceQueryRecord(&buddy->txt_query, 0, 0, args->fqn, 
				kDNSServiceType_TXT, kDNSServiceClass_IN, _mdns_text_record_query_callback, buddy))
		{
			gint fd = DNSServiceRefSockFD(buddy->txt_query);
			buddy->txt_query_fd = purple_input_add(fd, PURPLE_INPUT_READ, _mdns_handle_event, buddy->txt_query);
			
			bonjour_buddy_add_to_purple(buddy);
		}
		else
		{
			bonjour_buddy_delete(buddy);
		}
		
	}
	
	/* free the hosts list*/
	g_slist_free(hosts);
	
	/* free the remaining args memory */				
	purple_dnsquery_destroy(args->query);
	g_free(args->fqn);
	free(args);
}

void DNSSD_API 
_mdns_text_record_query_callback(DNSServiceRef DNSServiceRef, DNSServiceFlags flags, uint32_t interfaceIndex,
    DNSServiceErrorType errorCode, const char *fullname, uint16_t rrtype, uint16_t rrclass, uint16_t rdlen,
    const void *rdata, uint32_t ttl, void *context)
{
	if (kDNSServiceErr_NoError != errorCode)
	{
		purple_debug_error("bonjour", "text record query - callback error.\n");
	}
	else if (flags & kDNSServiceFlagsAdd)
	{
		BonjourBuddy *buddy = (BonjourBuddy*)context;	
		_mdns_parse_text_record(buddy, rdata, rdlen);
		bonjour_buddy_add_to_purple(buddy);
	}
}

void DNSSD_API 
_mdns_service_resolve_callback(DNSServiceRef sdRef, DNSServiceFlags flags, uint32_t interfaceIndex, DNSServiceErrorType errorCode, 
    const char *fullname, const char *hosttarget, uint16_t port, uint16_t txtLen, const char *txtRecord, void *context)
{
	ResolveCallbackArgs *args = (ResolveCallbackArgs*)context;

	/* remove the input fd and destroy the service ref */
	purple_input_remove(args->resolver_fd);
	DNSServiceRefDeallocate(args->resolver);

	if (kDNSServiceErr_NoError != errorCode)
	{
		purple_debug_error("bonjour", "service resolver - callback error.\n");
		bonjour_buddy_delete(args->buddy);
		free(args);
	}
	else
	{
		args->buddy->port_p2pj = port;
				
		/* parse the text record */
		_mdns_parse_text_record(args->buddy, txtRecord, txtLen);
		
		/* set more arguments, and start the host resolver */
		args->fqn = g_strdup(fullname);
		
		if (NULL == (args->query =
			purple_dnsquery_a(hosttarget, port, _mdns_resolve_host_callback, args)))
		{
			purple_debug_error("bonjour", "service resolver - host resolution failed.\n");
			bonjour_buddy_delete(args->buddy);
			g_free(args->fqn);
			free(args);
		}
	}
	
}

void DNSSD_API 
_mdns_service_register_callback(DNSServiceRef sdRef, DNSServiceFlags flags, DNSServiceErrorType errorCode,
    const char *name, const char *regtype, const char *domain, void *context)
{
	/* we don't actually care about anything said in this callback - this is only here because Bonjour for windows is broken */
	if (kDNSServiceErr_NoError != errorCode)
		{
		purple_debug_error("bonjour", "service advertisement - callback error.\n");
		}
	else
		{
		purple_debug_info("bonjour", "service advertisement - callback.\n");
		}
}

void DNSSD_API 
_mdns_service_browse_callback(DNSServiceRef sdRef, DNSServiceFlags flags, uint32_t interfaceIndex,
    DNSServiceErrorType errorCode, const char *serviceName, const char *regtype, const char *replyDomain, void *context)
{
	PurpleAccount *account = (PurpleAccount*)context;
	PurpleBuddy *gb = NULL;
	
	if (kDNSServiceErr_NoError != errorCode)
	{
		purple_debug_error("bonjour", "service browser - callback error");
	}
	else if (flags & kDNSServiceFlagsAdd)
	{
		/* A presence service instance has been discovered... check it isn't us! */
		if (0 != g_ascii_strcasecmp(serviceName, account->username))
		{
			/* OK, lets go ahead and resolve it to add to the buddy list */
			ResolveCallbackArgs *args = malloc(sizeof(ResolveCallbackArgs));
			args->buddy = bonjour_buddy_new(serviceName, account);
			
			if (kDNSServiceErr_NoError != DNSServiceResolve(&args->resolver, 0, 0, serviceName, regtype, replyDomain, _mdns_service_resolve_callback, args))
			{
				bonjour_buddy_delete(args->buddy);
				free(args);
				purple_debug_error("bonjour", "service browser - failed to resolve service.\n");
			}
			else
			{
				/* get a file descriptor for this service ref, and add it to the input list */
				gint resolver_fd = DNSServiceRefSockFD(args->resolver);
				args->resolver_fd = purple_input_add(resolver_fd, PURPLE_INPUT_READ, _mdns_handle_event, args->resolver);
			}
		}
	}
	else
	{
		/* A peer has sent a goodbye packet, remove them from the buddy list */
		purple_debug_info("bonjour", "service browser - remove notification\n");
		gb = purple_find_buddy(account, serviceName);
		if (gb != NULL)
		{
			bonjour_buddy_delete(gb->proto_data);
			purple_blist_remove_buddy(gb);
		}
	}
}

void 
_mdns_parse_text_record(BonjourBuddy* buddy, const char* record, uint16_t record_len)
{
	char *txt_entry;
	uint8_t txt_len;

	if (NULL != (txt_entry = (char*)TXTRecordGetValuePtr(record_len, record, "1st", &txt_len)))
	{
		set_bonjour_buddy_value(buddy, E_BUDDY_FIRST, txt_entry, txt_len);
	}
		
	if (NULL != (txt_entry = (char*)TXTRecordGetValuePtr(record_len, record, "last", &txt_len)))
	{
		set_bonjour_buddy_value(buddy, E_BUDDY_LAST, txt_entry, txt_len);
	}
			
	if (NULL != (txt_entry = (char*)TXTRecordGetValuePtr(record_len, record, "status", &txt_len)))
	{
		set_bonjour_buddy_value(buddy, E_BUDDY_STATUS, txt_entry, txt_len);
	}
				
	if (NULL != (txt_entry = (char*)TXTRecordGetValuePtr(record_len, record, "email", &txt_len)))
	{
		set_bonjour_buddy_value(buddy, E_BUDDY_EMAIL, txt_entry, txt_len);
	}
		
	if (NULL != (txt_entry = (char*)TXTRecordGetValuePtr(record_len, record, "jid", &txt_len)))
	{
		set_bonjour_buddy_value(buddy, E_BUDDY_JID, txt_entry, txt_len);
	}
		
	if (NULL != (txt_entry = (char*)TXTRecordGetValuePtr(record_len, record, "AIM", &txt_len)))
	{
		set_bonjour_buddy_value(buddy, E_BUDDY_AIM, txt_entry, txt_len);
	}
	
	if (NULL != (txt_entry = (char*)TXTRecordGetValuePtr(record_len, record, "VC", &txt_len)))
	{
		set_bonjour_buddy_value(buddy, E_BUDDY_VC, txt_entry, txt_len);
	}
	
	if (NULL != (txt_entry = (char*)TXTRecordGetValuePtr(record_len, record, "phsh", &txt_len)))
	{
		set_bonjour_buddy_value(buddy, E_BUDDY_PHSH, txt_entry, txt_len);
	}
	
	if (NULL != (txt_entry = (char*)TXTRecordGetValuePtr(record_len, record, "msg", &txt_len)))
	{
		set_bonjour_buddy_value(buddy, E_BUDDY_MSG, txt_entry, txt_len);
	}	
}

int
_mdns_publish(BonjourDnsSd *data, PublishType type)
{
	TXTRecordRef dns_data;
	char portstring[6];
	int ret = 0;
	const char *jid, *aim, *email;
	DNSServiceErrorType set_ret;

	TXTRecordCreate(&dns_data, 256, NULL);

	/* Convert the port to a string */
	snprintf(portstring, sizeof(portstring), "%d", data->port_p2pj);

	jid = purple_account_get_string(data->account, "jid", NULL);
	aim = purple_account_get_string(data->account, "AIM", NULL);
	email = purple_account_get_string(data->account, "email", NULL);

	/* We should try to follow XEP-0174, but some clients have "issues", so we humor them.
	 * See http://telepathy.freedesktop.org/wiki/SalutInteroperability
	 */

	/* Needed by iChat */
	set_ret = TXTRecordSetValue(&dns_data, "txtvers", 1, "1");
	/* Needed by Gaim/Pidgin <= 2.0.1 (remove at some point) */
	if (set_ret == kDNSServiceErr_NoError)
		set_ret = TXTRecordSetValue(&dns_data, "1st", strlen(data->first), data->first);
	/* Needed by Gaim/Pidgin <= 2.0.1 (remove at some point) */
	if (set_ret == kDNSServiceErr_NoError)
		set_ret = TXTRecordSetValue(&dns_data, "last", strlen(data->last), data->last);
	/* Needed by Adium */
	if (set_ret == kDNSServiceErr_NoError)
		set_ret = TXTRecordSetValue(&dns_data, "port.p2pj", strlen(portstring), portstring);
	/* Needed by iChat, Gaim/Pidgin <= 2.0.1 */
	if (set_ret == kDNSServiceErr_NoError)
		set_ret = TXTRecordSetValue(&dns_data, "status", strlen(data->status), data->status);
	if (set_ret == kDNSServiceErr_NoError)
		set_ret = TXTRecordSetValue(&dns_data, "ver", strlen(VERSION), VERSION);
	/* Currently always set to "!" since we don't support AV and wont ever be in a conference */
	if (set_ret == kDNSServiceErr_NoError)
		set_ret = TXTRecordSetValue(&dns_data, "vc", strlen(data->vc), data->vc);
	if (set_ret == kDNSServiceErr_NoError && email != NULL && *email != '\0')
		set_ret = TXTRecordSetValue(&dns_data, "email", strlen(email), email);
	if (set_ret == kDNSServiceErr_NoError && jid != NULL && *jid != '\0')
		set_ret = TXTRecordSetValue(&dns_data, "jid", strlen(jid), jid);
	/* Nonstandard, but used by iChat */
	if (set_ret == kDNSServiceErr_NoError && aim != NULL && *aim != '\0')
		set_ret = TXTRecordSetValue(&dns_data, "AIM", strlen(aim), aim);
	if (set_ret == kDNSServiceErr_NoError && data->msg != NULL && *data->msg != '\0')
		set_ret = TXTRecordSetValue(&dns_data, "msg", strlen(data->msg), data->msg);
	if (set_ret == kDNSServiceErr_NoError && data->phsh != NULL && *data->phsh != '\0')
		set_ret = TXTRecordSetValue(&dns_data, "phsh", strlen(data->phsh), data->phsh);

	/* TODO: ext, nick, node */

	if (set_ret != kDNSServiceErr_NoError)
	{
		purple_debug_error("bonjour", "Unable to allocate memory for text record.\n");
		ret = -1;
	}
	else
	{
		DNSServiceErrorType err = kDNSServiceErr_NoError;
	
		/* OK, we're done constructing the text record, (re)publish the service */
		
		switch (type)
		{
			case PUBLISH_START:
				err = DNSServiceRegister(&data->advertisement, 0, 0, purple_account_get_username(data->account), ICHAT_SERVICE,
					NULL, NULL, data->port_p2pj, TXTRecordGetLength(&dns_data), TXTRecordGetBytesPtr(&dns_data),
					_mdns_service_register_callback, NULL);
				break;
			
			case PUBLISH_UPDATE:
				err = DNSServiceUpdateRecord(data->advertisement, NULL, 0, TXTRecordGetLength(&dns_data), TXTRecordGetBytesPtr(&dns_data), 0);
				break;
		}
		
		if (kDNSServiceErr_NoError != err)
		{
			purple_debug_error("bonjour", "Failed to publish presence service.\n");
			ret = -1;
		}
		else if (PUBLISH_START == type)
		{
			/* hack: Bonjour on windows is broken. We don't care about the callback but we have to listen anyway */
			gint advertisement_fd = DNSServiceRefSockFD(data->advertisement);
			data->advertisement_fd = purple_input_add(advertisement_fd, PURPLE_INPUT_READ, _mdns_handle_event, data->advertisement);
		}
	}
	
	/* Free the memory used by temp data */
	TXTRecordDeallocate(&dns_data);
	return ret;
}

void 
_mdns_handle_event(gpointer data, gint source, PurpleInputCondition condition)
{
	DNSServiceProcessResult((DNSServiceRef)data);
}
