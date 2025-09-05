// $Id: fcd.h,v 1.1 2016/10/14 00:22:52 karn Exp $
// Modified by KA9Q
/***************************************************************************
 *  This file is part of Qthid.
 *
 *  Copyright (C) 2010  Howard Long, G6LVB
 *  CopyRight (C) 2011  Alexandru Csete, OZ9AEC
 *                      Mario Lorenz, DL5MLO
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

#ifndef FCD_H
#define FCD_H 1

#include "fcdhidcmd.h"
#include "hidapi.h"

#include <inttypes.h>


/** \brief FCD mode enumeration. */
typedef enum {
    FCD_MODE_NONE,  /*!< No FCD detected. */
    FCD_MODE_BL,    /*!< FCD present in bootloader mode. */
    FCD_MODE_APP    /*!< FCD present in aplpication mode. */
} FCD_MODE_ENUM; // The current mode of the FCD: none inserted, in bootloader mode or in normal application mode

/** \brief FCD capabilities that depend on both hardware and firmware. */
typedef struct {
    unsigned char hasBiasT;     /*!< Whether FCD has hardware bias tee (1=yes, 0=no) */
    unsigned char hasCellBlock; /*!< Whether FCD has cellular blocking. */
} FCD_CAPS_STRUCT;

#ifdef __cplusplus
extern "C" {
#endif

hid_device *fcdOpen(char *,int,int);
void fcdClose(hid_device *);

/* Application functions */
FCD_MODE_ENUM fcdGetMode(hid_device *);
FCD_MODE_ENUM fcdGetFwVerStr(hid_device *,char *str);
FCD_MODE_ENUM fcdGetCaps(hid_device *,FCD_CAPS_STRUCT *fcd_caps);
FCD_MODE_ENUM fcdGetCapsStr(hid_device *,char *caps_str);
FCD_MODE_ENUM fcdAppReset(hid_device *);
FCD_MODE_ENUM fcdAppSetFreqkHz(hid_device *,int nFreq);
FCD_MODE_ENUM fcdAppSetFreq(hid_device *,int nFreq);
FCD_MODE_ENUM fcdAppSetParam(hid_device *,uint8_t u8Cmd, uint8_t *pu8Data, uint8_t u8len);
FCD_MODE_ENUM fcdAppGetParam(hid_device *,uint8_t u8Cmd, uint8_t *pu8Data, uint8_t u8len);

/* Bootloader functions */
FCD_MODE_ENUM fcdBlReset(hid_device *);
  
FCD_MODE_ENUM fcdBlErase(hid_device *);
FCD_MODE_ENUM fcdBlWriteFirmware(hid_device *,char *pc, int64_t n64Size);
FCD_MODE_ENUM fcdBlVerifyFirmware(hid_device *,char *pc, int64_t n64Size);


#ifdef __cplusplus
}
#endif

#endif // FCD_H
