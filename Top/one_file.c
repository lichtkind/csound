/*
    one_file.c:

    Copyright (C) 1998 John ffitch

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
#if defined(HAVE_CONFIG_H)
#include "config.h"
#endif

#include "cs.h"
#include "csound.h"
#include <ctype.h>

#define CSD_MAX_LINE_LEN    4096

/* FIXME: remove all these static variables */
static char buffer[CSD_MAX_LINE_LEN];
#ifdef WIN32
#undef L_tmpnam
#define L_tmpnam (200)
#endif
static char orcname[L_tmpnam+4];
       char sconame[L_tmpnam+4];
static char midname[L_tmpnam+4];
static int midiSet;
#ifndef TRUE
#define TRUE (1)
#define FALSE (0)
#endif

typedef struct namelst {
  char           *name;
  struct namelst *next;
} NAMELST;

#ifdef WIN32
char *mytmpnam(char *a)
{
    char *dir = csoundGetEnv(&cenviron, "SFDIR");
    if (dir==NULL) dir = csoundGetEnv(&cenviron, "HOME");
    dir = _tempnam(dir, "cs");
    strcpy(a, dir);
    free(dir);
    return a;
}
#endif

static char *my_fgets(char *s, int n, FILE *stream)
{
    char *a = s;
    if (n <= 1) return NULL;                 /* best of a bad deal */
    do {
      int ch = getc(stream);
      if (ch == EOF) {                       /* error or EOF       */
        if (s == a) return NULL;             /* no chars -> leave  */
        if (ferror(stream)) a = NULL;
        break; /* add NULL even if ferror(), spec says 'indeterminate' */
      }
      if (ch == '\n' || ch == '\r') {   /* end of line ? */
        *(s++) = '\n';                  /* convert */
        if (ch == '\r') {
          ch = getc(stream);
          if (ch != '\n')               /* Mac format */
            ungetc(ch, stream);
        }
        break;
      }
      *(s++) = ch;
    } while (--n > 1);
    *s = '\0';
    return a;
}

void remove_tmpfiles(void *csound)              /* IV - Feb 03 2005 */
{                               /* use one fn to delete all temporary files */
    NAMELST **toremove;
    toremove = (NAMELST**) csoundQueryGlobalVariable(csound, "*tmpFileList");
    if (toremove == NULL)
      return;
    while ((*toremove) != NULL) {
      NAMELST *nxt = (*toremove)->next;
#ifdef BETA
      err_printf("Removing temporary file %s ...\n", (*toremove)->name);
#endif
      if (remove((*toremove)->name))
        err_printf(Str("WARNING: could not remove %s\n"), (*toremove)->name);
      mfree(csound, (*toremove)->name);
      mfree(csound, (*toremove));
      (*toremove) = nxt;
    }
    csoundDestroyGlobalVariable(csound, "*tmpFileList");
}

void add_tmpfile(void *csound, char *name)      /* IV - Feb 03 2005 */
{                               /* add temporary file to delete list */
    NAMELST *tmp, **toremove;
    toremove = (NAMELST**) csoundQueryGlobalVariable(csound, "*tmpFileList");
    if (toremove == NULL) {
      csoundCreateGlobalVariable(csound, "*tmpFileList", sizeof(NAMELST*));
      toremove = (NAMELST**) csoundQueryGlobalVariable(csound, "*tmpFileList");
      (*toremove) = (NAMELST*) NULL;
    }
    tmp = (NAMELST*) mmalloc(csound, sizeof(NAMELST));
    tmp->name = (char*) mmalloc(csound, strlen(name) + 1);
    strcpy(tmp->name, name);
    tmp->next = (*toremove);
    (*toremove) = tmp;
}

extern int argdecode(void*, int, char**);

int readOptions(void *csound, FILE *unf)
{
    char *p;
    int argc = 0;
    char *argv[100];

    while (my_fgets(buffer, CSD_MAX_LINE_LEN, unf)!= NULL) {
      p = buffer;
      while (*p==' ' || *p=='\t') p++;
      if (strstr(p,"</CsOptions>") == buffer) {
        return TRUE;
      }
      /**
       * Allow command options in unified CSD files
       * to begin with the Csound command, so that
       * the command line arguments can be exactly the same in unified files
       * as for regular command line invocation.
       */
      if (*p==';' || *p=='#' || *p=='\n') continue; /* empty or comment line? */
      argc = 0;
      argv[0] = p;
      while (*p==' ' || *p=='\t') p++;  /* Ignore leading space */
      if (*p=='-') {        /* Deal with case where no command name is given */
        argv[0] = "csound";
        argv[1] = p;
        argc++;
      }
      while (*p != '\0') {
        if (*p==' ' || *p=='\t') {
          *p++ = '\0';
#ifdef _DEBUG
          printf("argc=%d argv[%d]=%s\n", argc, argc, argv[argc]);
#endif
          while (*p == ' ' || *p=='\t') p++;
          if (*p==';' ||
              *p=='#' ||
              (*p == '/' && *(p+1) == '/')) { /* Comment line? */
            *p = '\0'; break;
          }
          if (*p == '/' && *(p+1) == '*') {  /* Comment line? */
            p += 2;
          top:
            while (*p != '*' && *p != '\0') p++;
            if (*p == '*' && *(p+1)== '/') {
              p += 2; break;
            }
            if (*p=='*') {
              p++; goto top;
            }
            my_fgets(buffer, CSD_MAX_LINE_LEN, unf);
            p = buffer; goto top;
          }
          argv[++argc] = p;
        }
        else if (*p=='\n') {
          *p = '\0';
          break;
        }
        p++;
      }
#ifdef _DEBUG
      printf("argc=%d argv[%d]=%s\n", argc, argc, argv[argc]);
#endif
      /*      argc++; */                  /* according to Nicola but wrong */
      /* Read an argv thing */
      argdecode(csound, argc, argv);
    }
    return FALSE;
}

static int createOrchestra(void *csound, FILE *unf)
{
    char *p;
    FILE *orcf;

    tmpnam(orcname);            /* Generate orchestra name */
    if ((p=strchr(orcname, '.')) != NULL) *p='\0'; /* with extention */
    strcat(orcname, ".orc");
    orcf = fopen(orcname, "w");
    printf(Str("Creating %s (%p)\n"), orcname, orcf);
    if (orcf==NULL) {
      perror(Str("Failed to create\n"));
      longjmp(((ENVIRON*) csound)->exitjmp_, 1);
    }
    while (my_fgets(buffer, CSD_MAX_LINE_LEN, unf)!= NULL) {
      p = buffer;
      while (*p==' '||*p=='\t') p++;
      if (strstr(p,"</CsInstruments>") == buffer) {
        fclose(orcf);
        add_tmpfile(csound, orcname);           /* IV - Feb 03 2005 */
        return TRUE;
      }
      else fputs(buffer, orcf);
    }
    return FALSE;
}


static int createScore(void *csound, FILE *unf)
{
    char *p;
    FILE *scof;

    tmpnam(sconame);            /* Generate score name */
    if ((p=strchr(sconame, '.')) != NULL) *p='\0'; /* with extention */
    strcat(sconame, ".sco");
    scof = fopen(sconame, "w");
        /*RWD 3:2000*/
    if (scof==NULL)
      return FALSE;

    while (my_fgets(buffer, CSD_MAX_LINE_LEN, unf)!= NULL) {
      p = buffer;
      while (*p==' '||*p=='\t') p++;
     if (strstr(p,"</CsScore>") == buffer) {
        fclose(scof);
        add_tmpfile(csound, sconame);           /* IV - Feb 03 2005 */
        return TRUE;
      }
      else fputs(buffer, scof);
    }
    return FALSE;
}

static int createMIDI(void *csound, FILE *unf)
{
    int size;
    char *p;
    FILE *midf;
    int c;

    if (tmpnam(midname)==NULL) { /* Generate MIDI file name */
      printf(Str("Cannot create temporary file for MIDI subfile\n"));
      longjmp(((ENVIRON*) csound)->exitjmp_, 1);
    }
    if ((p=strchr(midname, '.')) != NULL) *p='\0'; /* with extention */
    strcat(midname, ".mid");
    midf = fopen(midname, "wb");
    if (midf==NULL) {
      printf(Str("Cannot open temporary file (%s) for MIDI subfile\n"),
             midname);
      longjmp(((ENVIRON*) csound)->exitjmp_, 1);
    }
    my_fgets(buffer, CSD_MAX_LINE_LEN, unf);
    if (sscanf(buffer, Str("Size = %d"), &size)==0) {
      printf(Str("Error in reading MIDI subfile -- no size read\n"));
      longjmp(((ENVIRON*) csound)->exitjmp_, 1);
    }
    for (; size > 0; size--) {
      c = getc(unf);
      putc(c, midf);
    }
    fclose(midf);
    add_tmpfile(csound, midname);               /* IV - Feb 03 2005 */
    midiSet = TRUE;
    while (TRUE) {
      if (my_fgets(buffer, CSD_MAX_LINE_LEN, unf)!= NULL) {
        p = buffer;
        while (*p==' '||*p=='\t') p++;
        if (strstr(p,"</CsMidifile>") == buffer) {
          return TRUE;
        }
      }
    }
    return FALSE;
}

static void read_base64(void *csound, FILE *in, FILE *out)
{
    int c;
    int n, nbits;

    n = nbits = 0;
    while ((c = getc(in)) != '=' && c != '<') {
      while (c == ' ' || c == '\t' || c == '\n')
        c = getc(in);
      if (c == '=' || c == '<' || c == EOF)
        break;
      n <<= 6;
      nbits += 6;
      if (isupper(c))
        c -= 'A';
      else if (islower(c))
        c -= ((int) 'a' - 26);
      else if (isdigit(c))
        c -= ((int) '0' - 52);
      else if (c == '+')
        c = 62;
      else if (c == '/')
        c = 63;
      else {
        err_printf("Non base64 character %c(%2x)\n", c, c);
        longjmp(((ENVIRON*) csound)->exitjmp_, 1);
      }
      n |= (c & 0x3F);
      if (nbits >= 8) {
        nbits -= 8;
        c = (n >> nbits) & 0xFF;
        n &= ((1 << nbits) - 1);
        putc(c, out);
      }
    }
    if (c == '<')
      ungetc(c, in);
    if (nbits >= 8) {
      nbits -= 8;
      c = (n >> nbits) & 0xFF;
      n &= ((1 << nbits) - 1);
      putc(c, out);
    }
    if (nbits > 0 && n != 0) {
      err_printf("Truncated byte at end of base64 stream\n");
      longjmp(((ENVIRON*) csound)->exitjmp_, 1);
    }
}

static int createMIDI2(void *csound, FILE *unf)
{
    char *p;
    FILE *midf;

    if (tmpnam(midname)==NULL) { /* Generate MIDI file name */
      printf(Str("Cannot create temporary file for MIDI subfile\n"));
      longjmp(((ENVIRON*) csound)->exitjmp_, 1);
    }
    if ((p=strchr(midname, '.')) != NULL) *p='\0'; /* with extention */
    strcat(midname, ".mid");
    midf = fopen(midname, "wb");
    if (midf==NULL) {
      printf(Str("Cannot open temporary file (%s) for MIDI subfile\n"),
             midname);
      longjmp(((ENVIRON*) csound)->exitjmp_, 1);
    }
    read_base64(csound, unf, midf);
    fclose(midf);
    add_tmpfile(csound, midname);               /* IV - Feb 03 2005 */
    midiSet = TRUE;
    while (TRUE) {
      if (my_fgets(buffer, CSD_MAX_LINE_LEN, unf)!= NULL) {
        p = buffer;
        while (*p==' '||*p=='\t') p++;
        if (strstr(p,"</CsMidifileB>") == buffer) {
          return TRUE;
        }
      }
    }
    return FALSE;
}

static int createSample(void *csound, FILE *unf)
{
    int num;
    FILE *smpf;
    char sampname[256];

    sscanf(buffer, "<CsSampleB filename=%d>", &num);
    sprintf(sampname, "soundin.%d", num);
    if ((smpf=fopen(sampname, "r")) != NULL) {
      printf(Str("File %s already exists\n"), sampname);
      fclose(smpf);
      longjmp(((ENVIRON*) csound)->exitjmp_, 1);
    }
    smpf = fopen(sampname, "wb");
    if (smpf==NULL) {
      printf(Str("Cannot open sample file (%s) subfile\n"), sampname);
      longjmp(((ENVIRON*) csound)->exitjmp_, 1);
    }
    read_base64(csound, unf, smpf);
    fclose(smpf);
    add_tmpfile(csound, sampname);              /* IV - Feb 03 2005 */
    while (TRUE) {
      if (my_fgets(buffer, CSD_MAX_LINE_LEN, unf)!= NULL) {
        char *p = buffer;
        while (*p==' '||*p=='\t') p++;
        if (strstr(p,"</CsSampleB>") == buffer) {
          return TRUE;
        }
      }
    }
    return FALSE;
}

static int createFile(void *csound, FILE *unf)
{
    FILE *smpf;
    char filename[256];

    filename[0] = '\0';
    sscanf(buffer, "<CsFileB filename=%s>", filename);
    if (filename[0] != '\0' && filename[strlen(filename) - 1] == '>')
      filename[strlen(filename) - 1] = '\0';
    if ((smpf=fopen(filename, "r")) != NULL) {
      fclose(smpf);
      printf(Str("File %s already exists\n"), filename);
      longjmp(((ENVIRON*) csound)->exitjmp_, 1);
    }
    smpf = fopen(filename, "wb");
    if (smpf==NULL) {
      printf(Str("Cannot open file (%s) subfile\n"), filename);
      longjmp(((ENVIRON*) csound)->exitjmp_, 1);
    }
    read_base64(csound, unf, smpf);
    fclose(smpf);
    add_tmpfile(csound, filename);              /* IV - Feb 03 2005 */

    while (TRUE) {
      if (my_fgets(buffer, CSD_MAX_LINE_LEN, unf)!= NULL) {
        char *p = buffer;
        while (*p==' '||*p=='\t') p++;
        if (strstr(p,"</CsFileB>") == buffer) {
          return TRUE;
        }
      }
    }
    return FALSE;
}

static int checkVersion(FILE *unf)
{
    char *p;
    int major = 0, minor = 0;
    int result = TRUE;
    int version = csoundGetVersion();
    while (my_fgets(buffer, CSD_MAX_LINE_LEN, unf)!= NULL) {
      p = buffer;
      while (*p==' '||*p=='\t') p++;
      if (strstr(p, "</CsVersion>")==0)
        return result;
      if (strstr(p, "Before")==0) {
        sscanf(p, "Before %d.%d", &major, &minor);
        if (version > ((major * 100) + minor))
          result = FALSE;
      }
      else if (strstr(p, "After")==0) {
        sscanf(p, "After %d.%d", &major, &minor);
        if (version < ((major * 100) + minor))
          result = FALSE;
      }
      else if (sscanf(p, "%d.%d", &major, &minor)==2) {
        sscanf(p, "Before %d.%d", &major, &minor);
        if (version > ((major * 100) + minor))
          result = FALSE;
      }
    }
    return result;
}

static int eat_to_eol(char *buf)
{
    int i=0;
    while(buf[i] != '\n') i++;
    return i;   /* keep the \n for further processing */
}

int blank_buffer(void)
{
    int i=0;
    if (buffer[i] == ';')
      i += eat_to_eol(&buffer[i]);
    while (buffer[i]!='\n' && buffer[i]!='\0') {
      if (buffer[i]!=' ' && buffer[i]!='\t') return FALSE;
      i++;
    }
    return TRUE;
}

int read_unified_file(void *csound, char **pname, char **score)
{
    char *name = *pname;
    FILE *unf  = fopen(name, "rb"); /* Need to open in binary to deal with
                                       MIDI and the like. */
    int result = TRUE;
    int started = FALSE;
    int r;
        /*RWD 3:2000  fopen can fail...*/
    if (unf==NULL)
      return 0;

    orcname[0] = sconame[0] = midname[0] = '\0';
    midiSet = FALSE;
#ifdef _DEBUG
    printf("Calling unified file system with %s\n", name);
#endif
    while (my_fgets(buffer, CSD_MAX_LINE_LEN, unf)) {
      char *p = buffer;
      while (*p==' '||*p=='\t') p++;
      if (strstr(p,"<CsoundSynthesizer>") == buffer ||
          strstr(p,"<CsoundSynthesiser>") == buffer) {
        printf(Str("STARTING FILE\n"));
        started = TRUE;
      }
      else if (strstr(p,"</CsoundSynthesizer>") == buffer ||
               strstr(p,"</CsoundSynthesiser>") == buffer) {
        *pname = orcname;
        *score = sconame;
        if (midiSet) {
          O.FMidiname = midname;
          O.FMidiin = 1;
        }
        fclose(unf);
        return result;
      }
      else if (strstr(p,"<CsOptions>") == buffer) {
        printf(Str("Creating options\n"));
        orchname = NULL;  /* allow orchestra/score name in CSD file */
        r = readOptions(csound, unf);
        result = r && result;
      }
      else if (strstr(p,"<CsInstruments>") == buffer) {
        printf(Str("Creating orchestra\n"));
        r = createOrchestra(csound, unf);
        result = r && result;
      }
      else if (strstr(p,"<CsScore>") == buffer) {
        printf(Str("Creating score\n"));
        r = createScore(csound, unf);
        result = r && result;
      }
      else if (strstr(p,"<CsMidifile>") == buffer) {
        r = createMIDI(csound, unf);
        result = r && result;
      }
      else if (strstr(p,"<CsMidifileB>") == buffer) {
        r = createMIDI2(csound, unf);
        result = r && result;
      }
      else if (strstr(p,"<CsSampleB filename=") == buffer) {
        r = createSample(csound, unf);
        result = r && result;
      }
      else if (strstr(p,"<CsFileB filename=") == buffer) {
        r = createFile(csound, unf);
        result = r && result;
      }
      else if (strstr(p,"<CsVersion>") == buffer) {
        r = checkVersion(unf);
        result = r && result;
      }
      else if (blank_buffer()) continue;
      else if (started && strchr(p, '<') == buffer) {
        printf(Str("unknown command: %s\n"), buffer);
      }
    }
    *pname = orcname;
    *score = sconame;
    if (midiSet) {
      O.FMidiname = midname;
      O.FMidiin = 1;
    }
    fclose(unf);
    return result;
}

