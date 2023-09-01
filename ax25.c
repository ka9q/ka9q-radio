// AX.25 frame header decoding (this takes me wayyyyy back)
// Copyright 2018, Phil Karn, KA9Q
#define _GNU_SOURCE 1
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>

#include "ax25.h"


// Retrieve callsign field from AX.25 header
// return pointer to string of form "KA9Q-11" in user-provided buffer which must be at least 10 bytes long
char *get_callsign(char *result,uint8_t const *in){
  char callsign[7],c;
  
  memset(callsign,0,sizeof(callsign));
  for(int i=0;i<6;i++){
    c = in[i] >> 1;
    if(c == ' ')
      break;
    callsign[i] = c;
  }
  int ssid = (in[6] >> 1) & 0xf;
  if(ssid != 0)
    snprintf(result,10,"%s-%d",callsign,ssid);
  else
    snprintf(result,10,"%s",callsign);    
  return result;
}

// Dump an AX.25 frame to standard output
// Decode address headers as source -> digi1 -> digi2 -> dest
// show currently transmitting station in UPPER CASE
// show type and control field
// dump entire frame in hex/ascii
int dump_frame(FILE *stream,uint8_t *frame,int bytes){

  // By default, no digipeaters; will update if there are any
  uint8_t *control = frame + 14;

  // Source address
  // Is this the transmitter?
  int this_transmitter = 1;
  int digipeaters = 0;
  // Look for digipeaters
  if(!(frame[13] & 1)){
    // Scan digipeater list; have any repeated it?
    for(int i=0;i<8;i++){
      int digi_ssid = frame[20 + 7*i];
      
      digipeaters++;
      if(digi_ssid & 0x80){
	// Yes, passed this one, keep looking
	this_transmitter = 2 + i;
      }
      if(digi_ssid & 1)
	break; // Last digi
    }
  }
  // Show source address, in upper case if this is the transmitter, otherwise lower case
  for(int n=7; n < 13; n++){
    char c = frame[n] >> 1;
    if(c == ' ')
      break;
    if(this_transmitter == 1)
      fputc(toupper(c),stream);
    else
      fputc(tolower(c),stream);
  }
  int ssid = (frame[13] >> 1) & 0xf; // SSID
  if(ssid > 0)
    fprintf(stream,"-%d",ssid);
  
  fprintf(stream," -> ");
  
  // List digipeaters

  if(!(frame[13] & 0x1)){
    // Digipeaters are present
    for(int i=0; i<digipeaters; i++){
      for(int k=0;k<6;k++){
	char c = (frame[14 + 7*i + k] >> 1) & 0x7f;
	if(c == ' ')
	  break;
	
	if(this_transmitter == 2+i)
	  fputc(toupper(c),stream);
	else
	  fputc(tolower(c),stream);
      }
      int ssid = frame[14 + 7*i + 6];
      if(ssid > 0)
	fprintf(stream,"-%d",(ssid>> 1) & 0xf); // SSID
      fprintf(stream," -> ");
      if(ssid  & 0x1){ // Last one
	control = frame + 14 + 7*i + 7;
	break;
      }
    }
  }
  // NOW print destination, in lower case since it's not the transmitter
  for(int n=0; n < 6; n++){
    char c = (frame[n] >> 1) & 0x7f;
    if(c == ' ')
      break;
    fputc(tolower(c),stream);
  }
  ssid = (frame[6] >> 1) & 0xf; // SSID
  if(ssid > 0)
    fprintf(stream,"-%d",ssid);

  // Type field
  fprintf(stream,"; control = %02x",*control++ & 0xff);
  fprintf(stream,"; type = %02x\n",*control & 0xff);

  for(int i = 0; i < bytes; i++){
    fprintf(stream,"%02x ",frame[i] & 0xff);
    if((i % 16) == 15 || i == bytes-1){
      for(int k = (i % 16); k < 15; k++)
	fprintf(stream,"   "); // blanks as needed in last line

      fprintf(stream," |  ");
      for(int k=(i & ~0xf );k <= i; k++){
	if(frame[k] >= 0x20 && frame[k] < 0x7e)
	  fputc(frame[k],stream);
	else
	  fputc('.',stream);
      }
      fputc('\n',stream);
    }
  }
  fputc('\n',stream);
  return 0;
}

// Check 16-bit AX.25 standard CRC-CCITT on frame
// return 1 if good, 0 otherwise
int crc_good(uint8_t *frame,int length){
  unsigned int const crc_poly = 0x8408;
	
  uint16_t crc = 0xffff;
  while(length-- > 0){
    uint8_t byte = *frame++;
    for(int i=0; i < 8; i++){
      uint16_t feedback = 0;
      if((crc ^ byte) & 1)
	feedback = crc_poly;

      crc = (crc >> 1) ^ feedback;
      byte >>= 1;
    }
  }
  return(crc == 0xf0b8); // Note comparison
}

// Base 91 encoding used by APRS
int decode_base91(char *in){
  int result = 0;

  for(int i=0;i<4;i++)
    result = 91 * result + in[i] - 33;
  return result;
}

// Break an incoming AX.25 frame into its parts
int ax25_parse(struct ax25_frame *out,uint8_t const *in,int len){
  if(len < 16) // Frame length NOT including CRC
    return -1; // Too short

  // Find end of address field
  int ctl_offs;
  for(ctl_offs=0; ctl_offs<len; ctl_offs++){
    if(in[ctl_offs] & 1)
      break;
  }
  if(ctl_offs == len)
    return -1; // Can't find end of address field!

  ctl_offs++;
  // Determine number of digipeaters
  if((ctl_offs % 7) != 0)
    return -1; // Addresses must be multiples of 7 bytes long

  out->ndigi = (ctl_offs / 7) - 2;

  get_callsign(out->source,in+7);
  get_callsign(out->dest,in+0);

  // Process digipeaters, if any
  for(int i=0; i<out->ndigi; i++){
    if(i >= MAX_DIGI)
      return -1; // too many!
    get_callsign(out->digipeaters[i].name,in+7*(2+i));
    if(in[7*(2+i)+6] & 0x80)
      out->digipeaters[i].h = 1;
    else
      out->digipeaters[i].h = 0;      
  }
  out->control = in[ctl_offs];
  out->type = in[ctl_offs+1];
  out->info_len = len - (ctl_offs+2) - 2; // drop ctl/type before, crc after

  if(out->info_len > sizeof(out->information))
    return -1;

  memcpy(out->information,in+ctl_offs+2,out->info_len);
  return 0;
}
