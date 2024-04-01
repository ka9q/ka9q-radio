// Brand new version that just forks and execs the commands 'avahi-browse' or 'avahi-publish'
// Using the Avahi API is a nightmare; it's a twisted mess of callbacks between "callbacks", "services", "resolvers", etc.
// March 2024, Phil Karn KA9Q
#include <stdio.h>
#include <unistd.h>
#include <arpa/inet.h>
#include "avahi.h"

int avahi_start(char const *service_name,char const *service_type,int const service_port,char const *dns_name,int address,char const *description,void *sock,int *socksize){
  int pid = getpid();
  if(fork() == 0){
#if 0
    fprintf(stdout,"avahi-publish-service child pid %d\n",getpid());
#endif
    // run avahi-publish-service --no-fail --host=dns_name service_name service_type service_port description pid hostname in subprocess
    char port_string[1024];
    snprintf(port_string, sizeof(port_string), "%d", service_port);

    char host_string[1024];
    snprintf(host_string, sizeof(host_string), "--host=%s",dns_name);

    char pid_string[1024];
    snprintf(pid_string, sizeof(pid_string), "pid=%d", pid);

    char hostname[sysconf(_SC_HOST_NAME_MAX)];
    gethostname(hostname,sizeof(hostname));
    char source_string[1024];
    snprintf(source_string, sizeof(source_string), "source=%s", hostname);

#if 0
    fprintf(stdout,"%s %s %s %s %s %s %s %s %s %s\n",
	    "avahi-publish-service", "avahi-publish-service", "--no-fail", host_string, service_name, service_type, port_string, description, pid_string, hostname);
#endif
    execlp("avahi-publish-service", "avahi-publish-service", "--no-fail", host_string, service_name, service_type, port_string, description, pid_string, hostname, NULL);
    perror("exec avahi publish service");
    return -1;
  }
  char ip_address_string[1024];
  snprintf(ip_address_string,sizeof(ip_address_string),"%d.%d.%d.%d",(address >> 24) & 0xff, (address >> 16) & 0xff, (address >> 8) & 0xff, address & 0xff);
#if 0
  fprintf(stdout,"avahi start: ip address string = %s\n",ip_address_string);
#endif

  if(sock != NULL && socksize != NULL){
    // Return sockaddr structure
    if(*socksize >= sizeof(struct sockaddr_in)){
      struct sockaddr_in *sin = sock;
      sin->sin_family = AF_INET;
      sin->sin_addr.s_addr = htonl(address);
      sin->sin_port = htons(service_port);
      *socksize = sizeof(struct sockaddr_in);
    } else
      *socksize = 0;
  }

  if(fork() == 0){
    fprintf(stdout,"avahi-publish-address child pid %d\n",getpid());
    // run "avahi-publish-address dns_name address"
#if 0
    fprintf(stdout,"%s %s %s %s\n",
	    "avahi-publish-address", "avahi-publish-address",dns_name,ip_address_string);
#endif
    execlp("avahi-publish-address", "avahi-publish-address",dns_name,ip_address_string, NULL);
    perror("exec avahi publish address");
    return -1;
  }

  return 0;
}
