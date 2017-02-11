#include <stdio.h>
#include "mosquitto_broker.h"

#define _mosquitto_malloc(x) malloc(x)
#define _mosquitto_free(x) free(x)

int custom_init(struct mqtt3_config *config, struct mosquitto_db *db)
{
	char *client_id = NULL;
	struct mosquitto *context;
	int ret;
	
	printf("**** Custom listener\n");
	
		config->listener_count++;
		config->listeners = realloc(config->listeners, sizeof(struct _mqtt3_listener)*config->listener_count);
		if(!config->listeners){
			mosquitto_log_printf(MOSQ_LOG_ERR, "Error: Out of memory.");
			return MOSQ_ERR_NOMEM;
		}
		memset(&config->listeners[config->listener_count-1], 0, sizeof(struct _mqtt3_listener));
		
		config->listeners[config->listener_count-1].protocol = mp_custom;
		
	client_id = "custom";
	
	context = calloc(1, sizeof(struct mosquitto));
	
	mosquitto_log_printf(MOSQ_LOG_NOTICE, "Custom client connected as id '%s'.", client_id);
	
	context->id = client_id;
	HASH_ADD_KEYPTR(hh_id, db->contexts_by_id, context->id, strlen(context->id), context);
	context->state = mosq_cs_connected;
	
	ret = mqtt3_sub_add(db, context, "test", 0, &db->subs);
	printf("Subscribing 'test' (%d)\n", ret);
	return 0;
}

