#include "internal.h"

#include "debug.h"
#include "log.h"
#include "plugin.h"
#include "version.h"
#include <dbi/dbi.h>

#ifdef _WIN32
#include <glib.h>
#endif

typedef struct {
	PurpleLogLogger *logger;
	dbi_conn db_conn;
	gboolean conn_established;
	guint db_callback_id;
} DatabaseLoggerInfo;

typedef struct {
	int id;
} ConversationInfo;

typedef enum {
	PURPLE_DATABASE_LOGGER_INIT,
	PURPLE_DATABASE_LOGGER_READ,
	PURPLE_DATABASE_LOGGER_WRITE,
	PURPLE_DATABASE_LOGGER_LIST,
	PURPLE_DATABASE_LOGGER_SETS,
	PURPLE_DATABASE_LOGGER_LIST_SYSLOG,
	PURPLE_DATABASE_LOGGER_SIZE,
	PURPLE_DATABASE_LOGGER_TOTAL_SIZE,
	PURPLE_DATABASE_LOGGER_REMOVE_LOG,
	PURPLE_DATABASE_LOGGER_CLOSE,
} DatabaseOperationType;

#define DATABASE_OPERATION_COUNT 10
#define SYSTEM_LOG_BUDDY_NAME "--SYSTEM-LOG--"
#define CHAT_BUDDY_NAME_TEMPLATE "--%s-CHAT--"

#define BUDDY_IM_TYPE 		0
#define BUDDY_CHAT_TYPE 	1
#define BUDDY_SYSTEM_TYPE 	2
#define BUDDY_ME_TYPE		3

typedef struct  {
	DatabaseOperationType type;
} DatabaseOperation;

typedef struct {
	DatabaseOperationType type;
	PurpleLog * log;
	PurpleMessageFlags flags;
	const char *from;
	time_t time;
	const char *message;
	PurpleLogSizeCallback cb;
	void *data;
	int ret_value;
} DatabaseWriteOperation;

typedef struct {
	DatabaseOperationType type;
	PurpleLog * log;
	PurpleLogReadFlags *flags;
	PurpleLogReadCallback cb;
	void *data;
	char *ret_value;
} DatabaseReadOperation;

typedef struct {
	DatabaseOperationType type;
	PurpleLogType log_type;
	const char *sn;
	PurpleAccount *account;
	PurpleLogListCallback cb;
	void *data;
	GList *ret_value;
} DatabaseListOperation;

typedef struct {
	DatabaseOperationType type;
	PurpleAccount *account;
	PurpleLogListCallback cb;
	void *data;
	GList *ret_value;
} DatabaseSyslogListOperation;

typedef struct {
	DatabaseOperationType type;
	PurpleLog *log;
	PurpleLogSizeCallback cb;
	void *data;
	int ret_value;
} DatabaseSizeOperation;

typedef struct {
	DatabaseOperationType type;
	PurpleLogType log_type;
	const char *name;
	PurpleAccount *account;
	PurpleLogSizeCallback cb;
	void *data;
	int ret_value;
} DatabaseTotalSizeOperation;

typedef struct {
	DatabaseOperationType type;
	GHashTable *sets;
	PurpleLogVoidCallback cb;
	void *data;
	GHashTable *ret_value;
} DatabaseSetsOperation;

typedef struct {
	DatabaseOperationType type;
	PurpleLog *log;
	PurpleLogBooleanCallback cb;
	void *data;
	gboolean ret_value;
} DatabaseRemoveLogOperation;

static DatabaseLoggerInfo *db_logger = NULL;

static gpointer (*db_thread_func[DATABASE_OPERATION_COUNT])(gpointer data);
static void (*db_notify_func[DATABASE_OPERATION_COUNT])(gpointer data);

/* List of finished in thread operations, such as getting total size, reading or writing */
static GList *db_finished_op = NULL;
static GMutex *db_mutex = NULL;

static gpointer db_thread(gpointer data);

static void database_logger_write(PurpleLog *log, PurpleMessageFlags type,
							  const char *from, time_t time, const char *message,
							  PurpleLogSizeCallback cb, void *data)
{
	purple_debug_info("Database Logger", "Write function\n");
	if (db_thread_func[PURPLE_DATABASE_LOGGER_WRITE] != NULL) {
		DatabaseWriteOperation *op = g_new(DatabaseWriteOperation, 1);
		op->type = PURPLE_DATABASE_LOGGER_WRITE;
		op->log = log;
		op->flags = type;
		op->from = from;
		op->time = time;
		op->message = message;
		op->cb = cb;
		op->data = data;
		op->ret_value = 0;
		g_thread_create(db_thread, op, FALSE, NULL);
	} else {
		cb(0, data);
	}
}

static void database_logger_read(PurpleLog *log, PurpleLogReadFlags *flags,
							PurpleLogReadCallback cb, void *data)
{
	purple_debug_info("Database Logger", "Read function\n");
	if (db_thread_func[PURPLE_DATABASE_LOGGER_READ] != NULL) {
		DatabaseReadOperation *op = g_new(DatabaseReadOperation, 1);
		op->type = PURPLE_DATABASE_LOGGER_READ;
		op->log = log;
		op->flags = flags;
		op->cb = cb;
		op->data = data;
		op->ret_value = NULL;
		g_thread_create(db_thread, op, FALSE, NULL);
	} else {
		cb(NULL, flags, data);
	}
}

static void database_logger_list(PurpleLogType type, const char *sn, PurpleAccount *account, 
								PurpleLogListCallback cb, void *data)
{
	purple_debug_info("Database Logger", "List function\n");
	if (db_thread_func[PURPLE_DATABASE_LOGGER_LIST] != NULL) {
		DatabaseListOperation *op = g_new(DatabaseListOperation, 1);
		op->type = PURPLE_DATABASE_LOGGER_LIST;
		op->log_type = type;
		op->sn = sn;
		op->account = account;
		op->cb = cb;
		op->data = data;
		op->ret_value = NULL;
		g_thread_create(db_thread, op, FALSE, NULL);
	} else {
		cb(NULL, data);
	}
}

static void database_logger_size(PurpleLog *log, PurpleLogSizeCallback cb, void *data)
{
	purple_debug_info("Database Logger", "Size function\n");
	if (db_thread_func[PURPLE_DATABASE_LOGGER_SIZE] != NULL) {
	
	} else {
		cb(0, data);
	}
}

static void database_logger_total_size(PurpleLogType type, const char *name, PurpleAccount *account,
									PurpleLogSizeCallback cb, void *data)
{
	purple_debug_info("Database Logger", "Total size function\n");
	if (db_thread_func[PURPLE_DATABASE_LOGGER_TOTAL_SIZE] != NULL) {
	
	} else {
		cb(0, data);
	}
}

static void database_logger_sets(GHashTable *sets, PurpleLogVoidCallback cb, void *data)
{
	purple_debug_info("Database Logger", "Sets function\n");
	if (db_thread_func[PURPLE_DATABASE_LOGGER_SETS] != NULL) {
	
	} else {
		cb(data);
	}
}

static void database_logger_list_syslog(PurpleAccount *account, PurpleLogListCallback cb, void *data)
{
	purple_debug_info("Database Logger", "List syslog function\n");
	if (db_thread_func[PURPLE_DATABASE_LOGGER_LIST_SYSLOG] != NULL) {
	
	} else {
		cb(NULL, data);
	}
}

static void database_logger_remove_log(PurpleLog *log, PurpleLogBooleanCallback cb, void *data)
{
	purple_debug_info("Database Logger", "Remove log function\n");
	if (db_thread_func[PURPLE_DATABASE_LOGGER_REMOVE_LOG] != NULL) {
	
	} else {
		cb(FALSE, data);
	}
}

/****************************************************************************
* Notify operation which let you know that datbase operation has finished.
****************************************************************************/

static void db_write_notify(gpointer data) 
{
	DatabaseWriteOperation *op = data;
	if (op->cb != NULL) {
		op->cb(op->ret_value, op->data);
	}
}

static void db_read_notify(gpointer data) 
{
	DatabaseReadOperation *op = data;
	if (op->cb != NULL) {
		op->cb(op->ret_value, op->flags, op->data);
	}
}

static void db_list_notify(gpointer data) 
{
	DatabaseListOperation *op = data;
	if (op->cb != NULL) {
		op->cb(op->ret_value, op->data);
	}
}

static void db_syslog_list_notify(gpointer data) 
{
	DatabaseSyslogListOperation *op = data;
	if (op->cb != NULL) {
		op->cb(op->ret_value, op->data);
	}
}

static void db_size_notify(gpointer data) 
{
	DatabaseSizeOperation *op = data;
	if (op->cb != NULL) {
		op->cb(op->ret_value, op->data);
	}
}

static void db_sets_notify(gpointer data) 
{
	DatabaseSetsOperation *op = data;
	if (op->cb != NULL) {
		op->cb(op->data);
	}
}

static void db_total_size_notify(gpointer data) 
{
	DatabaseTotalSizeOperation *op = data;
	if (op->cb != NULL) {
		op->cb(op->ret_value, op->data);
	}
}

static void db_remove_notify(gpointer data) 
{
	DatabaseRemoveLogOperation *op = data;
	if (op->cb != NULL) {
		op->cb(op->ret_value, op->data);
	}
}
/****************************************************************************
* 						UTILS FUNCTIONS 
****************************************************************************/

static void lock()
{
	g_mutex_lock(db_mutex);
}

static void unlock()
{
	g_mutex_unlock(db_mutex);
}

static int dbi_retrieve_id(dbi_result dres, int *id) 
{
	if (dres) {
		/* TODO: check if there are several rows */
		while (dbi_result_next_row(dres)) {
			*id = dbi_result_get_uint(dres, "id_name");
		}
		dbi_result_free(dres);
		return 0;
	} 
	return -1;
}

static int get_protocol_id(PurpleAccount *account) 
{
	PurplePlugin *prpl = purple_find_prpl(purple_account_get_protocol_id(account));
	PurplePluginProtocolInfo *prpl_info = PURPLE_PLUGIN_PROTOCOL_INFO(prpl);
	const char *protocol_name = prpl_info->list_icon(account, NULL);
	int id = -1;

	/* TODO: optimization */

	if (prpl == NULL) 
		return -1;

	/* we can use dbi_conn_sequence_last to get row ID generated by the last INSERT command
	    when we add an account, but it seems that the result depends on database */
	while (id == -1) {
		dbi_result dres = dbi_conn_queryf(db_logger->db_conn, 
						"SELECT id FROM protocols WHERE name = \"%s\"", protocol_name);
		if (dbi_retrieve_id(dres, &id) == -1) 
			break;

		/*if id not found we should insert new protocol */
		if (id == -1) {
			dres = dbi_conn_queryf(db_logger->db_conn, "INSERT INTO protocols(`name`) VALUES(\"%s\")", 
								protocol_name);
			if (dres)
				dbi_result_free(dres);
			else 
				break;
		}
	}

	return id;
}

static int get_account_id(PurpleAccount *account) 
{
	char *account_name = g_strdup(purple_escape_filename(purple_normalize(account,
				purple_account_get_username(account))));
	int id = -1;

	/* TODO: optimization */

	/* we can use dbi_conn_sequence_last to get row ID generated by the last INSERT command
	    when we add an account, but it seems that the result depends on database */
	while (id == -1) {
		dbi_result dres = dbi_conn_queryf(db_logger->db_conn, "SELECT id FROM accounts WHERE name = \"%s\"", account_name);

		if (dbi_retrieve_id(dres, &id) == -1) 
			break;

		/*if id not found we should insert new account */
		if (id == -1) {
			dres = dbi_conn_queryf(db_logger->db_conn, "INSERT INTO accounts(`name`, `protocolId`) VALUES(\"%s\", %i)", 
								account_name, get_protocol_id(account));
			if (dres)
				dbi_result_free(dres);
			else
				break;
		}
	}

	g_free(account_name);
	return id;
}

static int get_buddy_id(PurpleLog *log) 
{
	char *buddy_name = NULL;
	int id = -1;
	int buddy_type = 0;
	PurpleAccount *account = log->account;

	if (log->type == PURPLE_LOG_SYSTEM) {
		buddy_name = g_strdup(SYSTEM_LOG_BUDDY_NAME);
		buddy_type = BUDDY_SYSTEM_TYPE;
	} else if (log->type == PURPLE_LOG_CHAT) {
		buddy_name = g_strdup_printf(CHAT_BUDDY_NAME_TEMPLATE, purple_normalize(account, log->name));
		buddy_type = BUDDY_CHAT_TYPE;
	} else {
		buddy_name = g_strdup(purple_normalize(account, log->name));
		buddy_type = BUDDY_IM_TYPE;
	}

	/* TODO: optimization */

	/* we can use dbi_conn_sequence_last to get row ID generated by the last INSERT command
	    when we add an account, but it seems that the result depends on database */
	while (id == -1) {
		dbi_result dres = dbi_conn_queryf(db_logger->db_conn, "SELECT id FROM buddies WHERE name = \"%s\" AND type =%i AND accountID = %i ", 
									buddy_name, buddy_type, get_account_id(account));
		if (dbi_retrieve_id(dres, &id) == -1) 
			break;

		/*if id not found we should insert new account */
		if (id == -1) {
			dres = dbi_conn_queryf(db_logger->db_conn, "INSERT INTO buddies(`name`, `type`, `accountId`) VALUES(\"%s\", %i, %i)", 
								buddy_name, buddy_type, get_account_id(account));
			if (dres)
				dbi_result_free(dres);
			else
				break;
		}
	}

	g_free(buddy_name);
	return id;
}


/****************************************************************************
* Main database functions which perform in thread
****************************************************************************/
/**
 * Try to connect to database and create database logger
 */
static gpointer db_init(gpointer data) 
{
	if (dbi_conn_connect(db_logger->db_conn) < 0) {
		purple_debug_info("Database Logger", "Could not connect. Please check the option settings\n");
	} else {

		/* TODO: check where table has proper infrastructure */

		lock();
		db_logger->logger = purple_log_logger_new("database", _("Database Logger"), 16,
										   NULL,
										   NULL,
										   NULL,
										   NULL,
										   NULL,
										   NULL,
										   NULL,
										   NULL,
										   NULL,
										   NULL,
										   NULL,
										   NULL,
										   database_logger_write,
										   NULL,
										   database_logger_list,
										   database_logger_read,
										   database_logger_size,
										   database_logger_total_size,
										   database_logger_list_syslog,
										   database_logger_sets,
										   database_logger_remove_log);
		db_logger->conn_established = TRUE;

		purple_debug_info("Database Logger", "Mutex UNlocked in db_init\n");
		unlock();

		purple_log_logger_add(db_logger->logger);
	}

	return NULL;
}

static gpointer db_write(gpointer data)
{
	DatabaseWriteOperation *op = data;
	PurpleLog *log = op->log;
	PurpleMessageFlags flags = op->flags;
	const char *from = op->from;
	time_t time = op->time;
	const char *message = op->message;
	ConversationInfo *conv_info = NULL;
	dbi_result dres;


	lock();

	op->ret_value = 0;

	if (log->logger_data == NULL) {
		log->logger_data = conv_info = g_new(ConversationInfo, 1);
		conv_info->id = -1;

		/* create new conversation in log */
		dres = dbi_conn_queryf(db_logger->db_conn, 
			"INSERT INTO conversations(`datetime`, `size`, `accountId`, `buddyId`) VALUES(%i, %i, %i, %i )", 
			time, 0, get_account_id(log->account), get_buddy_id(log));
		if (dres) 
			dbi_result_free(dres);

		/* we can use dbi_conn_sequence_last to get row ID generated by the last INSERT command
		when we add an account, but it seems that the result depends on database, because
		some databases need exlicit sequences*/
		dres = dbi_conn_queryf(db_logger->db_conn, 
			"SELECT id FROM conversations WHERE datetime = %i AND accountID = %i AND buddyId = %i", 
									time, get_account_id(log->account), get_buddy_id(log));
		dbi_retrieve_id(dres, &conv_info->id);
	}

	if (conv_info->id == -1) {
		unlock();
		return NULL;
	}

	dres = dbi_conn_queryf(db_logger->db_conn, 
			"INSERT INTO messages (`conversationId`, `ownerName`, `datetime`, `text`, `flags`) VALUES(%i, \"%s\", %i, \"%s\", %i)",
			conv_info->id, from, time, message, flags);

	unlock();

	return NULL;
}

static gpointer db_read(gpointer data)
{
	//DatabaseReadOperation *op = data;

	return NULL;
}

static gpointer db_list(gpointer data)
{
	return NULL;
}

static gpointer db_syslog_list(gpointer data)
{
	return NULL;
}

static gpointer db_size(gpointer data)
{
	return NULL;
}

static gpointer db_total_size(gpointer data)
{
	return NULL;
}

static gpointer db_sets(gpointer data)
{
	return NULL;
}

static gpointer db_remove(gpointer data)
{
	return NULL;
}
/**
 * Thread function, from which we do all operation with DB
 */
static gpointer db_thread(gpointer data) 
{
	DatabaseOperation *op = data;
	gpointer return_val = NULL;

	int id = op->type;
	return_val = db_thread_func[id](data);

	/* Locking mutex, because we are going to add new 
	   item to db_funished_op 
	 */
	lock();
	db_finished_op = g_list_append(db_finished_op, op);
	unlock();

	return return_val;
}

/**
 * This is a callback from main thread
 * if connection is established and we have finished in thread operations
 * then we notify about this sender
 */
static gboolean db_main_callback(gpointer data)
{
	if (!db_logger->conn_established || !db_finished_op)
		return TRUE;

	purple_debug_info("Database Logger", "Main callback: there is finished operation\n");

	lock();
	for(; db_finished_op != NULL; db_finished_op = g_list_delete_link(db_finished_op, db_finished_op)) {
		DatabaseOperation *op = db_finished_op->data;
		if (db_notify_func[op->type] != NULL)
			db_notify_func[op->type](op);
		g_free(op);
	}
	unlock();

	return TRUE;
}

static gboolean
plugin_load(PurplePlugin *plugin)
{
	int cnt = 0;

	db_logger = g_new0(DatabaseLoggerInfo, 1);
	g_return_val_if_fail(plugin != NULL, FALSE);

	db_mutex = g_mutex_new();

#ifdef _WIN32
	cnt = dbi_initialize("libdbi-drivers");
#else
	cnt = dbi_initialize(NULL);
#endif
	purple_debug_info("Database Logger", "Count of loaded drivers =  %i\n", cnt);

	db_logger->db_conn = dbi_conn_new("mysql");
	if (db_logger->db_conn == NULL) {
		purple_debug_info("Database Logger", "created connection is NULL\n");
		return FALSE;
	}

	/*TODO: make customizable for several DB type*/
	dbi_conn_set_option(db_logger->db_conn, "host", "192.168.0.128");
	dbi_conn_set_option(db_logger->db_conn, "username", "pidgin");
	dbi_conn_set_option(db_logger->db_conn, "password", "pidgin");
	dbi_conn_set_option(db_logger->db_conn, "dbname", "pidgin");
	dbi_conn_set_option(db_logger->db_conn, "encoding", "UTF-8");

	db_logger->db_callback_id = purple_timeout_add(100, db_main_callback, NULL);

	purple_debug_info("Database Logger", "Creating thread and making init operation\n");
	if (db_thread_func[PURPLE_DATABASE_LOGGER_INIT] != NULL) {
		DatabaseOperation *op = g_new(DatabaseOperation, 1);
		op->type = PURPLE_DATABASE_LOGGER_INIT;
		g_thread_create(db_thread, op, FALSE, NULL);
	} 

	return TRUE;
}

static gboolean
plugin_unload(PurplePlugin *plugin)
{
	purple_debug_info("Database Logger", "plugin_unload\n");
	g_return_val_if_fail(plugin != NULL, FALSE);

	if (db_logger != NULL)
		dbi_conn_close(db_logger->db_conn);

	dbi_shutdown();

	g_mutex_free(db_mutex);

	purple_timeout_remove(db_logger->db_callback_id);
	purple_log_logger_remove(db_logger->logger);
	g_free(db_logger);

	return TRUE;
}

static void                        
init_plugin(PurplePlugin *plugin)
{
	db_thread_func[PURPLE_DATABASE_LOGGER_INIT] = db_init;
	db_notify_func[PURPLE_DATABASE_LOGGER_INIT] = NULL;

	db_thread_func[PURPLE_DATABASE_LOGGER_WRITE] = db_write;
	db_notify_func[PURPLE_DATABASE_LOGGER_WRITE] = db_write_notify;

	db_thread_func[PURPLE_DATABASE_LOGGER_READ] = db_read;
	db_notify_func[PURPLE_DATABASE_LOGGER_READ] = db_read_notify;

	db_thread_func[PURPLE_DATABASE_LOGGER_LIST] = db_list;
	db_notify_func[PURPLE_DATABASE_LOGGER_LIST] = db_list_notify;

	db_thread_func[PURPLE_DATABASE_LOGGER_LIST_SYSLOG] = db_syslog_list;
	db_notify_func[PURPLE_DATABASE_LOGGER_LIST_SYSLOG] = db_syslog_list_notify;

	db_thread_func[PURPLE_DATABASE_LOGGER_SIZE] = db_size;
	db_notify_func[PURPLE_DATABASE_LOGGER_SIZE] = db_size_notify;

	db_thread_func[PURPLE_DATABASE_LOGGER_TOTAL_SIZE] = db_total_size;
	db_notify_func[PURPLE_DATABASE_LOGGER_TOTAL_SIZE] = db_total_size_notify;

	db_thread_func[PURPLE_DATABASE_LOGGER_SETS] = db_sets;
	db_notify_func[PURPLE_DATABASE_LOGGER_SETS] = db_sets_notify;

	db_thread_func[PURPLE_DATABASE_LOGGER_REMOVE_LOG] = db_remove;
	db_notify_func[PURPLE_DATABASE_LOGGER_REMOVE_LOG] = db_remove_notify;
}

static PurplePluginInfo info =
{
	PURPLE_PLUGIN_MAGIC,
	PURPLE_MAJOR_VERSION,
	PURPLE_MINOR_VERSION,
	PURPLE_PLUGIN_STANDARD,                             /**< type           */
	NULL,                                             /**< ui_requirement */
	0,                                                /**< flags          */
	NULL,                                             /**< dependencies   */
	PURPLE_PRIORITY_DEFAULT,                            /**< priority       */
	"core-database_logger",                                /**< id             */
	N_("Database Logger"),                                 /**< name           */
	VERSION,                                          /**< version        */

	/** summary */
	N_("Store your logs in database"),

	/** description */
	N_("Database Logger allows you to"
	   "store logs in database"),

	"Michael Shkutkov <mish@rsu.ru>",             /**< author         */
	PURPLE_WEBSITE,                                     /**< homepage       */
	plugin_load,                                      /**< load           */
	plugin_unload,                                    /**< unload         */
	NULL,                                             /**< destroy        */
	NULL,                                             /**< ui_info        */
	NULL,                                             /**< extra_info     */
	NULL,                                      /**< prefs_info     */
	NULL,                                             /**< actions        */

	/* padding */
	NULL,
	NULL,
	NULL,
	NULL
};

PURPLE_INIT_PLUGIN(database_logger, init_plugin, info)