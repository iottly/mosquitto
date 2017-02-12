#include "mosquitto_broker.h"
/* Viene incluso "dummypthread.h", ma devo usare la libreria reale */
#undef pthread_create 
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <pthread.h>
#include <sys/select.h>

#define _mosquitto_malloc(x) malloc(x)
#define _mosquitto_free(x) free(x)

void* custom_loop(void *sock)
{
  unsigned char buf[1024];
  int fd, n;
  fd_set fds;

  fd = *((int*)sock);
  FD_ZERO(&fds);
  while(1)
  {
    FD_SET(fd, &fds);
    n = select(fd+1, &fds, NULL, NULL, NULL);
    
    n = read(fd, buf, sizeof(buf));
    printf("read: %d\n", n);
    if(n <= 0) return NULL;
  }
}

int custom_init(struct mqtt3_config *config, struct mosquitto_db *db)
{
	char *client_id = NULL;
	struct mosquitto *context;
	int ret, sock[2], *s;
	pthread_t p;
	
	printf("**** Custom listener\n");
	
	config->listener_count++;
	config->listeners = realloc(config->listeners, sizeof(struct _mqtt3_listener)*config->listener_count);
	if(!config->listeners){
		mosquitto_log_printf(MOSQ_LOG_ERR, "Error: Out of memory.");
		return MOSQ_ERR_NOMEM;
	}
	memset(&config->listeners[config->listener_count-1], 0, sizeof(struct _mqtt3_listener));
	
	config->listeners[config->listener_count-1].protocol = mp_custom;
	
	client_id = strdup("custom");
	
	context = calloc(1, sizeof(struct mosquitto));
	
	mosquitto_log_printf(MOSQ_LOG_NOTICE, "Custom client connected as id '%s'.", client_id);
	
	socketpair(AF_LOCAL, SOCK_STREAM, 0, sock);
	s = malloc(sizeof(int));
	*s = sock[0];
	pthread_create(&p, NULL, custom_loop, s);
	pthread_detach(p);
	
	context->sock = sock[1];
	context->id = client_id;
	
	HASH_ADD_KEYPTR(hh_id, db->contexts_by_id, context->id, strlen(context->id), context);
	HASH_ADD(hh_sock, db->contexts_by_sock, sock, sizeof(context->sock), context);
	context->state = mosq_cs_connected;
	
	ret = mqtt3_sub_add(db, context, "test", 0, &db->subs);
	printf("Subscribing 'test' (%d)\n", ret);
	return 0;
}

