/**
 * @file account.c Account API
 * @ingroup core
 */

/* purple
 *
 * Purple is the legal property of its developers, whose names are too numerous
 * to list here.  Please refer to the COPYRIGHT file distributed with this
 * source distribution.
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
#include "internal.h"
#include "account.h"
#include "accountmanager.h"
#include "core.h"
#include "dbus-maybe.h"
#include "debug.h"
#include "marshallers.h"
#include "network.h"
#include "notify.h"
#include "pounce.h"
#include "prefs.h"
#include "privacy.h"
#include "prpl.h"
#include "request.h"
#include "server.h"
#include "signals.h"
#include "status.h"
#include "util.h"
#include "xmlnode.h"

struct _PurpleAccountPrivate
{
	PurpleConnectionErrorInfo *current_error;
	gboolean check_mail;
	gboolean enabled;
	PurplePlugin *prpl;
};

#define PURPLE_ACCOUNT_GET_PRIVATE(account) \
	((PurpleAccountPrivate *) (account->priv))

/* TODO: Should use PurpleValue instead of this?  What about "ui"? */
typedef struct
{
	PurplePrefType type;

	char *ui;

	union
	{
		int integer;
		char *string;
		gboolean boolean;

	} value;

} PurpleAccountSetting;

typedef struct
{
	PurpleAccountRequestType type;
	PurpleAccount *account;
	void *ui_handle;
	char *user;
	gpointer userdata;
	PurpleAccountRequestAuthorizationCb auth_cb;
	PurpleAccountRequestAuthorizationCb deny_cb;
} PurpleAccountRequestInfo;

static PurpleAccountUiOps *account_ui_ops = NULL;

static GList *handles = NULL;

static void set_current_error(PurpleAccount *account,
	PurpleConnectionErrorInfo *new_err);

static void
schedule_accounts_save(void)
{
#warning Remove this when it's no longer needed
}

static void
setting_to_xmlnode(gpointer key, gpointer value, gpointer user_data)
{
	const char *name;
	PurpleAccountSetting *setting;
	xmlnode *node, *child;
	char buf[20];

	name    = (const char *)key;
	setting = (PurpleAccountSetting *)value;
	node    = (xmlnode *)user_data;

	child = xmlnode_new_child(node, "setting");
	xmlnode_set_attrib(child, "name", name);

	if (setting->type == PURPLE_PREF_INT) {
		xmlnode_set_attrib(child, "type", "int");
		snprintf(buf, sizeof(buf), "%d", setting->value.integer);
		xmlnode_insert_data(child, buf, -1);
	}
	else if (setting->type == PURPLE_PREF_STRING && setting->value.string != NULL) {
		xmlnode_set_attrib(child, "type", "string");
		xmlnode_insert_data(child, setting->value.string, -1);
	}
	else if (setting->type == PURPLE_PREF_BOOLEAN) {
		xmlnode_set_attrib(child, "type", "bool");
		snprintf(buf, sizeof(buf), "%d", setting->value.boolean);
		xmlnode_insert_data(child, buf, -1);
	}
}

static void
ui_setting_to_xmlnode(gpointer key, gpointer value, gpointer user_data)
{
	const char *ui;
	GHashTable *table;
	xmlnode *node, *child;

	ui    = (const char *)key;
	table = (GHashTable *)value;
	node  = (xmlnode *)user_data;

	if (g_hash_table_size(table) > 0)
	{
		child = xmlnode_new_child(node, "settings");
		xmlnode_set_attrib(child, "ui", ui);
		g_hash_table_foreach(table, setting_to_xmlnode, child);
	}
}

static xmlnode *
status_attr_to_xmlnode(const PurpleStatus *status, const PurpleStatusType *type, const PurpleStatusAttr *attr)
{
	xmlnode *node;
	const char *id;
	char *value = NULL;
	PurpleStatusAttr *default_attr;
	PurpleValue *default_value;
	PurpleType attr_type;
	PurpleValue *attr_value;

	id = purple_status_attr_get_id(attr);
	g_return_val_if_fail(id, NULL);

	attr_value = purple_status_get_attr_value(status, id);
	g_return_val_if_fail(attr_value, NULL);
	attr_type = purple_value_get_type(attr_value);

	/*
	 * If attr_value is a different type than it should be
	 * then don't write it to the file.
	 */
	default_attr = purple_status_type_get_attr(type, id);
	default_value = purple_status_attr_get_value(default_attr);
	if (attr_type != purple_value_get_type(default_value))
		return NULL;

	/*
	 * If attr_value is the same as the default for this status
	 * then there is no need to write it to the file.
	 */
	if (attr_type == PURPLE_TYPE_STRING)
	{
		const char *string_value = purple_value_get_string(attr_value);
		const char *default_string_value = purple_value_get_string(default_value);
		if (((string_value == NULL) && (default_string_value == NULL)) ||
			((string_value != NULL) && (default_string_value != NULL) &&
			 !strcmp(string_value, default_string_value)))
			return NULL;
		value = g_strdup(purple_value_get_string(attr_value));
	}
	else if (attr_type == PURPLE_TYPE_INT)
	{
		int int_value = purple_value_get_int(attr_value);
		if (int_value == purple_value_get_int(default_value))
			return NULL;
		value = g_strdup_printf("%d", int_value);
	}
	else if (attr_type == PURPLE_TYPE_BOOLEAN)
	{
		gboolean boolean_value = purple_value_get_boolean(attr_value);
		if (boolean_value == purple_value_get_boolean(default_value))
			return NULL;
		value = g_strdup(boolean_value ?
								"true" : "false");
	}
	else
	{
		return NULL;
	}

	g_return_val_if_fail(value, NULL);

	node = xmlnode_new("attribute");

	xmlnode_set_attrib(node, "id", id);
	xmlnode_set_attrib(node, "value", value);

	g_free(value);

	return node;
}

static xmlnode *
status_attrs_to_xmlnode(const PurpleStatus *status)
{
	PurpleStatusType *type = purple_status_get_type(status);
	xmlnode *node, *child;
	GList *attrs, *attr;

	node = xmlnode_new("attributes");

	attrs = purple_status_type_get_attrs(type);
	for (attr = attrs; attr != NULL; attr = attr->next)
	{
		child = status_attr_to_xmlnode(status, type, (const PurpleStatusAttr *)attr->data);
		if (child)
			xmlnode_insert_child(node, child);
	}

	return node;
}

static xmlnode *
status_to_xmlnode(const PurpleStatus *status)
{
	xmlnode *node, *child;

	node = xmlnode_new("status");
	xmlnode_set_attrib(node, "type", purple_status_get_id(status));
	if (purple_status_get_name(status) != NULL)
		xmlnode_set_attrib(node, "name", purple_status_get_name(status));
	xmlnode_set_attrib(node, "active", purple_status_is_active(status) ? "true" : "false");

	child = status_attrs_to_xmlnode(status);
	xmlnode_insert_child(node, child);

	return node;
}

static xmlnode *
statuses_to_xmlnode(const PurplePresence *presence)
{
	xmlnode *node, *child;
	GList *statuses, *status;

	node = xmlnode_new("statuses");

	statuses = purple_presence_get_statuses(presence);
	for (status = statuses; status != NULL; status = status->next)
	{
		child = status_to_xmlnode((PurpleStatus *)status->data);
		xmlnode_insert_child(node, child);
	}

	return node;
}

static xmlnode *
proxy_settings_to_xmlnode(PurpleProxyInfo *proxy_info)
{
	xmlnode *node, *child;
	PurpleProxyType proxy_type;
	const char *value;
	int int_value;
	char buf[20];

	proxy_type = purple_proxy_info_get_type(proxy_info);

	node = xmlnode_new("proxy");

	child = xmlnode_new_child(node, "type");
	xmlnode_insert_data(child,
			(proxy_type == PURPLE_PROXY_USE_GLOBAL ? "global" :
			 proxy_type == PURPLE_PROXY_NONE       ? "none"   :
			 proxy_type == PURPLE_PROXY_HTTP       ? "http"   :
			 proxy_type == PURPLE_PROXY_SOCKS4     ? "socks4" :
			 proxy_type == PURPLE_PROXY_SOCKS5     ? "socks5" :
			 proxy_type == PURPLE_PROXY_USE_ENVVAR ? "envvar" : "unknown"), -1);

	if ((value = purple_proxy_info_get_host(proxy_info)) != NULL)
	{
		child = xmlnode_new_child(node, "host");
		xmlnode_insert_data(child, value, -1);
	}

	if ((int_value = purple_proxy_info_get_port(proxy_info)) != 0)
	{
		snprintf(buf, sizeof(buf), "%d", int_value);
		child = xmlnode_new_child(node, "port");
		xmlnode_insert_data(child, buf, -1);
	}

	if ((value = purple_proxy_info_get_username(proxy_info)) != NULL)
	{
		child = xmlnode_new_child(node, "username");
		xmlnode_insert_data(child, value, -1);
	}

	if ((value = purple_proxy_info_get_password(proxy_info)) != NULL)
	{
		child = xmlnode_new_child(node, "password");
		xmlnode_insert_data(child, value, -1);
	}

	return node;
}

static xmlnode *
current_error_to_xmlnode(PurpleConnectionErrorInfo *err)
{
	xmlnode *node, *child;
	char type_str[3];

	node = xmlnode_new("current_error");

	if(err == NULL)
		return node;

	/* It doesn't make sense to have transient errors persist across a
	 * restart.
	 */
	if(!purple_connection_error_is_fatal (err->type))
		return node;

	child = xmlnode_new_child(node, "type");
	snprintf(type_str, sizeof(type_str), "%u", err->type);
	xmlnode_insert_data(child, type_str, -1);

	child = xmlnode_new_child(node, "description");
	if(err->description) {
		char *utf8ized = purple_utf8_try_convert(err->description);
		if(utf8ized == NULL)
			utf8ized = purple_utf8_salvage(err->description);
		xmlnode_insert_data(child, utf8ized, -1);
		g_free(utf8ized);
	}

	return node;
}

static void
delete_setting(void *data)
{
	PurpleAccountSetting *setting = (PurpleAccountSetting *)data;

	g_free(setting->ui);

	if (setting->type == PURPLE_PREF_STRING)
		g_free(setting->value.string);

	g_free(setting);
}

/****************
 * GObject Code *
 ****************/
static GObjectClass *parent_class = NULL;

/* GObject Property enums */
enum
{
	PROP_0,
	PROP_USERNAME,
	PROP_PASSWORD,
	PROP_PRIVATE_ALIAS,
	PROP_PUBLIC_ALIAS,
	PROP_ENABLED,
	PROP_CONNECTION,
	PROP_PRPL,
	PROP_USER_INFO,
	PROP_BUDDY_ICON_PATH,
	PROP_REMEMBER_PASSWORD,
	PROP_CHECK_MAIL,
	PROP_LAST
};

/* GObject Property names */
#define PROP_USERNAME_S        "username"
#define PROP_PASSWORD_S        "password"
#define PROP_PRIVATE_ALIAS_S   "private-alias"
#define PROP_PUBLIC_ALIAS_S    "public-alias"
#define PROP_ENABLED_S         "enabled"
#define PROP_CONNECTION_S      "connection"
#define PROP_PRPL_S            "prpl"
#define PROP_USER_INFO_S       "userinfo"
#define PROP_BUDDY_ICON_PATH_S "buddy-icon-path"
#define PROP_REMEMBER_PASSWORD_S "remember-password"
#define PROP_CHECK_MAIL_S      "check-mail"

/* GObject Signal enums */
enum
{
	SIG_SETTING_INFO,
	SIG_SETTINGS_CHANGED,
	SIG_LAST
};
static guint signals[SIG_LAST] = { 0, };

/* Set method for GObject properties */
static void
purple_account_set_property(GObject *obj, guint param_id, const GValue *value,
		GParamSpec *pspec)
{
	PurpleAccount *account = PURPLE_ACCOUNT(obj);
	switch (param_id) {
		case PROP_USERNAME:
			purple_account_set_username(account, g_value_get_string(value));
			break;
		case PROP_PASSWORD:
			purple_account_set_password(account, g_value_get_string(value));
			break;
		case PROP_PRIVATE_ALIAS:
			purple_account_set_alias(account, g_value_get_string(value));
			break;
		case PROP_PUBLIC_ALIAS:
#warning TODO: _public_alias and _private_alias foo
			break;
		case PROP_ENABLED:
			purple_account_set_enabled(account, g_value_get_boolean(value));
			break;
		case PROP_CONNECTION:
			purple_account_set_connection(account,
				PURPLE_CONNECTION(g_value_get_object(value)));
			break;
		case PROP_PRPL:
#warning use _object when the prpls are GObjects
			account->priv->prpl = g_value_get_pointer(value);
			break;
		case PROP_USER_INFO:
			purple_account_set_user_info(account, g_value_get_string(value));
			break;
		case PROP_BUDDY_ICON_PATH:
			purple_account_set_buddy_icon_path(account,
					g_value_get_string(value));
			break;
		case PROP_REMEMBER_PASSWORD:
			purple_account_set_remember_password(account,
					g_value_get_boolean(value));
			break;
		case PROP_CHECK_MAIL:
			purple_account_set_check_mail(account, g_value_get_boolean(value));
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, param_id, pspec);
			break;
	}
}

/* Get method for GObject properties */
static void
purple_account_get_property(GObject *obj, guint param_id, GValue *value,
		GParamSpec *pspec)
{
	PurpleAccount *account = PURPLE_ACCOUNT(obj);
	switch (param_id) {
		case PROP_USERNAME:
			g_value_set_string(value, purple_account_get_username(account));
			break;
		case PROP_PASSWORD:
			g_value_set_string(value, purple_account_get_password(account));
			break;
		case PROP_PRIVATE_ALIAS:
			g_value_set_string(value, purple_account_get_alias(account));
			break;
		case PROP_PUBLIC_ALIAS:
#warning TODO: _public_alias and _private_alias foo
			break;
		case PROP_ENABLED:
			g_value_set_boolean(value,
					purple_account_get_enabled(account));
			break;
		case PROP_CONNECTION:
			g_value_set_object(value,
					purple_account_get_connection(account));
			break;
		case PROP_PRPL:
#warning Use _object when prpls are GObjects
			g_value_set_pointer(value, account->priv->prpl);
			break;
		case PROP_USER_INFO:
			g_value_set_string(value, purple_account_get_user_info(account));
			break;
		case PROP_BUDDY_ICON_PATH:
			g_value_set_string(value, purple_account_get_buddy_icon_path(account));
			break;
		case PROP_REMEMBER_PASSWORD:
			g_value_set_boolean(value, purple_account_get_remember_password(account));
			break;
		case PROP_CHECK_MAIL:
			g_value_set_boolean(value, purple_account_get_check_mail(account));
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, param_id, pspec);
			break;
	}
}

/* Gobject initialization function */
static void purple_account_init(GTypeInstance *instance, gpointer klass)
{
	PurpleAccount *account = PURPLE_ACCOUNT(instance);

	account->priv = g_new0(PurpleAccountPrivate, 1);

	account->settings = g_hash_table_new_full(g_str_hash, g_str_equal,
			g_free, delete_setting);
	account->ui_settings = g_hash_table_new_full(g_str_hash, g_str_equal,
			g_free, (GDestroyNotify)g_hash_table_destroy);
	account->system_log = NULL;

	account->perm_deny = PURPLE_PRIVACY_ALLOW_ALL;
}

/* GObject destructor function */
static void
purple_account_finalize(GObject *object)
{
	PurpleAccountPrivate *priv = NULL;
	GList *l;
	PurpleAccount *account = PURPLE_ACCOUNT(object);

	purple_debug_info("account", "Destroying account %p\n", account);

	/* Make sure we disconnect first */
	purple_account_set_connection(account, NULL);

	/* Clearing the error ensures that account-error-changed is emitted,
	 * which is the end of the guarantee that the the error's pointer is
	 * valid.
	 */
	purple_account_clear_current_error(account);

	for (l = purple_get_conversations(); l != NULL; l = l->next) {
		PurpleConversation *conv = (PurpleConversation *)l->data;

		if (purple_conversation_get_account(conv) == account)
			purple_conversation_set_account(conv, NULL);
	}

	g_free(account->username);
	g_free(account->alias);
	g_free(account->password);
	g_free(account->user_info);
	g_free(account->buddy_icon_path);
	g_free(account->protocol_id);

	g_hash_table_destroy(account->settings);
	g_hash_table_destroy(account->ui_settings);

	purple_account_set_status_types(account, NULL);

	purple_presence_destroy(account->presence);

	if(account->system_log)
		purple_log_free(account->system_log);

	priv = PURPLE_ACCOUNT_GET_PRIVATE(account);
	PURPLE_DBUS_UNREGISTER_POINTER(priv->current_error);
	g_free(priv->current_error);
	g_free(priv);

	PURPLE_DBUS_UNREGISTER_POINTER(account);
	parent_class->finalize(object);
}

/* Class initializer function */
static void purple_account_class_init(PurpleAccountClass *klass)
{
	GObjectClass *obj_class = G_OBJECT_CLASS(klass);

	parent_class = g_type_class_peek_parent(klass);

	obj_class->finalize = purple_account_finalize;

	/* Setup properties */
	obj_class->get_property = purple_account_get_property;
	obj_class->set_property = purple_account_set_property;

	g_object_class_install_property(obj_class, PROP_USERNAME,
			g_param_spec_string(PROP_USERNAME_S, _("Username"),
				_("The username for the account."), NULL,
				G_PARAM_READWRITE | G_PARAM_CONSTRUCT)
			);

	g_object_class_install_property(obj_class, PROP_PASSWORD,
			g_param_spec_string(PROP_PASSWORD_S, _("Password"),
				_("The password for the account."), NULL,
				G_PARAM_READWRITE)
			);

	g_object_class_install_property(obj_class, PROP_PRIVATE_ALIAS,
			g_param_spec_string(PROP_PRIVATE_ALIAS_S, _("Private Alias"),
				_("The private alias for the account."), NULL,
				G_PARAM_READWRITE)
			);

	g_object_class_install_property(obj_class, PROP_PUBLIC_ALIAS,
			g_param_spec_string(PROP_PUBLIC_ALIAS_S, _("Public Alias"),
				_("The public alias for the account."), NULL,
				G_PARAM_READWRITE)
			);

	g_object_class_install_property(obj_class, PROP_USER_INFO,
			g_param_spec_string(PROP_USER_INFO_S, _("User information"),
				_("Detailed user information for the account."), NULL,
				G_PARAM_READWRITE)
			);

	g_object_class_install_property(obj_class, PROP_BUDDY_ICON_PATH,
			g_param_spec_string(PROP_BUDDY_ICON_PATH_S, _("Buddy icon path"),
				_("Path to the buddyicon for the account."), NULL,
				G_PARAM_READWRITE)
			);

	g_object_class_install_property(obj_class, PROP_ENABLED,
			g_param_spec_boolean(PROP_ENABLED_S, _("Enabled"),
				_("Whether the account is enabled or not."), FALSE,
				G_PARAM_READWRITE | G_PARAM_CONSTRUCT)
			);

	g_object_class_install_property(obj_class, PROP_REMEMBER_PASSWORD,
			g_param_spec_boolean(PROP_REMEMBER_PASSWORD_S, _("Remember password"),
				_("Whether to remember and store the password for this account."), FALSE,
				G_PARAM_READWRITE | G_PARAM_CONSTRUCT)
			);

	g_object_class_install_property(obj_class, PROP_CHECK_MAIL,
			g_param_spec_boolean(PROP_CHECK_MAIL_S, _("Check mail"),
				_("Whether to check mails for this account."), FALSE,
				G_PARAM_READWRITE | G_PARAM_CONSTRUCT)
			);

	g_object_class_install_property(obj_class, PROP_CONNECTION,
			g_param_spec_object(PROP_CONNECTION_S, _("Connection"),
				_("The PurpleConnection object for the account."),
				PURPLE_TYPE_CONNECTION,
				G_PARAM_READWRITE)
			);
#warning Use _object when protocol plugins are objects
	g_object_class_install_property(obj_class, PROP_PRPL,
			g_param_spec_pointer(PROP_PRPL_S, _("Protocol Plugin"),
				_("The protocol plugin that is responsible for the account."),
				G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY)
			);


	/* Setup signals */
	signals[SIG_SETTINGS_CHANGED] =
		g_signal_new("settings-changed", G_OBJECT_CLASS_TYPE(klass),
				G_SIGNAL_ACTION | G_SIGNAL_DETAILED, 0, NULL, NULL,
				g_cclosure_marshal_VOID__VOID,
				G_TYPE_NONE, 0);
#warning TODO: Setup more signals
}

GType purple_account_get_gtype(void)
{
	static GType type = 0;

	if(type == 0) {
		static const GTypeInfo info = {
			sizeof(PurpleAccountClass),
			NULL,
			NULL,
			(GClassInitFunc)purple_account_class_init,
			NULL,
			NULL,
			sizeof(PurpleAccount),
			0,
			(GInstanceInitFunc)purple_account_init,
			NULL,
		};

		type = g_type_register_static(PURPLE_TYPE_OBJECT,
				"PurpleAccount",
				&info, 0);
	}

	return type;
}

PurpleAccount *
purple_account_new(const char *username, const char *protocol_id)
{
	PurpleAccount *account = NULL;
	PurplePlugin *prpl = NULL;
	PurplePluginProtocolInfo *prpl_info = NULL;
	PurpleStatusType *status_type;

	g_return_val_if_fail(username != NULL, NULL);
	g_return_val_if_fail(protocol_id != NULL, NULL);

	account = purple_accounts_find(username, protocol_id);

	if (account != NULL)
		return account;

	prpl = purple_find_prpl(protocol_id);
	g_return_val_if_fail(prpl != NULL, NULL);

	account = g_object_new(PURPLE_TYPE_ACCOUNT,
			PROP_USERNAME_S, username,
			PROP_PRPL_S, prpl,
			NULL);

	PURPLE_DBUS_REGISTER_POINTER(account, PurpleAccount);

	prpl_info = PURPLE_PLUGIN_PROTOCOL_INFO(prpl);
	if (prpl_info != NULL && prpl_info->status_types != NULL)
		purple_account_set_status_types(account, prpl_info->status_types(account));

	account->presence = purple_presence_new_for_account(account);

	status_type = purple_account_get_status_type_with_primitive(account, PURPLE_STATUS_AVAILABLE);
	if (status_type != NULL) {
		purple_presence_set_status_active(account->presence,
				purple_status_type_get_id(status_type),
				TRUE);
	} else {
		purple_presence_set_status_active(account->presence,
				"offline",
				TRUE);
	}

	return account;
}

void
purple_account_destroy(PurpleAccount *account)
{
	g_return_if_fail(PURPLE_IS_ACCOUNT(account));
	g_object_unref(G_OBJECT(account));
}

void
purple_account_set_register_callback(PurpleAccount *account, PurpleAccountRegistrationCb cb, void *user_data)
{
	g_return_if_fail(PURPLE_IS_ACCOUNT(account));

	account->registration_cb = cb;
	account->registration_cb_user_data = user_data;
}

void
purple_account_register(PurpleAccount *account)
{
	g_return_if_fail(PURPLE_IS_ACCOUNT(account));

	purple_debug_info("account", "Registering account %s\n",
					purple_account_get_username(account));

	purple_connection_new(account, TRUE, purple_account_get_password(account));
}

void
purple_account_unregister(PurpleAccount *account, PurpleAccountUnregistrationCb cb, void *user_data)
{
	g_return_if_fail(PURPLE_IS_ACCOUNT(account));

	purple_debug_info("account", "Unregistering account %s\n",
					  purple_account_get_username(account));

	purple_connection_new_unregister(account, purple_account_get_password(account), cb, user_data);
}

static void
request_password_ok_cb(PurpleAccount *account, PurpleRequestFields *fields)
{
	const char *entry;
	gboolean remember;

	entry = purple_request_fields_get_string(fields, "password");
	remember = purple_request_fields_get_bool(fields, "remember");

	if (!entry || !*entry)
	{
		purple_notify_error(account, NULL, _("Password is required to sign on."), NULL);
		return;
	}

	if(remember)
		purple_account_set_remember_password(account, TRUE);

	purple_account_set_password(account, entry);

	purple_connection_new(account, FALSE, entry);
}

static void
request_password_cancel_cb(PurpleAccount *account, PurpleRequestFields *fields)
{
	/* Disable the account as the user has canceled connecting */
	purple_account_set_enabled(account, FALSE);
}


void
purple_account_request_password(PurpleAccount *account, GCallback ok_cb,
				GCallback cancel_cb, void *user_data)
{
	gchar *primary;
	const gchar *username;
	PurpleRequestFieldGroup *group;
	PurpleRequestField *field;
	PurpleRequestFields *fields;

	/* Close any previous password request windows */
	purple_request_close_with_handle(account);

	username = purple_account_get_username(account);
	primary = g_strdup_printf(_("Enter password for %s (%s)"), username,
								  purple_account_get_protocol_name(account));

	fields = purple_request_fields_new();
	group = purple_request_field_group_new(NULL);
	purple_request_fields_add_group(fields, group);

	field = purple_request_field_string_new("password", _("Enter Password"), NULL, FALSE);
	purple_request_field_string_set_masked(field, TRUE);
	purple_request_field_set_required(field, TRUE);
	purple_request_field_group_add_field(group, field);

	field = purple_request_field_bool_new("remember", _("Save password"), FALSE);
	purple_request_field_group_add_field(group, field);

	purple_request_fields(account,
                        NULL,
                        primary,
                        NULL,
                        fields,
                        _("OK"), ok_cb,
                        _("Cancel"), cancel_cb,
						account, NULL, NULL,
                        user_data);
	g_free(primary);
}

void
purple_account_connect(PurpleAccount *account)
{
	PurplePlugin *prpl;
	PurplePluginProtocolInfo *prpl_info;
	const char *password;

	g_return_if_fail(PURPLE_IS_ACCOUNT(account));

	purple_debug_info("account", "Connecting to account %s\n",
					purple_account_get_username(account));

	if (!purple_account_get_enabled(account))
		return;

	prpl = purple_find_prpl(purple_account_get_protocol_id(account));
	if (prpl == NULL)
	{
		gchar *message;

		message = g_strdup_printf(_("Missing protocol plugin for %s"),
			purple_account_get_username(account));
		purple_notify_error(account, _("Connection Error"), message, NULL);
		g_free(message);
		return;
	}

	prpl_info = PURPLE_PLUGIN_PROTOCOL_INFO(prpl);
	password = purple_account_get_password(account);
	if ((password == NULL) &&
		!(prpl_info->options & OPT_PROTO_NO_PASSWORD) &&
		!(prpl_info->options & OPT_PROTO_PASSWORD_OPTIONAL))
		purple_account_request_password(account, G_CALLBACK(request_password_ok_cb), G_CALLBACK(request_password_cancel_cb), account);
	else
		purple_connection_new(account, FALSE, password);
}

void
purple_account_disconnect(PurpleAccount *account)
{
	g_return_if_fail(PURPLE_IS_ACCOUNT(account));
	g_return_if_fail(!purple_account_is_disconnected(account));

	purple_debug_info("account", "Disconnecting account %p\n", account);

	account->disconnecting = TRUE;

	purple_account_set_connection(account, NULL);
	if (!purple_account_get_remember_password(account))
		purple_account_set_password(account, NULL);

	account->disconnecting = FALSE;
}

void
purple_account_notify_added(PurpleAccount *account, const char *remote_user,
                          const char *id, const char *alias,
                          const char *message)
{
	PurpleAccountUiOps *ui_ops;

	g_return_if_fail(account     != NULL);
	g_return_if_fail(remote_user != NULL);

	ui_ops = purple_accounts_get_ui_ops();

	if (ui_ops != NULL && ui_ops->notify_added != NULL)
		ui_ops->notify_added(account, remote_user, id, alias, message);
}

void
purple_account_request_add(PurpleAccount *account, const char *remote_user,
                         const char *id, const char *alias,
                         const char *message)
{
	PurpleAccountUiOps *ui_ops;

	g_return_if_fail(account     != NULL);
	g_return_if_fail(remote_user != NULL);

	ui_ops = purple_accounts_get_ui_ops();

	if (ui_ops != NULL && ui_ops->request_add != NULL)
		ui_ops->request_add(account, remote_user, id, alias, message);
}

static void
purple_account_request_close_info(PurpleAccountRequestInfo *info)
{
	PurpleAccountUiOps *ops;

	ops = purple_accounts_get_ui_ops();

	if (ops != NULL && ops->close_account_request != NULL)
		ops->close_account_request(info->ui_handle);

	/* TODO: This will leak info->user_data, but there is no callback to just clean that up */

	g_free(info->user);
	g_free(info);

}

void
purple_account_request_close_with_account(PurpleAccount *account)
{
	GList *l, *l_next;

	g_return_if_fail(PURPLE_IS_ACCOUNT(account));

	for (l = handles; l != NULL; l = l_next) {
		PurpleAccountRequestInfo *info = l->data;

		l_next = l->next;

		if (info->account == account) {
			handles = g_list_remove(handles, info);
			purple_account_request_close_info(info);
		}
	}
}

void
purple_account_request_close(void *ui_handle)
{
	GList *l, *l_next;

	g_return_if_fail(ui_handle != NULL);

	for (l = handles; l != NULL; l = l_next) {
		PurpleAccountRequestInfo *info = l->data;

		l_next = l->next;

		if (info->ui_handle == ui_handle) {
			handles = g_list_remove(handles, info);
			purple_account_request_close_info(info);
		}
	}
}

static void
request_auth_cb(void *data)
{
	PurpleAccountRequestInfo *info = data;

	handles = g_list_remove(handles, info);

	info->auth_cb(info->userdata);

	purple_signal_emit(purple_accounts_get_handle(),
			"account-authorization-granted", info->account, info->user);

	g_free(info->user);
	g_free(info);
}

static void
request_deny_cb(void *data)
{
	PurpleAccountRequestInfo *info = data;

	handles = g_list_remove(handles, info);

	info->deny_cb(info->userdata);

	purple_signal_emit(purple_accounts_get_handle(),
			"account-authorization-denied", info->account, info->user);

	g_free(info->user);
	g_free(info);
}

void *
purple_account_request_authorization(PurpleAccount *account, const char *remote_user,
				     const char *id, const char *alias, const char *message, gboolean on_list,
				     PurpleAccountRequestAuthorizationCb auth_cb, PurpleAccountRequestAuthorizationCb deny_cb, void *user_data)
{
	PurpleAccountUiOps *ui_ops;
	PurpleAccountRequestInfo *info;
	int plugin_return;

	g_return_val_if_fail(account     != NULL, NULL);
	g_return_val_if_fail(remote_user != NULL, NULL);

	ui_ops = purple_accounts_get_ui_ops();

	plugin_return = GPOINTER_TO_INT(
			purple_signal_emit_return_1(purple_accounts_get_handle(),
				"account-authorization-requested", account, remote_user));

	if (plugin_return > 0) {
		auth_cb(user_data);
		return NULL;
	} else if (plugin_return < 0) {
		deny_cb(user_data);
		return NULL;
	}

	if (ui_ops != NULL && ui_ops->request_authorize != NULL) {
		info            = g_new0(PurpleAccountRequestInfo, 1);
		info->type      = PURPLE_ACCOUNT_REQUEST_AUTHORIZATION;
		info->account   = account;
		info->auth_cb   = auth_cb;
		info->deny_cb   = deny_cb;
		info->userdata  = user_data;
		info->user      = g_strdup(remote_user);
		info->ui_handle = ui_ops->request_authorize(account, remote_user, id, alias, message,
							    on_list, request_auth_cb, request_deny_cb, info);

		handles = g_list_append(handles, info);
		return info->ui_handle;
	}

	return NULL;
}

static void
change_password_cb(PurpleAccount *account, PurpleRequestFields *fields)
{
	const char *orig_pass, *new_pass_1, *new_pass_2;

	orig_pass  = purple_request_fields_get_string(fields, "password");
	new_pass_1 = purple_request_fields_get_string(fields, "new_password_1");
	new_pass_2 = purple_request_fields_get_string(fields, "new_password_2");

	if (g_utf8_collate(new_pass_1, new_pass_2))
	{
		purple_notify_error(account, NULL,
						  _("New passwords do not match."), NULL);

		return;
	}

	if ((purple_request_fields_is_field_required(fields, "password") &&
			(orig_pass == NULL || *orig_pass == '\0')) ||
		(purple_request_fields_is_field_required(fields, "new_password_1") &&
			(new_pass_1 == NULL || *new_pass_1 == '\0')) ||
		(purple_request_fields_is_field_required(fields, "new_password_2") &&
			(new_pass_2 == NULL || *new_pass_2 == '\0')))
	{
		purple_notify_error(account, NULL,
						  _("Fill out all fields completely."), NULL);
		return;
	}

	purple_account_change_password(account, orig_pass, new_pass_1);
}

void
purple_account_request_change_password(PurpleAccount *account)
{
	PurpleRequestFields *fields;
	PurpleRequestFieldGroup *group;
	PurpleRequestField *field;
	PurpleConnection *gc;
	PurplePlugin *prpl = NULL;
	PurplePluginProtocolInfo *prpl_info = NULL;
	char primary[256];

	g_return_if_fail(PURPLE_IS_ACCOUNT(account));
	g_return_if_fail(purple_account_is_connected(account));

	gc = purple_account_get_connection(account);
	if (gc != NULL)
		prpl = purple_connection_get_prpl(gc);
	if (prpl != NULL)
		prpl_info = PURPLE_PLUGIN_PROTOCOL_INFO(prpl);

	fields = purple_request_fields_new();

	group = purple_request_field_group_new(NULL);
	purple_request_fields_add_group(fields, group);

	field = purple_request_field_string_new("password", _("Original password"),
										  NULL, FALSE);
	purple_request_field_string_set_masked(field, TRUE);
	if (!(prpl_info && (prpl_info->options | OPT_PROTO_PASSWORD_OPTIONAL)))
		purple_request_field_set_required(field, TRUE);
	purple_request_field_group_add_field(group, field);

	field = purple_request_field_string_new("new_password_1",
										  _("New password"),
										  NULL, FALSE);
	purple_request_field_string_set_masked(field, TRUE);
	if (!(prpl_info && (prpl_info->options | OPT_PROTO_PASSWORD_OPTIONAL)))
		purple_request_field_set_required(field, TRUE);
	purple_request_field_group_add_field(group, field);

	field = purple_request_field_string_new("new_password_2",
										  _("New password (again)"),
										  NULL, FALSE);
	purple_request_field_string_set_masked(field, TRUE);
	if (!(prpl_info && (prpl_info->options | OPT_PROTO_PASSWORD_OPTIONAL)))
		purple_request_field_set_required(field, TRUE);
	purple_request_field_group_add_field(group, field);

	g_snprintf(primary, sizeof(primary), _("Change password for %s"),
			   purple_account_get_username(account));

	/* I'm sticking this somewhere in the code: bologna */

	purple_request_fields(purple_account_get_connection(account),
						NULL,
						primary,
						_("Please enter your current password and your "
						  "new password."),
						fields,
						_("OK"), G_CALLBACK(change_password_cb),
						_("Cancel"), NULL,
						account, NULL, NULL,
						account);
}

static void
set_user_info_cb(PurpleAccount *account, const char *user_info)
{
	PurpleConnection *gc;

	purple_account_set_user_info(account, user_info);
	gc = purple_account_get_connection(account);
	serv_set_info(gc, user_info);
}

void
purple_account_request_change_user_info(PurpleAccount *account)
{
	PurpleConnection *gc;
	char primary[256];
	PurpleConnectionFlags flags = 0;

	g_return_if_fail(PURPLE_IS_ACCOUNT(account));
	g_return_if_fail(purple_account_is_connected(account));

	gc = purple_account_get_connection(account);

	g_snprintf(primary, sizeof(primary),
			   _("Change user information for %s"),
			   purple_account_get_username(account));

	g_object_get(G_OBJECT(gc), "flags", &flags, NULL);
	purple_request_input(gc, _("Set User Info"), primary, NULL,
					   purple_account_get_user_info(account),
					   TRUE, FALSE, ((gc != NULL) &&
					   (flags & PURPLE_CONNECTION_FLAGS_HTML) ? "html" : NULL),
					   _("Save"), G_CALLBACK(set_user_info_cb),
					   _("Cancel"), NULL,
					   account, NULL, NULL,
					   account);
}

void
purple_account_set_username(PurpleAccount *account, const char *username)
{
	g_return_if_fail(PURPLE_IS_ACCOUNT(account));
	g_return_if_fail(username != NULL);

	if (purple_util_strings_equal(account->username, username))
		return;

	g_free(account->username);
	account->username = g_strdup(username);
	g_object_notify(G_OBJECT(account), PROP_USERNAME_S);

	/* if the name changes, we should re-write the buddy list
	 * to disk with the new name */
	purple_blist_schedule_save();
}

void
purple_account_set_password(PurpleAccount *account, const char *password)
{
	g_return_if_fail(PURPLE_IS_ACCOUNT(account));

	if (purple_util_strings_equal(account->password, password))
		return;

	g_free(account->password);
	account->password = g_strdup(password);
	g_object_notify(G_OBJECT(account), PROP_PASSWORD_S);
}

void
purple_account_set_alias(PurpleAccount *account, const char *alias)
{
	g_return_if_fail(PURPLE_IS_ACCOUNT(account));

	if (purple_util_strings_equal(account->alias, alias))
		return;

	g_free(account->alias);
	account->alias = g_strdup(alias);
	g_object_notify(G_OBJECT(account), PROP_PRIVATE_ALIAS_S);
#if 0
	/* XXX: For now, we don't have a signale for this event, since the
	 * 'notify::private-alias' signal is emitted due to g_object_notify anyway.
	 * If we decide that the old alias is also useful, then we can bring back
	 * the signal */
	purple_signal_emit(purple_accounts_get_handle(), "account-alias-changed",
					 account, old);
#endif
}

void
purple_account_set_user_info(PurpleAccount *account, const char *user_info)
{
	g_return_if_fail(PURPLE_IS_ACCOUNT(account));

	if (purple_util_strings_equal(account->user_info, user_info))
		return;

	g_free(account->user_info);
	account->user_info = g_strdup(user_info);
	g_object_notify(G_OBJECT(account), PROP_USER_INFO_S);
}

void purple_account_set_buddy_icon_path(PurpleAccount *account, const char *path)
{
	g_return_if_fail(PURPLE_IS_ACCOUNT(account));

	if (purple_util_strings_equal(account->buddy_icon_path, path))
		return;

	g_free(account->buddy_icon_path);
	account->buddy_icon_path = g_strdup(path);
	g_object_notify(G_OBJECT(account), PROP_BUDDY_ICON_PATH_S);
}

void
purple_account_set_connection(PurpleAccount *account, PurpleConnection *gc)
{
	PurpleConnection *old;

	g_return_if_fail(PURPLE_IS_ACCOUNT(account));
	g_return_if_fail(gc == NULL || PURPLE_IS_CONNECTION(gc));

#warning Connect and disconnect to 'signed-on' and 'connection-error' on gc and set/clear current_error.
	if (account->gc == gc)
		return;

	old = account->gc;
	account->gc = gc;
	if (old)
		g_object_unref(old);
	g_object_notify(G_OBJECT(account), PROP_CONNECTION_S);
}

void
purple_account_set_remember_password(PurpleAccount *account, gboolean value)
{
	g_return_if_fail(PURPLE_IS_ACCOUNT(account));

	if (account->remember_pass == value)
		return;
	account->remember_pass = value;
	g_object_notify(G_OBJECT(account), PROP_REMEMBER_PASSWORD_S);
}

void
purple_account_set_check_mail(PurpleAccount *account, gboolean value)
{
	PurpleAccountPrivate *priv = PURPLE_ACCOUNT_GET_PRIVATE(account);

	g_return_if_fail(PURPLE_IS_ACCOUNT(account));

	if (priv->check_mail == value)
		return;
	priv->check_mail = value;
	g_object_notify(G_OBJECT(account), PROP_CHECK_MAIL_S);
}

void
purple_account_set_enabled(PurpleAccount *account, gboolean value)
{
	PurpleConnection *gc;
	gboolean was_enabled = FALSE;

	g_return_if_fail(PURPLE_IS_ACCOUNT(account));

	was_enabled = purple_account_get_enabled(account);
	if (was_enabled == value)
		return;

	PURPLE_ACCOUNT_GET_PRIVATE(account)->enabled = value;
	g_object_notify(G_OBJECT(account), PROP_ENABLED_S);

	purple_account_set_ui_bool(account, purple_core_get_ui(),
			"enabled", value);

	gc = purple_account_get_connection(account);

	/* XXX: I don't know where to move these signals. */
	if(was_enabled && !value)
		purple_signal_emit(purple_accounts_get_handle(), "account-disabled", account);
	else if(!was_enabled && value)
		purple_signal_emit(purple_accounts_get_handle(), "account-enabled", account);

#warning Do something about wants-to-die. Perhaps check if current_error is fatal?
#if 0
	if ((gc != NULL) && (gc->wants_to_die == TRUE))
		return;
#endif

	if (value && purple_presence_is_online(account->presence))
		purple_account_connect(account);
	else if (!value && !purple_account_is_disconnected(account))
		purple_account_disconnect(account);
}

void
purple_account_set_proxy_info(PurpleAccount *account, PurpleProxyInfo *info)
{
	g_return_if_fail(PURPLE_IS_ACCOUNT(account));

	if (account->proxy_info != NULL)
		purple_proxy_info_destroy(account->proxy_info);

	account->proxy_info = info;

	schedule_accounts_save();
}

void
purple_account_set_status_types(PurpleAccount *account, GList *status_types)
{
	g_return_if_fail(PURPLE_IS_ACCOUNT(account));

	/* Out with the old... */
	if (account->status_types != NULL)
	{
		g_list_foreach(account->status_types, (GFunc)purple_status_type_destroy, NULL);
		g_list_free(account->status_types);
	}

	/* In with the new... */
	account->status_types = status_types;
}

void
purple_account_set_status(PurpleAccount *account, const char *status_id,
						gboolean active, ...)
{
	GList *attrs = NULL;
	const gchar *id;
	gpointer data;
	va_list args;

	va_start(args, active);
	while ((id = va_arg(args, const char *)) != NULL)
	{
		attrs = g_list_append(attrs, (char *)id);
		data = va_arg(args, void *);
		attrs = g_list_append(attrs, data);
	}
	purple_account_set_status_list(account, status_id, active, attrs);
	g_list_free(attrs);
	va_end(args);
}

void
purple_account_set_status_list(PurpleAccount *account, const char *status_id,
							 gboolean active, GList *attrs)
{
	PurpleStatus *status;

	g_return_if_fail(account   != NULL);
	g_return_if_fail(status_id != NULL);

	status = purple_account_get_status(account, status_id);
	if (status == NULL)
	{
		purple_debug_error("account",
				   "Invalid status ID '%s' for account %s (%s)\n",
				   status_id, purple_account_get_username(account),
				   purple_account_get_protocol_id(account));
		return;
	}

	if (active || purple_status_is_independent(status))
		purple_status_set_active_with_attrs_list(status, active, attrs);

	/*
	 * Our current statuses are saved to accounts.xml (so that when we
	 * reconnect, we go back to the previous status).
	 */
	schedule_accounts_save();
}

void
purple_account_clear_settings(PurpleAccount *account)
{
	g_return_if_fail(PURPLE_IS_ACCOUNT(account));

	g_hash_table_destroy(account->settings);

	account->settings = g_hash_table_new_full(g_str_hash, g_str_equal,
			g_free, delete_setting);
}

static gboolean
account_setting_value_changed(PurpleAccount *account, const char *name,
		GType type, ...)
{
	va_list args;
	PurpleAccountSetting *setting;
	gboolean changed = TRUE;

	setting = g_hash_table_lookup(account->settings, name);
	if (!setting)
		return TRUE;  /* This is a new setting */

	va_start(args, type);
	switch (type) {
		case G_TYPE_STRING: {
			const char *string = va_arg(args, const char *);
			changed = (g_utf8_collate(string, setting->value.string) != 0);
			break;
		}
		case G_TYPE_INT: {
			int value = va_arg(args, int);
			changed = (value != setting->value.integer);
			break;
		}
		case G_TYPE_BOOLEAN: {
			gboolean value = va_arg(args, gboolean);
			changed = (value != setting->value.boolean);
			break;
		}
	}
	va_end(args);
	return changed;
}

void
purple_account_set_int(PurpleAccount *account, const char *name, int value)
{
	PurpleAccountSetting *setting;

	g_return_if_fail(PURPLE_IS_ACCOUNT(account));
	g_return_if_fail(name    != NULL);

	if (!account_setting_value_changed(account, name, G_TYPE_INT, value))
		return;

	setting = g_new0(PurpleAccountSetting, 1);

	setting->type          = PURPLE_PREF_INT;
	setting->value.integer = value;

	g_hash_table_insert(account->settings, g_strdup(name), setting);
	g_signal_emit(G_OBJECT(account), signals[SIG_SETTINGS_CHANGED],
			g_quark_from_string(name));
}

void
purple_account_set_string(PurpleAccount *account, const char *name,
						const char *value)
{
	PurpleAccountSetting *setting;

	g_return_if_fail(PURPLE_IS_ACCOUNT(account));
	g_return_if_fail(name    != NULL);

	if (!account_setting_value_changed(account, name, G_TYPE_STRING, value))
		return;

	setting = g_new0(PurpleAccountSetting, 1);

	setting->type         = PURPLE_PREF_STRING;
	setting->value.string = g_strdup(value);

	g_hash_table_insert(account->settings, g_strdup(name), setting);

	g_signal_emit(G_OBJECT(account), signals[SIG_SETTINGS_CHANGED],
			g_quark_from_string(name));
}

void
purple_account_set_bool(PurpleAccount *account, const char *name, gboolean value)
{
	PurpleAccountSetting *setting;

	g_return_if_fail(PURPLE_IS_ACCOUNT(account));
	g_return_if_fail(name    != NULL);

	if (!account_setting_value_changed(account, name, G_TYPE_BOOLEAN, value))
		return;

	setting = g_new0(PurpleAccountSetting, 1);

	setting->type       = PURPLE_PREF_BOOLEAN;
	setting->value.boolean = value;

	g_hash_table_insert(account->settings, g_strdup(name), setting);

	g_signal_emit(G_OBJECT(account), signals[SIG_SETTINGS_CHANGED],
			g_quark_from_string(name));
}

static GHashTable *
get_ui_settings_table(PurpleAccount *account, const char *ui)
{
	GHashTable *table;

	table = g_hash_table_lookup(account->ui_settings, ui);

	if (table == NULL) {
		table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
									  delete_setting);
		g_hash_table_insert(account->ui_settings, g_strdup(ui), table);
	}

	return table;
}

static gboolean
account_ui_setting_value_changed(PurpleAccount *account, const char *ui,
		const char *name, GType type, ...)
{
	va_list args;
	PurpleAccountSetting *setting;
	gboolean changed = TRUE;
	GHashTable *table;

	table = get_ui_settings_table(account, ui);
	setting = table ? g_hash_table_lookup(table, name) : NULL;
	if (!setting)
		return TRUE;  /* This is a new setting */

	va_start(args, type);
	switch (type) {
		case G_TYPE_STRING: {
			const char *string = va_arg(args, const char *);
			changed = (g_utf8_collate(string, setting->value.string) != 0);
			break;
		}
		case G_TYPE_INT: {
			int value = va_arg(args, int);
			changed = (value != setting->value.integer);
			break;
		}
		case G_TYPE_BOOLEAN: {
			gboolean value = va_arg(args, gboolean);
			changed = (value != setting->value.boolean);
			break;
		}
	}
	va_end(args);
	return changed;
}

void
purple_account_set_ui_int(PurpleAccount *account, const char *ui,
						const char *name, int value)
{
	PurpleAccountSetting *setting;
	GHashTable *table;
	char *uiname;

	g_return_if_fail(PURPLE_IS_ACCOUNT(account));
	g_return_if_fail(ui      != NULL);
	g_return_if_fail(name    != NULL);

	if (!account_ui_setting_value_changed(account, ui, name, G_TYPE_INT, value))
		return;

	setting = g_new0(PurpleAccountSetting, 1);

	setting->type          = PURPLE_PREF_INT;
	setting->ui            = g_strdup(ui);
	setting->value.integer = value;

	table = get_ui_settings_table(account, ui);

	g_hash_table_insert(table, g_strdup(name), setting);

	/* XXX: Or do want a seperate ui-settings-changed signal? */
	uiname = g_strconcat("ui:", name, NULL);
	g_signal_emit(G_OBJECT(account), signals[SIG_SETTINGS_CHANGED],
			g_quark_from_string(uiname));
	g_free(uiname);
}

void
purple_account_set_ui_string(PurpleAccount *account, const char *ui,
						   const char *name, const char *value)
{
	PurpleAccountSetting *setting;
	GHashTable *table;
	char *uiname;

	g_return_if_fail(PURPLE_IS_ACCOUNT(account));
	g_return_if_fail(ui      != NULL);
	g_return_if_fail(name    != NULL);

	if (!account_ui_setting_value_changed(account, ui, name, G_TYPE_STRING, value))
		return;

	setting = g_new0(PurpleAccountSetting, 1);

	setting->type         = PURPLE_PREF_STRING;
	setting->ui           = g_strdup(ui);
	setting->value.string = g_strdup(value);

	table = get_ui_settings_table(account, ui);

	g_hash_table_insert(table, g_strdup(name), setting);

	/* XXX: Or do want a seperate ui-settings-changed signal? */
	uiname = g_strconcat("ui:", name, NULL);
	g_signal_emit(G_OBJECT(account), signals[SIG_SETTINGS_CHANGED],
			g_quark_from_string(uiname));
	g_free(uiname);
}

void
purple_account_set_ui_bool(PurpleAccount *account, const char *ui,
						 const char *name, gboolean value)
{
	PurpleAccountSetting *setting;
	GHashTable *table;
	char *uiname;

	g_return_if_fail(PURPLE_IS_ACCOUNT(account));
	g_return_if_fail(ui      != NULL);
	g_return_if_fail(name    != NULL);

	if (!account_ui_setting_value_changed(account, ui, name, G_TYPE_BOOLEAN, value))
		return;

	setting = g_new0(PurpleAccountSetting, 1);

	setting->type       = PURPLE_PREF_BOOLEAN;
	setting->ui         = g_strdup(ui);
	setting->value.boolean = value;

	table = get_ui_settings_table(account, ui);

	g_hash_table_insert(table, g_strdup(name), setting);

	if (strcmp(ui, purple_core_get_ui()) == 0 &&
			strcmp(name, "enabled") == 0) {
		purple_account_set_enabled(account, value);
	}

	/* XXX: Or do want a seperate ui-settings-changed signal? */
	uiname = g_strconcat("ui:", name, NULL);
	g_signal_emit(G_OBJECT(account), signals[SIG_SETTINGS_CHANGED],
			g_quark_from_string(uiname));
	g_free(uiname);
}

static PurpleConnectionState
purple_account_get_state(const PurpleAccount *account)
{
	PurpleConnection *gc;

	g_return_val_if_fail(PURPLE_IS_ACCOUNT(account), PURPLE_CONNECTION_STATE_DISCONNECTED);

	gc = purple_account_get_connection(account);
	if (!gc)
		return PURPLE_CONNECTION_STATE_DISCONNECTED;

	return purple_connection_get_state(gc);
}

gboolean
purple_account_is_connected(const PurpleAccount *account)
{
	return (purple_account_get_state(account) == PURPLE_CONNECTION_STATE_CONNECTED);
}

gboolean
purple_account_is_connecting(const PurpleAccount *account)
{
	return (purple_account_get_state(account) == PURPLE_CONNECTION_STATE_CONNECTING);
}

gboolean
purple_account_is_disconnected(const PurpleAccount *account)
{
	return (purple_account_get_state(account) == PURPLE_CONNECTION_STATE_DISCONNECTED);
}

const char *
purple_account_get_username(const PurpleAccount *account)
{
	g_return_val_if_fail(PURPLE_IS_ACCOUNT(account), NULL);

	return account->username;
}

const char *
purple_account_get_password(const PurpleAccount *account)
{
	g_return_val_if_fail(PURPLE_IS_ACCOUNT(account), NULL);

	return account->password;
}

const char *
purple_account_get_alias(const PurpleAccount *account)
{
	g_return_val_if_fail(PURPLE_IS_ACCOUNT(account), NULL);

	return account->alias;
}

const char *
purple_account_get_user_info(const PurpleAccount *account)
{
	g_return_val_if_fail(PURPLE_IS_ACCOUNT(account), NULL);

	return account->user_info;
}

const char *
purple_account_get_buddy_icon_path(const PurpleAccount *account)
{
	g_return_val_if_fail(PURPLE_IS_ACCOUNT(account), NULL);

	return account->buddy_icon_path;
}

const char *
purple_account_get_protocol_id(const PurpleAccount *account)
{
	g_return_val_if_fail(PURPLE_IS_ACCOUNT(account), NULL);
	return account->protocol_id;
}

const char *
purple_account_get_protocol_name(const PurpleAccount *account)
{
	PurplePlugin *p;

	g_return_val_if_fail(PURPLE_IS_ACCOUNT(account), NULL);

	p = purple_find_prpl(purple_account_get_protocol_id(account));

	return ((p && p->info->name) ? _(p->info->name) : _("Unknown"));
}

PurpleConnection *
purple_account_get_connection(const PurpleAccount *account)
{
	g_return_val_if_fail(PURPLE_IS_ACCOUNT(account), NULL);

	return account->gc;
}

gboolean
purple_account_get_remember_password(const PurpleAccount *account)
{
	g_return_val_if_fail(PURPLE_IS_ACCOUNT(account), FALSE);

	return account->remember_pass;
}

gboolean
purple_account_get_check_mail(const PurpleAccount *account)
{
	g_return_val_if_fail(PURPLE_IS_ACCOUNT(account), FALSE);

	return PURPLE_ACCOUNT_GET_PRIVATE(account)->check_mail;
}

gboolean
purple_account_get_enabled(const PurpleAccount *account)
{
	g_return_val_if_fail(PURPLE_IS_ACCOUNT(account), FALSE);

	return PURPLE_ACCOUNT_GET_PRIVATE(account)->enabled;
}

PurpleProxyInfo *
purple_account_get_proxy_info(const PurpleAccount *account)
{
	g_return_val_if_fail(PURPLE_IS_ACCOUNT(account), NULL);

	return account->proxy_info;
}

PurpleStatus *
purple_account_get_active_status(const PurpleAccount *account)
{
	g_return_val_if_fail(account   != NULL, NULL);

	return purple_presence_get_active_status(account->presence);
}

PurpleStatus *
purple_account_get_status(const PurpleAccount *account, const char *status_id)
{
	g_return_val_if_fail(account   != NULL, NULL);
	g_return_val_if_fail(status_id != NULL, NULL);

	return purple_presence_get_status(account->presence, status_id);
}

PurpleStatusType *
purple_account_get_status_type(const PurpleAccount *account, const char *id)
{
	GList *l;

	g_return_val_if_fail(PURPLE_IS_ACCOUNT(account), NULL);
	g_return_val_if_fail(id      != NULL, NULL);

	for (l = purple_account_get_status_types(account); l != NULL; l = l->next)
	{
		PurpleStatusType *status_type = (PurpleStatusType *)l->data;

		if (!strcmp(purple_status_type_get_id(status_type), id))
			return status_type;
	}

	return NULL;
}

PurpleStatusType *
purple_account_get_status_type_with_primitive(const PurpleAccount *account, PurpleStatusPrimitive primitive)
{
	GList *l;

	g_return_val_if_fail(PURPLE_IS_ACCOUNT(account), NULL);

	for (l = purple_account_get_status_types(account); l != NULL; l = l->next)
	{
		PurpleStatusType *status_type = (PurpleStatusType *)l->data;

		if (purple_status_type_get_primitive(status_type) == primitive)
			return status_type;
	}

	return NULL;
}

PurplePresence *
purple_account_get_presence(const PurpleAccount *account)
{
	g_return_val_if_fail(PURPLE_IS_ACCOUNT(account), NULL);

	return account->presence;
}

gboolean
purple_account_is_status_active(const PurpleAccount *account,
							  const char *status_id)
{
	g_return_val_if_fail(account   != NULL, FALSE);
	g_return_val_if_fail(status_id != NULL, FALSE);

	return purple_presence_is_status_active(account->presence, status_id);
}

GList *
purple_account_get_status_types(const PurpleAccount *account)
{
	g_return_val_if_fail(PURPLE_IS_ACCOUNT(account), NULL);

	return account->status_types;
}

int
purple_account_get_int(const PurpleAccount *account, const char *name,
					 int default_value)
{
	PurpleAccountSetting *setting;

	g_return_val_if_fail(PURPLE_IS_ACCOUNT(account), default_value);
	g_return_val_if_fail(name    != NULL, default_value);

	setting = g_hash_table_lookup(account->settings, name);

	if (setting == NULL)
		return default_value;

	g_return_val_if_fail(setting->type == PURPLE_PREF_INT, default_value);

	return setting->value.integer;
}

const char *
purple_account_get_string(const PurpleAccount *account, const char *name,
						const char *default_value)
{
	PurpleAccountSetting *setting;

	g_return_val_if_fail(PURPLE_IS_ACCOUNT(account), default_value);
	g_return_val_if_fail(name    != NULL, default_value);

	setting = g_hash_table_lookup(account->settings, name);

	if (setting == NULL)
		return default_value;

	g_return_val_if_fail(setting->type == PURPLE_PREF_STRING, default_value);

	return setting->value.string;
}

gboolean
purple_account_get_bool(const PurpleAccount *account, const char *name,
					  gboolean default_value)
{
	PurpleAccountSetting *setting;

	g_return_val_if_fail(PURPLE_IS_ACCOUNT(account), default_value);
	g_return_val_if_fail(name    != NULL, default_value);

	setting = g_hash_table_lookup(account->settings, name);

	if (setting == NULL)
		return default_value;

	g_return_val_if_fail(setting->type == PURPLE_PREF_BOOLEAN, default_value);

	return setting->value.boolean;
}

int
purple_account_get_ui_int(const PurpleAccount *account, const char *ui,
						const char *name, int default_value)
{
	PurpleAccountSetting *setting;
	GHashTable *table;

	g_return_val_if_fail(PURPLE_IS_ACCOUNT(account), default_value);
	g_return_val_if_fail(ui      != NULL, default_value);
	g_return_val_if_fail(name    != NULL, default_value);

	if ((table = g_hash_table_lookup(account->ui_settings, ui)) == NULL)
		return default_value;

	if ((setting = g_hash_table_lookup(table, name)) == NULL)
		return default_value;

	g_return_val_if_fail(setting->type == PURPLE_PREF_INT, default_value);

	return setting->value.integer;
}

const char *
purple_account_get_ui_string(const PurpleAccount *account, const char *ui,
						   const char *name, const char *default_value)
{
	PurpleAccountSetting *setting;
	GHashTable *table;

	g_return_val_if_fail(PURPLE_IS_ACCOUNT(account), default_value);
	g_return_val_if_fail(ui      != NULL, default_value);
	g_return_val_if_fail(name    != NULL, default_value);

	if ((table = g_hash_table_lookup(account->ui_settings, ui)) == NULL)
		return default_value;

	if ((setting = g_hash_table_lookup(table, name)) == NULL)
		return default_value;

	g_return_val_if_fail(setting->type == PURPLE_PREF_STRING, default_value);

	return setting->value.string;
}

gboolean
purple_account_get_ui_bool(const PurpleAccount *account, const char *ui,
						 const char *name, gboolean default_value)
{
	PurpleAccountSetting *setting;
	GHashTable *table;

	g_return_val_if_fail(PURPLE_IS_ACCOUNT(account), default_value);
	g_return_val_if_fail(ui      != NULL, default_value);
	g_return_val_if_fail(name    != NULL, default_value);

	if ((table = g_hash_table_lookup(account->ui_settings, ui)) == NULL)
		return default_value;

	if ((setting = g_hash_table_lookup(table, name)) == NULL)
		return default_value;

	g_return_val_if_fail(setting->type == PURPLE_PREF_BOOLEAN, default_value);

	return setting->value.boolean;
}

PurpleLog *
purple_account_get_log(PurpleAccount *account, gboolean create)
{
	g_return_val_if_fail(PURPLE_IS_ACCOUNT(account), NULL);

	if(!account->system_log && create){
		PurplePresence *presence;
		int login_time;

		presence = purple_account_get_presence(account);
		login_time = purple_presence_get_login_time(presence);

		account->system_log	 = purple_log_new(PURPLE_LOG_SYSTEM,
				purple_account_get_username(account), account, NULL,
				(login_time != 0) ? login_time : time(NULL), NULL);
	}

	return account->system_log;
}

void
purple_account_destroy_log(PurpleAccount *account)
{
	g_return_if_fail(PURPLE_IS_ACCOUNT(account));

	if(account->system_log){
		purple_log_free(account->system_log);
		account->system_log = NULL;
	}
}

void
purple_account_add_buddy(PurpleAccount *account, PurpleBuddy *buddy)
{
	PurplePluginProtocolInfo *prpl_info = NULL;
	PurpleConnection *gc = purple_account_get_connection(account);
	PurplePlugin *prpl = NULL;
	
	if (gc != NULL)
	        prpl = purple_connection_get_prpl(gc);      

	if (prpl != NULL)
		prpl_info = PURPLE_PLUGIN_PROTOCOL_INFO(prpl);

	if (prpl_info != NULL && prpl_info->add_buddy != NULL)
		prpl_info->add_buddy(gc, buddy, purple_buddy_get_group(buddy));
}

void
purple_account_add_buddies(PurpleAccount *account, GList *buddies)
{
	PurplePluginProtocolInfo *prpl_info = NULL;
	PurpleConnection *gc = purple_account_get_connection(account);
	PurplePlugin *prpl = NULL;
	
	if (gc != NULL)
	        prpl = purple_connection_get_prpl(gc);      

	if (prpl != NULL)
		prpl_info = PURPLE_PLUGIN_PROTOCOL_INFO(prpl);
		
	if (prpl_info) {
		GList *cur, *groups = NULL;

		/* Make a list of what group each buddy is in */
		for (cur = buddies; cur != NULL; cur = cur->next) {
			PurpleBlistNode *node = cur->data;
			groups = g_list_append(groups, node->parent->parent);
		}

		if (prpl_info->add_buddies != NULL)
			prpl_info->add_buddies(gc, buddies, groups);
		else if (prpl_info->add_buddy != NULL) {
			GList *curb = buddies, *curg = groups;

			while ((curb != NULL) && (curg != NULL)) {
				prpl_info->add_buddy(gc, curb->data, curg->data);
				curb = curb->next;
				curg = curg->next;
			}
		}

		g_list_free(groups);
	}
}

void
purple_account_remove_buddy(PurpleAccount *account, PurpleBuddy *buddy,
		PurpleGroup *group)
{
	PurplePluginProtocolInfo *prpl_info = NULL;
	PurpleConnection *gc = purple_account_get_connection(account);
	PurplePlugin *prpl = NULL;
	
	if (gc != NULL)
	        prpl = purple_connection_get_prpl(gc);      

	if (prpl != NULL)
		prpl_info = PURPLE_PLUGIN_PROTOCOL_INFO(prpl);
		
	if (prpl_info && prpl_info->remove_buddy)
		prpl_info->remove_buddy(gc, buddy, group);
}

void
purple_account_remove_buddies(PurpleAccount *account, GList *buddies, GList *groups)
{
	PurplePluginProtocolInfo *prpl_info = NULL;
	PurpleConnection *gc = purple_account_get_connection(account);
	PurplePlugin *prpl = NULL;
	
	if (gc != NULL)
	        prpl = purple_connection_get_prpl(gc);      

	if (prpl != NULL)
		prpl_info = PURPLE_PLUGIN_PROTOCOL_INFO(prpl);
		
	if (prpl_info) {
		if (prpl_info->remove_buddies)
			prpl_info->remove_buddies(gc, buddies, groups);
		else {
			GList *curb = buddies;
			GList *curg = groups;
			while ((curb != NULL) && (curg != NULL)) {
				purple_account_remove_buddy(account, curb->data, curg->data);
				curb = curb->next;
				curg = curg->next;
			}
		}
	}
}

void
purple_account_remove_group(PurpleAccount *account, PurpleGroup *group)
{
	PurplePluginProtocolInfo *prpl_info = NULL;
	PurpleConnection *gc = purple_account_get_connection(account);
	PurplePlugin *prpl = NULL;
	
	if (gc != NULL)
	        prpl = purple_connection_get_prpl(gc);      

	if (prpl != NULL)
		prpl_info = PURPLE_PLUGIN_PROTOCOL_INFO(prpl);

	if (prpl_info && prpl_info->remove_group)
		prpl_info->remove_group(gc, group);
}

void
purple_account_change_password(PurpleAccount *account, const char *orig_pw,
		const char *new_pw)
{
	PurplePluginProtocolInfo *prpl_info = NULL;
	PurpleConnection *gc = purple_account_get_connection(account);
	PurplePlugin *prpl = NULL;
	
	purple_account_set_password(account, new_pw);
	
	if (gc != NULL)
	        prpl = purple_connection_get_prpl(gc);      

	if (prpl != NULL)
		prpl_info = PURPLE_PLUGIN_PROTOCOL_INFO(prpl);

	if (prpl_info && prpl_info->change_passwd)
		prpl_info->change_passwd(gc, orig_pw, new_pw);
}

gboolean purple_account_supports_offline_message(PurpleAccount *account, PurpleBuddy *buddy)
{
	PurpleConnection *gc;
	PurplePluginProtocolInfo *prpl_info = NULL;
	PurplePlugin *prpl = NULL;
	
	g_return_val_if_fail(account, FALSE);
	g_return_val_if_fail(buddy, FALSE);

	gc = purple_account_get_connection(account);
	if (gc == NULL)
		return FALSE;
	
	prpl = purple_connection_get_prpl(gc);      

	if (prpl != NULL)
		prpl_info = PURPLE_PLUGIN_PROTOCOL_INFO(prpl);

	if (!prpl_info || !prpl_info->offline_message)
		return FALSE;
	return prpl_info->offline_message(buddy);
}

static void
signed_on_cb(PurpleConnection *gc,
             gpointer unused)
{
	PurpleAccount *account = purple_connection_get_account(gc);
	purple_account_clear_current_error(account);
}

static void
set_current_error(PurpleAccount *account,
                  PurpleConnectionErrorInfo *new_err)
{
	PurpleAccountPrivate *priv;
	PurpleConnectionErrorInfo *old_err;

	g_return_if_fail(PURPLE_IS_ACCOUNT(account));

	priv = PURPLE_ACCOUNT_GET_PRIVATE(account);
	old_err = priv->current_error;

	if(new_err == old_err)
		return;

	priv->current_error = new_err;

	purple_signal_emit(purple_accounts_get_handle(),
	                   "account-error-changed",
	                   account, old_err, new_err);
	schedule_accounts_save();

	if(old_err)
		g_free(old_err->description);

	PURPLE_DBUS_UNREGISTER_POINTER(old_err);
	g_free(old_err);
}

static void
connection_error_cb(PurpleConnection *gc,
                    PurpleConnectionError type,
                    const gchar *description,
                    gpointer unused)
{
	PurpleAccount *account;
	PurpleConnectionErrorInfo *err;

	account = purple_connection_get_account(gc);

	g_return_if_fail(PURPLE_IS_ACCOUNT(account));

	err = g_new0(PurpleConnectionErrorInfo, 1);
	PURPLE_DBUS_REGISTER_POINTER(err, PurpleConnectionErrorInfo);

	err->type = type;
	err->description = g_strdup(description);

	set_current_error(account, err);
}

const PurpleConnectionErrorInfo *
purple_account_get_current_error(PurpleAccount *account)
{
	PurpleAccountPrivate *priv = PURPLE_ACCOUNT_GET_PRIVATE(account);
	return priv->current_error;
}

void
purple_account_clear_current_error(PurpleAccount *account)
{
	set_current_error(account, NULL);
}

xmlnode * purple_account_to_xmlnode(PurpleAccount *account)
{
	PurpleAccountPrivate *priv;
	xmlnode *node, *child;
	const char *tmp;
	PurplePresence *presence;
	PurpleProxyInfo *proxy_info;

	g_return_val_if_fail(PURPLE_IS_ACCOUNT(account), NULL);

	priv = PURPLE_ACCOUNT_GET_PRIVATE(account);

	node = xmlnode_new("account");

	child = xmlnode_new_child(node, "protocol");
	xmlnode_insert_data(child, purple_account_get_protocol_id(account), -1);

	child = xmlnode_new_child(node, "name");
	xmlnode_insert_data(child, purple_account_get_username(account), -1);

	if (purple_account_get_remember_password(account) &&
		((tmp = purple_account_get_password(account)) != NULL))
	{
		child = xmlnode_new_child(node, "password");
		xmlnode_insert_data(child, tmp, -1);
	}

	if ((tmp = purple_account_get_alias(account)) != NULL)
	{
		child = xmlnode_new_child(node, "alias");
		xmlnode_insert_data(child, tmp, -1);
	}

	if ((presence = purple_account_get_presence(account)) != NULL)
	{
		child = statuses_to_xmlnode(presence);
		xmlnode_insert_child(node, child);
	}

	if ((tmp = purple_account_get_user_info(account)) != NULL)
	{
		/* TODO: Do we need to call purple_str_strip_char(tmp, '\r') here? */
		child = xmlnode_new_child(node, "userinfo");
		xmlnode_insert_data(child, tmp, -1);
	}

	child = xmlnode_new_child(node, "check-mail");
	xmlnode_insert_data(child, purple_account_get_check_mail(account) ? "1" : "0", -1);

	if (g_hash_table_size(account->settings) > 0)
	{
		child = xmlnode_new_child(node, "settings");
		g_hash_table_foreach(account->settings, setting_to_xmlnode, child);
	}

	if (g_hash_table_size(account->ui_settings) > 0)
	{
		g_hash_table_foreach(account->ui_settings, ui_setting_to_xmlnode, node);
	}

	if ((proxy_info = purple_account_get_proxy_info(account)) != NULL)
	{
		child = proxy_settings_to_xmlnode(proxy_info);
		xmlnode_insert_child(node, child);
	}

	child = current_error_to_xmlnode(priv->current_error);
	xmlnode_insert_child(node, child);

	return node;
}

void
purple_accounts_add(PurpleAccount *account)
{
	g_return_if_fail(PURPLE_IS_ACCOUNT(account));

	purple_account_manager_add_account(purple_account_manager_get(), account);
}

void
purple_accounts_remove(PurpleAccount *account)
{
	g_return_if_fail(PURPLE_IS_ACCOUNT(account));

	purple_account_manager_remove_account(purple_account_manager_get(), account);
}

void
purple_accounts_delete(PurpleAccount *account)
{
	PurpleBlistNode *gnode, *cnode, *bnode;
	GList *iter;

	g_return_if_fail(PURPLE_IS_ACCOUNT(account));

	/*
	 * Disable the account before blowing it out of the water.
	 * Conceptually it probably makes more sense to disable the
	 * account for all UIs rather than the just the current UI,
	 * but it doesn't really matter.
	 */
	purple_account_set_enabled(account, FALSE);

	purple_notify_close_with_handle(account);
	purple_request_close_with_handle(account);

	purple_accounts_remove(account);

	/* Remove this account's buddies */
	for (gnode = purple_get_blist()->root; gnode != NULL; gnode = gnode->next) {
		if (!PURPLE_BLIST_NODE_IS_GROUP(gnode))
			continue;

		cnode = gnode->child;
		while (cnode) {
			PurpleBlistNode *cnode_next = cnode->next;

			if(PURPLE_BLIST_NODE_IS_CONTACT(cnode)) {
				bnode = cnode->child;
				while (bnode) {
					PurpleBlistNode *bnode_next = bnode->next;

					if (PURPLE_BLIST_NODE_IS_BUDDY(bnode)) {
						PurpleBuddy *b = (PurpleBuddy *)bnode;

						if (b->account == account)
							purple_blist_remove_buddy(b);
					}
					bnode = bnode_next;
				}
			} else if (PURPLE_BLIST_NODE_IS_CHAT(cnode)) {
				PurpleChat *c = (PurpleChat *)cnode;

				if (c->account == account)
					purple_blist_remove_chat(c);
			}
			cnode = cnode_next;
		}
	}

	/* Remove any open conversation for this account */
	for (iter = purple_get_conversations(); iter; ) {
		PurpleConversation *conv = iter->data;
		iter = iter->next;
		if (purple_conversation_get_account(conv) == account)
			purple_conversation_destroy(conv);
	}

	/* Remove this account's pounces */
	purple_pounce_destroy_all_by_account(account);

	/* This will cause the deletion of an old buddy icon. */
	purple_buddy_icons_set_account_icon(account, NULL, 0);

	purple_account_destroy(account);
}

void
purple_accounts_reorder(PurpleAccount *account, gint new_index)
{
	purple_account_manager_reorder_account(purple_account_manager_get(),
			account, new_index);
}

GList *
purple_accounts_get_all(void)
{
	return purple_account_manager_get_all_accounts(purple_account_manager_get());
}

GList *
purple_accounts_get_all_active(void)
{
	GList *list = NULL;
	GList *all = purple_accounts_get_all();

	while (all != NULL) {
		PurpleAccount *account = all->data;

		if (purple_account_get_enabled(account))
			list = g_list_append(list, account);

		all = all->next;
	}

	return list;
}

PurpleAccount *
purple_accounts_find(const char *name, const char *protocol_id)
{
	PurpleAccount *account = NULL;
	GList *l;
	char *who;

	g_return_val_if_fail(name != NULL, NULL);

	for (l = purple_accounts_get_all(); l != NULL; l = l->next) {
		account = (PurpleAccount *)l->data;

		who = g_strdup(purple_normalize(account, name));
		if (!strcmp(purple_normalize(account, purple_account_get_username(account)), who) &&
			(!protocol_id || !strcmp(purple_account_get_protocol_id(account), protocol_id))) {
			g_free(who);
			break;
		}
		g_free(who);
		account = NULL;
	}

	return account;
}

void
purple_accounts_restore_current_statuses()
{
	GList *l;
	PurpleAccount *account;

	/* If we're not connected to the Internet right now, we bail on this */
	if (!purple_network_is_available())
	{
		purple_debug_info("account", "Network not connected; skipping reconnect\n");
		return;
	}

	for (l = purple_accounts_get_all(); l != NULL; l = l->next)
	{
		account = (PurpleAccount *)l->data;
		if (purple_account_get_enabled(account) &&
			(purple_presence_is_online(account->presence)))
		{
			purple_account_connect(account);
		}
	}
}

void
purple_accounts_set_ui_ops(PurpleAccountUiOps *ops)
{
	account_ui_ops = ops;
}

PurpleAccountUiOps *
purple_accounts_get_ui_ops(void)
{
	return account_ui_ops;
}

void *
purple_accounts_get_handle(void)
{
	static int handle;

	return &handle;
}

void
purple_accounts_init(void)
{
	void *handle = purple_accounts_get_handle();

	purple_signal_register(handle, "account-connecting",
						 purple_marshal_VOID__POINTER, NULL, 1,
						 purple_value_new(PURPLE_TYPE_SUBTYPE,
										PURPLE_SUBTYPE_ACCOUNT));

	purple_signal_register(handle, "account-disabled",
						 purple_marshal_VOID__POINTER, NULL, 1,
						 purple_value_new(PURPLE_TYPE_SUBTYPE,
										PURPLE_SUBTYPE_ACCOUNT));

	purple_signal_register(handle, "account-enabled",
						 purple_marshal_VOID__POINTER, NULL, 1,
						 purple_value_new(PURPLE_TYPE_SUBTYPE,
										PURPLE_SUBTYPE_ACCOUNT));

	purple_signal_register(handle, "account-setting-info",
						 purple_marshal_VOID__POINTER_POINTER, NULL, 2,
						 purple_value_new(PURPLE_TYPE_SUBTYPE,
										PURPLE_SUBTYPE_ACCOUNT),
						 purple_value_new(PURPLE_TYPE_STRING));

	purple_signal_register(handle, "account-set-info",
						 purple_marshal_VOID__POINTER_POINTER, NULL, 2,
						 purple_value_new(PURPLE_TYPE_SUBTYPE,
										PURPLE_SUBTYPE_ACCOUNT),
						 purple_value_new(PURPLE_TYPE_STRING));

	purple_signal_register(handle, "account-status-changed",
						 purple_marshal_VOID__POINTER_POINTER_POINTER, NULL, 3,
						 purple_value_new(PURPLE_TYPE_SUBTYPE,
										PURPLE_SUBTYPE_ACCOUNT),
						 purple_value_new(PURPLE_TYPE_SUBTYPE,
										PURPLE_SUBTYPE_STATUS),
						 purple_value_new(PURPLE_TYPE_SUBTYPE,
										PURPLE_SUBTYPE_STATUS));

	purple_signal_register(handle, "account-authorization-requested",
						purple_marshal_INT__POINTER_POINTER,
						purple_value_new(PURPLE_TYPE_INT), 2,
						purple_value_new(PURPLE_TYPE_SUBTYPE,
										PURPLE_SUBTYPE_ACCOUNT),
						purple_value_new(PURPLE_TYPE_STRING));

	purple_signal_register(handle, "account-authorization-denied",
						purple_marshal_VOID__POINTER_POINTER, NULL, 2,
						purple_value_new(PURPLE_TYPE_SUBTYPE,
										PURPLE_SUBTYPE_ACCOUNT),
						purple_value_new(PURPLE_TYPE_STRING));

	purple_signal_register(handle, "account-authorization-granted",
						purple_marshal_VOID__POINTER_POINTER, NULL, 2,
						purple_value_new(PURPLE_TYPE_SUBTYPE,
										PURPLE_SUBTYPE_ACCOUNT),
						purple_value_new(PURPLE_TYPE_STRING));

	purple_signal_register(handle, "account-error-changed",
	                       purple_marshal_VOID__POINTER_POINTER_POINTER,
	                       NULL, 3,
	                       purple_value_new(PURPLE_TYPE_SUBTYPE,
	                                        PURPLE_SUBTYPE_ACCOUNT),
	                       purple_value_new(PURPLE_TYPE_POINTER),
	                       purple_value_new(PURPLE_TYPE_POINTER));

#if 0
	purple_signal_connect(conn_handle, "signed-on", handle,
	                      PURPLE_CALLBACK(signed_on_cb), NULL);
	purple_signal_connect(conn_handle, "connection-error", handle,
	                      PURPLE_CALLBACK(connection_error_cb), NULL);
#endif
}

void
purple_accounts_uninit(void)
{
	gpointer handle = purple_accounts_get_handle();

	purple_signals_disconnect_by_handle(handle);
	purple_signals_unregister_by_instance(handle);
}


