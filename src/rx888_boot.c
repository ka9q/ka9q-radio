// Standalone RX888 firmware loader
// Invoked by udev when it sees device 00f3 (FX3 in DFU boot mode)
// Loading the firmware and starting it will cause the device to detatch from the USB and reappear as device 00f1
// with the true serial number. udevd can then invoke radiod with the proper instance name
// Phil Karn April 2026
#define _GNU_SOURCE 1
#include <assert.h>
#include <pthread.h>
#include <libusb-1.0/libusb.h>
#include <iniparser/iniparser.h>
#if defined(linux)
#include <bsd/string.h>
#endif
#include <sysexits.h>
#include <unistd.h>
#include <strings.h>
#include <assert.h>
#include <getopt.h>
#include <libgen.h>
#include <sys/stat.h>

#include "misc.h"
#include "status.h"
#include "config.h"
#include "radio.h"
#include "rx888.h"
#include "ezusb.h"

char const *App_path;
char const *Libdir = "/var/lib/ka9q-radio/";
int Ezusb_verbose = 0; // used by ezusb.c

int main(int argc,char *argv[]){
  char const *firmware = "/usr/local/share/ka9q-radio/SDDC_FX3.img"; // default
  App_path = argv[0];

  int c;
  while((c = getopt(argc,argv,"f:")) != -1){
    switch(c){
    case 'f':
      firmware = optarg;
      break;
    default:
      fprintf(stderr,"usage: %s [-f bootimage]\n",App_path);
      exit(EX_USAGE);
      break;
    }
  }
  
  if(firmware == NULL){
    fprintf(stderr,"Firmware not loaded and not available\n");
    exit(EX_NOINPUT);
  }
  char full_firmware_file[PATH_MAX] = {0};
  dist_path(full_firmware_file,sizeof(full_firmware_file),firmware);

  {
    int ret = libusb_init(NULL);
    if(ret != 0){
      fprintf(stderr,"Error initializing libusb: %s\n",
	      libusb_error_name(ret));
      return -1;
    }
  }
  uint16_t const vendor_id = 0x04b4;
  uint16_t const unloaded_product_id = 0x00f3;
  //  uint16_t const loaded_product_id = 0x00f1;

  // Search for unloaded rx888s (0x04b4:0x00f3) with the desired serial, or all such devices if no serial specified
  // and load with firmware
  libusb_device **device_list = {0};
  ssize_t dev_count = libusb_get_device_list(NULL,&device_list);
  for(ssize_t i=0; i < dev_count; i++){
    libusb_device *device = device_list[i];
    if(device == NULL)
      break; // End of list

    struct libusb_device_descriptor desc = {0};
    int rc = libusb_get_device_descriptor(device,&desc);
    if(rc != 0){
      fprintf(stderr," libusb_get_device_descriptor() failed: %s\n",libusb_strerror(rc));
      continue;
    }
    if(desc.idVendor != vendor_id || desc.idProduct != unloaded_product_id)
      continue;

    fprintf(stderr,"found rx888 vendor %04x, device %04x",desc.idVendor,desc.idProduct);
    libusb_device_handle *handle = NULL;
    rc = libusb_open(device,&handle);
    if(rc != 0 || handle == NULL){
      fprintf(stderr,", libusb_open() failed: %s\n",libusb_strerror(rc));
      continue;
    }
    if(desc.iManufacturer){
      char manufacturer[100] = {0};
      int ret = libusb_get_string_descriptor_ascii(handle,desc.iManufacturer,(unsigned char *)manufacturer,sizeof(manufacturer));
      if(ret > 0)
	fprintf(stderr,", manufacturer '%s'",manufacturer);
    }
    if(desc.iProduct){
      char product[100] = {0};
      int ret = libusb_get_string_descriptor_ascii(handle,desc.iProduct,(unsigned char *)product,sizeof(product));
      if(ret > 0)
	fprintf(stderr,", product '%s'",product);
    }
    char serial[100] = {0};
    if(desc.iSerialNumber){
      int ret = libusb_get_string_descriptor_ascii(handle,desc.iSerialNumber,(unsigned char *)serial,sizeof(serial));
      if(ret > 0){
	fprintf(stderr,", serial '%s'",serial);
      }
    }
    // The proper serial number doesn't appear until the device is loaded with firmware, so load all we find
    fprintf(stderr,", loading rx888 firmware file %s",full_firmware_file);
    if(ezusb_load_ram(handle,full_firmware_file,FX_TYPE_FX3,IMG_TYPE_IMG,1) == 0){
      fprintf(stderr,", done\n");
      sleep(1); // how long should this be?
    } else {
      fprintf(stderr,", failed for device %d.%d (logical)\n",
	      libusb_get_bus_number(device),libusb_get_device_address(device));
    }
    libusb_close(handle);
    handle = NULL;
  }
  libusb_free_device_list(device_list,1);
  device_list = NULL;
  exit(0);
}

// Return path to file which is part of the application distribution.
// This allows to run the program either from build directory or from
// installation directory.
int dist_path(char *path,int path_len,const char *fname){
  if(path == NULL)
    return -1;
  char cwd[PATH_MAX];
  struct stat st;

  if(fname[0] == '/') {
    strlcpy(path, fname, path_len);
    return 0;
  }

  dirname(realpath(App_path,cwd));
  snprintf(path,path_len,"%s/%s",cwd,fname);
  if(stat(path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFREG)
    return 0;

  snprintf(path,path_len,"%s/%s",Libdir,fname);
  if(stat(path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFREG)
    return 0;
  return -1;
}
