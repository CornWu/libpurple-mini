#ifndef _PURPLE_MONO_LOADER_MONO_HELPER_H_
#define _PURPLE_MONO_LOADER_MONO_HELPER_H_

#include <mono/jit/jit.h>
#include <mono/metadata/object.h>
#include <mono/metadata/environment.h>
#include <mono/metadata/assembly.h>
#include <mono/metadata/debug-helpers.h>
#include <mono/metadata/tokentype.h>
#include "plugin.h"
#include "value.h"
#include "debug.h"

typedef struct {
	PurplePlugin *plugin;
	
	MonoAssembly *assm;
	MonoClass *klass;
	MonoObject *obj;	
	
	MonoMethod *init;
	MonoMethod *load;
	MonoMethod *unload;
	MonoMethod *destroy;
	
} PurpleMonoPlugin;

gboolean ml_init(void);

void ml_uninit(void);

MonoObject* ml_plugin_get_handle(MonoObject *plugin);

MonoObject* ml_invoke(MonoMethod *method, void *obj, void **params);

// DEPRECATED
MonoObject* ml_delegate_invoke(MonoObject *method, void **params);

MonoClass* ml_find_plugin_class(MonoImage *image);

gchar* ml_get_prop_string(MonoObject *obj, char *field);

void ml_set_prop_string(MonoObject *obj, char *field, char *data);

MonoObject* ml_get_info_prop(MonoObject *obj);

gboolean ml_is_api_dll(MonoImage *image);

MonoDomain* ml_get_domain(void);

void ml_set_domain(MonoDomain *d);

void ml_init_internal_calls(void);

// DEPRECATED
MonoObject* ml_object_from_purple_type(PurpleType type, gpointer data);

// DEPRECATED
MonoObject* ml_object_from_purple_subtype(PurpleSubType type, gpointer data);

// DEPRECATED
MonoObject* ml_create_api_object(char *class_name, void *object);

void ml_set_api_image(MonoImage *image);

MonoImage* ml_get_api_image(void);

// DEPRECATED
void ml_add_plugin(PurpleMonoPlugin *plugin);

// DEPRECATED
gboolean ml_remove_plugin(PurpleMonoPlugin *plugin);

// DEPRECATED
gpointer ml_find_plugin(PurpleMonoPlugin *plugin);

// DEPRECATED
gpointer ml_find_plugin_by_class(MonoClass *klass);

// DEPRECATED
GHashTable* ml_get_plugin_hash(void);

#endif
