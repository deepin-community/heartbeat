/*
 * bcast.c: UDP/IP broadcast-based communication code for heartbeat.
 *
 * Copyright (C) 1999, 2000,2001 Alan Robertson <alanr@unix.sh>
 *
 * About 150 lines of the code in this file originally borrowed in
 * 1999 from Tom Vogt's "Heart" program, and significantly mangled by
 *	Alan Robertson <alanr@unix.sh>
 *	
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */
#include <lha_internal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <net/if.h>
#include <arpa/inet.h>

#include <heartbeat.h>
#include <HBcomm.h>

#if defined(SO_BINDTODEVICE)
#	include <net/if.h>
#endif

#define PIL_PLUGINTYPE          HB_COMM_TYPE
#define PIL_PLUGINTYPE_S        HB_COMM_TYPE_S
#define PIL_PLUGIN              bcast
#define PIL_PLUGIN_S            "bcast"
#define PIL_PLUGINLICENSE 	LICENSE_LGPL
#define PIL_PLUGINLICENSEURL 	URL_LGPL
#include <pils/plugin.h>

struct ip_private {
        char *  interface;      /* Interface name */
	struct in_addr bcast;   /* Broadcast address */
        struct sockaddr_in      addr;   /* Broadcast addr */
        int     port;
        int     rsocket;        /* Read-socket */
        int     wsocket;        /* Write-socket */
};


static int		bcast_init(void);
struct hb_media*	bcast_new(const char* interface);
static int		bcast_open(struct hb_media* mp);
static int		bcast_close(struct hb_media* mp);
static void*		bcast_read(struct hb_media* mp, int *lenp);
static int		bcast_write(struct hb_media* mp, void* msg, int len);
static int		bcast_make_receive_sock(struct hb_media* ei);
static int		bcast_make_send_sock(struct hb_media * mp);
static struct ip_private *
			new_ip_interface(const char * ifn, int port);
static int		bcast_descr(char** buffer);
static int		bcast_mtype(char** buffer);
static int		bcast_isping(void);
static int		localudpport = -1;


int if_get_broadaddr(const char *ifn, struct in_addr *broadaddr);

static struct hb_media_fns bcastOps ={
	bcast_new,	/* Create single object function */
	NULL,		/* whole-line parse function */
	bcast_open,
	bcast_close,
	bcast_read,
	bcast_write,
	bcast_mtype,
	bcast_descr,
	bcast_isping,
};

PIL_PLUGIN_BOILERPLATE2("1.0", Debug)
static const PILPluginImports*  PluginImports;
static PILPlugin*               OurPlugin;
static PILInterface*		OurInterface;
static struct hb_media_imports*	OurImports;
static void*			interfprivate;

#define LOG	PluginImports->log
#define MALLOC	PluginImports->alloc
#define STRDUP  PluginImports->mstrdup
#define FREE	PluginImports->mfree

PIL_rc
PIL_PLUGIN_INIT(PILPlugin*us, const PILPluginImports* imports);

PIL_rc
PIL_PLUGIN_INIT(PILPlugin*us, const PILPluginImports* imports)
{
	/* Force the compiler to do a little type checking */
	(void)(PILPluginInitFun)PIL_PLUGIN_INIT;

	PluginImports = imports;
	OurPlugin = us;

	/* Register ourself as a plugin */
	imports->register_plugin(us, &OurPIExports);  

	/*  Register our interface implementation */
 	return imports->register_interface(us, PIL_PLUGINTYPE_S
	,	PIL_PLUGIN_S
	,	&bcastOps
	,	NULL		/*close */
	,	&OurInterface
	,	(void*)&OurImports
	,	interfprivate); 
}

#define		ISBCASTOBJECT(mp) ((mp) && ((mp)->vf == (void*)&bcastOps))
#define		BCASTASSERT(mp)	g_assert(ISBCASTOBJECT(mp))

static int 
bcast_mtype(char** buffer) { 
	*buffer = STRDUP(PIL_PLUGIN_S);
	if (!*buffer) {
		return 0;
	}

	return STRLEN_CONST(PIL_PLUGIN_S);
}

static int
bcast_descr(char **buffer) { 
	const char constret[] = "UDP/IP broadcast";
	*buffer = STRDUP(constret);
	if (!*buffer) {
		return 0;
	}

	return STRLEN_CONST(constret);
}

static int
bcast_isping(void) {
    return 0;
}

static int
bcast_init(void)
{
	struct servent*	service;

	g_assert(OurImports != NULL);

	if (localudpport <= 0) {
		const char *	chport;
		if ((chport  = OurImports->ParamValue("udpport")) != NULL) {
			if (sscanf(chport, "%d", &localudpport) <= 0
				|| localudpport <= 0) {
				PILCallLog(LOG, PIL_CRIT
				,	"bad port number %s"
				,	chport);
				return HA_FAIL;
			}
		}
	}

	/* No port specified in the configuration... */

	if (localudpport <= 0) {
		/* If our service name is in /etc/services, then use it */
		if ((service=getservbyname(HA_SERVICENAME, "udp")) != NULL){
			localudpport = ntohs(service->s_port);
		}else{
			localudpport = UDPPORT;
		}
	}
	return(HA_OK);
}

/*
 *	Create new UDP/IP broadcast heartbeat object 
 *	Name of interface is passed as a parameter
 */
struct hb_media *
bcast_new(const char * intf)
{
	struct ip_private*	ipi;
	struct hb_media *	ret;

	bcast_init();
	ipi = new_ip_interface(intf, localudpport);
	if (DEBUGPKT) {
		PILCallLog(LOG, PIL_DEBUG, "bcast_new: attempting to open %s:%d", intf
		,	localudpport);
	}

	if (ipi == NULL) {
		PILCallLog(LOG, PIL_CRIT, "IP interface [%s] does not exist"
		,	intf);
		return(NULL);
	}
	ret = (struct hb_media*) MALLOC(sizeof(struct hb_media));
	if (ret != NULL) {
		char * name;
		memset(ret, 0, sizeof(*ret));
		ret->pd = (void*)ipi;
		name = STRDUP(intf);
		if (name != NULL) {
			ret->name = name;
		} else {
			FREE(ret);
			ret = NULL;
		}
	}
	if (ret != NULL) {
		if (DEBUGPKT) {
			PILCallLog(LOG, PIL_DEBUG, 
					"bcast_new: returning ret (%s)", 
					ret->name);
		}
	}else{
		FREE(ipi->interface);
		FREE(ipi);
		if (DEBUGPKT) {
			PILCallLog(LOG, PIL_DEBUG, "bcast_new: ret was NULL");
		}
	}
	return(ret);
}

/*
 *	Open UDP/IP broadcast heartbeat interface
 */
static int
bcast_open(struct hb_media* mp)
{
	struct ip_private * ei;

	BCASTASSERT(mp);
	ei = (struct ip_private *) mp->pd;

	if ((ei->wsocket = bcast_make_send_sock(mp)) < 0) {
		return(HA_FAIL);
	}
	if ((ei->rsocket = bcast_make_receive_sock(mp)) < 0) {
		bcast_close(mp);
		return(HA_FAIL);
	}
	PILCallLog(LOG, PIL_INFO
	,	"UDP Broadcast heartbeat started on port %d (%d) interface %s"
	,	localudpport, ei->port, mp->name);

	if (DEBUGPKT) {
		PILCallLog(LOG, PIL_DEBUG
		,	"bcast_open : Socket %d opened for reading"
		", socket %d opened for writing."
		,	ei->rsocket, ei->wsocket);
	}

	return(HA_OK);
}

/*
 *	Close UDP/IP broadcast heartbeat interface
 */
static int
bcast_close(struct hb_media* mp)
{
	struct ip_private * ei;
	int	rc = HA_OK;

	BCASTASSERT(mp);
	ei = (struct ip_private *) mp->pd;

	if (ei->rsocket >= 0) {
		if (close(ei->rsocket) < 0) {
			rc = HA_FAIL;
		}
		ei->rsocket=-1;
	}
	if (ei->wsocket >= 0) {
		if (close(ei->wsocket) < 0) {
			rc = HA_FAIL;
		}
		ei->wsocket=-1;
	}
	PILCallLog(LOG, PIL_INFO
	, "UDP Broadcast heartbeat closed on port %d interface %s - Status: %d"
	,	localudpport, mp->name, rc);

	return(rc);
}




/*
 * Receive a heartbeat broadcast packet from BCAST interface
 */

char			bcast_pkt[MAXMSG];
void *
bcast_read(struct hb_media* mp, int * lenp)
{
	struct ip_private *	ei;
	socklen_t		addr_len = sizeof(struct sockaddr);
   	struct sockaddr_in	their_addr; /* connector's addr information */
	int	numbytes;

	BCASTASSERT(mp);
	ei = (struct ip_private *) mp->pd;

	if (DEBUGPKT) {
		PILCallLog(LOG, PIL_DEBUG
		,	"bcast_read : reading from socket %d (writing to socket %d)"
			   ,	ei->rsocket, ei->wsocket);
	}

	if ((numbytes=recvfrom(ei->rsocket, bcast_pkt, MAXMSG-1, MSG_WAITALL
	,	(struct sockaddr *)&their_addr, &addr_len)) == -1) {
		if (errno != EINTR) {
			PILCallLog(LOG, PIL_CRIT
			,	"Error receiving from socket: %s"
			,	strerror(errno));
		}
		return NULL;
	}
	/* Avoid possible buffer overruns */
	bcast_pkt[numbytes] = EOS;

	if (DEBUGPKT) {
		PILCallLog(LOG, PIL_DEBUG, "got %d byte packet from %s"
		,	numbytes, inet_ntoa(their_addr.sin_addr));
	}
	if (DEBUGPKTCONT && numbytes > 0) {
		PILCallLog(LOG, PIL_DEBUG, "%s", bcast_pkt);
	}
	
	*lenp = numbytes +1;
	
	return bcast_pkt;
}


/*
 * Send a heartbeat packet over broadcast UDP/IP interface
 */


static int
bcast_write(struct hb_media* mp, void *pkt, int len)
{
	struct ip_private *	ei;
	int			rc;

	BCASTASSERT(mp);
	ei = (struct ip_private *) mp->pd;
	
	if ((rc=sendto(ei->wsocket, pkt, len, 0
	,	(struct sockaddr *)&ei->addr
	,	sizeof(struct sockaddr))) != len) {

		struct ha_msg* m;

		int		err = errno;
		if (!mp->suppresserrs) {
			PILCallLog(LOG, PIL_CRIT
			,	"%s: Unable to send " PIL_PLUGINTYPE_S " packet %s %s:%u len=%d [%d]: %s"
			,	__FUNCTION__, ei->interface, inet_ntoa(ei->addr.sin_addr), ei->port
			,	len, rc, strerror(errno));
		}
		
		if (ANYDEBUG) {
			m =  wirefmt2msg(pkt, len,MSG_NEEDAUTH);
			if (m){
				cl_log_message(LOG_ERR, m);
				ha_msg_del(m);
			}
		}

		errno = err;
		return(HA_FAIL);
	}

	if (DEBUGPKT) {
		PILCallLog(LOG, PIL_DEBUG
		,	"bcast_write : writing %d bytes to %s (socket %d)"
		,	rc, inet_ntoa(ei->addr.sin_addr), ei->wsocket);
   	}

	if (DEBUGPKTCONT) {
		PILCallLog(LOG, PIL_DEBUG, "bcast pkt out: [%s]", (char*)pkt);
   	}
	return(HA_OK);
}


/*
 * Set up socket for sending broadcast UDP heartbeats
 */

static int
bcast_make_send_sock(struct hb_media * mp)
{
	int sockfd, one = 1;
	BCASTASSERT(mp);

	if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
		PILCallLog(LOG, PIL_CRIT
		,	"Error getting socket: %s", strerror(errno));
		return(sockfd);
   	}

	if (DEBUGPKT) {
		PILCallLog(LOG, PIL_DEBUG
		,	"bcast_make_send_sock: Opened socket %d", sockfd);
	}

	/* Warn that we're going to broadcast */
	if (setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST, (const void *) &one, sizeof(one))==-1){
		PILCallLog(LOG, PIL_CRIT
		,	"Error setting socket option SO_BROADCAST: %s"
		,	strerror(errno));
		close(sockfd);
		return(-1);
	}

	if (DEBUGPKT) {
		PILCallLog(LOG, PIL_DEBUG
		,	"bcast_make_send_sock: Modified %d"
		" Added option SO_BROADCAST."
			, sockfd);
	}

#if defined(SO_DONTROUTE) && !defined(USE_ROUTING)
	/* usually, we don't want to be subject to routing. */
	if (setsockopt(sockfd, SOL_SOCKET, SO_DONTROUTE,(const void *) &one,sizeof(int))==-1) {
		PILCallLog(LOG, PIL_CRIT
		,	"Error setting socket option SO_DONTROUTE: %s"
		,	strerror(errno));
		close(sockfd);
		return(-1);
	}

	if (DEBUGPKT) {
		PILCallLog(LOG, PIL_DEBUG, "bcast_make_send_sock:"
		" Modified %d Added option SO_DONTROUTE."
			, sockfd);
	}

#endif
#if defined(SO_BINDTODEVICE)
	{
		/*
		 *  We want to send out this particular interface
		 *
		 * This is so we can have redundant NICs, and heartbeat on both
		 */
		struct ifreq i;
		strcpy(i.ifr_name,  mp->name);

		if (setsockopt(sockfd, SOL_SOCKET, SO_BINDTODEVICE
		,	(const void *) &i, sizeof(i)) == -1) {
			PILCallLog(LOG, PIL_CRIT
			,	"Error setting socket option SO_BINDTODEVICE"
			": %s"
			,	strerror(errno));
			close(sockfd);
			return(-1);
		}

		if (DEBUGPKT) {
			PILCallLog(LOG, PIL_DEBUG
			, "bcast_make_send_sock: Modified %d"
			" Added option SO_BINDTODEVICE."
			,	sockfd);
		}

	}
#endif
	if (fcntl(sockfd,F_SETFD, FD_CLOEXEC)) {
		PILCallLog(LOG, PIL_CRIT
		,	"Error setting close-on-exec flag: %s"
		,	strerror(errno));
	}
	return(sockfd);
}

/*
 * Set up socket for listening to heartbeats (UDP broadcasts)
 */

#define	MAXBINDTRIES	10
static int
bcast_make_receive_sock(struct hb_media * mp) {

	struct ip_private * ei;
	struct sockaddr_in my_addr;    /* my address information */
	int	sockfd;
	int	bindtries;
	int	boundyet=0;
	int	j;

	BCASTASSERT(mp);
	ei = (struct ip_private *) mp->pd;
	memset(&(my_addr), 0, sizeof(my_addr));	/* zero my address struct */
	my_addr.sin_family = AF_INET;		/* host byte order */
	my_addr.sin_port = htons(ei->port);	/* short, network byte order */
	my_addr.sin_addr.s_addr = INADDR_ANY;	/* auto-fill with my IP */

	if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
		PILCallLog(LOG, PIL_CRIT, "Error getting socket: %s"
		,	strerror(errno));
		return(-1);
	}
	/* 
 	 * Set SO_REUSEADDR on the server socket s. Variable j is used
 	 * as a scratch varable.
 	 *
 	 * 16th February 2000
 	 * Added by Horms <horms@vergenet.net>
 	 * with thanks to Clinton Work <work@scripty.com>
 	 */
	j = 1;
	if(setsockopt(sockfd,SOL_SOCKET,SO_REUSEADDR, (const void *)&j, sizeof j) <0){
		/* Ignore it.  It will almost always be OK anyway. */
		PILCallLog(LOG, PIL_CRIT
		,	"Error setting socket option SO_REUSEADDR: %s"
		,	strerror(errno));
	}        

        if (DEBUGPKT) {
		PILCallLog(LOG, PIL_DEBUG
		,	"bcast_make_receive_sock: Modified %d Added option SO_REUSEADDR."
		, sockfd);
	}

#if defined(SO_BINDTODEVICE)
	{
		/*
		 *  We want to receive packets only from this interface...
		 */
		struct ifreq i;
		strcpy(i.ifr_name,  ei->interface);

		if (setsockopt(sockfd, SOL_SOCKET, SO_BINDTODEVICE
		,	(const void *)&i, sizeof(i)) == -1) {
			PILCallLog(LOG, PIL_CRIT
			,	"Error setting socket option"
			" SO_BINDTODEVICE(r) on %s: %s"
			,	i.ifr_name, strerror(errno));
			close(sockfd);
			return(-1);
		}
		if (ANYDEBUG) {
			PILCallLog(LOG, PIL_DEBUG
			,	"SO_BINDTODEVICE(r) set for device %s"
			,	i.ifr_name);
		}
	}
#endif

	/* Try binding a few times before giving up */
	/* Sometimes a process with it open is exiting right now */

	for(bindtries=0; !boundyet && bindtries < MAXBINDTRIES; ++bindtries) {
		if (bind(sockfd, (struct sockaddr *)&my_addr
		,	sizeof(struct sockaddr)) < 0) {
			PILCallLog(LOG, PIL_CRIT
			,	"Error binding socket (%s). Retrying."
			,	strerror(errno));
			sleep(1);
		}else{
			boundyet = 1;
		}
	}
	if (!boundyet) {
#if !defined(SO_BINDTODEVICE)
		if (errno == EADDRINUSE) {
			/* This happens with multiple bcast or ppp interfaces */
			PILCallLog(LOG, PIL_INFO
			,	"Someone already listening on port %d [%s]"
			,	ei->port
			,	ei->interface);
			PILCallLog(LOG, PIL_INFO, "BCAST read process exiting");
			close(sockfd);
			cleanexit(0);
		}
#else
		PILCallLog(LOG, PIL_CRIT
		,	"Unable to bind socket (%s). Giving up."
		,	strerror(errno));
		close(sockfd);
		return(-1);
#endif
	}
	if (fcntl(sockfd,F_SETFD, FD_CLOEXEC)) {
		PILCallLog(LOG, PIL_CRIT
		,	"Error setting the close-on-exec flag: %s"
		,	strerror(errno));
	}
	if (DEBUGPKT) {
		PILCallLog(LOG, PIL_DEBUG
		,	"bcast_make_receive_sock: Returning %d", sockfd);
	}
	return(sockfd);
}



static struct ip_private *
new_ip_interface(const char * ifn, int port)
{
	struct ip_private * ep;
	struct in_addr broadaddr;

	/* Fetch the broadcast address for this interface */
	if (if_get_broadaddr(ifn, &broadaddr) < 0) {
		/* this function whines about problems... */
		return (NULL);
	}
	
	/*
	 * We now have all the information we need.  Populate our
	 * structure with the information we've gotten.
	 */

	ep = (struct ip_private *)MALLOC(sizeof(struct ip_private));
	if (ep == NULL)  {
		return(NULL);
	}
	memset(ep, 0, sizeof(*ep));

	ep->bcast = broadaddr;

	ep->interface = (char *)STRDUP(ifn);
	if(ep->interface == NULL) {
		FREE(ep);
		return(NULL);
	}
	
	memset(&ep->addr, 0, sizeof(ep->addr));	/* zero the struct */
	ep->addr.sin_family = AF_INET;		/* host byte order */
	ep->addr.sin_port = htons(port);	/* short, network byte order */
	ep->port = port;
	ep->wsocket = -1;
	ep->rsocket = -1;
	ep->addr.sin_addr = ep->bcast;
	return(ep);
}


/*
 * ha_if.c - code that extracts information about a network interface
 *
 * See the linux ifconfig source code for more examples.
 *
 * Works on HP_UX 10.20, freebsd, linux rh6.2
 * Works on solaris or Unixware (SVR4) with:
 *   gcc -DBSD_COMP -c ha_if.c
 * Doesn't seem to work at all on Digital Unix (?)
 *
 * Author: Eric Z. Ayers <eric.ayers@compgen.com>
 *
 * Copyright (C) 2000 Computer Generation Incorporated
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 */



#include <sys/ioctl.h>


#ifdef HAVE_SYS_SOCKIO_H
#	include <sys/sockio.h>
#endif


/*
  if_get_broadaddr
     Retrieve the ipv4 broadcast address for the specified network interface.

  Inputs:  ifn - the name of the network interface:
                 e.g. eth0, eth1, ppp0, plip0, plusb0 ...
  Outputs: broadaddr - returned broadcast address.

  Returns: 0 on success
           -1 on failure - sets errno.
 */
int
if_get_broadaddr(const char *ifn, struct in_addr *broadaddr)
{
	int		return_val;
	int		fd = -1;
	struct ifreq ifr; /* points to one interface returned from ioctl */

	fd = socket (PF_INET, SOCK_DGRAM, 0);
	
	if (fd < 0) {
		PILCallLog(LOG, PIL_CRIT
		,	"Error opening socket for interface %s: %s"
		,	ifn, strerror(errno));
		return -1;
	}
	
	strncpy (ifr.ifr_name, ifn, sizeof(ifr.ifr_name));

	/* Fetch the broadcast address of this interface by calling ioctl() */
	return_val = ioctl(fd,SIOCGIFBRDADDR, &ifr);
	
	if (return_val == 0 ) {
		if (ifr.ifr_broadaddr.sa_family == AF_INET) {
			struct sockaddr_in sin_ptr;
			
			memcpy(&sin_ptr, &ifr.ifr_broadaddr,
				sizeof(sin_ptr));
			
			memcpy(broadaddr, &sin_ptr.sin_addr,
				sizeof(*broadaddr));
			
			/* leave return_val set to 0 to return success! */
		}else{
			PILCallLog(LOG, PIL_CRIT
			,	"Wrong family for broadcast interface %s: %s"
			,	ifn, strerror(errno));
			return_val = -1;
		}
		
	}else{
		PILCallLog(LOG, PIL_CRIT
		,	"Get broadcast for interface %s failed: %s"
		,	ifn, strerror(errno));
		return_val = -1;
	}
	
	close (fd);

	return return_val;
	
} /* end if_get_broadaddr() */

