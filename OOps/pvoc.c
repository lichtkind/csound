/*
    pvoc.c:

    Copyright (C) 1990 Dan Ellis, John ffitch

    This file is part of Csound.

    The Csound Library is free software; you can redistribute it
    and/or modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    Csound is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with Csound; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
    02111-1307 USA
*/

/***************************************************************\
*       pvoc.c                                                  *
*       file in/out functions for pvoc FFT files                *
*       'inspired' by the NeXT SND system                       *
*       01aug90 dpwe                                            *
\***************************************************************/

#include "sysdep.h"

#ifdef HAVE_MALLOC_H
#  include <malloc.h>
#endif

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#if defined(mac_classic) || defined(SYMANTEC) || defined(__FreeBSD__) || defined(__NetBSD__)
#  define READMODE "rb"
#  define WRITEMODE "wb+"
#else
#  ifdef HAVE_SYS_TYPES_H
#   include <sys/types.h>
#  endif
#  define READMODE "r"
#  define WRITEMODE "w+"
#endif /* SYMANTEC */

#include "cs.h"
#include "pvoc.h"

#ifdef HAVE_FCNTL_H
# include <fcntl.h>
#endif

/* static variables */
static PVSTRUCT tmphdr;         /* scratch space for pre-load */
static char unspecMsg[27] = "Unspecified error :        ";
/* want to 'fill in' at      012345678901234567890... [20..27] */
#define USMINDX 20      /* where to write into above string ^ */

char *PVDataLoc(PVSTRUCT *phdr)
{
    return( ((char *)phdr)+phdr->headBsize );
}

int PVReadHdr(FILE *fil, PVSTRUCT *phdr)
    /* read just the header from a candidate pvoc file */
    /* returns PVE_RDERR if read fails (or file too small)
            or PVE_NPV   if magic number doesn't fit
            or PVE_OK     otherwise                     */
{
    size_t      num;

    phdr->magic = 0L;
    if (fseek(fil, 0L, SEEK_SET) != 0)
        return(PVE_RDERR);
    if ((num = fread((void *)phdr, (size_t)1, (size_t)sizeof(PVSTRUCT), fil))
                    < (size_t)sizeof(PVSTRUCT)) {
      err_printf( Str("PVRdH: wanted %d got %d\n"),
                  (size_t)sizeof(PVSTRUCT), num);
      return(PVE_RDERR);
    }
    if (phdr->magic != PVMAGIC)
        return(PVE_NPV);
    return(PVE_OK);
}

int PVWriteHdr(FILE *fil, PVSTRUCT *phdr)
{
    long        bSize;

    if (phdr->magic != PVMAGIC)
      return PVE_NPV;
    if (fseek(fil, 0L, SEEK_SET) != 0)
      return PVE_RDERR;
    bSize = phdr->headBsize;
    if (fwrite((void *)phdr, (size_t)1, (size_t)bSize, fil) < (size_t)bSize )
      return PVE_WRERR;
    return PVE_OK;
}



FILE *PVOpenAllocRdHdr(char *path, PVSTRUCT **phdr)
    /* read all of candidate header - variable length header */
{
    FILE        *pvf;
    long        hSize, rem;
    int         err = 0;

    if ((pvf = fopen(path,"r"))!= NULL) {
      if (PVReadHdr(pvf, &tmphdr) == PVE_OK ) {
        hSize = tmphdr.headBsize;
        *phdr = (PVSTRUCT *)malloc((size_t)hSize);
        if ((*phdr)!=NULL) {
          **phdr = tmphdr;      /* copies elements of struct ?? */
          rem = hSize - sizeof(PVSTRUCT);
          if (rem > 0)
            fread((void *)((*phdr)+1),(size_t)1,(size_t)rem,pvf);
        }
        else
          err = 1;      /* malloc error */
      }
      else
        err = 1;                /* header read error - e.g. not pv file */
    }
    if (err) {
      fclose(pvf);
      pvf = NULL;
    }
    return(pvf);
}

#ifdef never
int PVReadFile(char *filename, PVSTRUCT **pphdr)
    /* read in the header and all the data */
    /* returns PVE_NOPEN   if cannot open
               PVE_NPV     if not a PVOC file
               PVE_RDERR   if reads fail
               PVE_MALLOC  if cannot get memory
               PVE_OK      otherwise    */
{
    FILE        *fil;
    int         i, err = PVE_OK;
    long        headBsize, dataBsize, count;
    int         infoBsize;
    size_t      num;

    if ((fil = fopen(filename, READMODE)) == NULL)
        return(PVE_NOPEN);
    *pphdr = NULL;
    err = PVReadHdr(fil, &tmphdr);
    if (err == PVE_OK)
        {
        headBsize = tmphdr.headBsize;
        infoBsize = headBsize - sizeof(PVSTRUCT) + PVDFLTBYTS;
        if (infoBsize < 0)
            err = PVE_NPV;
        }
    if (err == PVE_OK)
        {
            dataBsize = tmphdr.dataBsize;
            err = PVAlloc(pphdr, dataBsize, PVMYFLT, (MYFLT)1000,
                          1, 0L, 0L, 0L, PVPOLAR, (MYFLT)0, (MYFLT)1,
                          PVLIN, infoBsize);
        }
    if (err == PVE_OK)
        {
        /* copy over what we already read */
            for (i = 0; i < sizeof(PVSTRUCT)/2; ++i)
                ((short *)*pphdr)[i] = ((short *)&tmphdr)[i];
            /* figure how many bytes expected left */
            if (dataBsize == PV_UNK_LEN)
                count = infoBsize - PVDFLTBYTS;
            else
                count = dataBsize + infoBsize - PVDFLTBYTS;
            if ((num = fread( (void *)((*pphdr)+1),
                              (size_t)1, (size_t)count, fil)) < (size_t)count )
                {
                    err_printf(
                            Str("PVRead: wanted %d got %ld\n"), num, count);
                    err = PVE_RDERR;
                }
        }
    if ((err != PVE_OK) && (*pphdr != NULL))
        {
            PVFree(*pphdr);
            *pphdr = NULL;
        }
    fclose(fil);
    return(err);
}
#endif

FILE *PVOpenWrHdr(char *filename, PVSTRUCT *phdr)
{
    FILE        *fil = NULL;

    if (phdr->magic != PVMAGIC)
      return NULL;    /* PVE_NPV   */
    if ((fil = fopen(filename,WRITEMODE)) == NULL)
      return NULL;    /* PVE_NOPEN */
    if (PVWriteHdr(fil, phdr)!= PVE_OK ) {
      fclose(fil);
      return NULL;        /* PVE_WRERR */
    }
    return fil;
}

int PVWriteFile(char *filename, PVSTRUCT *phdr)
    /* write out a PVOC block in memory to a file
       returns PV_NOPEN  if cannot open file
               PV_NPV    if *phdr isn't magic
               PV_WRERR  if write fails  */
{
    FILE        *fil;
    int         err = PVE_OK;
    long        bSize;
    char        *buf;

    if (phdr->magic != PVMAGIC)
      return(PVE_NPV);
    if ((fil = PVOpenWrHdr(filename, phdr)) == NULL)
      return(PVE_NOPEN);
    if (phdr->dataBsize == PV_UNK_LEN)
      bSize = 0;
    else
      bSize = phdr->dataBsize;
    buf   = (char *)PVDataLoc(phdr);
    if (fwrite(buf, (size_t)1, (size_t)bSize, fil) < (size_t)bSize )
      err = PVE_WRERR;
    fclose(fil);
    return(err);
}

void PVCloseWrHdr(FILE *file, PVSTRUCT *phdr)
{
    long len;

    len = ftell(file);
    if (PVReadHdr(file, &tmphdr) == PVE_OK ) {  /* only if we can seek back */
      len -= tmphdr.headBsize;
      if (phdr == NULL) {
        tmphdr.dataBsize = len;
        PVWriteHdr(file, &tmphdr);
      }
      else {
        if (phdr->dataBsize == 0 || phdr->dataBsize == PV_UNK_LEN)
          phdr->dataBsize = len;
        PVWriteHdr(file, phdr);
      }
    }
}

int PVAlloc(
    PVSTRUCT    **pphdr,        /* returns address of new block */
    long        dataBsize,      /* desired bytesize of datablock */
    int         dataFormat,     /* data format - PVMYFLT etc */
    MYFLT       srate,          /* sampling rate of original in Hz */
    int         chans,          /* channels of original .. ? */
    long        frSize,         /* frame size of analysis */
    long        frIncr,         /* frame increment (hop) of analysis */
    long        fBsize,         /* bytes in each data frame of file */
    int         frMode,         /* format of frames: PVPOLAR, PVPVOC etc */
    MYFLT       minF,           /* frequency of lowest bin */
    MYFLT       maxF,           /* frequency of highest bin */
    int         fqMode,         /* freq. spacing mode - PVLIN / PVLOG */
    int         infoBsize)      /* bytes to allocate in info region */
    /* Allocate memory for a new PVSTRUCT+data block;
       fill in header according to passed in data.
       Returns PVE_MALLOC  (& **pphdr = NULL) if malloc fails
               PVE_OK      otherwise  */
{
    long        bSize, hSize;

    hSize = sizeof(PVSTRUCT) + infoBsize - PVDFLTBYTS;
    if (dataBsize == PV_UNK_LEN)
        bSize = hSize;
    else
        bSize = dataBsize + hSize;
/*      bSize += sizeof(PVSTRUCT) + infoBsize - PVDFLTBYTS; Greg Sullivan<<<<< */
    if (( (*pphdr) = (PVSTRUCT *)malloc((size_t)bSize)) == NULL )
      return(PVE_MALLOC);
    (*pphdr)->magic        = PVMAGIC;
    (*pphdr)->headBsize    = hSize;
    (*pphdr)->dataBsize    = dataBsize;
    (*pphdr)->dataFormat   = dataFormat;
    (*pphdr)->samplingRate = srate;
    (*pphdr)->channels     = chans;
    (*pphdr)->frameSize    = frSize;
    (*pphdr)->frameIncr    = frIncr;
    (*pphdr)->frameBsize   = fBsize;
    (*pphdr)->frameFormat  = frMode;
    (*pphdr)->minFreq      = minF;
    (*pphdr)->maxFreq      = maxF;
    (*pphdr)->freqFormat   = fqMode;
    /* leave info bytes undefined */
    return(PVE_OK);
}

void PVFree(PVSTRUCT *phdr)     /* release a PVOC block */
{
    mfree(&cenviron, phdr);     /* let operating system sort it out */
}

char *PVErrMsg(int err)         /* return string for error code */
{
    switch (err) {
    case PVE_OK:        return(Str("No PV error"));
    case PVE_NOPEN:     return(Str("Cannot open PV file"));
    case PVE_NPV:       return(Str("Object/file not PVOC"));
    case PVE_MALLOC:    return(Str("No memory for PVOC"));
    case PVE_RDERR:     return(Str("Error reading PVOC file"));
    case PVE_WRERR:     return(Str("Error writing PVOC file"));
    default:
      sprintf(unspecMsg, "Unspecified error : %d",err);
      return(unspecMsg);
    }
}

void PVDie(int err, char *msg)  /* what else to do with your error code */
{
    if (msg != NULL && *msg)
      err_printf(Str("%s: error: %s (%s)\n"),"pvoc",PVErrMsg(err),msg);
    else
      err_printf(Str("%s: error: %s\n"),"pvoc",PVErrMsg(err));
    exit(1);
}





