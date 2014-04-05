/*
	Copyright (C) 2013 CurlyMo

	This file is part of pilight.

    pilight is free software: you can redistribute it and/or modify it under the 
	terms of the GNU General Public License as published by the Free Software 
	Foundation, either version 3 of the License, or (at your option) any later 
	version.

    pilight is distributed in the hope that it will be useful, but WITHOUT ANY 
	WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR 
	A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with pilight. If not, see	<http://www.gnu.org/licenses/>
*/

#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <signal.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netdb.h>
#include <stdint.h>
#include <math.h>

#include "../../pilight.h"
#include "common.h"
#include "log.h"
#include "threads.h"
#include "protocol.h"
#include "hardware.h"
#include "binary.h"
#include "json.h"
#include "gc.h"
#include "xbmc.h"

typedef struct xbmc_data_t {
	char *server;
	int port;
	int sockfd;
	struct xbmc_data_t *next;
} xbmc_data_t;

struct xbmc_data_t *xbmc_data;
unsigned short xbmc_loop = 1;
unsigned short xbmc_threads = 0;

void *xbmcParse(void *param) {
	struct protocol_threads_t *node = (struct protocol_threads_t *)param;
	struct JsonNode *json = (struct JsonNode *)node->param;
	struct JsonNode *jid = NULL;
	struct JsonNode *jchild = NULL;
	struct JsonNode *jchild1 = NULL;
	struct sockaddr_in serv_addr;
	struct xbmc_data_t *xnode = malloc(sizeof(struct xbmc_data_t));

	char recvBuff[BUFFER_SIZE], action[10], type[15];
	char *m = NULL, *t = NULL;
	int nrloops = 0, bytes = 0, n = 0, has_server = 0, has_port = 0, maxfd = 0;
	fd_set fdsread;	
	struct timeval timeout;
	timeout.tv_sec = 1;
	timeout.tv_usec = 0;

	if(!xnode) {
		logprintf(LOG_ERR, "out of memory");
		exit(EXIT_FAILURE);
	}	
	
	/* Clear the server address */
    memset(&serv_addr, '\0', sizeof(serv_addr));
	memset(&recvBuff, '\0', BUFFER_SIZE);
	memset(&action, '\0', 10);
	memset(&type, '\0', 15);
	
	xbmc_threads++;

	if((jid = json_find_member(json, "id"))) {
		jchild = json_first_child(jid);
		while(jchild) {
			jchild1 = json_first_child(jchild);

			while(jchild1) {
				if(strcmp(jchild1->key, "server") == 0) {
					if(!(xnode->server = malloc(strlen(jchild1->string_)+1))) {
						logprintf(LOG_ERR, "out of memory");
						exit(EXIT_FAILURE);
					}
					strcpy(xnode->server, jchild1->string_);
					has_server = 1;
				}
				if(strcmp(jchild1->key, "port") == 0) {
					xnode->port = (int)round(jchild1->number_);
					has_port = 1;
				}
				jchild1 = jchild1->next;
			}
			if(has_server == 1 && has_port == 1) {
				xnode->sockfd = -1;
				xnode->next = xbmc_data;
				xbmc_data = xnode;
			} else {
				if(has_server == 1) {
					sfree((void *)&xnode->server);
				}
				sfree((void *)&xnode);
				xnode = NULL;
			}
			jchild = jchild->next;
		}
	}

	if(!xnode) {
		return 0;
	}

	while(xbmc_loop) {
		if(xnode->sockfd > -1) {
			close(xnode->sockfd);
			xnode->sockfd = -1;
		}

		/* Try to open a new socket */
		if((xnode->sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
			logprintf(LOG_DEBUG, "could not create XBMC socket");
			break;
		}

		serv_addr.sin_family = AF_INET;
		serv_addr.sin_port = htons((unsigned short)xnode->port);
		inet_pton(AF_INET, xnode->server, &serv_addr.sin_addr);	
		
		/* Connect to the server */
		if(connect(xnode->sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
			protocol_thread_wait(node, 3, &nrloops);
			continue;
		}

		struct xbmc_data_t *xtmp = xbmc_data;
		while(xtmp) {
			if(xtmp->sockfd > -1) {
				if(maxfd < xtmp->sockfd) {
					maxfd = xtmp->sockfd;
				}
			}
			xtmp = xtmp->next;
		}
		
		while(xbmc_loop) {
			FD_ZERO(&fdsread);
			FD_SET((unsigned long)xnode->sockfd, &fdsread);

			do {
				n = select(maxfd+1, &fdsread, NULL, NULL, &timeout);
			} while(n == -1 && errno == EINTR && xbmc_loop);

			if(xbmc_loop == 0) {
				break;
			}

			if(n == -1) {
				break;
			} else if(n == 0) {
				usleep(10000);
			} else if(n > 0) {
				if(FD_ISSET((unsigned long)xnode->sockfd, &fdsread)) {
					bytes = recv(xnode->sockfd, recvBuff, BUFFER_SIZE, 0);
					if(bytes <= 0) {
						break;
					} else {
						if(json_validate(recvBuff) == true) {
							JsonNode *joutput = json_decode(recvBuff);
							JsonNode *params = NULL;
							JsonNode *data = NULL;
							JsonNode *item = NULL;

							if(json_find_string(joutput, "method", &m) == 0) {
								if(strcmp(m, "GUI.OnScreensaverActivated") == 0) {
									strcpy(type, "screensaver");
									strcpy(action, "active");
								} else if(strcmp(m, "GUI.OnScreensaverDeactivated") == 0) {
									strcpy(type, "screensaver");
									strcpy(action, "inactive");
								} else {
									if((params = json_find_member(joutput, "params")) != NULL) {
										if((data = json_find_member(params, "data")) != NULL) {
											if((item = json_find_member(data, "item")) != NULL) {
												if(json_find_string(item, "type", &t) == 0) {
													xbmc->message = json_mkobject();

													strcpy(type, t);
													if(strcmp(m, "Player.OnPlay") == 0) {
														strcpy(action, "play");
													} else if(strcmp(m, "Player.OnStop") == 0) {
														strcpy(action, "stop");
													} else if(strcmp(m, "Player.OnPause") == 0) {
														strcpy(action, "pause");
													}

													xbmc->message = json_mkobject();
													JsonNode *code = json_mkobject();
													json_append_member(code, "action", json_mkstring(action));
													json_append_member(code, "type", json_mkstring(type));
													json_append_member(code, "server", json_mkstring(xnode->server));
													json_append_member(code, "port", json_mknumber(xnode->port));
																		
													json_append_member(xbmc->message, "message", code);
													json_append_member(xbmc->message, "origin", json_mkstring("receiver"));
													json_append_member(xbmc->message, "protocol", json_mkstring(xbmc->id));
																		
													pilight.broadcast(xbmc->id, xbmc->message);
													json_delete(xbmc->message);
													xbmc->message = NULL;
												}
											}
										}
									}
								}
							}
							json_delete(joutput);
						}
						memset(recvBuff, '\0', BUFFER_SIZE);
					}
				}
			}
		}
	}

	xbmc_threads--;
	return (void *)NULL;
}

struct threadqueue_t *xbmcInitDev(JsonNode *jdevice) {
	xbmc_loop = 1;
	char *output = json_stringify(jdevice, NULL);
	JsonNode *json = json_decode(output);
	sfree((void *)&output);

	struct protocol_threads_t *node = protocol_thread_init(xbmc, json);	
	return threads_register("xbmc", &xbmcParse, (void *)node, 0);
}

void xbmcThreadGC(void) {
	xbmc_loop = 0;
	struct xbmc_data_t *xtmp = NULL;
	
	while(xbmc_data) {
		xtmp = xbmc_data;
		if(xtmp->sockfd > -1) {
			close(xtmp->sockfd);
			xtmp->sockfd = -1;
		}
		sfree((void *)&xtmp->server);
		xbmc_data = xbmc_data->next;
		sfree((void *)&xtmp);
	}

	protocol_thread_stop(xbmc);
	while(xbmc_threads > 0) {
		usleep(10);
	}
	protocol_thread_free(xbmc);
}

void xbmcInit(void) {

	protocol_register(&xbmc);
	protocol_set_id(xbmc, "xbmc");
	protocol_device_add(xbmc, "xbmc", "XBMC API");
	xbmc->devtype = XBMC;
	xbmc->hwtype = API;

	options_add(&xbmc->options, 'a', "action", OPTION_HAS_VALUE, CONFIG_VALUE, JSON_STRING, NULL, NULL);
	options_add(&xbmc->options, 't', "type", OPTION_HAS_VALUE, CONFIG_VALUE, JSON_STRING, NULL, NULL);
	options_add(&xbmc->options, 's', "server", OPTION_HAS_VALUE, CONFIG_ID, JSON_STRING, NULL, NULL);
	options_add(&xbmc->options, 'p', "port", OPTION_HAS_VALUE, CONFIG_ID, JSON_NUMBER, NULL, NULL);

	xbmc->initDev=&xbmcInitDev;
	xbmc->threadGC=&xbmcThreadGC;
}
