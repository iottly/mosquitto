#include "mosquitto_broker.h"
#include "mqtt3_protocol.h"
#include "http.h"
#include "uthash.h"

/* Viene incluso "dummypthread.h", ma devo usare la libreria reale */
#undef pthread_create
#undef pthread_join
#undef pthread_cancel
#undef pthread_mutex_init
#undef pthread_mutex_destroy
#undef pthread_mutex_lock
#undef pthread_mutex_unlock

#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/time.h>

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <hiredis/hiredis.h>

#include <errno.h>

#define _mosquitto_malloc(x) malloc(x)
#define _mosquitto_free(x) free(x)

#define TOPIC_MAX_LEN 128
#define REDIS_KEY_PREFIX "mqtt_rt_"
//GET mqtt_rt_<PID>\0  => 12 + 24 + 1
#define REDIS_CMD_LEN 37

struct custom_data {
  struct mqtt3_config *config;
  int sock;
  redisContext *redis;
  pthread_mutex_t redis_lock;
  pthread_cond_t redis_disconnected;
};

struct msglist {
  char *topic;
  char *value;
  int qos;
  int mid;
  int done;
  struct msglist *next;
  struct timeval post_time;
} *msg_head, *msg_tail;

struct http_fds_entry {
    const char *url;
    int fd;
    UT_hash_handle hh;
};

struct timeval now;
double elapsedTime;


void get_routing_key_from_topic(char *topic, char *scratchpad, char **routing_key) {
  // get redis key from topic string
  // NOTE this is not general
  strncpy(scratchpad, topic, TOPIC_MAX_LEN);
  char *saveptr;
  char *in = scratchpad;
  for (int i = 0; i < 3; i++) {
    *routing_key = strtok_r(in, "/", &saveptr);
    in = NULL;
    if (*routing_key == NULL) {
      break;
    }
  }
}

void search_post_url_in_redis(struct custom_data *cdata, char **http_post_url, char* redis_cmd, redisReply *reply) {
  // COND *reply == NULL
  // Here we try to lock the mutex, if we cannot lock the mutex
  //    => redis is disconnected.
  pthread_mutex_t *redis_lock = &cdata->redis_lock;

  int r = pthread_mutex_trylock( redis_lock );
  if ( r == 0 && cdata->redis ) {
    reply = redisCommand(cdata->redis, redis_cmd);
  }

  if (reply) {
    if (reply->type == REDIS_REPLY_STRING) {
      *http_post_url = reply->str;
      // mosquitto_log_printf(MOSQ_LOG_ERR, "REDIS:%s", reply->str);
    } else  if (reply->type == REDIS_REPLY_NIL) {
      // mosquitto_log_printf(MOSQ_LOG_ERR, "REDIS: NOT FOUND");
    } else if (reply->type == REDIS_REPLY_ERROR) {
      mosquitto_log_printf(MOSQ_LOG_ERR, "REDIS ERROR: %s", reply->str);
    }
  } else {
    // ERROR
    mosquitto_log_printf(MOSQ_LOG_ERR, "REDIS: NO REPLY");
    if (cdata->redis) {
      // Clean-up and signal reconnection
      redisFree( cdata->redis );
      cdata->redis = NULL;
      pthread_cond_signal( &cdata->redis_disconnected );
    }
  }

  // Release the mutex
  if (r == 0) {
    pthread_mutex_unlock( redis_lock );
  }
}


void* custom_loop(void *data)
{
  struct custom_data *cdata = data;
  unsigned char *buf, puback[4], *message;
  int fd, fdhttp, fdmax, n, /*i,*/ lbuf, ibuf, ibase;
  int qos, mid, len, tlen, tlen_len, tlen_valid, plen;
  fd_set fds;
  char *topic;
  struct msglist *tmsg, *cmsg;
  char *ptopic[3];
  char *pvalue[3];
  struct http_message hmsg;

  char *http_post_url = NULL;
  char topic_str_scratchpad[TOPIC_MAX_LEN];
  memset(&topic_str_scratchpad, 0, TOPIC_MAX_LEN);
  char redis_cmd[REDIS_CMD_LEN];
  memset(&redis_cmd, 0, REDIS_CMD_LEN);

  // Decalration of hash handle for HTTP fds
  struct http_fds_entry *http_fds = NULL;
  struct http_fds_entry *current_http_fd = NULL;

  ibuf = 0;
  lbuf = 1024;
  buf = malloc(lbuf);
  cmsg = NULL;
  fdhttp = -1;

  fd = cdata->sock;
  FD_ZERO(&fds);
  while(1)
  {
    if((!cmsg || cmsg->done == 1) && msg_head)
    {
      if (cmsg) {
        free(cmsg);
      }

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
          mosquitto_log_printf(MOSQ_LOG_ERR, "HTTP_POST - DROP - Message dropped! (85)");
          free(cmsg->topic);
          free(cmsg->value);
          free(cmsg);
          cmsg = NULL;
        }
        else
        {
          strcpy(pvalue[2], cdata->config->post_header);
          strcat(pvalue[2], " ");
          strcat(pvalue[2], cmsg->value);
        }
      }
      else
      {
        pvalue[2] = strdup(cmsg->value);
        if(pvalue[2] == NULL)
        {
          mosquitto_log_printf(MOSQ_LOG_ERR, "HTTP_POST - DROP - Message dropped! (90)");
          free(cmsg->topic);
          free(cmsg->value);
          free(cmsg);
          cmsg = NULL;
        }
      }
      
      if(cmsg)
      {
        redisReply *reply = NULL;
        char *routing_key = NULL;
        // use default routing for msg
        http_post_url = cdata->config->post_url;

        get_routing_key_from_topic(cmsg->topic, topic_str_scratchpad, &routing_key);

        if (routing_key != NULL) {
          snprintf(redis_cmd, REDIS_CMD_LEN, "GET "REDIS_KEY_PREFIX"%s", routing_key);
          // Call redis to choose the post URL
          search_post_url_in_redis(cdata, &http_post_url, redis_cmd, reply);
        }

        // HERE I need to get the right socket_fd to make the HTTP call
        // based on the URL
        HASH_FIND_STR( http_fds, http_post_url, current_http_fd);
        if ( ! current_http_fd ) {
          current_http_fd = (struct http_fds_entry *) malloc(sizeof *current_http_fd);
          current_http_fd->url = strdup(http_post_url);
          // Init the socket fd (-1 is disconnected)
          current_http_fd->fd = -1;
          HASH_ADD_KEYPTR( hh, http_fds, current_http_fd->url, strlen(current_http_fd->url), current_http_fd);
          int total_fd = HASH_COUNT(http_fds);
          mosquitto_log_printf(
            MOSQ_LOG_NOTICE,
            "HTTP_POST - added FD for HTTP to url %s tot: %d", http_post_url, total_fd
          );
        }

        current_http_fd->fd = http_post(current_http_fd->fd, http_post_url, 3, ptopic, pvalue);
        fdhttp = current_http_fd->fd;
        // free Redis reply
        freeReplyObject(reply);

        free(pvalue[2]);
        gettimeofday(&(cmsg->post_time), NULL);

        if(fdhttp < 0)
        {
          free(cmsg->topic);
          free(cmsg->value);
          free(cmsg);
          cmsg = NULL;
          mosquitto_log_printf(MOSQ_LOG_ERR, "HTTP_POST - POSTFAIL - HTTP POST failed!");
        }
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
      mosquitto_log_printf(MOSQ_LOG_ERR, "HTTP_POST - ERROR - loop custom exit (%d) ****", errno);
      /* Forse meglio uccidere tutto mosquitto? Con "exit(0)". */
      return NULL;
    }

    if(FD_ISSET(fd, &fds))
    {
      n = read(fd, buf+ibuf, lbuf-ibuf);
      if(n <= 0)
      {
        mosquitto_log_printf(MOSQ_LOG_ERR, "HTTP_POST - ERROR - loop custom exit ****");
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

      if((tlen+tlen_len+1) > lbuf)
      {
        mosquitto_log_printf(MOSQ_LOG_NOTICE, "HTTP_POST - REALLOC - Recv buffer realloc (%d->%d) ****", lbuf, ((tlen+tlen_len+1)+1023) & ~0x3ff);
        lbuf = ((tlen+tlen_len+1)+1023) & ~0x3ff;
        buf = realloc(buf, lbuf);
        if(!buf)
        {
          mosquitto_log_printf(MOSQ_LOG_ERR, "HTTP_POST - ERROR - realloc error - exit ****");
          return NULL;
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
            mosquitto_log_printf(MOSQ_LOG_ERR, "HTTP_POST - DROP - Message dropped! (155)");
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
            mosquitto_log_printf(MOSQ_LOG_ERR, "HTTP_POST - DROP - Message dropped! (175)");
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
          mosquitto_log_printf(MOSQ_LOG_NOTICE, "\nHTTP_POST - RECEIVED - plen=%d QoS=%d MID=%d topic='%s'", plen, qos, mid, topic);

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
            mosquitto_log_printf(MOSQ_LOG_ERR, "HTTP_POST - DROP - Message dropped! (176)");

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
        gettimeofday(&now, NULL);
        elapsedTime = (now.tv_sec - cmsg->post_time.tv_sec) * 1000.0;      // sec to ms
        elapsedTime += (now.tv_usec - cmsg->post_time.tv_usec) / 1000.0;   // us to ms

        mosquitto_log_printf(MOSQ_LOG_NOTICE, "HTTP_POST - POST - Code=%d Elapsedms=%f MID=%d topic='%s'", hmsg.header.code, elapsedTime, cmsg->mid, cmsg->topic);

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

        // This will trigger the processing of a new msg from MQTT
        cmsg->done = 1;
      }
      else if(n < 0)
      {
        if(cmsg->done)
        {
          close(fdhttp);
          free(cmsg->topic);
          free(cmsg->value);
          free(cmsg);
          cmsg = NULL;
        }
        current_http_fd->fd = -1;
        mosquitto_log_printf(MOSQ_LOG_ERR, "HTTP_POST - HTTP FD CLOSED (447)");
        if(cmsg)
        {
          /* Il socket è stato chiuso dal server */
          mosquitto_log_printf(MOSQ_LOG_ERR, "HTTP_POST - DROP - Message dropped! (180)");
          free(cmsg->topic);
          free(cmsg->value);
          free(cmsg);
          cmsg = NULL;
        }
      }
    }
  }
}

void* redis_reconn_loop (void *data) {
  struct custom_data *cdata = data;
  pthread_mutex_t *redis_lock = &cdata->redis_lock;
  pthread_cond_t *redis_disconnected = &cdata->redis_disconnected;

  struct timespec backoff = {0, 0L};

  while(1) {
    if (cdata->redis != NULL) {
      mosquitto_log_printf(MOSQ_LOG_NOTICE, "REDIS: conn_t - waiting for disconnection ...");
      // Here we acquire the lock to use the pthread condition
      pthread_mutex_lock( redis_lock );
      while (cdata->redis != NULL) {
        // re-check condition to filter out spurious wakeups
        // NOTE: cond_wait release the lock before waiting
        pthread_cond_wait( redis_disconnected, redis_lock );
      }
      // COND: here redis is NULL and we have the lock

      mosquitto_log_printf(MOSQ_LOG_NOTICE, "REDIS: conn_t - disconnected");
    }
    // else we short-circuit to the connection code

      // handle backoff in the re-connection
      nanosleep(&backoff, NULL);
      if (backoff.tv_sec < 10) {
        backoff.tv_sec += 1;
      }

    mosquitto_log_printf(MOSQ_LOG_NOTICE, "REDIS: Attempting connection ...");
    // then try to reconnect 
    redisContext *c;
    struct timeval timeout = { 1, 500000 }; // 1.5 seconds
    
    char *host = cdata->config->post_redis_host;
    int port = cdata->config->post_redis_port;
    c = redisConnectWithTimeout(host, port, timeout);
    // at this point we have an answer so we can lock the mutex
    if (c == NULL || c->err) {
      if (c) {
        mosquitto_log_printf(MOSQ_LOG_ERR, "REDIS: Connection error: %s\n", c->errstr);
        redisFree(c);
      } else {
        mosquitto_log_printf(MOSQ_LOG_ERR, "REDIS: Connection error: can't allocate redis context\n");
      }
      cdata->redis = NULL;
    } else {
      // CONNECTION YEAAHH!!
      mosquitto_log_printf(MOSQ_LOG_NOTICE, "REDIS: connected!");
      // reset backoff
      backoff.tv_sec = 0;
      redisEnableKeepAlive(c);
      cdata->redis = c;
    }
    pthread_mutex_unlock( redis_lock ); // MUTEX RELEASE
    // wait for next disconnection
  }
}

int custom_init(struct mqtt3_config *config, struct mosquitto_db *db)
{
	char *client_id = NULL;
	struct mosquitto *context = NULL;
	int i, ret, sock[2];
	pthread_t p;
 	pthread_t redis_reconn_th;
	struct custom_data *data = NULL;
	struct _mosquitto_acl *acl = NULL;

	msg_head = msg_tail = NULL;

	client_id = strdup(config->post_clientid);
	if(client_id == NULL) goto CLEANUP;

	context = calloc(1, sizeof(struct mosquitto));
	if(context == NULL) goto CLEANUP;

	mosquitto_log_printf(MOSQ_LOG_NOTICE, "HTTP_POST - CONNECTED - Custom client connected as id '%s'.", client_id);

	socketpair(AF_LOCAL, SOCK_STREAM, 0, sock);
	data = malloc(sizeof(struct custom_data));
	if(data == NULL) goto CLEANUP;

	data->config = config;
	data->sock = sock[0];
  data->redis = NULL;

  ret = pthread_mutex_init(&data->redis_lock, NULL);
  if (ret != 0) goto CLEANUP;
  ret = pthread_cond_init(&data->redis_disconnected, NULL);
  if (ret != 0) goto CLEANUP;

	context->sock = sock[1];
	context->id = client_id;
	/* Da popolare con la lista dei topic sottoscritti */
	context->acl_list = calloc(1, sizeof(struct _mosquitto_acl_user));
	if(context->acl_list == NULL) goto CLEANUP;


	HASH_ADD_KEYPTR(hh_id, db->contexts_by_id, context->id, strlen(context->id), context);
	HASH_ADD(hh_sock, db->contexts_by_sock, sock, sizeof(context->sock), context);
	context->state = mosq_cs_connected;
	context->in_packet.remaining_mult = 1;

	for(i=0; i<config->post_topic_num; i++)
	{
		ret = mqtt3_sub_add(db, context, config->post_topic[i], config->post_topic_qos[i], &db->subs);
		mosquitto_log_printf(MOSQ_LOG_NOTICE, "HTTP_POST - SUBSCR - Subscribing '%s' QoS=%d (%d)", config->post_topic[i], config->post_topic_qos[i], ret);

		/* Popolo la lista ACL con i topic registrati. */
		acl = calloc(1, sizeof(struct _mosquitto_acl));
		if(acl == NULL) goto CLEANUP;

		acl->topic = config->post_topic[i];
		acl->access = 1;
		acl->next = context->acl_list->acl;
		context->acl_list->acl = acl;
	}

  mosquitto_log_printf(MOSQ_LOG_NOTICE, "HTTP_POST - REDIS starting thread");
  pthread_create(&redis_reconn_th, NULL, redis_reconn_loop, data);
  pthread_detach(redis_reconn_th);
  mosquitto_log_printf(MOSQ_LOG_NOTICE, "HTTP_POST - REDIS thread created");

	pthread_create(&p, NULL, custom_loop, data);
	pthread_detach(p);

	return 0;

CLEANUP:
  // Clean-up code
  if ( context != NULL && context->acl_list != NULL ) {
    while( context->acl_list->acl ){
      acl = context->acl_list->acl->next;
      free(context->acl_list->acl);
      context->acl_list->acl = acl;
    }
    free(context->acl_list);
  }
  if (data != NULL) free(data);
  if (context != NULL) free(context);
  if (client_id != NULL) free(client_id);

  mosquitto_log_printf(MOSQ_LOG_ERR, "HTTP_POST - NOTINIT - Custom client NOT initialized!");
  return -1;
}
