/*
 * Copyright (C) 2013 Henner Zeller
 *
 * This file is part of GMediaRender.
 *
 * GMediaRender is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * GMediaRender is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Library General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GMediaRender; if not, write to the Free Software 
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, 
 * MA 02110-1301, USA.
 *
 */ 

#include "variable-container.h"

#define _GNU_SOURCE
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdint.h>
#include <unistd.h>
#include <upnp/ithread.h>

#include "upnp_device.h"
#include "xmlescape.h"
#include "xmldoc.h"


//static ithread_t notify_thread;
static pthread_t notify_thread;
//static ithread_mutex_t  notify_mutex;
static pthread_mutex_t  notify_mutex;
//static ithread_cond_t  notify_cond;
static pthread_cond_t  notify_cond;

static int send_notifications = 0;
static upnp_var_change_collector_t *collectors = NULL;

static void *notification_thread_func(void * unused);

void variable_container_init(void)
{
	ithread_mutex_init(&notify_mutex, NULL);
	ithread_cond_init(&notify_cond, NULL);
	ithread_create(&notify_thread, NULL, notification_thread_func, NULL);
}

// -- VariableContainer
struct cb_list {
	variable_change_listener_t callback;
	void *userdata;
	struct cb_list *next;
};

struct variable_container {
	int variable_num;
	struct var_meta *variable_meta;
	char **values;
	ithread_mutex_t mutex;
	struct cb_list *callbacks;
};

variable_container_t *VariableContainer_new(int variable_num,
					    struct var_meta *variable_meta,
					    const char **variable_init_values) {
	assert(variable_num > 0);
	variable_container_t *result
		= (variable_container_t*)malloc(sizeof(variable_container_t));
	result->variable_num = variable_num;
	result->variable_meta = variable_meta;
	result->values = (char **) malloc(variable_num * sizeof(char*));
	result->callbacks = NULL;
	for (int i = 0; i < variable_num; ++i) {
		result->values[i] = strdup(variable_init_values[i]
					   ? variable_init_values[i]
					   : "");
	}
	ithread_mutex_init(&result->mutex, NULL);
	return result;
}

void VariableContainer_delete(variable_container_t *object) {
	for (int i = 0; i < object->variable_num; ++i) {
		free(object->values[i]);
	}
	free(object->values);

	for (struct cb_list *list = object->callbacks; list; /**/) {
		struct cb_list *next = list->next;
		free(list);
		list = next;
	}
	free(object);
}

// Get number of variables.
int VariableContainer_get_num_vars(variable_container_t *object) {
	return object->variable_num;
}

const char *VariableContainer_get(variable_container_t *object,
				  int var, const char **name, param_event *evented) {
	if (var < 0 || var >= object->variable_num)
		return NULL;
	const char *varname = object->variable_meta[var].name;
	if (name) *name = varname;
	if (evented)
		*evented = object->variable_meta[var].sendevents;
	// Names of not used variables are set to NULL.
	return varname ? object->values[var] : NULL;
}

// Change content of variable with given number to NUL terminated content.
int VariableContainer_change(variable_container_t *object,
			     int var_num, const char *value) {
	assert(var_num >= 0 && var_num < object->variable_num);
	if (value == NULL) value = "";
	if (strcmp(value, object->values[var_num]) == 0)
		return 0;  // no change.
	char *old_value = object->values[var_num];
	char *new_value = strdup(value);
	object->values[var_num] = new_value;
	for (struct cb_list *it = object->callbacks; it; it = it->next) {
		it->callback(it->userdata,
			     var_num, object->variable_meta[var_num].name,
			     old_value, new_value);
	}
	free(old_value);
	return 1;
}

void VariableContainer_register_callback(variable_container_t *object,
					 variable_change_listener_t callback,
					 void *userdata) {
	// Order is not guaranteed, so we just register it at the front.
	struct cb_list *item = (struct cb_list*) malloc(sizeof(struct cb_list));
	item->next = object->callbacks;
	item->userdata = userdata;
	item->callback = callback;
	object->callbacks = item;
}

void VariableContainer_lock(variable_container_t *object)
{
	ithread_mutex_lock(&object->mutex);
}

void VariableContainer_unlock(variable_container_t *object)
{
	ithread_mutex_unlock(&object->mutex);
}


// -- UPnPLastChangeBuilder
struct upnp_last_change_builder {
	const char *xml_namespace;
	struct xmldoc *change_event_doc;
	struct xmlelement *instance_element;
};

upnp_last_change_builder_t *UPnPLastChangeBuilder_new(const char *xml_namespace) {
	upnp_last_change_builder_t *result = (upnp_last_change_builder_t*)
		malloc(sizeof(upnp_last_change_builder_t));
	result->xml_namespace = xml_namespace;
	result->change_event_doc = NULL;
	result->instance_element = NULL;
	return result;
}

void UPnPLastChangeBuilder_delete(upnp_last_change_builder_t *builder) {
	if (builder->change_event_doc != NULL) {
		xmldoc_free(builder->change_event_doc);
	}
	free(builder);
}

void UPnPLastChangeBuilder_add(upnp_last_change_builder_t *builder,
			       const char *name, const char *value) {
	assert(name != NULL);
	assert(value != NULL);
	if (builder->change_event_doc == NULL) {
		builder->change_event_doc = xmldoc_new();
		struct xmlelement *toplevel =
			xmldoc_new_topelement(builder->change_event_doc, "Event",
				     builder->xml_namespace);
		// Right now, we only have exactly one instance.
		builder->instance_element =
			add_attributevalue_element(builder->change_event_doc,
						   toplevel,
						   "InstanceID", "val", "0");
	}
	
	struct xmlelement *xml_value;
	xml_value = add_attributevalue_element(builder->change_event_doc,
                 builder->instance_element,
                 name, "val", value);
	
	// HACK!
		// The volume related events need another qualifying
		// attribute that represents the channel. Since all other elements just
		// have one value to transmit without qualifier, the variable container
		// is oblivious about this notion of a qualifier.
		// So this is a bit ugly: if we see the variables in question,
		// we add the attribute manually.

	if (strcmp(name, "Volume") == 0
			|| strcmp(name, "VolumeDB") == 0
			|| strcmp(name, "Mute") == 0
			|| strcmp(name, "Loudness") == 0) {
	    xmlelement_set_attribute(builder->change_event_doc,
           xml_value, "channel", "Master");
	} 
}

char *UPnPLastChangeBuilder_to_xml(upnp_last_change_builder_t *builder) {
	if (builder->change_event_doc == NULL)
		return NULL;

	char *xml_doc_string = xmldoc_tostring(builder->change_event_doc);
	xmldoc_free(builder->change_event_doc);
	builder->change_event_doc = NULL;
	builder->instance_element = NULL;
	return xml_doc_string;
}

// -- UPnPVarChangeCollector
struct upnp_var_change_collector {
	variable_container_t *variable_container;
	int last_change_variable_num;      // the variable we manipulate.
	uint32_t changed_variables;  // bit mask for variables changed in current transaction
	struct upnp_device *upnp_device;
	const char *service_id;
	int open_transactions;
	upnp_last_change_builder_t *builder;
	struct upnp_var_change_collector *next;
};

static void UPnPVarChangeCollector_notify(upnp_var_change_collector_t *obj);
static void UPnPVarChangeCollector_callback(void *userdata,
					     int var_num, const char *var_name,
					     const char *old_value,
					     const char *new_value);

upnp_var_change_collector_t *
UPnPVarChangeCollector_new(variable_container_t *variable_container,
		const char *event_xml_namespace,
		struct upnp_device *upnp_device,
		const char *service_id) {
	upnp_var_change_collector_t *result = (upnp_var_change_collector_t*)
		malloc(sizeof(upnp_var_change_collector_t));
	result->variable_container = variable_container;
	result->last_change_variable_num = -1;
	result->changed_variables = 0;
	result->upnp_device = upnp_device;
	result->service_id = service_id;
	result->open_transactions = 0;
	result->builder = UPnPLastChangeBuilder_new(event_xml_namespace);

	// Create initial LastChange that contains all variables in their
	// current state. This might help devices that silently re-connect
	// without proper registration.
	// Also determine, which variable is actually the "LastChange" one.
	const int var_count = VariableContainer_get_num_vars(variable_container);
	assert(var_count < 32);  // otherwise widen changed_variables
	for (int i = 0; i < var_count; ++i) {
		const char *name;
		const char *value = VariableContainer_get(variable_container,
							  i, &name, NULL);
		if (!value) {
			continue;
		}
		if (strcmp("LastChange", name) == 0) {
			result->last_change_variable_num = i;
			continue;
		}
		if (variable_container->variable_meta[i].sendevents != SENDEVENT_LASTCHANGE) {
			continue;
		}
		result->changed_variables |= (1 << i);
	}

	result->next = collectors;
	collectors = result;

	UPnPVarChangeCollector_notify(result);

	VariableContainer_register_callback(variable_container,
					    UPnPVarChangeCollector_callback,
					    result);
	return result;
}

void UPnPVarChangeCollector_start(upnp_var_change_collector_t *object) {
	VariableContainer_lock(object->variable_container);
	object->open_transactions += 1;
	VariableContainer_unlock(object->variable_container);
}

void UPnPVarChangeCollector_finish(upnp_var_change_collector_t *object) {
	VariableContainer_lock(object->variable_container);
	assert(object->open_transactions >= 1);
	object->open_transactions -= 1;
	VariableContainer_unlock(object->variable_container);
	UPnPVarChangeCollector_notify(object);
}


static int UPnPVarChangeCollector_collect_all(upnp_var_change_collector_t *obj, const char ***varnames, const char ***varvalues)
{
	int i, j;
	int changed_count = 0;
	for (i = 0; i < obj->variable_container->variable_num; i++) {
		if (obj->changed_variables & (1 << i)) {
			changed_count++;
		}
	}
	if (!changed_count) {
		return 0;
	}
	*varnames = malloc(sizeof(char*) * (changed_count + 1));
	assert(*varnames != NULL);;
	*varvalues = malloc(sizeof(char*) * (changed_count + 1));
	assert(*varvalues != NULL);
	j = 0;
	for (i = 0; i < obj->variable_container->variable_num; i++) {
		if (!(obj->changed_variables & (1 << i)))
			continue;
		(*varnames)[j] = obj->variable_container->variable_meta[i].name;
		(*varvalues)[j] = xmlescape(obj->variable_container->values[i], 0);
		j++;
	}
	(*varnames)[changed_count] = NULL;
	(*varvalues)[changed_count] = NULL;
	return changed_count;
}

static int UPnPVarChangeCollector_collect_lastchange(upnp_var_change_collector_t *obj, const char ***varnames, const char ***varvalues)
{
	int i;
	for (i = 0; i < obj->variable_container->variable_num; i++) {
		if (i == obj->last_change_variable_num);
		if (obj->changed_variables & (1 << i)) {
			if (obj->variable_container->variable_meta[i].name != NULL) {
				UPnPLastChangeBuilder_add(
						obj->builder, 
						obj->variable_container->variable_meta[i].name, 
						obj->variable_container->values[i]);
			}
		}
	}
	char *xml_doc_string = UPnPLastChangeBuilder_to_xml(obj->builder);
	if (xml_doc_string == NULL) {
		return 0;
	}

	// Only if there is actually a change, send it over.
	if (VariableContainer_change(obj->variable_container,
				     obj->last_change_variable_num,
				     xml_doc_string)) {
		*varnames = malloc(sizeof(char*) * 2);
		*varvalues = malloc(sizeof(char*) * 2);
		(*varnames)[0] = "LastChange";
		(*varnames)[1] = NULL;
		(*varvalues)[0] = NULL;
		(*varvalues)[1] = NULL;
		// Yes, now, the whole XML document is encapsulated in
		// XML so needs to be XML quoted. The time around 2000 was
		// pretty sick - people did everything in XML.
		(*varvalues)[0] = xmlescape(xml_doc_string, 0);
	}

	free(xml_doc_string);
	return 1;
}


// TODO(hzeller): add rate limiting. The standard talks about some limited
// amount of events per time-unit.
static void UPnPVarChangeCollector_notify(upnp_var_change_collector_t *obj) {
	ithread_mutex_lock(&notify_mutex);
	if (obj->open_transactions != 0) {
		ithread_mutex_unlock(&notify_mutex);
		return;
	}

	if (obj->changed_variables) {
		send_notifications = 1;
		ithread_cond_signal(&notify_cond);
	}
	ithread_mutex_unlock(&notify_mutex);
}

static void *notification_thread_func(void * unused)
{
	for (;;) {
		ithread_mutex_lock(&notify_mutex);
		// send_notifications could be set by other threads 
		// while previous notifications were being sent.
		// If that is true, do not wait for condition
		if (!send_notifications)
			ithread_cond_wait(&notify_cond, &notify_mutex);
		// check for spurious wakeup
		if (!send_notifications) {
			ithread_mutex_unlock(&notify_mutex);
			continue;
		}
		send_notifications = 0;
		ithread_mutex_unlock(&notify_mutex);
		upnp_var_change_collector_t *collector = collectors;
		while (collector != NULL) {
			VariableContainer_lock(collector->variable_container);
			// if LastChange variable is present, send update in that variable.
			// Otherwise, use normal variable eventing.
			const char **varnames = NULL, **varvalues = NULL;
			int num_vars;
			if (collector->last_change_variable_num >= 0) {
				num_vars = UPnPVarChangeCollector_collect_lastchange(collector, &varnames, &varvalues);
			} else {
				num_vars = UPnPVarChangeCollector_collect_all(collector, &varnames, &varvalues);
			}
			collector->changed_variables = 0;
			VariableContainer_unlock(collector->variable_container);

			if (num_vars > 0) {
				upnp_device_notify(collector->upnp_device, collector->service_id, varnames, varvalues, num_vars);
				for (int i = 0; i < num_vars; i++) {
					free((char*)varvalues[i]);
				}
				free(varnames);
				free(varvalues);
			}

			collector = collector->next;
		}
		usleep(150000);
	}
	return NULL;
}

// The actual callback collecting changes by building an <Event/> XML document.
// This is not very robust if in the same transaction, we get the same variable
// changed twice -- it emits two changes.
static void UPnPVarChangeCollector_callback(void *userdata,
					     int var_num, const char *var_name,
					     const char *old_value,
					     const char *new_value) {
	upnp_var_change_collector_t *object = 
		(upnp_var_change_collector_t*) userdata;

	if (var_num == object->last_change_variable_num) {
		return;
	}
	if (object->last_change_variable_num >= 0 && object->variable_container->variable_meta[var_num].sendevents != SENDEVENT_LASTCHANGE) {
		return;  // ignore changes on non-eventable variables.
	}
	if (object->last_change_variable_num < 0 && object->variable_container->variable_meta[var_num].sendevents != SENDEVENT_YES) {
		return;  // ignore changes on non-eventable variables.
	}
	object->changed_variables |= (1 << var_num);
	UPnPVarChangeCollector_notify(object);
}
