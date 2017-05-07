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

#include <errno.h>

#define _mosquitto_malloc(x) malloc(x)
#define _mosquitto_free(x) free(x)

struct custom_data {
  struct mqtt3_config *config;
  int sock;
};

struct msglist {
  char *topic;
  char *value;
  int qos;
  int mid;
  int done;
  struct msglist *next;
} *msg_head, *msg_tail;

void* custom_loop(void *data)
{
  struct custom_data *cdata = data;
  unsigned char buf[1024], puback[4], *message;
  int fd, fdhttp, fdmax, n, /*i,*/ ibuf, ibase;
  int qos, mid, len, tlen, tlen_len, tlen_valid, plen;
  fd_set fds;
  char *topic;
  struct msglist *tmsg, *cmsg;
  char *ptopic[3];
  char *pvalue[3];
  struct http_message hmsg;
	
  ibuf = 0;
  cmsg = NULL;
  fdhttp = -1;
  
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
      if(cdata->config->post_header)
      {
        pvalue[2] = malloc(strlen(cdata->config->post_header)+strlen(cmsg->value)+2);
        if(pvalue[2] == NULL)
        {
          /* Scarto il messaggio. Sarebbe forse meglio riaccodare e aspettare
             un po' di tempo? Non credo serva, se ormai la lista dei topic da
             inoltrare ha saturato la memoria non ho più modo di liberarla se
             non scartando i messaggi da qui. */
          mosquitto_log_printf(MOSQ_LOG_ERR, "|- Message dropped! (85)");
          free(cmsg->topic);
          free(cmsg->value);
          free(cmsg);
          cmsg = NULL;
        }
        
        strcpy(pvalue[2], cdata->config->post_header);
        strcat(pvalue[2], " ");
        strcat(pvalue[2], cmsg->value);
      }
      else
      {
        pvalue[2] = strdup(cmsg->value);
        if(pvalue[2] == NULL)
        {
          mosquitto_log_printf(MOSQ_LOG_ERR, "|- Message dropped! (90)");
          free(cmsg->topic);
          free(cmsg->value);
          free(cmsg);
          cmsg = NULL;
        }
      }
      
      fdhttp = http_post(cdata->config->post_url, 3, ptopic, pvalue);

      free(pvalue[2]);
      free(cmsg->topic);
      free(cmsg->value);
      
      if(fdhttp < 0)
      {
        free(cmsg);
        cmsg = NULL;
        mosquitto_log_printf(MOSQ_LOG_ERR, "|- HTTP POST failed!");
      }
    }
    
    fdmax = fd;
    FD_SET(fd, &fds);
    
    if(fdhttp >= 0)
    {
      FD_SET(fdhttp, &fds);
      if(fdhttp > fdmax) fdmax = fdhttp;
    }
    n = select(fdmax+1, &fds, NULL, NULL, NULL);
    
    if(n < 0)
    {
      if(errno == EINTR) continue;
      mosquitto_log_printf(MOSQ_LOG_ERR, "**** ERROR - loop custom exit (%d) ****", errno);
      /* Forse meglio uccidere tutto mosquitto? Con "exit(0)". */
      return NULL;
    }
    
    if(FD_ISSET(fd, &fds))
    {
      n = read(fd, buf+ibuf, sizeof(buf)-ibuf);
      if(n <= 0)
      {
        mosquitto_log_printf(MOSQ_LOG_ERR, "**** ERROR - loop custom exit ****");
        return NULL;
      }
      ibuf += n;
      
      /* Verifica se ho tutti i byte per tlen e calcola la base
         di partenza del messaggio. */
      tlen_valid = 0;
      tlen = 0;
      for(tlen_len=1; tlen_len<=4; tlen_len++)
      {
        if(ibuf > tlen_len)
        {
          tlen += (buf[tlen_len] & 0x7f) << (7*(tlen_len-1));
          if(!(buf[tlen_len] & 0x80))
          {
            tlen_valid = 1;
            break;
          }
        }
      }
      
      while(tlen_valid && (ibuf >= (tlen+tlen_len+1)))
      {
        if((buf[0] & 0xF0) == PUBLISH)
        {
          /* QoS */
          qos = (buf[0] & 0x06) >> 1;
          
          /* Topic */
          len = (buf[tlen_len+1]<<8) + buf[tlen_len+2];
          topic = malloc(len+1);
          if(topic == NULL)
          {
            mosquitto_log_printf(MOSQ_LOG_ERR, "|- Message dropped! (155)");
#warning Rispondere NAK se QoS > 0
            break;
          }
          memcpy(topic, buf+tlen_len+3, len);
          topic[len] = 0;

          /* Message ID */
          mid = 0;
          ibase = len+tlen_len+3;
          if(qos > 0)
          {
            mid = (buf[len+tlen_len+3]<<8) + buf[len+tlen_len+4];
            ibase += 2;
          }

          /* Message */
          plen = tlen+tlen_len+1 - ibase;
          message = malloc(plen+1);
          if(message == NULL)
          {
            free(topic);
            mosquitto_log_printf(MOSQ_LOG_ERR, "|- Message dropped! (175)");
#warning Rispondere NAK se QoS > 0
            break;
          }
          memcpy(message, buf+ibase, plen);
          message[plen] = 0;
          
          /* Stampa dei dati ricevuti -- DA SOSTITUIRE CON CODICE REALE */
          /* Nota: il QoS ricevuto è sempre 0, anche se il messaggio di origine aveva QoS maggiori */
          //printf("** PUBLISH QoS=%d (MID=%d) topic='%s' plen=%d\n** payload=", qos, mid, topic, plen);
          //for(i=0; i<plen; i++) printf("%02x", message[i]);
          //printf("\n");
          mosquitto_log_printf(MOSQ_LOG_NOTICE, "|- PUBLISH QoS=%d (MID=%d) topic='%s' plen=%d", qos, mid, topic, plen);
          
          tmsg = malloc(sizeof(struct msglist));
          if(tmsg)
          {
            tmsg->topic = topic;
            tmsg->value = (char*)message;
            tmsg->qos = qos;
            tmsg->mid = mid;
            tmsg->done = 0;
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
            mosquitto_log_printf(MOSQ_LOG_ERR, "|- Message dropped! (176)");
            
            free(topic);
            free(message);
          }
        }
        
        memcpy(buf, buf+tlen+tlen_len+1, ibuf-(tlen+tlen_len+1));
        ibuf -= tlen+tlen_len+1;
        
        /* Controllo se c'è un altro messaggio accodato del quale conosco la lunghezza. */
        tlen_valid = 0;
        tlen = 0;
        for(tlen_len=1; tlen_len<=4; tlen_len++)
        {
          if(ibuf > tlen_len)
          {
            tlen += (buf[tlen_len] & 0x7f) << (7*(tlen_len-1));
            if(!(buf[tlen_len] & 0x80))
            {
              tlen_valid = 1;
              break;
            }
          }
        }
      }
    }

    if((fdhttp >= 0) && FD_ISSET(fdhttp, &fds))
    {
      n = http_read(fdhttp, &hmsg);

      if(n == 0)
      {
        mosquitto_log_printf(MOSQ_LOG_NOTICE, "|-- Code: %d", hmsg.header.code);
        if((hmsg.header.code == 200) && (cmsg->qos == 1))
        {
          /* Invia il PUBACK al sender */
          puback[0] = PUBACK | (1<<1);	// PUBACK + QoS 1
          puback[1] = 2;
          puback[2] = cmsg->mid>>8;
          puback[3] = cmsg->mid;
          write(fd, puback, 4);
        }
        /* if(qos == 2) _mosquitto_send_pubrec(context, mid);
             ... che a sua volta riceve PUBREL che richiede il PUBCOMP.
        */
        //free(cmsg);
        //cmsg = NULL;
        cmsg->done = 1;
        shutdown(fdhttp, 2);
      }
      else if(n < 0)
      {
        if(cmsg->done)
        {
          close(fdhttp);
          free(cmsg);
          cmsg = NULL;
        }
        fdhttp = -1;
        if(cmsg)
        {
          /* Il socket è stato chiuso dal server */
          mosquitto_log_printf(MOSQ_LOG_ERR, "|- Message dropped! (180)");
          free(cmsg);
          cmsg = NULL;
        }
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
	struct _mosquitto_acl *acl;
	
	msg_head = msg_tail = NULL;
	
	client_id = strdup(config->post_clientid);
	if(client_id == NULL)
	{
	  mosquitto_log_printf(MOSQ_LOG_ERR, "|- Custom client NOT initialized!");
	  return -1;
	}
	
	context = calloc(1, sizeof(struct mosquitto));
	if(context == NULL)
	{
	  free(client_id);
	  mosquitto_log_printf(MOSQ_LOG_ERR, "|- Custom client NOT initialized!");
	  return -1;
	}
	
	mosquitto_log_printf(MOSQ_LOG_NOTICE, "Custom client connected as id '%s'.", client_id);
	
	socketpair(AF_LOCAL, SOCK_STREAM, 0, sock);
	data = malloc(sizeof(struct custom_data));
	if(data == NULL)
	{
	  free(client_id);
	  free(context);
	  mosquitto_log_printf(MOSQ_LOG_ERR, "|- Custom client NOT initialized!");
	  return -1;
	}

	data->sock = sock[0];
	data->config = config;
	
	context->sock = sock[1];
	context->id = client_id;
	/* Da popolare con la lista dei topic sottoscritti */
	context->acl_list = calloc(1, sizeof(struct _mosquitto_acl_user));
	if(context->acl_list == NULL)
	{
	  free(client_id);
	  free(context);
	  free(data);
	  mosquitto_log_printf(MOSQ_LOG_ERR, "|- Custom client NOT initialized!");
	  return -1;
	}
	
	
	HASH_ADD_KEYPTR(hh_id, db->contexts_by_id, context->id, strlen(context->id), context);
	HASH_ADD(hh_sock, db->contexts_by_sock, sock, sizeof(context->sock), context);
	context->state = mosq_cs_connected;
	context->in_packet.remaining_mult = 1;
	
	for(i=0; i<config->post_topic_num; i++)
	{
		ret = mqtt3_sub_add(db, context, config->post_topic[i], config->post_topic_qos[i], &db->subs);
		mosquitto_log_printf(MOSQ_LOG_NOTICE, "|-- Subscribing '%s' QoS=%d (%d)", config->post_topic[i], config->post_topic_qos[i], ret);
		
		/* Popolo la lista ACL con i topic registrati. */
		acl = calloc(1, sizeof(struct _mosquitto_acl));
		if(acl == NULL)
		{
		  while(context->acl_list->acl)
		  {
		    acl = context->acl_list->acl->next;
		    free(context->acl_list->acl);
		    context->acl_list->acl = acl;
		  }
		  free(context->acl_list);
		  free(client_id);
		  free(data);
		  free(context);
		  mosquitto_log_printf(MOSQ_LOG_ERR, "|- Custom client NOT initialized!");
		  return -1;
		}
		
		acl->topic = config->post_topic[i];
		acl->access = 1;
		acl->next = context->acl_list->acl;
		context->acl_list->acl = acl;
	}

	pthread_create(&p, NULL, custom_loop, data);
	pthread_detach(p);
	
	return 0;
}

