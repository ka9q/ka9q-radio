// Low-level encode/decode routines for TLV status packets from/to ka9q-radio's radiod
// Copyright 2020-2023 Phil Karn, KA9Q

#define _GNU_SOURCE 1
#include <assert.h>
#include <string.h>
#if defined(linux)
#include <bsd/string.h>
#endif
#include <math.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <stdlib.h>
#include <stdint.h>
#include <netdb.h>

#include "misc.h"
#include "status.h"
#include "radio.h"

union result {
  uint64_t ll;
  uint32_t l;
  float f;
  double d;
};


// Encode 64-bit integer, big endian, leading zeroes suppressed
// The nice thing about big-endian encoding with suppressed leading zeroes
// is that all (unsigned) integer types can be easily encoded
// by simply casting them to uint64_t, without wasted space
int encode_int64(uint8_t **buf,enum status_type type,uint64_t x){
  uint8_t *cp = *buf;

  *cp++ = (uint8_t)type;

  if(x == 0){
    // Compress zero value to zero length
    *cp++ = 0;
    *buf = cp;
    return 2;
  }

  int len = sizeof(x);
  while(len > 0 && ((x >> 56) == 0)){
    x <<= 8;
    len--;
  }
  *cp++ = (uint8_t)len;

  for(int i=0; i<len; i++){
    *cp++ = x >> 56;
    x <<= 8;
  }

  *buf = cp;
  return 2+len;
}


// Special case: single null type byte means end of list
int encode_eol(uint8_t **buf){
  uint8_t *bp = *buf;

  *bp++ = EOL;
  *buf = bp;
  return 1;
}

int encode_bool(uint8_t **buf,enum status_type type,bool x){
  return encode_byte(buf,type,(uint8_t)x);
}

int encode_byte(uint8_t **buf,enum status_type type,uint8_t x){
  uint8_t *cp = *buf;
  *cp++ = (uint8_t)type;
  if(x == 0){
    // Compress zero value to zero length
    *cp++ = 0;
    *buf = cp;
    return 2;
  }
  *cp++ = sizeof(x);
  *cp++ = x;
  *buf = cp;
  return 2+sizeof(x);
}

int encode_int16(uint8_t **buf,enum status_type type,uint16_t x){
  return encode_int64(buf,type,(uint64_t)x);
}

int encode_int32(uint8_t **buf,enum status_type type,uint32_t x){
  return encode_int64(buf,type,(uint64_t)x);
}

int encode_int(uint8_t **buf,enum status_type type,int x){
  return encode_int64(buf,type,(uint64_t)x);
}


// Floating types are also byte-swapped to big-endian order
// Intentionally accepts a double so callers don't need to cast them
int encode_float(uint8_t **buf,enum status_type type,double x){
  if(isnan(x))
    return 0; // Never encode a NAN

  union result r;
  r.f = (float)x;
  return encode_int32(buf,type,r.l);
}

int encode_double(uint8_t **buf,enum status_type type,double x){
  if(isnan(x))
    return 0; // Never encode a NAN

  union result r;
  r.d = x;
  return encode_int64(buf,type,r.ll);
}

// Encode byte string without byte swapping
size_t encode_string(uint8_t **bp,enum status_type const type,void const *buf,size_t const buflen){
  uint8_t const *orig_bpp = *bp;
  uint8_t *cp = *bp;
  *cp++ = (uint8_t)type;

  if(buflen < 128){
    // send length directly
    *cp++ = (uint8_t)buflen;
  } else if(buflen < 65536){
    // Length is 2 bytes, big endian
    *cp++ = 0x80 | 2;
    *cp++ = (uint8_t)(buflen >> 8);
    *cp++ = (uint8_t)buflen;
  } else if(buflen < 16777216){
    *cp++ = 0x80 | 3;
    *cp++ = (uint8_t)(buflen >> 16);
    *cp++ = (uint8_t)(buflen >> 8);
    *cp++ = (uint8_t)buflen;
  } else { // Handle more than 4 GB??
    *cp++ = 0x80 | 4;
    *cp++ = (uint8_t)(buflen >> 24);
    *cp++ = (uint8_t)(buflen >> 16);
    *cp++ = (uint8_t)(buflen >> 8);
    *cp++ = (uint8_t)buflen;
  }
  memcpy(cp,buf,buflen);
  cp += buflen;
  *bp = cp;
  return cp - orig_bpp;
}
// Unique to spectrum energies
// array -> vector of 32-bit floats
// size = number of floats
// Sent in big endian order just like other floats
// Because it can be very long, handle large sizes
size_t encode_vector(uint8_t **bp,enum status_type type,float const *array,size_t size){
  uint8_t const *orig_bp = *bp;
  uint8_t *cp = *bp;
  *cp++ = (uint8_t)type;

  size_t const bytes = sizeof(*array) * size; // Number of bytes in data
  if(bytes < 128){
    *cp++ = (uint8_t)bytes;    // Send length directly
  } else if(bytes < 65536){
    *cp++ = 0x80 | 2; // length takes 2 bytes
    *cp++ = (uint8_t)(bytes >> 8);
    *cp++ = (uint8_t)bytes;
  } else if(bytes < 16777216){
    *cp++ = 0x80 | 3;
    *cp++ = (uint8_t)(bytes >> 16);
    *cp++ = (uint8_t)(bytes >> 8);
    *cp++ = (uint8_t)bytes;
  } else {
    *cp++ = 0x80 | 4;
    *cp++ = (uint8_t)(bytes >> 24);
    *cp++ = (uint8_t)(bytes >> 16);
    *cp++ = (uint8_t)(bytes >> 8);
    *cp++ = (uint8_t)bytes;
  }
  // Encode the individual array elements
  // Right now they're DC....maxpositive maxnegative...minnegative
  for(size_t i=0;i < size;i++){
    // Swap but don't bother compressing leading zeroes for now
    union {
      uint32_t i;
      float f;
    } foo;
    foo.f = array[i];
    *cp++ = (uint8_t)(foo.i >> 24);
    *cp++ = (uint8_t)(foo.i >> 16);
    *cp++ = (uint8_t)(foo.i >> 8);
    *cp++ = (uint8_t)foo.i;
  }
  *bp = cp;
  return cp - orig_bp;
}



// Decode byte string without byte swapping
// NB! optlen has already been 'fixed' by the caller in case it's >= 128
// Now allocates the result from the heap, the caller must free.
// OTOH, the caller doesn't have to figure out a buffer size anymore
char *decode_string(uint8_t const *cp,int optlen){
  char *result = malloc(optlen+1);
  if(result != NULL)
    memcpy(result,cp,optlen);
  result[optlen] = '\0'; // force null termination
  return result;
}


// Decode encoded variable-length UNSIGNED integers
// At entry, *bp -> length field (not type!)
// Works for byte, short/int16_t, long/int32_t, long long/int64_t
// If used for signed values, must be cast
uint64_t decode_int64(uint8_t const *cp,int len){
  uint64_t result = 0;
  // cp now points to beginning of abbreviated int
  // Byte swap as we accumulate
  while(len-- > 0)
    result = (result << 8) | *cp++;

  return result;
}
uint32_t decode_int32(uint8_t const *cp,int len){
  return decode_int64(cp,len) & UINT32_MAX;
}
uint16_t decode_int16(uint8_t const *cp,int len){
  return decode_int64(cp,len) & UINT16_MAX;
}

uint8_t decode_int8(uint8_t const *cp,int len){
  return decode_int64(cp,len) & UINT8_MAX;
}
bool decode_bool(uint8_t const *cp,int len){
  return decode_int64(cp,len) ? true : false;
}

int decode_int(uint8_t const *cp,int len){
  return decode_int64(cp,len) & UINT_MAX; // mask to the size of an int
}
// Will recognize a double as long as no more than 3 of the leading bytes are zeroes and are compressed away
// Note this returns double.
// The only compressed doubles that could masquerade as floats are:
// +0, which encodes into 0 bytes (same as a zero integer) as an important special case.
// or the smallest positive denormals (biased 10-bit exponent == 0 *and* the top 21 bits of the mantissa
// (Recall denormals are where the hidden bit to the left of the mantissa is a 0 instead of the usual implied 1)
// If misinterpreted as a compressed float the rightmost 32 bits of the double's mantissa could re-emerge as a totally
// a bogus 32-bit float that might be very large
// Denormals aren't very common but still it's best to be careful
double decode_float(uint8_t const *cp,int len){
  if(len == 0)
    return 0;

  if(len > (int)sizeof(float))
    return decode_double(cp,len); // seems safe, just in case it's really a double

  union result r;
  r.ll = decode_int64(cp,len);
  return r.f;
}

// No float can masquerade as a double except as a very small positive denormal
// even if the float was very large to start with
// So always interpret as a possibly compressed double
double decode_double(uint8_t const *cp,int len){
  if(len == 0)
    return 0;

  union result r;
  r.ll = decode_int64(cp,len);
  return r.d;
}

// The Linux/UNIX socket data structures are a real mess...
int encode_socket(uint8_t **buf,enum status_type type,void const *sock){
  struct sockaddr_in const *sin = sock;
  struct sockaddr_in6 const *sin6 = sock;
  uint8_t *bp = *buf;
  int optlen = 0;

  switch(sin->sin_family){
  case AF_INET:
    optlen = 6;
    *bp++ = (uint8_t)type;
    *bp++ = (uint8_t)optlen;
    memcpy(bp,&sin->sin_addr.s_addr,4); // Already in network order
    bp += 4;
    memcpy(bp,&sin->sin_port,2);
    bp += 2;
    break;
  case AF_INET6:
    optlen = 18;
    *bp++ = (uint8_t)type;
    *bp++ = (uint8_t)optlen;
    memcpy(bp,&sin6->sin6_addr,16);
    bp += 16;
    memcpy(bp,&sin6->sin6_port,2);
    bp += 2;
    break;
  default:
    return 0; // Invalid, don't encode anything
  }
  *buf = bp;
  return optlen;
}


struct sockaddr *decode_socket(void *sock,uint8_t const *val,int optlen){
  struct sockaddr_in *sin = (struct sockaddr_in *)sock;
  struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)sock;

  // Infer family (AF_INET/AF_INET6) is inferred from length, not explicitly sent
  // Maybe this wasn't a good idea, but are any major families going to be added
  // with the same length?
  if(optlen == 6){
    sin->sin_family = AF_INET;
    memcpy(&sin->sin_addr.s_addr,val,4);
    memcpy(&sin->sin_port,val+4,2);
    return sock;
  } else if(optlen == 18){
    sin6->sin6_family = AF_INET6;
    memcpy(&sin6->sin6_addr,val,16);
    memcpy(&sin6->sin6_port,val+16,2);
    return sock;
  }
  return NULL;
}
