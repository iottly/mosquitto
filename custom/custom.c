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

struct custom_data {
  struct mqtt3_config *config;
  int sock;
};

struct msglist {
  char *topic;
  char *value;
  struct msglist *next;
} *msg_head, *msg_tail;

void* custom_loop(void *data)
{
  struct custom_data *cdata = data;
  unsigned char buf[1024], *message;
  int fd, fdhttp, fdmax, n, i, ibuf, ibase;
  int qos, mid, len, plen;
  fd_set fds;
  char *topic;
  struct msglist *tmsg, *cmsg;
  char *ptopic[3];
  char *pvalue[3];
  struct http_message hmsg;
	
  ibuf = 0;
  cmsg = NULL;
  
  fd = cdata->sock;
  FD_ZERO(&fds);
  while(1)
  {
    if(!cmsg && msg_head)
    {
      cmsg = msg_head;
      msg_head = msg_head->next;
      if(!msg_head) msg_tail = NULL;
      
      memset(&hmsg, 0, sizeof(struct http_message));
      ptopic[0] = "from";
      pvalue[0] = cmsg->topic;
      ptopic[1] = "to";
      pvalue[1] = cdata->config->post_dest;
      ptopic[2] = "msg";
      pvalue[2] = malloc(strlen(cdata->config->post_header)+strlen(cmsg->value)+2);
      strcpy(pvalue[2], cdata->config->post_header);
      strcat(pvalue[2], " ");
      strcat(pvalue[2], cmsg->value);
      fdhttp = http_post(cdata->config->post_url, 3, ptopic, pvalue);
      free(pvalue[2]);
      free(cmsg->topic);
      free(cmsg->value);
    }
    
    fdmax = fd;
    FD_SET(fd, &fds);
    if(fdhttp >= 0)
    {
      FD_SET(fdhttp, &fds);
      if(fdhttp > fdmax) fdmax = fdhttp;
    }
    printf("WAITING FOR DATA\n");
    n = select(fdmax+1, &fds, NULL, NULL, NULL);
    printf("RECEIVED DATA: %d\n", n);

    if(FD_ISSET(fd, &fds))
    {
      n = read(fd, buf+ibuf, sizeof(buf)-ibuf);
      printf("read: %d\n", n);
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
          message = malloc(plen+1);
          memcpy(message, buf+ibase, plen);
          message[plen] = 0;
          
          /* Stampa dei dati ricevuti -- DA SOSTITUIRE CON CODICE REALE */
          /* Nota: il QoS ricevuto è sempre 0, anche se il messaggio di origine aveva QoS maggiori */
          printf("** PUBLISH QoS=%d (MID=%d) topic='%s' plen=%d\n** payload=", qos, mid, topic, plen);
          for(i=0; i<plen; i++) printf("%02x", message[i]);
          printf("\n");
          
          tmsg = malloc(sizeof(struct msglist));
          if(tmsg)
          {
            tmsg->topic = topic;
            tmsg->value = message;
            tmsg->next = NULL;
            if(!msg_head)
            {
              msg_head = tmsg;
              msg_tail = tmsg;
            }
            else
            {
              msg_tail->next = tmsg;
              msg_tail = tmsg;
            }
          }
          else
          {
            free(topic);
            free(message);
          }
          
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
    
    if(FD_ISSET(fdhttp, &fds))
    {
      n = http_read(fdhttp, &hmsg);
      if(n == 0)
      {
        printf("Code: %d\n", hmsg.header.code);
        close(fdhttp);
        fdhttp = -1;
        cmsg = NULL;
      }
    }
  }
}

int custom_init(struct mqtt3_config *config, struct mosquitto_db *db)
{
	char *client_id = NULL;
	struct mosquitto *context;
	int i, ret, sock[2];
	pthread_t p;
	struct custom_data *data;
	
#if 0
	char *topic[3];
	char *value[3];
	
	struct http_message msg;
	memset(&msg, 0, sizeof(struct http_message));
	
	topic[0] = "from";
	value[0] = "mittente";
	topic[1] = "to";
	value[1] = "destinatario";
	topic[2] = "msg";
	value[2] = "messaggio";
	
	i = http_post(/*"http://requestb.in/147z7ab1"*/ config->post_url, 3, topic, value);

	/* Per come è fatta attualmente, http_response() ritorna 0 solo alla chiusura
	   del socket. Per la connessione keepalive bisogna ritoccare qualcosa. */
	while(http_response(i, &msg) > 0);
	printf("Code: %d\n", msg.header.code);
#endif

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
	
	msg_head = msg_tail = NULL;
	
	client_id = strdup("custom");
	context = calloc(1, sizeof(struct mosquitto));
	mosquitto_log_printf(MOSQ_LOG_NOTICE, "Custom client connected as id '%s'.", client_id);
	
	socketpair(AF_LOCAL, SOCK_STREAM, 0, sock);
	data = malloc(sizeof(struct custom_data));
	data->sock = sock[0];
	data->config = config;
	pthread_create(&p, NULL, custom_loop, data);
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

