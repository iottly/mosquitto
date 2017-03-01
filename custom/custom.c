#include "mosquitto_broker.h"
#include "mqtt3_protocol.h"
#include "http.h"
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
  unsigned char buf[1024], *message;
  int fd, n, i, ibuf, ibase;
  int qos, mid, len, plen;
  fd_set fds;
  char *topic;
  
  ibuf = 0;
  
  fd = *((int*)sock);
  FD_ZERO(&fds);
  while(1)
  {
    FD_SET(fd, &fds);
    n = select(fd+1, &fds, NULL, NULL, NULL);
    
    n = read(fd, buf+ibuf, sizeof(buf)-ibuf);
    //printf("read: %d\n", n);
    if(n <= 0)
    {
      printf("**** ERRORE - fine loop custom ****\n");
      return NULL;
    }
    
    //for(i=0; i<n; i++) printf("%02x", buf[i+ibuf]);
    //printf("\n");
    
    ibuf += n;
    
    while((ibuf > 1) && (ibuf >= (buf[1]+2)))
    {
      if((buf[0] & 0xF0) == PUBLISH)
      {
        /* QoS */
        qos = (buf[0] & 0x06) >> 1;
        
        /* Topic */
        len = (buf[2]<<8) + buf[3];
        topic = malloc(len+1);
        memcpy(topic, buf+4, len);
        topic[len] = 0;
        
        /* Message ID */
        mid = 0;
        ibase = len+4;
        if(qos > 0)
        {
          mid = (buf[len+4]<<8) + buf[len+5];
          ibase += 2;
        }
        
        /* Message */
        plen = buf[1] - ibase + 2;
        message = malloc(plen);
        memcpy(message, buf+ibase, plen);
        
        /* Stampa dei dati ricevuti -- DA SOSTITUIRE CON CODICE REALE */
        /* Nota: il QoS ricevuto è sempre 0, anche se il messaggio di origine aveva QoS maggiori */
        printf("** PUBLISH QoS=%d (MID=%d) topic='%s' plen=%d\n** payload=", qos, mid, topic, plen);
        for(i=0; i<plen; i++) printf("%02x", message[i]);
        printf("\n");
        
        free(topic);
        free(message);
        
        switch(qos)
        {
          case 1:
#warning Qualcosa da rivedere
            /* La riposta è corretta ma a mosquitto non piace, verificare la registrazione dei topic */
            buf[0] = PUBACK | (1<<1);
            buf[1] = 2;
            buf[2] = mid>>8;
            buf[3] = mid;
            write(fd, buf, 4);
            break;
          case 2:
            /* Da gestire, ma per ora i topic li registro massimo QoS 1 */
            break;
          default:
            break;
        }
        
        /* if(qos == 2) _mosquitto_send_pubrec(context, mid);
             ... che a sua volta riceve PUBREL che richiede il PUBCOMP.
        */
      }
      
      ibuf -= buf[1]+2;
      memcpy(buf, buf+buf[1]+2, buf[1]+2);
    }
  }
}

int custom_init(struct mqtt3_config *config, struct mosquitto_db *db)
{
	char *client_id = NULL;
	struct mosquitto *context;
	int i, ret, sock[2], *s;
	pthread_t p;
	
	struct http_message msg;
	memset(&msg, 0, sizeof(struct http_message));
	
	i = http_post(/*"http://requestb.in/147z7ab1"*/ config->post_url, "------multi----\r\n\
Content-Disposition: form-data; name=\"from\"\r\n\r\npippo\r\n\
------multi----\r\n\
Content-Disposition: form-data; name=\"to\"\r\n\r\npluto\r\n\
------multi------\r\n");

	while(http_response(i, &msg) > 0);
	printf("Code: %d\n", msg.header.code);

#if 0
	printf("**** Custom listener\n");
	
	config->listener_count++;
	config->listeners = realloc(config->listeners, sizeof(struct _mqtt3_listener)*config->listener_count);
	if(!config->listeners){
		mosquitto_log_printf(MOSQ_LOG_ERR, "Error: Out of memory.");
		return MOSQ_ERR_NOMEM;
	}
	memset(&config->listeners[config->listener_count-1], 0, sizeof(struct _mqtt3_listener));
	
	config->listeners[config->listener_count-1].protocol = mp_custom;
#endif
	
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
	
	for(i=0; i<config->post_topic_num; i++)
	{
		ret = mqtt3_sub_add(db, context, config->post_topic[i], config->post_topic_qos[i], &db->subs);
		printf("Subscribing '%s' QoS=%d (%d)\n", config->post_topic[i], config->post_topic_qos[i], ret);
	}
	return 0;
}

