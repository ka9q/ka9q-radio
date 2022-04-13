// $Id: fcd.c,v 1.4 2017/08/12 09:07:37 karn Exp $
// KA9Q version of fcd.c
/***************************************************************************
 *  This file is part of Qthid.
 * 
 *  Copyright (C) 2010  Howard Long, G6LVB
 *  CopyRight (C) 2011  Alexandru Csete, OZ9AEC
 *                      Mario Lorenz, DL5MLO
 * 
 *  Modified in (non-compatible) way by Phil Karn, KA9Q Aug 2016
 *  to use handles on each call so that multiple FCDs can be used on a single system
 *  at once without repeating the search every time you do an operation.
 *  Also handles both FCD Pro+ and older Pro at the same time
 *  
 *  Qthid is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Qthid is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Qthid.  If not, see <http://www.gnu.org/licenses/>.
 *
 ***************************************************************************/

#define FCD
#include <string.h>
#if defined(linux)
#include <bsd/string.h>
#endif
#include <stdlib.h>
#include <limits.h>
#include "hidapi.h"
#include "fcd.h"
#include "fcdhidcmd.h"
#include <stdio.h>
#include <dirent.h>

#define FALSE 0
#define TRUE 1
typedef int BOOL;


const unsigned short _usVID=0x04D8;  /*!< USB vendor ID. */
const unsigned short _usPID_plus=0xFB31;  /*!< USB product ID. */
const unsigned short _usPID=0xFB56;  /*!< USB product ID. */

/** \brief Open FCD device.
 * \return Pointer to the FCD HID device or NULL if none found
 *
 * This function looks for FCD devices connected to the computer and
 * opens the first one found.
 */
hid_device *fcdOpen(char *sound_name,int size,int which){
  struct hid_device_info *hid_list,*pp;
  hid_device *phd=NULL;
  int i;
  int usb1,usb2;
  DIR *dir;
  struct dirent *dp;

  // Scan for dongles

  // (0,0) means match all USB devices so we can look for Pro and Pro+, which have different product IDs
  hid_list = hid_enumerate(0,0);
  for(i=0,pp=hid_list; pp != NULL; pp = pp->next){
    if(pp->vendor_id == _usVID && (pp->product_id == _usPID || pp->product_id == _usPID_plus)){
      // This is either a Funcube Pro or Pro+, so count it
      if(i++ == which){
	// And it's the one we want
	phd = hid_open_path(pp->path);
	sscanf(pp->path,"%x:%x",&usb1,&usb2); // Save USB bus address
	break;
      }
    }
  }
  hid_free_enumeration(hid_list);
  if(phd == NULL){
    return NULL; // none found
  }
  if(sound_name == NULL || size == 0)
    return phd; // Don't look for the name when he doesn't want it

  // Find corresponding ALSA audio name
  // For each /proc/asound/card?/usbbus, read the bus addresses
  // and see if they match our device
  if((dir = opendir("/proc/asound")) == NULL){
    return phd;
  }
  sound_name[0] = '\0';
  while((dp = readdir(dir)) != NULL){
    char s[PATH_MAX];
    FILE *fp = NULL;
    int u1,u2,r;
    
    if(strncmp(dp->d_name,"card",4) != 0)
      continue; // ignore directory entries not starting with "card"
    
    snprintf(s,sizeof(s),"/proc/asound/%s/usbbus",dp->d_name);
    if((fp = fopen(s,"r")) == NULL){
      continue;
    }    
    // Contains one line of the form "001/007" (bus 1, device 7)
    r = fscanf(fp,"%u/%u",&u1,&u2);
    fclose(fp);
    if(r != 2 || u1 != usb1 || u2 != usb2)
      continue; // Not the droids we're looking for
    
    // The A/D is device 0 on this card, so form the ALSA name
    // with the prefix "hw:", then the card number (minus "card"), then ",0",
    // e.g., "hw:1,0" if it's card1.
    // We've already checked that sound_name is non-null
    snprintf(sound_name,size,"hw:%s,0",&dp->d_name[4]);
    break;
  }
  closedir(dir);  
  return phd;
}


/** \brief Close FCD HID device. */
void fcdClose(hid_device *phd)
{
    hid_close(phd);
}


/** \brief Get FCD mode.
 * \return The current FCD mode.
 * \sa FCD_MODE_ENUM
 */
FCD_MODE_ENUM fcdGetMode(hid_device *phd)
{
    unsigned char aucBufIn[65];
    unsigned char aucBufOut[65];
    FCD_MODE_ENUM fcd_mode = FCD_MODE_NONE;

    /* Send a BL Query Command */
    aucBufOut[0] = 0; // Report ID, ignored
    aucBufOut[1] = FCD_CMD_BL_QUERY;
    hid_write(phd, aucBufOut, 65);
    memset(aucBufIn, 0xCC, 65); // Clear out the response buffer
    hid_read(phd, aucBufIn, 65);

    /* first check status bytes then check which mode */
    if (aucBufIn[0]==FCD_CMD_BL_QUERY && aucBufIn[1]==1) {

        /* In bootloader mode we have the string "FCDBL" starting at acBufIn[2] **/
        if (strncmp((char *)(aucBufIn+2), "FCDBL", 5) == 0) {
            fcd_mode = FCD_MODE_BL;
        }
        /* In application mode we have "FCDAPP_18.06" where the number is the FW version */
        else if (strncmp((char *)(aucBufIn+2), "FCDAPP", 6) == 0) {
            fcd_mode = FCD_MODE_APP;
        }
        /* either no FCD or firmware less than 18f */
        else {
            fcd_mode = FCD_MODE_NONE;
        }
    }
    return fcd_mode;
}


/** \brief Get FCD firmware version as string.
 * \param str The returned vesion number as a 0 terminated string (must be pre-allocated)
 * \return The current FCD mode.
 * \sa FCD_MODE_ENUM
 */
FCD_MODE_ENUM fcdGetFwVerStr(hid_device *phd,char *str)
{
    unsigned char aucBufIn[65];
    unsigned char aucBufOut[65];
    FCD_MODE_ENUM fcd_mode = FCD_MODE_NONE;

    /* Send a BL Query Command */
    aucBufOut[0] = 0; // Report ID, ignored
    aucBufOut[1] = FCD_CMD_BL_QUERY;
    hid_write(phd, aucBufOut, 65);
    memset(aucBufIn, 0xCC, 65); // Clear out the response buffer
    hid_read(phd, aucBufIn, 65);

    /* first check status bytes then check which mode */
    if (aucBufIn[0]==FCD_CMD_BL_QUERY && aucBufIn[1]==1) {

        /* In bootloader mode we have the string "FCDBL" starting at acBufIn[2] **/
        if (strncmp((char *)(aucBufIn+2), "FCDBL", 5) == 0) {
            fcd_mode = FCD_MODE_BL;
        }
        /* In application mode we have "FCDAPP_18.06" where the number is the FW version */
        else if (strncmp((char *)(aucBufIn+2), "FCDAPP", 6) == 0) {
            strlcpy(str, (char *)(aucBufIn+9), 5);
            str[5] = 0;
            fcd_mode = FCD_MODE_APP;
        }
        /* either no FCD or firmware less than 18f */
        else {
            fcd_mode = FCD_MODE_NONE;
        }
    }

    return fcd_mode;
}


/** \brief Get hardware and firmware dependent FCD capabilities.
 * \param fcd_caps Pointer to an FCD_CAPS_STRUCT
 * \return The current FCD mode.
 *
 * This function queries the FCD and extracts the hardware and firmware dependent
 * capabilities. Currently these capabilities are:
 *  - Bias T (available since S/N TBD)
 *  - Cellular block (the certified version of the FCD)
 * When the FCD is in application mode, the string returned by the query command is
 * (starting at index 2):
 *    FCDAPP 18.08 Brd 1.0 No blk
 * 1.0 means no bias tee, 1.1 means there is a bias tee
 * 'No blk' means it is not cellular blocked.
 *
 * Ref: http://uk.groups.yahoo.com/group/FCDevelopment/message/303
 */
FCD_MODE_ENUM fcdGetCaps(hid_device *phd,FCD_CAPS_STRUCT *fcd_caps)
{
    unsigned char aucBufIn[65];
    unsigned char aucBufOut[65];
    FCD_MODE_ENUM fcd_mode = FCD_MODE_NONE;

    /* clear output buffer */
    fcd_caps->hasBiasT = 0;
    fcd_caps->hasCellBlock = 0;

    /* Send a BL Query Command */
    aucBufOut[0] = 0; // Report ID, ignored
    aucBufOut[1] = FCD_CMD_BL_QUERY;
    hid_write(phd, aucBufOut, 65);
    memset(aucBufIn, 0xCC, 65); // Clear out the response buffer
    hid_read(phd, aucBufIn, 65);

    /* first check status bytes then check which mode */
    if (aucBufIn[0]==FCD_CMD_BL_QUERY && aucBufIn[1]==1) {

        /* In bootloader mode we have the string "FCDBL" starting at acBufIn[2] **/
        if (strncmp((char *)(aucBufIn+2), "FCDBL", 5) == 0) {
            fcd_mode = FCD_MODE_BL;
        }
        /* In application mode we have "FCDAPP 18.08 Brd 1.0 No blk" (see API doc) */
        else if (strncmp((char *)(aucBufIn+2), "FCDAPP", 6) == 0) {

            /* Bias T */
            fcd_caps->hasBiasT = (aucBufIn[21] == '1') ? 1 : 0;

            /* cellular block */
            if (strncmp((char *)(aucBufIn+23), "No blk", 6) == 0) {
                fcd_caps->hasCellBlock = 0;
            } else {
                fcd_caps->hasCellBlock = 1;
            }

            fcd_mode = FCD_MODE_APP;
        }
        /* either no FCD or firmware less than 18f */
        else {
            fcd_mode = FCD_MODE_NONE;
        }
    }

    return fcd_mode;
}


/** \brief Get hardware and firmware dependent FCD capabilities as string.
 * \param caps_str Pointer to a pre-allocated string buffer where the info will be copied.
 * \return The current FCD mode.
 *
 * This function queries the FCD and copies the returned string into the caps_str parameter.
 * THe return buffer must be at least 28 characters.
 * When the FCD is in application mode, the string returned by the query command is
 * (starting at index 2):
 *    FCDAPP 18.08 Brd 1.0 No blk
 * 1.0 means no bias tee, 1.1 means there is a bias tee
 * 'No blk' means it is not cellular blocked.
 *
 * Ref: http://uk.groups.yahoo.com/group/FCDevelopment/message/303
 */
FCD_MODE_ENUM fcdGetCapsStr(hid_device *phd,char *caps_str)
{
    unsigned char aucBufIn[65];
    unsigned char aucBufOut[65];
    FCD_MODE_ENUM fcd_mode = FCD_MODE_NONE;


    /* Send a BL Query Command */
    aucBufOut[0] = 0; // Report ID, ignored
    aucBufOut[1] = FCD_CMD_BL_QUERY;
    hid_write(phd, aucBufOut, 65);
    memset(aucBufIn, 0xCC, 65); // Clear out the response buffer
    hid_read(phd, aucBufIn, 65);

    /* first check status bytes then check which mode */
    if (aucBufIn[0]==FCD_CMD_BL_QUERY && aucBufIn[1]==1) {

        /* In bootloader mode we have the string "FCDBL" starting at acBufIn[2] **/
        if (strncmp((char *)(aucBufIn+2), "FCDBL", 5) == 0) {
            fcd_mode = FCD_MODE_BL;
        }
        /* In application mode we have "FCDAPP 18.08 Brd 1.0 No blk" (see API doc) */
        else if (strncmp((char *)(aucBufIn+2), "FCDAPP", 6) == 0) {

            strlcpy(caps_str, (char *)(aucBufIn+2), 27);
            caps_str[27] = 0;

            fcd_mode = FCD_MODE_APP;
        }
        /* either no FCD or firmware less than 18f */
        else {
            fcd_mode = FCD_MODE_NONE;
        }
    }

    return fcd_mode;
}



/** \brief Reset FCD to bootloader mode.
 * \return FCD_MODE_NONE
 *
 * This function is used to switch the FCD into bootloader mode in which
 * various firmware operations can be performed.
 */
FCD_MODE_ENUM fcdAppReset(hid_device *phd)
{
    //unsigned char aucBufIn[65];
    unsigned char aucBufOut[65];

    // Send an App reset command
    aucBufOut[0] = 0; // Report ID, ignored
    aucBufOut[1] = FCD_CMD_APP_RESET;
    hid_write(phd, aucBufOut, 65);

    /** FIXME: hid_read() will occasionally hang due to a pthread_cond_wait() never returning.
      It seems that the read_callback() in hid-libusb.c will never receive any
      data during the reconfiguration. Since the same logic works in the native
      windows application, it could be a libusb thing. Anyhow, since the value
      returned by this function is not used, we may as well just skip the hid_read()
      and return FME_NONE.
      Correct switch from APP to BL mode can be observed in /var/log/messages (linux)
      (when in bootloader mode the device version includes 'BL')
      */
    /*
       memset(aucBufIn,0xCC,65); // Clear out the response buffer
       hid_read(phd,aucBufIn,65);

       if (aucBufIn[0]==FCDCMDAPPRESET && aucBufIn[1]==1)
       {
       FCDClose(phd);
       phd=NULL;
       return FME_APP;
       }
       FCDClose(phd);
       phd=NULL;
       return FME_BL;
       */

    return FCD_MODE_NONE;

}


/** \brief Set FCD frequency with kHz resolution.
 * \param nFreq The new frequency in kHz.
 * \return The FCD mode.
 *
 * This function sets the frequency of the FCD with 1 kHz resolution. The parameter
 * nFreq must already contain any necessary frequency correction.
 *
 * \sa fcdAppSetFreq
 */
FCD_MODE_ENUM fcdAppSetFreqkHz(hid_device *phd,int nFreq)
{
    unsigned char aucBufIn[65];
    unsigned char aucBufOut[65];

    // Send an App reset command
    aucBufOut[0] = 0; // Report ID, ignored
    aucBufOut[1] = FCD_CMD_APP_SET_FREQ_KHZ;
    aucBufOut[2] = (unsigned char)nFreq;
    aucBufOut[3] = (unsigned char)(nFreq>>8);
    aucBufOut[4] = (unsigned char)(nFreq>>16);
    hid_write(phd, aucBufOut, 65);
    memset(aucBufIn, 0xCC, 65); // Clear out the response buffer
    hid_read(phd, aucBufIn, 65);

    if (aucBufIn[0]==FCD_CMD_APP_SET_FREQ_KHZ && aucBufIn[1]==1)
    {
        return FCD_MODE_APP;
    }
    return FCD_MODE_BL;
}


/** \brief Set FCD frequency with Hz resolution.
 * \param nFreq The new frequency in Hz.
 * \return The FCD mode.
 *
 * This function sets the frequency of the FCD with 1 Hz resolution. The parameter
 * nFreq must already contain any necessary frequency correction.
 *
 * \sa fcdAppSetFreq
 */
FCD_MODE_ENUM fcdAppSetFreq(hid_device *phd,int nFreq)
{
    unsigned char aucBufIn[65];
    unsigned char aucBufOut[65];
    int cnt,i;

    for(i=0;i<10;i++){
      // Send an App reset command
      aucBufOut[0] = 0; // Report ID, ignored
      aucBufOut[1] = FCD_CMD_APP_SET_FREQ_HZ;
      aucBufOut[2] = (unsigned char)nFreq;
      aucBufOut[3] = (unsigned char)(nFreq>>8);
      aucBufOut[4] = (unsigned char)(nFreq>>16);
      aucBufOut[5] = (unsigned char)(nFreq>>24);
      hid_write(phd, aucBufOut, 65);
      memset(aucBufIn, 0xCC, 65); // Clear out the response buffer
      // KA9Q: commands occasionally hang; try reading with timeout
      cnt = hid_read_timeout(phd, aucBufIn, 65, 1000);
      if(cnt != 0)
	break;
    }      
    if(i == 10)
      return FCD_MODE_NONE; // Failed after 10 tries
    if (aucBufIn[0]==FCD_CMD_APP_SET_FREQ_HZ && aucBufIn[1]==1)
        return FCD_MODE_APP;
    return FCD_MODE_BL;
}



/** \brief Reset FCD to application mode.
 * \return FCD_MODE_NONE
 *
 * This function is used to switch the FCD from bootloader mode
 * into application mode.
 */
FCD_MODE_ENUM fcdBlReset(hid_device *phd)
{
    //    unsigned char aucBufIn[65];
    unsigned char aucBufOut[65];

    // Send an BL reset command
    aucBufOut[0] = 0; // Report ID, ignored
    aucBufOut[1] = FCD_CMD_BL_RESET;
    hid_write(phd, aucBufOut, 65);

    /** FIXME: hid_read() will hang due to a pthread_cond_wait() never returning.
      It seems that the read_callback() in hid-libusb.c will never receive any
      data during the reconfiguration. Since the same logic works in the native
      windows application, it could be a libusb thing. Anyhow, since the value
      returned by this function is not used, we may as well jsut skip the hid_read()
      and return FME_NONE.
      Correct switch from BL to APP mode can be observed in /var/log/messages (linux)
      (when in bootloader mode the device version includes 'BL')
      */
    /*
       memset(aucBufIn,0xCC,65); // Clear out the response buffer
       hid_read(phd,aucBufIn,65);

       if (aucBufIn[0]==FCDCMDBLRESET && aucBufIn[1]==1)
       {
       FCDClose(phd);
       phd=NULL;
       return FME_BL;
       }
       FCDClose(phd);
       phd=NULL;
       return FME_APP;
       */

    return FCD_MODE_NONE;

}


/** \brief Erase firmware from FCD.
 * \return The FCD mode
 *
 * This function deletes the firmware from the FCD. This is required
 * before writing new firmware into the FCD.
 *
 * \sa fcdBlWriteFirmware
 */
FCD_MODE_ENUM fcdBlErase(hid_device *phd)
{
    unsigned char aucBufIn[65];
    unsigned char aucBufOut[65];

    // Send an App reset command
    aucBufOut[0] = 0; // Report ID, ignored
    aucBufOut[1] = FCD_CMD_BL_ERASE;
    hid_write(phd, aucBufOut, 65);
    memset(aucBufIn, 0xCC, 65); // Clear out the response buffer
    hid_read(phd, aucBufIn, 65);

    if (aucBufIn[0]==FCD_CMD_BL_ERASE && aucBufIn[1]==1)
        return FCD_MODE_BL;

    return FCD_MODE_APP;
}


/** \brief Write new firmware into the FCD.
 * \param pc Pointer to the new firmware data
 * \param n64size The number of bytes in the data
 * \return The FCD mode
 *
 * This function is used to upload new firmware into the FCD flash.
 *
 * \sa fcdBlErase
 */
FCD_MODE_ENUM fcdBlWriteFirmware(hid_device *phd,char *pc, int64_t n64Size)
{
    unsigned char aucBufIn[65];
    unsigned char aucBufOut[65];
    uint32_t u32AddrStart;
    uint32_t u32AddrEnd;
    uint32_t u32Addr;
    BOOL bFinished=FALSE;

    // Get the valid flash address range
    aucBufOut[0] = 0; // Report ID, ignored
    aucBufOut[1] = FCD_CMD_BL_GET_BYTE_ADDR_RANGE;
    hid_write(phd, aucBufOut, 65);
    memset(aucBufIn, 0xCC, 65); // Clear out the response buffer
    hid_read(phd, aucBufIn, 65);

    if (aucBufIn[0]!=FCD_CMD_BL_GET_BYTE_ADDR_RANGE || aucBufIn[1]!=1)
        return FCD_MODE_APP;

    u32AddrStart=
        aucBufIn[2]+
        (((uint32_t)aucBufIn[3])<<8)+
        (((uint32_t)aucBufIn[4])<<16)+
        (((uint32_t)aucBufIn[5])<<24);
    u32AddrEnd=
        aucBufIn[6]+
        (((uint32_t)aucBufIn[7])<<8)+
        (((uint32_t)aucBufIn[8])<<16)+
        (((uint32_t)aucBufIn[9])<<24);

    // Set start address for flash
    aucBufOut[0] = 0; // Report ID, ignored
    aucBufOut[1] = FCD_CMD_BL_SET_BYTE_ADDR;
    aucBufOut[2] = ((unsigned char)u32AddrStart);
    aucBufOut[3] = ((unsigned char)(u32AddrStart>>8));
    aucBufOut[4] = ((unsigned char)(u32AddrStart>>16));
    aucBufOut[5] = ((unsigned char)(u32AddrStart>>24));
    hid_write(phd, aucBufOut, 65);
    memset(aucBufIn, 0xCC, 65); // Clear out the response buffer
    hid_read(phd, aucBufIn, 65);

    if (aucBufIn[0]!=FCD_CMD_BL_SET_BYTE_ADDR || aucBufIn[1]!=1)
        return FCD_MODE_APP;

    // Write blocks
    aucBufOut[0] = 0; // Report ID, ignored
    aucBufOut[1] = FCD_CMD_BL_WRITE_FLASH_BLOCK;
    for (u32Addr=u32AddrStart; u32Addr+47<u32AddrEnd && u32Addr+47<n64Size && !bFinished; u32Addr+=48)
    {
        memcpy(&aucBufOut[3], &pc[u32Addr], 48);

        hid_write(phd, aucBufOut, 65);
        memset(aucBufIn, 0xCC, 65); // Clear out the response buffer
        hid_read(phd, aucBufIn, 65);

        if (aucBufIn[0]!=FCD_CMD_BL_WRITE_FLASH_BLOCK || aucBufIn[1]!=1)
        {
            bFinished = TRUE;
            return FCD_MODE_APP;
        }
    }

    return FCD_MODE_BL;
}


/** \brief Verify firmware in FCd flash.
 * \param pc Pointer to firmware data to verify against.
 * \param n64Size Size of the data in pc.
 * \return The FCD_MODE_BL if verification was succesful.
 *
 * This function verifies the firmware currently in the FCd flash against the firmware
 * image pointed to by pc. The function return FCD_MODE_BL if the verification is OK and
 * FCD_MODE_APP otherwise.
 */
FCD_MODE_ENUM fcdBlVerifyFirmware(hid_device *phd,char *pc, int64_t n64Size)
{
    unsigned char aucBufIn[65];
    unsigned char aucBufOut[65];
    uint32_t u32AddrStart;
    uint32_t u32AddrEnd;
    uint32_t u32Addr;
    BOOL bFinished=FALSE;

    // Get the valid flash address range
    aucBufOut[0] = 0; // Report ID, ignored
    aucBufOut[1] = FCD_CMD_BL_GET_BYTE_ADDR_RANGE;
    hid_write(phd, aucBufOut, 65);
    memset(aucBufIn, 0xCC, 65); // Clear out the response buffer
    hid_read(phd, aucBufIn, 65);

    if (aucBufIn[0]!=FCD_CMD_BL_GET_BYTE_ADDR_RANGE || aucBufIn[1]!=1)
        return FCD_MODE_APP;

    u32AddrStart=
        aucBufIn[2]+
        (((uint32_t)aucBufIn[3])<<8)+
        (((uint32_t)aucBufIn[4])<<16)+
        (((uint32_t)aucBufIn[5])<<24);

    u32AddrEnd=
        aucBufIn[6]+
        (((uint32_t)aucBufIn[7])<<8)+
        (((uint32_t)aucBufIn[8])<<16)+
        (((uint32_t)aucBufIn[9])<<24);

    // Set start address for flash
    aucBufOut[0] = 0; // Report ID, ignored
    aucBufOut[1] = FCD_CMD_BL_SET_BYTE_ADDR;
    aucBufOut[2] = ((unsigned char)u32AddrStart);
    aucBufOut[3] = ((unsigned char)(u32AddrStart>>8));
    aucBufOut[4] = ((unsigned char)(u32AddrStart>>16));
    aucBufOut[5] = ((unsigned char)(u32AddrStart>>24));
    hid_write(phd, aucBufOut, 65);
    memset(aucBufIn, 0xCC, 65); // Clear out the response buffer
    hid_read(phd, aucBufIn, 65);

    if (aucBufIn[0]!=FCD_CMD_BL_SET_BYTE_ADDR || aucBufIn[1]!=1)
        return FCD_MODE_APP;

    // Read blocks
    aucBufOut[0] = 0; // Report ID, ignored
    aucBufOut[1] = FCD_CMD_BL_READ_FLASH_BLOCK;
    for (u32Addr=u32AddrStart; u32Addr+47<u32AddrEnd && u32Addr+47<n64Size && !bFinished; u32Addr+=48)
    {
        hid_write(phd, aucBufOut, 65);
        memset(aucBufIn, 0xCC, 65); // Clear out the response buffer
        hid_read(phd, aucBufIn, 65);

        if (aucBufIn[0]!=FCD_CMD_BL_READ_FLASH_BLOCK || aucBufIn[1]!=1)
        {
            bFinished = TRUE;
            return FCD_MODE_APP;
        }

        if (memcmp(&aucBufIn[2],&pc[u32Addr],48)!=0)
        {
            bFinished = TRUE;
            return FCD_MODE_APP;
        }
    }
    return FCD_MODE_BL;
}



/** \brief Write FCD parameter (e.g. gain or filter)
 * \param u8Cmd The command byte / parameter ID, see FCD_CMD_APP_SET_*
 * \param pu8Data The parameter value to be written
 * \param u8len Length of pu8Data in bytes
 * \return One of FCD_MODE_NONE, FCD_MODE_APP or FCD_MODE_BL (see description)
 *
 * This function can be used to set the value of a parameter in the FCD for which there is no
 * high level API call. It gives access to the low level API of the FCD and the caller is expected
 * to be aware of the various FCD commands, since they are required to be supplied as parameter
 * to this function.
 *
 * The return value can be used to determine the success or failure of the command execution:
 * - FCD_MODE_APP : Reply from FCD was as expected (nominal case).
 * - FCD_MODE_BL : Reply from FCD was not as expected.
 * - FCD_MODE_NONE : No FCD was found
 */
FCD_MODE_ENUM fcdAppSetParam(hid_device *phd,uint8_t u8Cmd, uint8_t *pu8Data, uint8_t u8len)
{
    unsigned char aucBufOut[65];
    unsigned char aucBufIn[65];

    aucBufOut[0]=0; // Report ID, ignored
    aucBufOut[1]=u8Cmd;
    memcpy(aucBufOut+2, pu8Data,u8len);
    hid_write(phd,aucBufOut,sizeof(aucBufOut));

    /* we must read after each write in order to empty FCD/HID buffer */
    memset(aucBufIn,0xCC,sizeof(aucBufIn)); // Clear out the response buffer
    hid_read(phd,aucBufIn,65);

    /* Check the response, if OK return FCD_MODE_APP */
    if (aucBufIn[0]==u8Cmd && aucBufIn[1]==1)
        return FCD_MODE_APP;

    /* Response did not contain the expected bytes */
    return FCD_MODE_BL;
}


/** \brief Read FCD parameter (e.g. gain or filter)
 * \param u8Cmd The command byte / parameter ID, see FCD_CMD_APP_GET_*
 * \param pu8Data TPointer to buffer where the parameter value(s) will be written
 * \param u8len Length of pu8Data in bytes
 * \return One of FCD_MODE_NONE, FCD_MODE_APP or FCD_MODE_BL (see description)
 *
 * This function can be used to read the value of a parameter in the FCD for which there is no
 * high level API call. It gives access to the low level API of the FCD and the caller is expected
 * to be aware of the various FCD commands, since they are required to be supplied as parameter
 * to this function.
 *
 * The return value can be used to determine the success or failure of the command execution:
 * - FCD_MODE_APP : Reply from FCD was as expected (nominal case).
 * - FCD_MODE_BL : Reply from FCD was not as expected.
 * - FCD_MODE_NONE : No FCD was found
 */
FCD_MODE_ENUM fcdAppGetParam(hid_device *phd,uint8_t u8Cmd, uint8_t *pu8Data, uint8_t u8len)
{
    unsigned char aucBufOut[65];
    unsigned char aucBufIn[65];

    aucBufOut[0]=0; // Report ID, ignored
    aucBufOut[1]=u8Cmd;
    hid_write(phd,aucBufOut,sizeof(aucBufOut));

    memset(aucBufIn,0xCC,sizeof(aucBufIn)); // Clear out the response buffer
    hid_read_timeout(phd,aucBufIn,sizeof(aucBufIn),1000);
    /* Copy return data to output buffer (even if cmd exec failed) */
    memcpy(pu8Data,aucBufIn+2,u8len);

    /* Check status bytes in returned data */
    if (aucBufIn[0]==u8Cmd && aucBufIn[1]==1)
        return FCD_MODE_APP;

    /* Response did not contain the expected bytes */
    return FCD_MODE_BL;
}

