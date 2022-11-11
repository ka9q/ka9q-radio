// $Id: aprs.c,v 1.26 2021/10/28 20:53:06 karn Exp $
// Process AX.25 frames containing APRS data, extract lat/long/altitude, compute az/el
// INCOMPLETE, doesn't yet drive antenna rotors
// Should also use RTP for AX.25 frames
// Should get station location from a GPS receiver
// Copyright 2018, Phil Karn, KA9Q

#define _GNU_SOURCE 1
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <locale.h>
#include <errno.h>
#include <ctype.h>
#include <sys/socket.h>
#include <math.h>
#include <time.h>
#if defined(linux)
#include <bsd/string.h>
#endif

#include "misc.h"
#include "multicast.h"
#include "ax25.h"

char *Mcast_address_text = "ax25.local:5004";
char *Source = NULL;
char *Dest = "127.0.0.1:4533";

double const WGS84_E = 0.081819190842622;  // Eccentricity
double const WGS84_A = 6378137;         // Equatorial radius, meters

const char *App_path;
int Verbose;
int Input_fd = -1;

#define square(x) ((x)*(x))

char *parse_timestamp(char *data,int *days,int *hours, int *minutes, int *seconds);
char *parse_position(char *data,double *latitude,double *longitude,double *altitude);
char *parse_mice_position(struct ax25_frame *frame,char *data,double *latitude, double *longitude);

int main(int argc,char *argv[]){
  App_path = argv[0];
  setlocale(LC_ALL,getenv("LANG"));
  setlinebuf(stdout);

  double latitude,longitude,altitude;

#if 1
  // Use defaults - KA9Q location, be sure to change elsewhere!!
  // KA9Q
  latitude = 32.8604;
  longitude = -117.1889;
  altitude = 0;
#elif 0
  // MCHSARC
  latitude = 32.967233;
  longitude = -117.122382;
  altitude = 200;
#else
  // UCSD Atkinson Hall
  latitude = 32.8825852;
  longitude = -117.2347093;
  altitude = 144; // estimate

#endif
    
  int c;
  while((c = getopt(argc,argv,"L:M:A:I:vs:R:")) != EOF){
    switch(c){
    case 'L':
      latitude = strtod(optarg,NULL);
      break;
    case 'M':
      longitude = strtod(optarg,NULL);
      break;
    case 'A':
      altitude = strtod(optarg,NULL);
      break;
    case 's':
      Source = optarg;
      break;
    case 'v':
      Verbose++;
      break;
    case 'I':
      Mcast_address_text = optarg;
      break;
    case 'R':
      Dest = optarg;
      break;
    default:
      fprintf(stdout,"Usage: %s [-L latitude] [-M longitude] [-A altitude] [-s sourcecall] [-v] [-I mcast_address]\n",argv[0]);
      fprintf(stdout,"Defaults: %s -L %lf -M %lf -A %lf -s %s -I %s\n",argv[0],
	      latitude,longitude,altitude,Source,Mcast_address_text);
      exit(1);
    }
  }
  fprintf(stdout,"APRS az/el program by KA9Q\n");

  // Open connection to rotctld
  int rot_fd; // File descriptor to connection
  FILE *rot_fp;
  struct addrinfo *results = NULL;
  {
    struct addrinfo hints;
    memset(&hints,0,sizeof(hints));
    hints.ai_family = PF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags = AI_ADDRCONFIG | AI_NUMERICSERV;
    
    // If no domain zone is specified, assume .local (i.e., for multicast DNS)
    char full_host[PATH_MAX+6];
    if(strchr(Dest,'.') == NULL)
      snprintf(full_host,sizeof(full_host),"%s.local",Dest);
    else
      strlcpy(full_host,Dest,sizeof(full_host));
    
    char *port;
    if((port = strrchr(Dest,':')) != NULL)
      *port++ = '\0';
    else
      port = "4533"; // Default for rotctld
    

    int const ecode = getaddrinfo(full_host,port,&hints,&results);
    if(ecode != 0){
      fprintf(stderr,"rotctl connect getaddrinfo(%s,%s): %s\n",full_host,port,gai_strerror(ecode));
      exit(1);
    }
    // Use first entry on list -- much simpler
    // I previously tried each entry in turn until one succeeded, but with UDP sockets and
    // flags set to only return supported addresses, how could any of them fail?
    struct sockaddr_storage rotctl;
    struct sockaddr *sock = (struct sockaddr *)&rotctl;

    memcpy(&rotctl,results->ai_addr,results->ai_addrlen);
    freeaddrinfo(results); results = NULL;

    if((rot_fd = socket(sock->sa_family,SOCK_STREAM,0)) == -1){
      perror("rotor socket");
      exit(1);
    }      
    if(connect(rot_fd,(struct sockaddr *)&rotctl,sizeof(rotctl)) == -1){
      perror("rotor socket connect");
      exit(1);
    }
    rot_fp = fdopen(rot_fd,"w+");
    if(rot_fp == NULL){
      perror("rotor fdopen");
      exit(1);
    }
  }
  if(Source){
    fprintf(stdout,"Watching for %s\n",Source);
  } else {
    fprintf(stdout,"Watching all stations\n");
  }
  fprintf(stdout,"Station coordinates: latitude %.6lf deg; longitude %.6lf deg; altitude %.1lf m\n",
	 latitude,longitude,altitude);

  // Station position in earth-centered ROTATING coordinate system
  double station_x,station_y,station_z;
  // Unit vectors defining station's orientation
  double up_x,up_y,up_z;
  double south_x,south_y,south_z;  
  double east_x,east_y,east_z;
  
  {
    double sinlat,coslat;
    sincos(RAPDEG*latitude,&sinlat,&coslat);
    double sinlong,coslong;
    sincos(RAPDEG*longitude,&sinlong,&coslong);
    
    double tmp = WGS84_A/sqrt(1-(square(WGS84_E)*square(sinlat)));
    station_x = (tmp + altitude) * coslat * coslong;
    station_y = (tmp + altitude) * coslat * sinlong;
    station_z = (tmp*(1-square(WGS84_E)) + altitude) * sinlat;
    
    // Zenith vector is (coslong*coslat, sinlong*coslat, sinlat)
    up_x = coslong * coslat;
    up_y = sinlong * coslat;
    up_z = sinlat;

    east_x = -sinlong;
    east_y = coslong;
    east_z = 0;

    south_x = coslong*sinlat;
    south_y = sinlong*sinlat;
    south_z = -(sinlong*sinlong*sinlat + coslong*coslong*coslat);
  }    

  // Set up multicast input
  struct sockaddr_storage sock;
  resolve_mcast(Mcast_address_text,&sock,DEFAULT_RTP_PORT,NULL,0);
  Input_fd = listen_mcast(&sock,NULL);

  if(Input_fd == -1){
    fprintf(stdout,"Can't set up input from %s\n",
	    Mcast_address_text);
    exit(1);
  }
  unsigned char packet[2048];
  int size;

  while((size = recv(Input_fd,packet,sizeof(packet),0)) > 0){
    struct rtp_header rtp_header;
    unsigned char const *dp = packet;

    dp = ntoh_rtp(&rtp_header,dp);
    size -= dp - packet;

    if(rtp_header.pad){
      // Remove padding
      size -= dp[size-1];
      rtp_header.pad = 0;
    }

    if(size <= 0)
      continue; // Bogus RTP header?

    if(rtp_header.type != AX25_PT)
      continue; // Wrong type

    struct ax25_frame frame;
    if(ax25_parse(&frame,dp,size) < 0)
      continue;      // Unparseable AX25 header

    // Is this the droid we're looking for?
    if(Source != NULL && strncasecmp(frame.source,Source,sizeof(frame.source)) != 0)
      continue; // Nope

    time_t t;
    struct tm *tmp;
    time(&t);
    tmp = gmtime(&t);
    fprintf(stdout,"%d %s %04d %02d:%02d:%02d UTC",tmp->tm_mday,Months[tmp->tm_mon],tmp->tm_year+1900,
	   tmp->tm_hour,tmp->tm_min,tmp->tm_sec);
    
    fprintf(stdout," ssrc %u seq %d",rtp_header.ssrc,rtp_header.seq);
    fprintf(stdout," %s:",frame.source);
    if(frame.control != 0x03 || frame.type != 0xf0){
      fprintf(stdout," Invalid ax25 type");
      goto done;
    }
    frame.information[frame.info_len] = '\0'; // Ensure termination
    chomp(frame.information);

    char *data = frame.information; // First byte of text field
    // Extract lat/long
    
    // Parse APRS position packets
    // The APRS spec is an UNBELIEVABLE FUCKING MESS THAT SHOULD BE SHOT, SHREDDED, BURNED AND SENT TO HELL!
    // There, now I feel a little better. But not much.
    double latitude=NAN,longitude=NAN,altitude=NAN;
    int hours=-1, minutes=-1,days=-1,seconds=-1;
    
    // Sample WB8ELK LU1ESY-3>APRS,TCPIP*,qAS,WB8ELK:/180205h3648.75S/04627.50WO000/000/A=039566 2 4.50 25 12060 GF63SE 1N7MSE 226
    // Sample PITS "!/%s%sO   /A=%06ld|%s|%s/%s,%d'C,http://www.pi-in-the-sky.com",
    
    switch(*data){
    case '/':
    case '@':
      // process timestamp
      data++;
      data = parse_timestamp(data,&days,&hours,&minutes,&seconds);
      // Process position
      data = parse_position(data,&latitude,&longitude,&altitude);
      break;
    case '!':
    case '=':
      // Position, no timestamp
      data++;
      if(*data == '!'){
	// Weather data, not position
	fprintf(stdout," %s",frame.information);
	goto done;
      }
      data = parse_position(data,&latitude,&longitude,&altitude);
      break;
    case '`': // back tick 0x60
    case '\'': // forward tick 0x27
      // MIC-E format: latitude is in dest callsign (!!)
      data = parse_mice_position(&frame,data,&latitude,&longitude);
      break;
    case '$': // NMEA sentence (to be implemented)
    default:
      // Status, telemetry, etc
      fprintf(stdout," %s",frame.information);
      goto done; // No more processing
    }
    if(days != -1 || hours != -1 || minutes != -1 || seconds != -1)
      fprintf(stdout," %d %02d:%02d:%02d;",days,hours,minutes,seconds);


    if(!isnan(latitude) && !isnan(longitude)){
      fprintf(stdout," Lat %.6lf Long %.6lf",latitude,longitude);

      int altitude_known = 0;
      if(!isnan(altitude)){
	altitude_known = 1;
	fprintf(stdout," Alt %.1lf m",altitude);
      } else
	altitude = 0;

      fputc(';',stdout);
      double target_x,target_y,target_z;
      {
	double sinlat,coslat;
	sincos(RAPDEG*latitude,&sinlat,&coslat);
	double sinlong,coslong;
	sincos(RAPDEG*longitude,&sinlong,&coslong);
	
	double tmp = WGS84_A/sqrt(1-(square(WGS84_E)*square(sinlat))); // Earth radius under target
	target_x = (tmp + altitude) * coslat * coslong;
	target_y = (tmp + altitude) * coslat * sinlong;
	target_z = (tmp*(1-square(WGS84_E)) + altitude) * sinlat;
      }
      double look_x,look_y,look_z;
      look_x = target_x - station_x;
      look_y = target_y - station_y;
      look_z = target_z - station_z;      
      double range = sqrt(square(look_x)+square(look_y)+square(look_z));
      
      double south = (south_x * look_x + south_y * look_y + south_z * look_z) / range;
      double east = (east_x * look_x + east_y * look_y + east_z * look_z) / range;
      double up = (up_x * look_x + up_y * look_y + up_z * look_z) / range;
      
      double elevation = asin(up);
      double azimuth = M_PI - atan2(east,south);
      
      if(altitude_known){
	fprintf(stdout," az %.1lf elev %.1lf range %'.1lf m",
	       azimuth*DEGPRA, elevation*DEGPRA,range);
	fprintf(rot_fp,"\\set_pos %.1lf %.1lf\n",azimuth*DEGPRA,elevation*DEGPRA);
	fflush(rot_fp);		
      } else {
	fprintf(stdout," az %.1lf range %'.1lf m",
	       azimuth*DEGPRA, range);
	fprintf(rot_fp,"\\set_pos %.1lf %.1lf\n",azimuth*DEGPRA,0.0);
	fflush(rot_fp);
      }
    }
  done:;
    fputc('\n',stdout);
  }
}  
char *parse_timestamp(char *data,int *days,int *hours, int *minutes, int *seconds){
  // process timestamp
  char *ncp = NULL;
  int t = strtol(data,&ncp,10);
  switch(*ncp){
  case 'h':
    // Hours, minutes, seconds
    *days = 0;
    *hours = t / 10000;
    t -= *hours * 10000;
    *minutes = t / 100;
    t -= *minutes * 100;
    *seconds = t;
    break;
  case 'z':
    // day, hours minutes zulu
    *days = t / 10000;
    t -= *days * 10000;
    *hours = t / 100;
    t -= *hours * 100;
    *minutes = t;
    *seconds = 0;
    break;
  case '/':
    // day, hours, minutes local -- HOW AM I SUPPOSED TO KNOW THE TIME ZONE ??!?!?
    *days = t / 10000;
    t -= *days * 10000;
    *hours = t / 100;
    t -= *hours * 100;
    *minutes = t;
    *seconds = 0;
    break;
  default:
    return NULL;
    break;
  }
  return ncp+1;
}

char *parse_position(char *data,double *latitude,double *longitude,double *altitude){
  if(data == NULL)
    return NULL;
  if(*data == '=')
    data++;
     
  if(*data == '/' || *data == '!'){
    // Compressed
    data++; // skip /
    *latitude = 90 - decode_base91(data)/380926.;
    *longitude = -180 + decode_base91(data+4) / 190463.;
    data += 12;
    return data;
  } else if(isdigit(*data)){
    // Uncompressed
    char *ncp = NULL;
    *latitude = strtod(data,&ncp) / 100.;
    *latitude = (int)(*latitude) + fmod(*latitude,1.0) / 0.6;
    if(tolower(*ncp) == 's')
      *latitude *= -1;
    data = ncp + 2; // Skip S and /
    *longitude = strtod(data,&ncp) / 100.;
    *longitude = (int)(*longitude) + fmod(*longitude,1.0) / 0.6;
    if(tolower(*ncp) == 'w')
      *longitude *= -1;
    data = ncp + 2; // Skip W and /
    // Look for A=
    while(*data != '\0' && *(data+1) != '\0'){
      if(*data == 'A' && data[1] == '='){
	*altitude = strtol(data+2,&ncp,10) * 0.3048; // in meters
	break;
      } else
	data++;
    }
    return data;
  } else
    return NULL;
}
char *parse_mice_position(struct ax25_frame *frame,char *data,double *latitude, double *longitude){
  if(frame == NULL || data == NULL)
    return NULL;

  {
    int deg = (frame->dest[0] & 0xf) * 10 + (frame->dest[1] & 0xf);
    int minutes = (frame->dest[2] & 0xf) * 10 + (frame->dest[3] & 0xf);
    int hun_mins = (frame->dest[4] & 0xf) * 10 + (frame->dest[5] & 0xf);
    *latitude = deg + minutes/60. + hun_mins / 6000.;
  }
  // longitude is in I field (did I say how incredibly painfully ugly this is??)
  data++;
  {
    int deg = *data++ - 28;
    if(180 <= deg && deg <= 189)
      deg -= 80;
    else if(190 <= deg && deg <= 199)
      deg -= 190;
    if(frame->dest[4] & 0x40)
      deg += 100;
    
    int minutes = *data++ - 28;
    if(minutes > 60)
      minutes -= 60;
    int hun_mins = *data++ - 28;
    
    *longitude = deg + minutes / 60. + hun_mins / 6000.;
  }
  if(frame->dest[3] & 0x40)
    *longitude *= -1;
  return data;
}




