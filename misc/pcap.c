/* OpenVAS Libraries
 * Copyright (C) 1999 Renaud Deraison
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the Free
 * Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <netinet/in.h>
#include <resolv.h>
#include <pcap.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <netdb.h>
#ifdef HAVE_SYS_SOCKIO_H
#include <sys/sockio.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bpf_share.h"
#include "pcap_openvas.h"
#include "system_internal.h"
#include "network.h"
#include "config.h"

#define MAXROUTES 1024


struct interface_info {
    char name[64];
    struct in_addr addr;
    struct in6_addr addr6;
    struct in6_addr mask;
};

struct myroute {
  struct interface_info *dev;
  struct in6_addr dest6;
  unsigned long mask;
  unsigned long dest;
};

struct interface_info *getinterfaces(int *howmany);
struct interface_info *v6_getinterfaces(int *howmany);
int getipv6routes(struct myroute *myroutes, int *numroutes);

static void ipv6addrmask(struct in6_addr *in6addr, int mask)
{
  int wordmask;
  int word;
  uint32_t *ptr;
  uint32_t addr;

  word = mask / 32;
  wordmask = mask % 32;
  ptr = (uint32_t *)in6addr;
  switch(word)
  {
    case 0:
      ptr[1] = ptr[2] = ptr[3] = 0;
      addr = ptr[0];
      addr = ntohl(addr) >> (32 - wordmask);
      addr = htonl(addr << (32 - wordmask));
      ptr[0] = addr;
      break;
    case 1:
      ptr[2] = ptr[3] = 0;
      addr = ptr[1];
      addr = ntohl(addr) >> (32 - wordmask);
      addr = htonl(addr << (32 - wordmask));
      ptr[1] = addr;
      break;
    case 2:
      ptr[3] = 0;
      addr = ptr[2];
      addr = ntohl(addr) >> (32 - wordmask);
      addr = htonl(addr << (32 - wordmask));
      ptr[2] = addr;
      break;
    case 3:
      addr = ptr[3];
      addr = ntohl(addr) >> (32 - wordmask);
      addr = htonl(addr << (32 - wordmask));
      ptr[3] = addr;
      break;
  }
}

int
v6_is_local_ip (struct in6_addr *addr)
{
  int ifaces;
  struct interface_info * ifs;
  int i;
  static struct myroute myroutes[MAXROUTES];
  int numroutes=0;
  struct in6_addr in6addr;
#if TCPIP_DEBUGGING
  char addr1[INET6_ADDRSTRLEN];
  char addr2[INET6_ADDRSTRLEN];
#endif

  if ((ifs = v6_getinterfaces(&ifaces)) == NULL)
    return -1;

  if(IN6_IS_ADDR_V4MAPPED(addr))
  {
    for(i=0;i<ifaces;i++)
    {
      bpf_u_int32 net, mask;
      char errbuf[PCAP_ERRBUF_SIZE];
      pcap_lookupnet(ifs[i].name, &net, &mask, errbuf);
      if((net & mask) == (addr->s6_addr32[3] & mask))
        return 1;
    }
  }
  else
  {
    if(IN6_IS_ADDR_LINKLOCAL(addr))
      return 1;
    if(IN6_IS_ADDR_LOOPBACK(addr))
      return 1;
    if(getipv6routes(myroutes, &numroutes) == 0)
    {
      for(i=0; i < numroutes; i++)
      {
        memcpy(&in6addr, addr, sizeof(struct in6_addr));
        ipv6addrmask(&in6addr, myroutes[i].mask);
#if TCPIP_DEBUGGING
        printf("comparing addresses %s and %s\n",inet_ntop(AF_INET6, &in6addr,addr1,sizeof(addr1)),inet_ntop(AF_INET6, &myroutes[i].dest6,addr2,sizeof(addr2)));
#endif
        if(IN6_ARE_ADDR_EQUAL(&in6addr, &myroutes[i].dest6))
        {
          return 1;
        }
      }
    }
  }
  return 0;
}

int
is_local_ip (struct in_addr addr)
{
 int ifaces;
 struct interface_info * ifs;
 int i;

 if ((ifs = getinterfaces(&ifaces)) == NULL) 
 	return -1;
 for(i=0;i<ifaces;i++)
 {
  bpf_u_int32 net, mask;
  char errbuf[PCAP_ERRBUF_SIZE];
  pcap_lookupnet(ifs[i].name, &net, &mask, errbuf);
  if((net & mask) == (addr.s_addr & mask))
  	return 1;
 }
 return 0;
}

/**
 * @brief We send an empty UDP packet to the remote host, and read back its mac
 * @brief address.
 *
 * (we should first interrogate the kernel's arp cache - we may
 * rely on libdnet in the future to do that)
 *
 * As a bonus, this function works well as a local ping.
 */
int
v6_get_mac_addr (struct in6_addr *addr, char ** mac)
{
  int soc;
  struct sockaddr_in soca;
  struct sockaddr_in6 soca6;
  int bpf;
  struct in6_addr me;
  struct in_addr inaddr;
  char * iface = v6_routethrough(addr, &me);
  char filter[255];
  char * src_host, * dst_host;
  unsigned char * packet;
  int len;
  char hostname[INET6_ADDRSTRLEN];

  if(IN6_IS_ADDR_V4MAPPED(addr))
  {
    soc = socket(AF_INET, SOCK_DGRAM, 0);
    *mac = NULL;
    if(soc < 0)
      return -1;

    inaddr.s_addr = me.s6_addr32[3];
    src_host = estrdup(inet_ntoa(inaddr));
    inaddr.s_addr = addr->s6_addr32[3];
    dst_host = estrdup(inet_ntoa(inaddr));
    snprintf(filter, sizeof(filter), "ip and (src host %s and dst host %s)",
        src_host, dst_host);
    efree(&src_host);
    efree(&dst_host);

    bpf = bpf_open_live(iface, filter);
    if(bpf < 0)
    {
      close(soc);
      return -1;
    }

    /*
     * We only deal with ethernet
     */
    if(bpf_datalink(bpf) != DLT_EN10MB)
    {
      bpf_close(bpf);
      close(soc);
      return -1;
    }


    soca.sin_addr.s_addr = addr->s6_addr32[3];
    soca.sin_port = htons(9); /* or whatever */
    soca.sin_family = AF_INET;
    if(sendto(soc, NULL, 0, 0, (struct sockaddr*)&soca, sizeof(soca)) != 0)
    {
      bpf_close(bpf);
      close(soc);
      return -1;
    }
  }
  else
  {
    soc = socket(AF_INET6, SOCK_DGRAM, 0);
    *mac = NULL;
    if(soc < 0)
      return -1;
    src_host = estrdup(inet_ntop(AF_INET6, &me, hostname, sizeof(hostname)));
    dst_host = estrdup(inet_ntop(AF_INET6, addr, hostname, sizeof(hostname)));
    snprintf(filter, sizeof(filter), "ip6 and (src host %s and dst host %s)",
        src_host, dst_host);
    efree(&src_host);
    efree(&dst_host);


    bpf = bpf_open_live(iface, filter);
    if(bpf < 0)
    {
      close(soc);
      return -1;
	  }

    /*
     * We only deal with ethernet
     */
    if(bpf_datalink(bpf) != DLT_EN10MB)
    {
      bpf_close(bpf);
      close(soc);
      return -1;
    }


    memcpy(&soca6.sin6_addr, addr, sizeof(struct in6_addr));
    soca6.sin6_port = htons(9); /* or whatever */
    soca6.sin6_family = AF_INET6;
    if(sendto(soc, NULL, 0, 0, (struct sockaddr*)&soca6, sizeof(soca6)) != 0)
    {
      bpf_close(bpf);
      close(soc);
      return -1;
    }
  }

  packet = (unsigned char*)bpf_next(bpf, &len);
  if(packet)
  {
    if(len >= get_datalink_size(bpf_datalink(bpf)))
    {
      int i;
      for(i=0;i<6;i++)
        if(packet[i]!=0xFF)break;

      if(i == 6)
      {
        bpf_close(bpf);
        close(soc);
        return 1;
      }

      *mac = emalloc(22);
      snprintf(*mac, 22, "%.2x.%.2x.%.2x.%.2x.%.2x.%.2x",
          (unsigned char)packet[0],
          (unsigned char)packet[1],
          (unsigned char)packet[2],
          (unsigned char)packet[3],
          (unsigned char)packet[4],
          (unsigned char)packet[5]);
      bpf_close(bpf);
      close(soc);
      return 0;
    }
  }
  else
  {
    bpf_close(bpf);
    close(soc);
    return 1;
  }
  return 1; /* keep the compiler happy */
}


/**
 * @brief We send an empty UDP packet to the remote host, and read back its mac
 * @brief address.
 *
 * (we should first interrogate the kernel's arp cache - we may
 * rely on libdnet in the future to do that)
 *
 * As a bonus, this function works well as a local ping.
 */
int
get_mac_addr (struct in_addr addr, char ** mac)
{
 int soc = socket(AF_INET, SOCK_DGRAM, 0);
 struct sockaddr_in soca;
 int bpf;
 struct in_addr me;
 char * iface = routethrough(&addr, &me);
 char filter[255];
 char * src_host, * dst_host;
 unsigned char * packet;
 int len;

 *mac = NULL;
 if(soc < 0)
  return -1;

 src_host = estrdup(inet_ntoa(me));
 dst_host = estrdup(inet_ntoa(addr));
 snprintf(filter, sizeof(filter), "ip and (src host %s and dst host %s)",
 	src_host, dst_host);
 efree(&src_host);
 efree(&dst_host);


 bpf = bpf_open_live(iface, filter);
 if(bpf < 0)
  {
  close(soc);
  return -1;
  }

 /*
  * We only deal with ethernet
  */
 if(bpf_datalink(bpf) != DLT_EN10MB)
 {
  bpf_close(bpf);
  close(soc);
  return -1;
 }


 soca.sin_addr.s_addr = addr.s_addr;
 soca.sin_port = htons(9); /* or whatever */
 soca.sin_family = AF_INET;
 if(sendto(soc, NULL, 0, 0, (struct sockaddr*)&soca, sizeof(soca)) == 0)
 {
  packet = (unsigned char*)bpf_next(bpf, &len);
  if(packet)
  {
   if(len >= get_datalink_size(bpf_datalink(bpf)))
   {
    int i;
    for(i=0;i<6;i++)
    	if(packet[i]!=0xFF)break;

    if(i == 6)
    {
     bpf_close(bpf);
     close(soc);
     return 1;
    }

    *mac = emalloc(22);
    snprintf(*mac, 22, "%.2x.%.2x.%.2x.%.2x.%.2x.%.2x",
    		(unsigned char)packet[0],
		(unsigned char)packet[1],
		(unsigned char)packet[2],
		(unsigned char)packet[3],
		(unsigned char)packet[4],
		(unsigned char)packet[5]);
   bpf_close(bpf);
   close(soc);
   return 0;
   }
  }
  else
  {
   bpf_close(bpf);
   close(soc);
   return 1;
  }
 }
 bpf_close(bpf);
 close(soc);
 return -1;
}


/*
 * Taken straight out of Fyodor's Nmap
 */
int
v6_ipaddr2devname (char *dev, int sz, struct in6_addr *addr )
{
  struct interface_info *mydevs;
  int numdevs;
  int i;
  mydevs = v6_getinterfaces(&numdevs);
#if TCPIP_DEBUGGING
  char addr1[INET6_ADDRSTRLEN];
  char addr2[INET6_ADDRSTRLEN];
#endif

  if (!mydevs) return -1;

  for(i=0; i < numdevs; i++)
  {
#if TCPIP_DEBUGGING
    printf("comparing addresses %s and %s\n",inet_ntop(AF_INET6, addr,addr1,sizeof(addr1)),inet_ntop(AF_INET6, &mydevs[i].addr6,addr2,sizeof(addr2)));
#endif
    if(IN6_ARE_ADDR_EQUAL(addr, &mydevs[i].addr6))
    {
      dev[sz - 1] = '\0';
      strncpy(dev, mydevs[i].name, sz);
      return 0;
    }
  }
  return -1;
}

/*
 * Taken straight out of Fyodor's Nmap
 */
int
ipaddr2devname (char *dev, int sz, struct in_addr *addr )
{
  struct interface_info *mydevs;
  int numdevs;
  int i;
  mydevs = getinterfaces(&numdevs);

  if (!mydevs) return -1;

  for(i=0; i < numdevs; i++) {
    if (addr->s_addr == mydevs[i].addr.s_addr) {
      dev[sz - 1] = '\0';
      strncpy(dev, mydevs[i].name, sz);
      return 0;
    }
  }
  return -1;
}

/**
 * @brief Tests whether a packet sent to IP is LIKELY to route through the
 * kernel localhost interface
 */
int
v6_islocalhost (struct in6_addr *addr)
{
  char dev[128];

  if(addr == NULL)
    return -1;

  if(IN6_IS_ADDR_V4MAPPED(addr))
  {
    /* If it is 0.0.0.0 or starts with 127.0.0.1 then it is
       probably localhost */
    if ((addr->s6_addr32[3] & htonl(0xFF000000)) == htonl(0x7F000000))
      return 1;

    if (!addr->s6_addr32[3])
      return 1;
  }

  if(IN6_IS_ADDR_LOOPBACK(addr))
    return 1;

  /* If it is the same addy as a local interface, then it is
     probably localhost */

  if (v6_ipaddr2devname(dev, sizeof(dev), addr) != -1)
    return 1;

  /* OK, so to a first approximation, this addy is probably not
     localhost */
  return 0;
}

/**
 * @brief Tests whether a packet sent to IP is LIKELY to route through the
 * kernel localhost interface
 */
int
islocalhost (struct in_addr *addr)
{
  char dev[128];

  if(addr == NULL)
  	return -1;

  /* If it is 0.0.0.0 or starts with 127.0.0.1 then it is 
     probably localhost */
  if ((addr->s_addr & htonl(0xFF000000)) == htonl(0x7F000000))
    return 1;

  if (!addr->s_addr)
    return 1;

  /* If it is the same addy as a local interface, then it is
     probably localhost */

  if (ipaddr2devname(dev, sizeof(dev), addr) != -1)
    return 1;

  /* OK, so to a first approximation, this addy is probably not
     localhost */
  return 0;
}

int
get_datalink_size (int datalink)
{
 int offset = -1;
 switch(datalink) {
  case DLT_EN10MB: offset = 14; break;
  case DLT_IEEE802: offset = 22; break;
  case DLT_NULL: offset = 4; break;
  case DLT_SLIP:
#if (FREEBSD || OPENBSD || NETBSD || BSDI || DARWIN)
    offset = 16;
#else
    offset = 24; /* Anyone use this??? */
#endif
    break;
  case DLT_PPP: 
#if (FREEBSD || OPENBSD || NETBSD || BSDI || DARWIN)
    offset = 4;
#else
#ifdef SOLARIS
    offset = 8;
#else
    offset = 24; /* Anyone use this? */
#endif /* ifdef solaris */
#endif /* if freebsd || openbsd || netbsd || bsdi */
    break;
  case DLT_RAW: offset = 0; break;
  }
  return(offset);
}

int
get_random_bytes (void *buf, int numbytes)
{
  static char bytebuf[2048];
  static char badrandomwarning = 0;
  static int bytesleft = 0;
  int res;
  int tmp;
  struct timeval tv;
  FILE *fp = NULL;
  int i;
  short *iptr;

  if (numbytes < 0 || numbytes > 0xFFFF) return -1;

  if (bytesleft == 0) {
    fp = fopen("/dev/urandom", "r");
    if (!fp) fp = fopen("/dev/random", "r");
    if (fp) {
      res = fread(bytebuf, 1, sizeof(bytebuf), fp);
      if (res != sizeof(bytebuf)) {
        fclose(fp);
        fp = NULL;
      }
      bytesleft = sizeof(bytebuf);
    }
    if (!fp) {
      if (badrandomwarning == 0) {
        badrandomwarning++;
      }
      /* Seed our random generator */
      gettimeofday(&tv, NULL);
      srand((tv.tv_sec ^ tv.tv_usec) ^ getpid()); /* RATS: ignore */

      for(i=0; i < sizeof(bytebuf) / sizeof(short); i++) {
        iptr = (short *) ((char *)bytebuf + i * sizeof(short));
        *iptr = rand();
      }
      bytesleft = (sizeof(bytebuf) / sizeof(short)) * sizeof(short);
      /*    ^^^^^^^^^^^^^^^not as meaningless as it looks  */
    } else fclose(fp);
  }
  if (numbytes <= bytesleft) { /* we can cover it */
    memcpy(buf, bytebuf + (sizeof(bytebuf) - bytesleft), numbytes);
    bytesleft -= numbytes;
    return 0;
  }

  /* We don't have enough */
  memcpy(buf, bytebuf + (sizeof(bytebuf) - bytesleft), bytesleft);
  tmp = bytesleft;
  bytesleft = 0;
  return get_random_bytes((char *)buf + tmp, numbytes - tmp);
}

struct interface_info *v6_getinterfaces(int *howmany)
{
  char errbuf[PCAP_ERRBUF_SIZE];
  pcap_if_t *alldevap;
  pcap_if_t *tmp;
  int retval;
  pcap_addr_t *addr;
  struct sockaddr *sa;
  struct sockaddr_in *saddr;
  struct sockaddr_in6 *s6addr;
  static struct interface_info mydevs[1024];
  int numinterfaces = 0;

  #ifdef TCPIP_DEBUGGING
    char ipaddr[INET6_ADDRSTRLEN];
  #endif

  memset(errbuf, 0, sizeof(errbuf));

  retval = pcap_findalldevs(&alldevap, errbuf);
  if(retval == -1)
  {
    printf("pcap_findalldevs returned error %s\n",errbuf);
  }
  else
  {
    tmp = alldevap;
    while(tmp)
    {
      if(tmp->addresses)
      {
        addr = tmp->addresses;
        while(addr)
        {
          sa = addr->addr;
          if(sa->sa_family == AF_INET)
          {
            memcpy(mydevs[numinterfaces].name,tmp->name, strlen(tmp->name));
            saddr = (struct sockaddr_in *) sa;
            mydevs[numinterfaces].addr6.s6_addr32[0] = 0;
            mydevs[numinterfaces].addr6.s6_addr32[1] = 0;
            mydevs[numinterfaces].addr6.s6_addr32[2] = htonl(0xffff);
            mydevs[numinterfaces].addr6.s6_addr32[3] = saddr->sin_addr.s_addr;
            saddr = (struct sockaddr_in *) addr->netmask;
            mydevs[numinterfaces].mask.s6_addr32[0] = 0;
            mydevs[numinterfaces].mask.s6_addr32[1] = 0;
            mydevs[numinterfaces].mask.s6_addr32[2] = htonl(0xffff);
            mydevs[numinterfaces].mask.s6_addr32[3] = saddr->sin_addr.s_addr;
#ifdef TCPIP_DEBUGGING
	          printf("interface name is %s\n",tmp->name);
            printf("\tAF_INET family\n");
            printf("\taddress is %s\n",inet_ntoa(saddr->sin_addr));
            printf("\tnetmask is %s\n",inet_ntoa(saddr->sin_addr));
#endif
            numinterfaces++;
          }
          else if(sa->sa_family == AF_INET6)
          {
            memcpy(mydevs[numinterfaces].name,tmp->name, strlen(tmp->name));
            s6addr = (struct sockaddr_in6 *) sa;
            memcpy(&(mydevs[numinterfaces].addr6), (char *) &(s6addr->sin6_addr), sizeof(struct in6_addr));
            s6addr = (struct sockaddr_in6 *) addr->netmask;
            memcpy(&(mydevs[numinterfaces].mask), (char *) &(s6addr->sin6_addr), sizeof(struct in6_addr));
            numinterfaces++;
#ifdef TCPIP_DEBUGGING
            printf("\tAF_INET6 family\n");
	          printf("interface name is %s\n",tmp->name);
            printf("\taddress is %s\n",inet_ntop(AF_INET6, &s6addr->sin6_addr, ipaddr, sizeof(ipaddr)));
            printf("\tnetmask is %s\n",inet_ntop(AF_INET6, &s6addr->sin6_addr, ipaddr, sizeof(ipaddr)));
#endif
          }
          else
          {
#ifdef TCPIP_DEBUGGING
            printf("\tfamily is %d\n",sa->sa_family);
#endif
          }
          addr = addr->next;
        }
      }
      tmp = tmp->next;
    }
    *howmany = numinterfaces;
  }
  return mydevs;
}

struct interface_info*
getinterfaces (int *howmany)
{
  static struct interface_info mydevs[1024];
  int numinterfaces = 0;
  int sd;
  int len;
  char *p;
  char buf[10240];
  struct ifconf ifc;
  struct ifreq *ifr;
  struct sockaddr_in *sin;

    /* Dummy socket for ioctl */
    sd = socket(AF_INET, SOCK_DGRAM, 0);
    bzero(buf, sizeof(buf));
    if (sd < 0) printf("socket in getinterfaces");
    ifc.ifc_len = sizeof(buf);
    ifc.ifc_buf = buf;
    if (ioctl(sd, SIOCGIFCONF, &ifc) < 0) {
      printf("Failed to determine your configured interfaces!\n");
    }
    close(sd);
    ifr = (struct ifreq *) buf;
    if (ifc.ifc_len == 0)
      printf("getinterfaces: SIOCGIFCONF claims you have no network interfaces!\n");
#ifdef HAVE_SOCKADDR_SA_LEN
    len = ifr->ifr_addr.sa_len;
#else
#ifdef HAVE_STRUCT_IFMAP
    len = sizeof(struct ifmap);
#else
    len = sizeof(struct sockaddr);
#endif
#endif
    for(; ifr && *((char *)ifr) && ((char *)ifr) < buf + ifc.ifc_len; 
    /* FIXME: for the next source code line the gentoo packaging process
     * reports the following problem (disregard the line number):
     * QA Notice: Package has poor programming practices which may compile
     *            fine but exhibit random runtime failures.
     * pcap.c:342: warning: dereferencing type-punned pointer will break strict-aliasing rules
     */
	((*(char **)&ifr) +=  sizeof(ifr->ifr_name) + len )) {
      sin = (struct sockaddr_in *) &ifr->ifr_addr;
      memcpy(&(mydevs[numinterfaces].addr), (char *) &(sin->sin_addr), sizeof(struct in_addr));
      /* In case it is a stinkin' alias */
      if ((p = strchr(ifr->ifr_name, ':')))
	*p = '\0';
      strncpy(mydevs[numinterfaces].name, ifr->ifr_name, 63);
      mydevs[numinterfaces].name[63] = '\0';
      numinterfaces++;
      if (numinterfaces == 1023)  {      
	printf("My god!  You seem to have WAY too many interfaces!  Things may not work right\n");
	break;
      }
#if HAVE_SOCKADDR_SA_LEN
      /* len = MAX(sizeof(struct sockaddr), ifr->ifr_addr.sa_len);*/
      len = ifr->ifr_addr.sa_len;
#endif
      mydevs[numinterfaces].name[0] = '\0';
  }
  if (howmany) *howmany = numinterfaces;
  return mydevs;
}

int
v6_getsourceip (struct in6_addr *src, struct in6_addr *dst)
{
  int sd;
  struct sockaddr_in sock;
  struct sockaddr_in6 sock6;
  unsigned int socklen;
  unsigned short p1;

  #ifdef TCPIP_DEBUGGING
    char name[INET6_ADDRSTRLEN];
  #endif

  if(IN6_IS_ADDR_V4MAPPED(dst))
    *src = socket_get_next_source_v4_addr(NULL);
  else
    *src = socket_get_next_source_v6_addr(NULL);
  if(!IN6_ARE_ADDR_EQUAL(src,&in6addr_any ))
  {
   return 1;
  }

  get_random_bytes(&p1, 2);
  if (p1 < 5000) p1 += 5000;

  if(IN6_IS_ADDR_V4MAPPED(dst))
  {
    if ((sd = socket(AF_INET, SOCK_DGRAM, 0)) == -1)
    {
      perror("Socket troubles");
      return 0;
    }
    sock.sin_family = AF_INET;
    sock.sin_addr.s_addr = dst->s6_addr32[3];
    sock.sin_port = htons(p1);
    if (connect(sd, (struct sockaddr *) &sock, sizeof(struct sockaddr_in)) == -1)
    {
      perror("UDP connect()");
      close(sd);
      return 0;
    }
    bzero(&sock, sizeof(struct sockaddr_in));
    socklen = sizeof(struct sockaddr_in);
    if (getsockname(sd, (struct sockaddr *)&sock, &socklen) == -1) {
      perror("getsockname");
      close(sd);
      return 0;
    }


    src->s6_addr32[0] = 0;
    src->s6_addr32[1] = 0;
    src->s6_addr32[2] = htonl(0xffff);
    src->s6_addr32[3] = sock.sin_addr.s_addr;
#ifdef TCPIP_DEBUGGING
    printf("source addrss is %s\n",inet_ntop(AF_INET6, src, name, sizeof(name)));
#endif
    close(sd);
  }
  else
  {
    if ((sd = socket(AF_INET6, SOCK_DGRAM, 0)) == -1)
    {
      perror("Socket troubles");
      return 0;
    }
    sock6.sin6_family = AF_INET6;
    sock6.sin6_addr.s6_addr32[0] = dst->s6_addr32[0];
    sock6.sin6_addr.s6_addr32[1] = dst->s6_addr32[1];
    sock6.sin6_addr.s6_addr32[2] = dst->s6_addr32[2];
    sock6.sin6_addr.s6_addr32[3] = dst->s6_addr32[3];
    sock6.sin6_port = htons(p1);
    if (connect(sd, (struct sockaddr *) &sock6, sizeof(struct sockaddr_in6)) == -1)
    {
      perror("UDP connect()");
      close(sd);
      return 0;
    }
    bzero(&sock6, sizeof(struct sockaddr_in6));
    socklen = sizeof(struct sockaddr_in6);
    if (getsockname(sd, (struct sockaddr *)&sock6, &socklen) == -1) {
      perror("getsockname");
      close(sd);
      return 0;
    }

    src->s6_addr32[0] = sock6.sin6_addr.s6_addr32[0];
    src->s6_addr32[1] = sock6.sin6_addr.s6_addr32[1];
    src->s6_addr32[2] = sock6.sin6_addr.s6_addr32[2];
    src->s6_addr32[3] = sock6.sin6_addr.s6_addr32[3];
    memcpy(src,&sock6.sin6_addr, sizeof(struct in6_addr));
#ifdef TCPIP_DEBUGGING
    printf("source addrss is %s\n",inet_ntop(AF_INET6, src, name, sizeof(name)));
#endif
    close(sd);
  }
  return 1; /* Calling function responsible for checking validity */
}

int
getsourceip (struct in_addr *src, struct in_addr *dst)
{
  int sd;
  struct sockaddr_in sock;
  unsigned int socklen = sizeof(struct sockaddr_in);
  unsigned short p1;


  *src = socket_get_next_source_addr(NULL);
  if ( src->s_addr != INADDR_ANY )
  {
   return 1;
  }

  get_random_bytes(&p1, 2);
  if (p1 < 5000) p1 += 5000;

  if ((sd = socket(AF_INET, SOCK_DGRAM, 0)) == -1)
    {perror("Socket troubles"); return 0;}
  sock.sin_family = AF_INET;
  sock.sin_addr = *dst;
  sock.sin_port = htons(p1);
  if (connect(sd, (struct sockaddr *) &sock, sizeof(struct sockaddr_in)) == -1)
    { perror("UDP connect()");
    close(sd);
    return 0;
    }
  bzero(&sock, sizeof(struct sockaddr_in));
  if (getsockname(sd, (struct sockaddr *)&sock, &socklen) == -1) {
    perror("getsockname");
    close(sd);
    return 0;
  }

  src->s_addr = sock.sin_addr.s_addr;
  close(sd);
  return 1; /* Calling function responsible for checking validity */
}

int getipv4routes(struct myroute *myroutes, int *numroutes)
{
  struct interface_info *mydevs;
  int i;
  int numinterfaces;
  char buf[1024];
  char *p, *endptr;
  char iface[64];
  FILE *routez;
  unsigned long dest;
  struct in_addr inaddr;
  unsigned long mask;
  unsigned long ones;

  /* Dummy socket for ioctl */
  mydevs = v6_getinterfaces(&numinterfaces);

  /* Now we must go through several techniques to determine info */
  routez = fopen("/proc/net/route", "r");

  if (routez) {
    /* OK, linux style /proc/net/route ... we can handle this ... */
    /* Now that we've got the interfaces, we g0 after the r0ut3Z */
    fgets(buf, sizeof(buf), routez); /* Kill the first line */
    while(fgets(buf,sizeof(buf), routez)) {
      p = strtok(buf, " \t\n");
      if (!p) {
        printf("Could not find interface in /proc/net/route line");
        continue;
      }
      strncpy(iface, p, sizeof(iface));
      if ((p = strchr(iface, ':'))) {
        *p = '\0'; /* To support IP aliasing */
      }
      p = strtok(NULL, " \t\n");
      endptr = NULL;
      dest = strtoul(p, &endptr, 16);
#ifdef TCPIP_DEBUGGING
      printf("ipv4 dest is %s\n",p);
#endif
      if (!endptr || *endptr) {
        printf("Failed to determine Destination from /proc/net/route");
        continue;
      }
      inaddr.s_addr = dest;
      myroutes[*numroutes].dest6.s6_addr32[0] = 0;
      myroutes[*numroutes].dest6.s6_addr32[1] = 0;
      myroutes[*numroutes].dest6.s6_addr32[2] = htonl(0xffff);
      myroutes[*numroutes].dest6.s6_addr32[3] = inaddr.s_addr;
      for(i=0; i < 6; i++) {
        p = strtok(NULL, " \t\n");
        if (!p) break;
      }
      if (!p) {
        printf("Failed to find field %d in /proc/net/route", i + 2);
        continue;
      }
      endptr = NULL;
      mask = strtoul(p, &endptr, 16);
      i = 31;
      ones = 0;
      i = 0;
      while(mask & (1 << i++) && i < 32)
        ones++;
      myroutes[*numroutes].mask = ones + 96;
#ifdef TCPIP_DEBUGGING
      printf("mask is %d\n",myroutes[*numroutes].mask);
#endif
      if (!endptr || *endptr) {
        printf("Failed to determine mask from /proc/net/route");
        continue;
      }


#if TCPIP_DEBUGGING
      printf("#%d: for dev %s, The dest is %lX and the mask is %lX\n", *numroutes, iface, myroutes[*numroutes].dest, myroutes[*numroutes].mask);
#endif
      for(i=0; i < numinterfaces; i++)
        if (!strcmp(iface, mydevs[i].name)) {
          myroutes[*numroutes].dev = &mydevs[i];
          break;
        }
      if (i == numinterfaces)
        printf("Failed to find interface %s mentioned in /proc/net/route\n", iface);
      (*numroutes)++;
      if (*numroutes >= MAXROUTES)
      {
        printf("My god!  You seem to have WAY to many routes!\n");
        break;
      }
    }
    fclose(routez);
    return 0;
  }
  else
    return -1;
}

int getipv6routes(struct myroute *myroutes, int *numroutes)
{
  struct interface_info *mydevs;
  int i,j;
  int len;
  struct in6_addr in6addr;
  char destaddr[100];
  int numinterfaces;
  char buf[1024];
  char  *endptr;
  char iface[64];
  FILE *routez;
  char v6addr[INET6_ADDRSTRLEN];
  char *token;
  int cnt;

  /* Dummy socket for ioctl */
  mydevs = v6_getinterfaces(&numinterfaces);
  routez = fopen("/proc/net/ipv6_route","r");
  if(routez)
  {
    /* linux style /proc/net/ipv6_route ... we can handle this too... */
    while(fgets(buf, sizeof(buf),routez) != NULL)
    {
#if TCPIP_DEBUGGING
      printf("%s\n",buf);
#endif
      token = strtok(buf, " \t\n");
      if(token)
      {
#if TCPIP_DEBUGGING
        printf("first token is %s\n",token);
#endif
        strcpy(destaddr, token);
	      len = strlen(destaddr);
        for(i = 0,j = 0; j < len; j++)
        {
          v6addr[i++] = destaddr[j];
          if(j % 4 == 3)
            v6addr[i++] = ':';
        }
        v6addr[--i] = '\0';
#if TCPIP_DEBUGGING
        printf("ipv6 dest is %s\n",v6addr);
#endif
        if(inet_pton(AF_INET6, v6addr, &in6addr) <= 0)
        {
          printf("invalid ipv6 addressd\n");
          continue;
        }
        memcpy(&myroutes[*numroutes].dest6,&in6addr, sizeof(struct in6_addr));
      }
      token = strtok(NULL, " \t\n");
      if(token)
      {
        endptr = NULL;
        myroutes[*numroutes].mask = strtoul(token, &endptr, 16);
      }
      cnt = 7;
      while(cnt--)
      {
        token = strtok(NULL, " \t\n");
        if(!token)
          printf("error\n");
      }

      token = strtok(NULL, " \t\n");
      if(token)
      {
        strcpy(iface, token);
#ifdef _DEBUG
        printf("name token is %s\n",token);
#endif
      }
      for(i=0; i < numinterfaces; i++)
        if (!strcmp(iface, mydevs[i].name) && !IN6_IS_ADDR_V4MAPPED(&mydevs[i].addr6)) {
          myroutes[*numroutes].dev = &mydevs[i];
          break;
        }
      if (i == numinterfaces)
        printf("Failed to find interface %s mentioned in /proc/net/route\n", iface);
      (*numroutes)++;
      if (*numroutes >= MAXROUTES)
      {
        printf("My god!  You seem to have WAY to many routes!\n");
        break;
      }
    }
    fclose(routez);
    return 0;
  }
  else
  {
    printf("returning error getipv6route\n");
    return -1;
  }
}

/** @brief An awesome function to determine what interface a packet to a given
 *  destination should be routed through.
 *
 * It returns NULL if no appropriate
 *  interface is found, oterwise it returns the device name and fills in the
 *   source parameter.   Some of the stuff is
 *  from Stevens' Unix Network Programming V2.  He had an easier suggestion
 *  for doing this (in the book), but it isn't portable :(
 */
char*
v6_routethrough (struct in6_addr *dest, struct in6_addr *source)
{
  static int initialized = 0;
  int i;
  struct in6_addr addy;
  static enum { procroutetechnique, connectsockettechnique, guesstechnique } technique = procroutetechnique;
  struct interface_info *mydevs;
  static struct myroute myroutes[MAXROUTES];
  int numinterfaces = 0;
  static int numroutes = 0;
  struct in6_addr in6addr;
#ifdef TCPIP_DEBUGGING
  char addr1[INET6_ADDRSTRLEN];
  char addr2[INET6_ADDRSTRLEN];
#endif
  struct in6_addr src;

  *source = in6addr_any;

  if (!dest) printf("ipaddr2devname passed a NULL dest address");

  if(IN6_IS_ADDR_V4MAPPED(dest))
    src = socket_get_next_source_v4_addr(NULL);
  else
    src = socket_get_next_source_v6_addr(NULL);

  if (!initialized) {
    /* Dummy socket for ioctl */
    initialized = 1;
    mydevs = v6_getinterfaces(&numinterfaces);
    if(getipv4routes(myroutes, &numroutes) < 0)
    {
      if(getipv6routes(myroutes, &numroutes) < 0)
        technique = connectsockettechnique;
    }
    if(getipv6routes(myroutes, &numroutes) < 0)
      technique = connectsockettechnique;
  } else {
    mydevs = v6_getinterfaces(&numinterfaces);
  }
  /* WHEW, that takes care of initializing, now we have the easy job of
     finding which route matches */
  if (v6_islocalhost(dest))
  {
    if (source)
    {
      if(IN6_IS_ADDR_V4MAPPED(source))
      {
        source->s6_addr32[0] = 0;
        source->s6_addr32[1] = 0;
        source->s6_addr32[2] = htonl(0xffff);
        source->s6_addr32[3] = htonl(0x7F000001);
      }
      else
      {
        source->s6_addr32[0] = 0;
        source->s6_addr32[1] = 0;
        source->s6_addr32[2] = 0;
        source->s6_addr32[3] = htonl(1);
      }
    }
    /* Now we find the localhost interface name, assuming 127.0.0.1
       or ::1 is localhost (it damn well better be!)... */
    for(i=0; i < numinterfaces; i++)
    {
      if(IN6_IS_ADDR_V4MAPPED(&mydevs[i].addr6))
      {
        if (mydevs[i].addr6.s6_addr32[3] == htonl(0x7F000001))
          return mydevs[i].name;
      }
      else
      {
        if(IN6_ARE_ADDR_EQUAL(&in6addr_any, &mydevs[i].addr6))
          return mydevs[i].name;
      }
    }
    return NULL;
  }

  if (technique == procroutetechnique)
  {
    for(i=0; i < numroutes; i++) {
      memcpy(&in6addr, dest, sizeof(struct in6_addr));
      ipv6addrmask(&in6addr, myroutes[i].mask);
#if TCPIP_DEBUGGING
      printf("comparing addresses %s and %s\n",inet_ntop(AF_INET6, &in6addr,addr1,sizeof(addr1)),inet_ntop(AF_INET6, &myroutes[i].dest6,addr2,sizeof(addr2)));
#endif
      if(IN6_ARE_ADDR_EQUAL(&in6addr, &myroutes[i].dest6))
      {
        if (source)
        {
          if (!IN6_ARE_ADDR_EQUAL(&src, &in6addr_any))
            memcpy(source, &src, sizeof(struct in6_addr));
          else
          {
#if TCPIP_DEBUGGING
	    printf("copying address %s\n",inet_ntop(AF_INET6,&myroutes[i].dev->addr6,addr1,sizeof(addr1)));
	    printf("dev name is %s\n",myroutes[i].dev->name);
#endif
            memcpy(source,&myroutes[i].dev->addr6, sizeof(struct in6_addr));
          }
        }
        return myroutes[i].dev->name;
      }
    }
  } else if (technique == connectsockettechnique) {
    if (!v6_getsourceip(&addy, dest))
      return NULL;
    if(IN6_ARE_ADDR_EQUAL(&addy, &in6addr))
    {
      struct hostent *myhostent = NULL;
      char myname[MAXHOSTNAMELEN + 1];

      myhostent = gethostbyname(myname);
      if (gethostname(myname, MAXHOSTNAMELEN) ||
          !myhostent)
        printf("Cannot get hostname!  Try using -S <my_IP_address> or -e <interface to scan through>\n");
      if(myhostent->h_addrtype == AF_INET)
      {
        addy.s6_addr32[0] = 0;
        addy.s6_addr32[1] = 0;
        addy.s6_addr32[2] = htonl(0xffff);
        memcpy(&addy.s6_addr32[0], myhostent->h_addr_list[0], sizeof(struct in6_addr));
      }
      else
        memcpy(&addy, myhostent->h_addr_list[0], sizeof(struct in6_addr));
    }

    /* Now we insure this claimed address is a real interface ... */
    for(i=0; i < numinterfaces; i++)
    {
#ifdef TCPIP_DEBUGGING
      printf("comparing addresses %s and %s\n",inet_ntop(AF_INET6, &mydevs[i].addr6,addr1,sizeof(addr1)),inet_ntop(AF_INET6, &addy,addr2,sizeof(addr2)));
#endif
      if (IN6_ARE_ADDR_EQUAL(&mydevs[i].addr6, &addy))
      {
        if (source)
        {
            memcpy(source,&addy, sizeof(struct in6_addr));
        }
        return mydevs[i].name;
      }
    }
    return NULL;
  } else
    printf("I know sendmail technique ... I know rdist technique ... but I don't know what the hell kindof technique you are attempting!!!");
    return NULL;
}

/** @brief An awesome function to determine what interface a packet to a given
 *  destination should be routed through.
 *
 * It returns NULL if no appropriate
 *  interface is found, oterwise it returns the device name and fills in the
 *   source parameter.   Some of the stuff is
 *  from Stevens' Unix Network Programming V2.  He had an easier suggestion
 *  for doing this (in the book), but it isn't portable :(
 */
char*
routethrough (struct in_addr *dest, struct in_addr *source)
{
  static int initialized = 0;
  int i;
  struct in_addr addy;
  static enum { procroutetechnique, connectsockettechnique, guesstechnique } technique = procroutetechnique;
  char buf[10240];
  struct interface_info *mydevs;
  static struct myroute {
    struct interface_info *dev;
    unsigned long mask;
    unsigned long dest;
  } myroutes[MAXROUTES];
  int numinterfaces = 0;
  char *p, *endptr;
  char iface[64];
  static int numroutes = 0;
  FILE *routez;

  struct in_addr src = socket_get_next_source_addr(NULL);

  if (!dest) printf("ipaddr2devname passed a NULL dest address");

  if (!initialized) {
    /* Dummy socket for ioctl */
    initialized = 1;
    mydevs = getinterfaces(&numinterfaces);

    /* Now we must go through several techniques to determine info */
    routez = fopen("/proc/net/route", "r");

    if (routez) {
      /* OK, linux style /proc/net/route ... we can handle this ... */
      /* Now that we've got the interfaces, we g0 after the r0ut3Z */
      fgets(buf, sizeof(buf), routez); /* Kill the first line */
      while(fgets(buf,sizeof(buf), routez)) {
        p = strtok(buf, " \t\n");
        if (!p) {
          printf("Could not find interface in /proc/net/route line");
          continue;
        }
        strncpy(iface, p, sizeof(iface));
        if ((p = strchr(iface, ':'))) {
          *p = '\0'; /* To support IP aliasing */
        }
        p = strtok(NULL, " \t\n");
        endptr = NULL;
        myroutes[numroutes].dest = strtoul(p, &endptr, 16);
        if (!endptr || *endptr) {
          printf("Failed to determine Destination from /proc/net/route");
          continue;
        }
        for(i=0; i < 6; i++) {
          p = strtok(NULL, " \t\n");
          if (!p) break;
        }
        if (!p) {
          printf("Failed to find field %d in /proc/net/route", i + 2);
          continue;
        }
        endptr = NULL;
        myroutes[numroutes].mask = strtoul(p, &endptr, 16);
        if (!endptr || *endptr) {
          printf("Failed to determine mask from /proc/net/route");
          continue;
        }


#if TCPIP_DEBUGGING
        printf("#%d: for dev %s, The dest is %lX and the mask is %lX\n", numroutes, iface, myroutes[numroutes].dest, myroutes[numroutes].mask);
#endif
        for(i=0; i < numinterfaces; i++)
          if (!strcmp(iface, mydevs[i].name)) {
            myroutes[numroutes].dev = &mydevs[i];
            break;
          }
        if (i == numinterfaces)
          printf("Failed to find interface %s mentioned in /proc/net/route\n", iface);
        numroutes++;
        if (numroutes >= MAXROUTES)
        {
          printf("My god!  You seem to have WAY to many routes!\n");
          break;
        }
      }
      fclose(routez);
    } else {
      technique = connectsockettechnique;
    }
  } else {
    mydevs = getinterfaces(&numinterfaces);
  }
  /* WHEW, that takes care of initializing, now we have the easy job of
     finding which route matches */
  if (islocalhost(dest)) {
    if (source)
      source->s_addr = htonl(0x7F000001);
    /* Now we find the localhost interface name, assuming 127.0.0.1 is
       localhost (it damn well better be!)... */
    for(i=0; i < numinterfaces; i++) {
      if (mydevs[i].addr.s_addr == htonl(0x7F000001)) {
        return mydevs[i].name;
      }
    }
    return NULL;
  }

  if (technique == procroutetechnique) {
    for(i=0; i < numroutes; i++) {
      if ((dest->s_addr & myroutes[i].mask) == myroutes[i].dest) {
        if (source) {
          if ( src.s_addr != INADDR_ANY )
            source->s_addr = src.s_addr;
          else
            source->s_addr = myroutes[i].dev->addr.s_addr;
        }
        return myroutes[i].dev->name;
      }
    }
  } else if (technique == connectsockettechnique) {
    if (!getsourceip(&addy, dest))
      return NULL;
    if (!addy.s_addr)  {  /* Solaris 2.4 */
      struct hostent *myhostent = NULL;
      char myname[MAXHOSTNAMELEN + 1];
#if defined(USE_PTHREADS) && defined(HAVE_GETHOSTBYNAME_R)
      int Errno = 0;
      char * buf = emalloc(4096);
      struct hostent * res = NULL;
      struct hostent * t = NULL;

      myhostent = emalloc(sizeof(struct hostent));
#ifdef HAVE_SOLARIS_GETHOSTBYNAME_R
      gethostbyname_r(myname, myhostent, buf, 4096, &Errno);
      if(Errno){
        free(myhostent);
        myhostent = NULL;
      }
#else
      gethostbyname_r(myname, myhostent, buf, 4096, &res, &Errno);
      t = myhostent;
      myhostent = res;
#endif /* HAVE_SOLARIS_... */
      myhostent = res;
#else
      myhostent = gethostbyname(myname);
#endif /* USE_PTHREADS     */
      if (gethostname(myname, MAXHOSTNAMELEN) ||
          !myhostent)
        printf("Cannot get hostname!  Try using -S <my_IP_address> or -e <interface to scan through>\n");
      memcpy(&(addy.s_addr), myhostent->h_addr_list[0], sizeof(struct in_addr));
#if defined(USE_PTHREADS) && defined(HAVE_GETHOSTBYNAME_R)
      if(myhostent)free(myhostent);
      free(buf);
#endif
    }

    /* Now we insure this claimed address is a real interface ... */
    for(i=0; i < numinterfaces; i++)
      if (mydevs[i].addr.s_addr == addy.s_addr) {
        if (source) {
          source->s_addr = addy.s_addr;
        }
        return mydevs[i].name;
      }
    return NULL;
  } else
    printf("I know sendmail technique ... I know rdist technique ... but I don't know what the hell kindof technique you are attempting!!!");
    return NULL;
}
