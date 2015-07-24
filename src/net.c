/*
Copyright (c) 2009-2014 Roger Light <roger@atchoo.org>

All rights reserved. This program and the accompanying materials
are made available under the terms of the Eclipse Public License v1.0
and Eclipse Distribution License v1.0 which accompany this distribution.
 
The Eclipse Public License is available at
   http://www.eclipse.org/legal/epl-v10.html
and the Eclipse Distribution License is available at
  http://www.eclipse.org/org/documents/edl-v10.php.
 
Contributors:
   Roger Light - initial implementation and documentation.
*/

#include <config.h>

#include <netdb.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#ifdef WITH_WRAP
#include <tcpd.h>
#endif

#ifdef __FreeBSD__
#  include <netinet/in.h>
#  include <sys/socket.h>
#endif

#ifdef __QNX__
#include <netinet/in.h>
#include <net/netbyte.h>
#include <sys/socket.h>
#endif

#include <mosquitto_broker.h>
#include <mqtt3_protocol.h>
#include <memory_mosq.h>
#include <net_mosq.h>
#include <util_mosq.h>


#ifdef WITH_SYS_TREE
extern unsigned int g_socket_connections;
#endif

int mqtt3_socket_accept(struct mosquitto_db *db, mosq_sock_t listensock)
{
	int i;
	int j;
	mosq_sock_t new_sock = INVALID_SOCKET;
	struct mosquitto *new_context;
#ifdef WITH_WRAP
	struct request_info wrap_req;
	char address[1024];
#endif

	new_sock = accept(listensock, NULL, 0);
	if(new_sock == INVALID_SOCKET) return -1;

#ifdef WITH_SYS_TREE
	g_socket_connections++;
#endif

	if(_mosquitto_socket_nonblock(new_sock)){
		return INVALID_SOCKET;
	}

#ifdef WITH_WRAP
	/* Use tcpd / libwrap to determine whether a connection is allowed. */
	request_init(&wrap_req, RQ_FILE, new_sock, RQ_DAEMON, "mosquitto", 0);
	fromhost(&wrap_req);
	if(!hosts_access(&wrap_req)){
		/* Access is denied */
		if(!_mosquitto_socket_get_address(new_sock, address, 1024)){
			_mosquitto_log_printf(NULL, MOSQ_LOG_NOTICE, "Client connection from %s denied access by tcpd.", address);
		}
		COMPAT_CLOSE(new_sock);
		return -1;
	}
#endif
	new_context = mqtt3_context_init(db, new_sock);
	if(!new_context){
		COMPAT_CLOSE(new_sock);
		return -1;
	}
	for(i=0; i<db->config->listener_count; i++){
		for(j=0; j<db->config->listeners[i].sock_count; j++){
			if(db->config->listeners[i].socks[j] == listensock){
				new_context->listener = &db->config->listeners[i];
				new_context->listener->client_count++;
				break;
			}
		}
	}
	if(!new_context->listener){
		mqtt3_context_cleanup(db, new_context, true);
		return -1;
	}

	if(new_context->listener->max_connections > 0 && new_context->listener->client_count > new_context->listener->max_connections){
		_mosquitto_log_printf(NULL, MOSQ_LOG_NOTICE, "Client connection from %s denied: max_connections exceeded.", new_context->address);
		mqtt3_context_cleanup(db, new_context, true);
		return -1;
	}


	_mosquitto_log_printf(NULL, MOSQ_LOG_NOTICE, "New connection from %s on port %d.", new_context->address, new_context->listener->port);

	return new_sock;
}




/* Creates a socket and listens on port 'port'.
 * Returns 1 on failure
 * Returns 0 on success.
 */
int mqtt3_socket_listen(struct _mqtt3_listener *listener)
{
	mosq_sock_t sock = INVALID_SOCKET;
	struct addrinfo hints;
	struct addrinfo *ainfo, *rp;
	char service[10];
	int ss_opt = 1;
	char buf[256];

	if(!listener) return MOSQ_ERR_INVAL;

	snprintf(service, 10, "%d", listener->port);
	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = PF_UNSPEC;
	hints.ai_flags = AI_PASSIVE;
	hints.ai_socktype = SOCK_STREAM;

	if(getaddrinfo(listener->host, service, &hints, &ainfo)) return INVALID_SOCKET;

	listener->sock_count = 0;
	listener->socks = NULL;

	for(rp = ainfo; rp; rp = rp->ai_next){
		if(rp->ai_family == AF_INET){
			_mosquitto_log_printf(NULL, MOSQ_LOG_INFO, "Opening ipv4 listen socket on port %d.", ntohs(((struct sockaddr_in *)rp->ai_addr)->sin_port));
		}else if(rp->ai_family == AF_INET6){
			_mosquitto_log_printf(NULL, MOSQ_LOG_INFO, "Opening ipv6 listen socket on port %d.", ntohs(((struct sockaddr_in6 *)rp->ai_addr)->sin6_port));
		}else{
			continue;
		}

		sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if(sock == INVALID_SOCKET){
			strerror_r(errno, buf, 256);
			_mosquitto_log_printf(NULL, MOSQ_LOG_WARNING, "Warning: %s", buf);
			continue;
		}
		listener->sock_count++;
		listener->socks = _mosquitto_realloc(listener->socks, sizeof(mosq_sock_t)*listener->sock_count);
		if(!listener->socks){
			_mosquitto_log_printf(NULL, MOSQ_LOG_ERR, "Error: Out of memory.");
			return MOSQ_ERR_NOMEM;
		}
		listener->socks[listener->sock_count-1] = sock;

		ss_opt = 1;
		setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &ss_opt, sizeof(ss_opt));
		ss_opt = 1;
		setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &ss_opt, sizeof(ss_opt));

		if(_mosquitto_socket_nonblock(sock)){
			return 1;
		}

		if(bind(sock, rp->ai_addr, rp->ai_addrlen) == -1){
			strerror_r(errno, buf, 256);
			_mosquitto_log_printf(NULL, MOSQ_LOG_ERR, "Error: %s", buf);
			COMPAT_CLOSE(sock);
			return 1;
		}

		if(listen(sock, 100) == -1){
			strerror_r(errno, buf, 256);
			_mosquitto_log_printf(NULL, MOSQ_LOG_ERR, "Error: %s", buf);
			COMPAT_CLOSE(sock);
			return 1;
		}
	}
	freeaddrinfo(ainfo);

	/* We need to have at least one working socket. */
	if(listener->sock_count > 0){
		return 0;
	}else{
		return 1;
	}
}

int _mosquitto_socket_get_address(mosq_sock_t sock, char *buf, int len)
{
	struct sockaddr_storage addr;
	socklen_t addrlen;

	addrlen = sizeof(addr);
	if(!getpeername(sock, (struct sockaddr *)&addr, &addrlen)){
		if(addr.ss_family == AF_INET){
			if(inet_ntop(AF_INET, &((struct sockaddr_in *)&addr)->sin_addr.s_addr, buf, len)){
				return 0;
			}
		}else if(addr.ss_family == AF_INET6){
			if(inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&addr)->sin6_addr.s6_addr, buf, len)){
				return 0;
			}
		}
	}
	return 1;
}
