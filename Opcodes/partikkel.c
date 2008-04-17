/*
Partikkel - a granular synthesis module for Csound 5
Copyright (C) 2006-2008 �yvind Brandtsegg, Torgeir Strand Henriksen,
Thom Johansen

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "partikkel.h"
#include <limits.h>
#include <math.h>

#define INITERROR(x) csound->InitError(csound, Str("partikkel: " x))
#define PERFERROR(x) csound->PerfError(csound, Str("partikkel: " x))
#define WARNING(x) csound->Warning(csound, Str("partikkel: " x))

/* Assume csound and p pointers are always available */
#define frand() (csound->RandMT(&p->randstate)/(double)(0xffffffff))

/* linear interpolation between x and y by z
 * NOTE: arguments evaluated more than once, don't pass anything with side
 * effects
 */
#define lrp(x, y, z) ((x) + ((y) - (x))*(z))

/* macro used to wrap an index back to start position if it's out of bounds. */
#define clip_index(index, from, to) \
    if (index > (unsigned)(to) || index < (unsigned)(from)) \
        index = (unsigned)(from);

/* here follows routines for maintaining a linked list of grains */

/* initialises a linked list of NODEs */
static int init_pool(GRAINPOOL *s, unsigned max_grains)
{
    unsigned i;
    NODE **p = &s->grainlist;
    NODE *grainpool = (NODE *)s->mempool;

    s->free_nodes = max_grains;
    /* build list of grains in pool */
    for (i = 0; i < max_grains; ++i) {
        NODE *node;
        *p = grainpool + i;
        node = *p;
        node->next = NULL;
        p = &(node->next);
    }
    return 1;
}

/* returns pointer to new node */
static NODE *get_grain(GRAINPOOL *s)
{
    NODE *ret = s->grainlist;

    if (s->grainlist)
        s->grainlist = s->grainlist->next;
    s->free_nodes--;
    return ret;
}

/* returns a NODE to the pool. function returns pointer to next node */
static NODE *return_grain(GRAINPOOL *s, NODE *c)
{
    NODE *oldnext = c->next;

    c->next = s->grainlist;
    s->grainlist = c;
    s->free_nodes++;
    return oldnext;
}

/* return oldest grain to the pool, we use this when we're out of grains */
static void kill_oldest_grain(GRAINPOOL *s, NODE *n)
{
    while (n->next->next)
        n = n->next;
    return_grain(s, n->next);
    n->next = NULL;
}

static int setup_globals(CSOUND *csound, PARTIKKEL *p)
{
    PARTIKKEL_GLOBALS *pg;
    PARTIKKEL_GLOBALS_ENTRY **pe;

    pg = csound->QueryGlobalVariable(csound, "partikkel");
    if (pg == NULL) {
        char *t;
        int i;

        if (csound->CreateGlobalVariable(csound, "partikkel",
                                         sizeof(PARTIKKEL_GLOBALS)) != 0)
            return INITERROR("could not allocate globals");
        pg = csound->QueryGlobalVariable(csound, "partikkel");
        pg->rootentry = NULL;
        /* build default tables. allocate enough for three, plus extra for the
         * ftable data itself */
        t = pg->tablestorage = csound->Calloc(csound, 4*sizeof(FUNC)
                                                      + 12*sizeof(MYFLT));
        pg->ooo_tab = (FUNC *)t;
        t += sizeof(FUNC) + 2*sizeof(MYFLT);
        pg->zzz_tab = (FUNC *)t;
        t += sizeof(FUNC) + 2*sizeof(MYFLT);
        pg->zzo_tab = (FUNC *)t;
        t += sizeof(FUNC) + 2*sizeof(MYFLT);
        pg->zzhhhhz_tab = (FUNC *)t;
        /* we only fill in the entries in the FUNC struct that we use */
        /* table with data [1.0, 1.0, 1.0], used as default by envelopes */
        pg->ooo_tab->flen = 2;
        pg->ooo_tab->lobits = 31;
        for (i = 0; i <= 2; ++i)
            pg->ooo_tab->ftable[i] = FL(1.0);
        /* table with data [0.0, 0.0, 0.0], used as default by grain
         * distribution table, channel masks and grain waveforms */
        pg->zzz_tab->flen = 2;
        pg->zzz_tab->lobits = 31;
        /* table with data [0.0, 0.0, 1.0], used as default by gain masks,
         * fm index table, and wave start and end freq tables */
        pg->zzo_tab->ftable[2] = FL(1.0);
        /* table with data [0.0, 0.0, 0.5, 0.5, 0.5, 0.5, 0.0], used as default
         * by wave gain table */
        for (i = 2; i <= 5; ++i)
            pg->zzhhhhz_tab->ftable[i] = FL(0.5);
    }
    p->globals = pg;
    if ((int)*p->opcodeid == 0) {
        /* opcodeid 0 means we don't bother with the sync opcode */
        p->globals_entry = NULL;
        return OK;
    }
    /* try to find entry corresponding to our opcodeid */
    pe = &pg->rootentry;
    while (*pe != NULL && (*pe)->id != *p->opcodeid)
        pe = &((*pe)->next);

    /* check if one already existed, if not, create one */
    if (*pe == NULL) {
        *pe = csound->Malloc(csound, sizeof(PARTIKKEL_GLOBALS_ENTRY));
        (*pe)->id = *p->opcodeid;
        /* allocate table for sync data */
        (*pe)->synctab = csound->Calloc(csound, 2*csound->ksmps*sizeof(MYFLT));
        (*pe)->next = NULL;
    }
    p->globals_entry = *pe;
    return OK;
}

/* look up a sample from a csound table using linear interpolation
 * tab: csound table pointer
 * index: fixed point index in the range 0..PHMASK inclusive
 * zscale: 1/(1 << tab->lobits)
 * shift: length of phase register in bits minus length of table in bits
 */
static inline MYFLT lrplookup(FUNC *tab, unsigned phase, MYFLT zscale,
                              unsigned shift)
{
    const unsigned index = phase >> shift;
    const unsigned mask = (1 << shift) - 1;

    MYFLT a = tab->ftable[index];
    MYFLT b = tab->ftable[index + 1];
    MYFLT z = (MYFLT)(phase & mask)*zscale;
    return lrp(a, b, z);
}

static inline double intpow(MYFLT x, unsigned n)
{
    double ans = 1.0;

    while (n != 0) {
        if (n & 1)
            ans *= x;
        n >>= 1;
        x *= x;
    }
    return ans;
}

/* dsf synthesis for trainlets */
static inline MYFLT dsf(FUNC *tab, GRAIN *grain, double beta, MYFLT zscale,
                        unsigned cosineshift)
{
    MYFLT numerator, denominator, cos_beta;
    MYFLT lastharmonic, result;
    unsigned fbeta, N = grain->harmonics;
    const MYFLT a = grain->falloff;
    const MYFLT a_pow_N = grain->falloff_pow_N;
    fbeta = (unsigned)(beta*(double)UINT_MAX);

    cos_beta = lrplookup(tab, fbeta, zscale, cosineshift);
    denominator = FL(1.0) - FL(2.0)*a*cos_beta + a*a;
    if (denominator < FL(1e-4) && denominator > FL(-1e-4)) {
        /* handle this special case to avoid divison by zero */
        result = N - FL(1.0);
    } else {
        /* this factor can also serve as a last, fadable harmonic, if we in the
         * future want to fade the number of harmonics smoothly */
        lastharmonic = a_pow_N*lrplookup(tab, fbeta*N, zscale, cosineshift);
        numerator = FL(1.0) - a*cos_beta - lastharmonic
            + a*a_pow_N*lrplookup(tab, (N - 1)*fbeta, zscale, cosineshift);
        result = numerator/denominator - FL(1.0);
    }
    return result;
}

static int partikkel_init(CSOUND *csound, PARTIKKEL *p)
{
    int32 size;
    int ret;

    if ((ret = setup_globals(csound, p)) != OK)
        return ret;

    p->grainroot = NULL;
    /* set grainphase to 1.0 to make grain scheduler create a grain immediately
     * after starting opcode */
    p->grainphase = 1.0;
    p->num_outputs = csound->GetOutputArgCnt(p); /* save for faster access */
    /* resolve tables with no default table handling */
    p->costab = csound->FTFind(csound, p->cosine);
    /* resolve some tables with default table handling */
    p->disttab = *p->dist >= FL(0.0)
                 ? csound->FTFind(csound, p->dist)
                 : p->globals->zzz_tab;
    p->gainmasktab = *p->gainmasks >= FL(0.0)
                     ? csound->FTFind(csound, p->gainmasks)
                     : p->globals->zzo_tab;
    p->channelmasktab = *p->channelmasks >= FL(0.0)
                        ? csound->FTFind(csound, p->channelmasks)
                        : p->globals->zzz_tab;
    p->env_attack_tab = *p->env_attack >= FL(0.0)
                        ? csound->FTFind(csound, p->env_attack)
                        : p->globals->ooo_tab;
    p->env_decay_tab = *p->env_decay >= FL(0.0)
                       ? csound->FTFind(csound, p->env_decay)
                       : p->globals->ooo_tab;
    p->env2_tab = *p->env2 >= FL(0.0)
                   ? csound->FTFind(csound, p->env2)
                   : p->globals->ooo_tab;
    p->wavfreqstarttab = *p->wavfreq_startmuls >= FL(0.0)
                         ? csound->FTFind(csound, p->wavfreq_startmuls)
                         : p->globals->zzo_tab;
    p->wavfreqendtab = *p->wavfreq_endmuls >= FL(0.0)
                       ? csound->FTFind(csound, p->wavfreq_endmuls)
                       : p->globals->zzo_tab;
    p->fmamptab = *p->fm_indices >= FL(0.0)
                  ? csound->FTFind(csound, p->fm_indices)
                  : p->globals->zzo_tab;
    p->wavgaintab = *p->waveamps >= FL(0.0)
                    ? csound->FTFind(csound, p->waveamps)
                    : p->globals->zzhhhhz_tab;

    if (!p->disttab)
        return INITERROR("unable to load distribution table");
    if (!p->costab)
        return INITERROR("unable to load cosine table");
    if (!p->gainmasktab)
        return INITERROR("unable to load gain mask table");
     if (!p->channelmasktab)
        return INITERROR("unable to load channel mask table");
    if (!p->env_attack_tab || !p->env_decay_tab || !p->env2_tab)
        return INITERROR("unable to load envelope table");
    if (!p->wavfreqstarttab)
        return INITERROR("unable to load start frequency scaler table");
    if (!p->wavfreqendtab)
        return INITERROR("unable to load end frequency scaler table");
    if (!p->fmamptab)
        return INITERROR("unable to load FM index table");
    if (!p->wavgaintab)
        return INITERROR("unable to load wave gain table");

    p->disttabshift = sizeof(unsigned)*CHAR_BIT -
                      (unsigned)(log((double)p->disttab->flen)/log(2.0));
    p->cosineshift = sizeof(unsigned)*CHAR_BIT -
                     (unsigned)(log((double)p->costab->flen)/log(2.0));
    p->zscale = FL(1.0)/FL(1 << p->cosineshift);
    p->wavfreqstartindex = p->wavfreqendindex = 0;
    p->gainmaskindex = p->channelmaskindex = 0;
    p->wavgainindex = 0;
    p->fmampindex = 0;
    p->distindex = 0;
    p->synced = 0;
    /* allocate memory for the grain mix buffer */
    size = csound->ksmps*sizeof(MYFLT);
    if (p->aux.auxp == NULL || p->aux.size < size)
        csound->AuxAlloc(csound, size, &p->aux);
    memset(p->aux.auxp, 0, sizeof(MYFLT)*csound->ksmps);

    /* allocate memory for the grain pool and initialize it*/
    if (*p->max_grains < FL(1.0))
        return INITERROR("maximum number of grains need to be non-zero "
                         "and positive");
    size = ((unsigned)*p->max_grains)*sizeof(NODE);
    if (p->aux2.auxp == NULL || p->aux2.size < size)
        csound->AuxAlloc(csound, size, &p->aux2);
    p->gpool.mempool = p->aux2.auxp;
    init_pool(&p->gpool, (unsigned)*p->max_grains);

    /* find out which of the xrate parameters are arate */
    p->grainfreq_arate = csound->GetInputArgAMask(p) & 1 ? 1 : 0;
    p->out_of_voices_warning = 0; /* reset user warning indicator */
    csound->SeedRandMT(&p->randstate, NULL, csound->GetRandomSeedFromTime());
    return OK;
}

/* n is sample number for which the grain is to be scheduled */
static int schedule_grain(CSOUND *csound, PARTIKKEL *p, NODE *node, int32 n)
{
    /* make a new grain */
    MYFLT startfreqscale, endfreqscale;
    MYFLT maskgain, maskchannel;
    int samples;
    double rcp_samples; /* 1/samples */
    double phase_corr;
    GRAIN *grain = &node->grain;
    unsigned int i;
    unsigned int chan;
    MYFLT graingain;
    MYFLT *gainmasks = p->gainmasktab->ftable;
    MYFLT *chanmasks = p->channelmasktab->ftable;
    MYFLT *freqstarts = p->wavfreqstarttab->ftable;
    MYFLT *freqends = p->wavfreqendtab->ftable;
    MYFLT *fmamps = p->fmamptab->ftable;
    MYFLT *wavgains = p->wavgaintab->ftable;
    unsigned wavgainsindex;

    /* the table boundary limits might well change at any time, so we do the
     * boundary clipping before using it to fetch a value */

    /* get gain mask */
    clip_index(p->gainmaskindex, gainmasks[0], gainmasks[1]);
    maskgain = gainmasks[p->gainmaskindex + 2];
    p->gainmaskindex++;

    /* get channel mask */
    clip_index(p->channelmaskindex, chanmasks[0], chanmasks[1]);
    maskchannel = chanmasks[p->channelmaskindex + 2];
    p->channelmaskindex++;

    /* get frequency sweep start scaler */
    clip_index(p->wavfreqstartindex, freqstarts[0], freqstarts[1]);
    startfreqscale = freqstarts[p->wavfreqstartindex + 2];
    p->wavfreqstartindex++;

    /* get frequency sweep end scaler */
    clip_index(p->wavfreqendindex, freqends[0], freqends[1]);
    endfreqscale = freqends[p->wavfreqendindex + 2];
    p->wavfreqendindex++;

    /* get fm modulation index */
    clip_index(p->fmampindex, fmamps[0], fmamps[1]);
    grain->fmamp = fmamps[p->fmampindex + 2];
    p->fmampindex++;

    /* calculate waveform gain table index for later use */
    clip_index(p->wavgainindex, wavgains[0], wavgains[1]);
    wavgainsindex = 5*p->wavgainindex++;

    /* check if our mask gain is zero or if stochastic masking takes place */
    if ((fabs(maskgain) < FL(1e-8)) || (frand() > 1.0 - *p->randommask)) {
        /* grain is either masked out or has a zero amplitude, so we cancel it
         * and proceed with scheduling our next grain */
        return_grain(&p->gpool, node);
        return OK;
    }

    grain->env2amount = *p->env2_amount;
    grain->envattacklen = (1.0 - *p->sustain_amount)*(*p->a_d_ratio);
    grain->envdecaystart = grain->envattacklen + *p->sustain_amount;
    grain->fmenvtab = p->fmenvtab;

    /* place a grain in between two channels according to channel mask value */
    chan = (unsigned)maskchannel;
    if (chan >= p->num_outputs) {
        return_grain(&p->gpool, node);
        return PERFERROR("channel mask specifies non-existing output channel");
    }
    grain->gain1 = FL(1.0) - (maskchannel - chan);
    grain->gain2 = maskchannel - chan;
    grain->chan1 = chan;
    grain->chan2 = p->num_outputs > chan + 1 ? chan + 1 : 0;

    graingain = *p->amplitude*maskgain;
    /* duration in samples */
    samples = (int)((csound->esr*(*p->duration)/1000.0) + 0.5);
    /* if grainlength is below one sample, we'll just cancel it */
    if (samples <= 0) {
        return_grain(&p->gpool, node);
        return OK;
    }
    rcp_samples = 1.0/(double)samples;
    grain->start = n;
    grain->stop = n + samples;
    /* if grainphase is larger than graininc, the grain is not synchronous and
     * we'll skip sub-sample grain placement. also check for divide by zero */
    if (p->grainphase < p->graininc && p->graininc != 0.0)
        phase_corr = p->graininc != 0.0 ? p->grainphase/p->graininc : 0.0;
    else
        phase_corr = 0;

    /* set up the four wavetables and dsf to use in the grain */
    for (i = 0; i < 5; ++i) {
        WAVEDATA *curwav = &grain->wav[i];
        MYFLT freqmult = i != WAV_TRAINLET
                         ? *(*(&p->wavekey1 + i))*(*p->wavfreq)
                         : *p->trainletfreq;
        MYFLT startfreq = freqmult*startfreqscale;
        MYFLT endfreq = freqmult*endfreqscale;
        MYFLT *samplepos = *(&p->samplepos1 + i);
        MYFLT enddelta;

        curwav->table = i != WAV_TRAINLET ? p->wavetabs[i] : p->costab;
        curwav->gain = wavgains[wavgainsindex + i + 2]*graingain;

        /* check if waveform gets zero volume. if so, let's mark it as unused
         * and move on to the next one */
        if (fabs(curwav->gain) < FL(1e-10)) {
            curwav->table = NULL;
            continue;
        }

        /* now do some trainlet specific setup */
        if (i == WAV_TRAINLET) {
            double normalize, nh;
            MYFLT maxfreq = startfreq > endfreq ? startfreq : endfreq;

            /* limit dsf harmonics to nyquist to avoid aliasing.
             * minumum number of harmonics is 2, since 1 would yield just dc,
             * which we remove anyway */
            nh = 0.5*csound->esr/fabs(maxfreq);
            if (nh > fabs(*p->harmonics))
                nh = fabs(*p->harmonics);
            grain->harmonics = (unsigned)nh + 1;
            if (grain->harmonics < 2)
                grain->harmonics = 2;
            grain->falloff = *p->falloff;
            grain->falloff_pow_N = intpow(grain->falloff, grain->harmonics);
            /* normalize trainlets to uniform peak, using geometric sum */
            if (fabs(grain->falloff) > 0.9999 && fabs(grain->falloff) < 1.0001)
                /* limit case for falloff = 1 */
                normalize = 1.0/(double)grain->harmonics;
            else
                normalize = (1.0 - fabs(grain->falloff))
                            /(1.0 - fabs(grain->falloff_pow_N));
            curwav->gain *= normalize;
        }

        curwav->delta = startfreq*csound->onedsr;
        enddelta = endfreq*csound->onedsr;

        if (i != WAV_TRAINLET) {
            /* set wavphase to samplepos parameter */
            curwav->phase = samplepos[n];
        } else {
            /* set to 0.5 so the dsf pulse doesn't occur at the very start of
             * the grain where it'll probably be enveloped away anyway */
            curwav->phase = 0.5;
        }
        /* place grain between samples. this is especially important to make
         * high frequency synchronous grain streams sounds right */
        curwav->phase += phase_corr*startfreq*csound->onedsr;

        /* clamp phase in case it's out of bounds */
        curwav->phase = curwav->phase > 1.0 ? 1.0 : curwav->phase;
        curwav->phase = curwav->phase < 0.0 ? 0.0 : curwav->phase;
        /* phase and delta for wavetable synthesis are scaled by table length */
        if (i != WAV_TRAINLET) {
            double tablen = (double)curwav->table->flen;

            curwav->phase *= tablen;
            curwav->delta *= tablen;
            enddelta *= tablen;
        }

        /* the sweep curve generator is a first order iir filter */
        if (curwav->delta == enddelta || *p->freqsweepshape == FL(0.5)) {
            /* special case for linear sweep */
            curwav->sweepdecay = 1.0;
            curwav->sweepoffset = (enddelta - curwav->delta)*rcp_samples;
        } else {
            /* handle extreme cases the generic code doesn't handle too well */
            if (*p->freqsweepshape < FL(0.001)) {
                curwav->sweepdecay = 1.0;
                curwav->sweepoffset = 0.0;
            } else if (*p->freqsweepshape > FL(0.999)) {
                curwav->sweepdecay = 0.0;
                curwav->sweepoffset = enddelta;
            } else {
                double start_offset, total_decay, t;

                t = fabs((*p->freqsweepshape - 1.0)/(*p->freqsweepshape));
                curwav->sweepdecay = pow(t, 2.0*rcp_samples);
                total_decay = t*t; /* pow(curwav->sweepdecay, samples) */
                start_offset = (enddelta - curwav->delta*total_decay)/
                               (1.0 - total_decay);
                curwav->sweepoffset = start_offset*(1.0 - curwav->sweepdecay);
            }
        }
    }

    grain->envinc = rcp_samples;
    grain->envphase = phase_corr*grain->envinc;
    /* link new grain into the list */
    node->next = p->grainroot;
    p->grainroot = node;
    return OK;
}

/* this function schedules the grains that are bound to happen this k-period */
static int schedule_grains(CSOUND *csound, PARTIKKEL *p)
{
    int32 n;
    NODE *node;
    MYFLT **waveformparams = &p->waveform1;
    MYFLT grainfreq = *p->grainfreq;

    /* krate table lookup, first look up waveform ftables */
    for (n = 0; n < 4; ++n) {
        p->wavetabs[n] = *waveformparams[n] >= FL(0.0)
                         ? csound->FTnp2Find(csound, waveformparams[n])
                         : p->globals->zzz_tab;
        if (p->wavetabs[n] == NULL)
            return PERFERROR("unable to load waveform table");
    }
    /* look up fm envelope table for use in grains scheduled this kperiod */
    p->fmenvtab = *p->fm_env >= FL(0.0)
                  ? csound->FTFind(csound, p->fm_env)
                  : p->globals->ooo_tab;
    if (!p->fmenvtab)
        return PERFERROR("unable to load FM envelope table");

    /* start grain scheduling */
    for (n = 0; n < csound->ksmps; ++n) {
        int phase_wrapped = 0;
        if (p->grainfreq_arate)
            grainfreq = p->grainfreq[n];
        p->graininc = fabs(grainfreq*csound->onedsr);

        if (p->sync[n] >= FL(1.0)) {
            /* we got a full sync pulse, hardsync grain clock if needed */
            if (!p->synced) {
                p->grainphase = 1.0;
                p->synced = 1;
            } else {
                /* if sync is held high, stop the grain clock until it goes
                 * back to zero or below again */
                p->graininc = 0.0;
            }
        } else {
            /* softsync-like functionality where we advance the grain clock by
             * the amount given by the sync value */
            if (p->sync[n]) {
                p->grainphase += p->sync[n];
                p->grainphase = p->grainphase > 1.0 ? 1.0 : p->grainphase;
                p->grainphase = p->grainphase < 0.0 ? 0.0 : p->grainphase;
            }
            p->synced = 0;
        }

        if (p->grainphase >= 1.0) {
            /* phase has wrapped because we're at a period, so find out where
             * to create the next grain */
            if (*p->distribution >= FL(0.0)) {
                /* positive distrib, choose random point in table */
                unsigned rnd = csound->RandMT(&p->randstate);
                MYFLT offset = p->disttab->ftable[rnd >> p->disttabshift];
                p->nextgrainphase = *p->distribution*offset;
            } else {
                /* negative distrib, choose sequential point in table */
                MYFLT offset = p->disttab->ftable[p->distindex++];
                p->nextgrainphase = -*p->distribution*offset;
                if (p->distindex >= p->disttab->flen)
                    p->distindex = 0;
            }
            while (p->grainphase >= 1.0)
                p->grainphase -= 1.0;
            phase_wrapped = 1;
        }

        /* trigger when we have passed nextgrainphase */
        if (p->grainphase >= p->nextgrainphase
            && (p->prevphase < p->nextgrainphase || phase_wrapped)) {
            /* check if there are any grains left in the pool */
            if (!p->gpool.free_nodes) {
                if (!p->out_of_voices_warning) {
                    WARNING("maximum number of grains reached");
                    p->out_of_voices_warning = 1; /* we only warn once */
                }
                kill_oldest_grain(&p->gpool, p->grainroot);
            }
            /* add a new grain */
            node = get_grain(&p->gpool);
            /* check first, in case we'll change the above behaviour of
             * killing a grain */
            if (node) {
                int ret = schedule_grain(csound, p, node, n);

                if (ret != OK)
                    return ret;
            }
            /* create a sync pulse for use in partikkelsync */
            if (p->globals_entry)
                p->globals_entry->synctab[n] = FL(1.0);
        }
        /* store away the scheduler phase for use in partikkelsync */
        if (p->globals_entry)
            p->globals_entry->synctab[csound->ksmps + n] = p->grainphase;
        p->prevphase = p->grainphase;
        p->grainphase += p->graininc;
    }
    return OK;
}

/* do the actual waveform synthesis */
static inline void render_grain(CSOUND *csound, PARTIKKEL *p, GRAIN *grain)
{
    int i;
    unsigned n;
    MYFLT *out1 = *(&(p->output1) + grain->chan1);
    MYFLT *out2 = *(&(p->output1) + grain->chan2);
    unsigned stop = grain->stop > csound->ksmps
                    ? csound->ksmps : grain->stop;
    MYFLT *buf = (MYFLT *)p->aux.auxp;

    for (i = 0; i < 5; ++i) {
        WAVEDATA *curwav = &grain->wav[i];
        double fmenvphase = grain->envphase;
        MYFLT fmenv;

        /* check if ftable is to be rendered */
        if (curwav->table == NULL)
            continue;

        /* NOTE: the main synthesis loop is duplicated for both wavetable and
         * trainlet synthesis for speed */
        if (i != WAV_TRAINLET) {
            /* wavetable synthesis */
            for (n = grain->start; n < stop; ++n) {
                double tablen = (double)curwav->table->flen;
                unsigned x0;
                MYFLT frac;

                /* make sure phase accumulator stays within bounds */
                while (curwav->phase >= tablen)
                    curwav->phase -= tablen;
                while (curwav->phase < 0.0)
                    curwav->phase += tablen;

                /* sample table lookup with linear interpolation */
                x0 = (unsigned)curwav->phase;
                frac = curwav->phase - x0;
                buf[n] += lrp(curwav->table->ftable[x0],
                              curwav->table->ftable[x0 + 1],
                              frac)*curwav->gain;

                fmenv = grain->fmenvtab->ftable[(unsigned)(fmenvphase*FMAXLEN)
                                                >> grain->fmenvtab->lobits];
                fmenvphase += grain->envinc;
                curwav->phase += curwav->delta
                                 + curwav->delta*p->fm[n]*grain->fmamp*fmenv;
                /* apply sweep */
                curwav->delta = curwav->delta*curwav->sweepdecay
                                + curwav->sweepoffset;
            }
        } else {
            /* trainlet synthesis */
            for (n = grain->start; n < stop; ++n) {
                while (curwav->phase >= 1.0)
                    curwav->phase -= 1.0;
                while (curwav->phase < 0.0)
                    curwav->phase += 1.0;

                /* dsf/trainlet synthesis */
                buf[n] += curwav->gain*dsf(p->costab, grain, curwav->phase,
                                           p->zscale, p->cosineshift);

                fmenv = grain->fmenvtab->ftable[(unsigned)(fmenvphase*FMAXLEN)
                                                >> grain->fmenvtab->lobits];
                fmenvphase += grain->envinc;
                curwav->phase += curwav->delta
                                 + curwav->delta*p->fm[n]*grain->fmamp*fmenv;
                curwav->delta = curwav->delta*curwav->sweepdecay
                                + curwav->sweepoffset;
            }
        }
    }

    /* apply envelopes */
    for (n = grain->start; n < stop; ++n) {
        MYFLT env, env2, output;
        double envphase;
        FUNC *envtable;

        /* apply envelopes */
        if (grain->envphase < grain->envattacklen) {
            envtable = p->env_attack_tab;
            envphase = grain->envphase/grain->envattacklen;
        } else if (grain->envphase < grain->envdecaystart) {
            /* for sustain, use last sample in attack table */
            envtable = p->env_attack_tab;
            envphase = 1.0;
        } else if (grain->envphase < 1.0) {
            envtable = p->env_decay_tab;
            envphase = (grain->envphase - grain->envdecaystart)/(1.0 -
                       grain->envdecaystart);
        } else {
            /* clamp envelope phase because of round-off errors */
            envtable = grain->envdecaystart < 1.0 ?
                       p->env_decay_tab : p->env_attack_tab;
            envphase = grain->envphase = 1.0;
        }

        /* fetch envelope values */
        env = envtable->ftable[(unsigned)(envphase*FMAXLEN)
                                >> envtable->lobits];
        env2 = p->env2_tab->ftable[(unsigned)(grain->envphase*FMAXLEN)
                                   >> p->env2_tab->lobits];
        env2 = FL(1.0) - grain->env2amount + grain->env2amount*env2;
        grain->envphase += grain->envinc;
        /* generate grain output sample */
        output = buf[n]*env*env2;
        /* now distribute this grain to the output channels it's supposed to
         * end up in, as decided by the channel mask */
        out1[n] += output*grain->gain1;
        out2[n] += output*grain->gain2;
    }
    /* now clear the area we just worked in */
    memset(buf + grain->start, 0, (stop - grain->start)*sizeof(MYFLT));
}

static int partikkel(CSOUND *csound, PARTIKKEL *p)
{
    int ret, n;
    NODE **nodeptr;
    MYFLT **outputs = &p->output1;

    if (p->aux.auxp == NULL || p->aux2.auxp == NULL)
        return PERFERROR("not initialised");

    /* schedule grains that'll happen this kperiod */
    if ((ret = schedule_grains(csound, p)) != OK)
        return ret;

    /* clear output buffers, we'll be accumulating our outputs */
    for (n = 0; n < p->num_outputs; ++n)
        memset(outputs[n], 0, sizeof(MYFLT)*csound->ksmps);

    /* prepare to traverse grain list, start at grain list root */
    nodeptr = &p->grainroot;
    while (*nodeptr) {
        GRAIN *grain = &((*nodeptr)->grain);

        /* render current grain to outputs */
        render_grain(csound, p, grain);
        /* check if grain is finished */
        if (grain->stop <= csound->ksmps) {
            /* grain is finished, deactivate it */
            *nodeptr = return_grain(&p->gpool, *nodeptr);
        } else {
            /* extend grain lifetime with one k-period and find next grain */
            grain->start = 0;
            grain->stop -= csound->ksmps;
            nodeptr = &((*nodeptr)->next);
        }
    }
    return OK;
}

/* partikkelsync stuff */
static int partikkelsync_init(CSOUND *csound, PARTIKKELSYNC *p)
{
    PARTIKKEL_GLOBALS *pg;
    PARTIKKEL_GLOBALS_ENTRY *pe;

    if ((int)*p->opcodeid == 0)
        return csound->InitError(csound,
            Str("partikkelsync: opcode id needs to be a non-zero integer"));
    pg = csound->QueryGlobalVariable(csound, "partikkel");
    if (pg == NULL || pg->rootentry == NULL)
        return csound->InitError(csound,
            Str("partikkelsync: could not find opcode id"));
    pe = pg->rootentry;
    while (pe->id != *p->opcodeid && pe->next != NULL)
        pe = pe->next;
    if (pe->id != *p->opcodeid)
        return csound->InitError(csound,
            Str("partikkelsync: could not find opcode id"));
    p->ge = pe;
    /* find out if we're supposed to output grain scheduler phase too */
    p->output_schedphase = csound->GetOutputArgCnt(p) > 1;
    return OK;
}

static int partikkelsync(CSOUND *csound, PARTIKKELSYNC *p)
{
    /* write sync pulse data */
    memcpy(p->syncout, p->ge->synctab, csound->ksmps*sizeof(MYFLT));
    /* write scheduler phase data, if user wanted it */
    if (p->output_schedphase) {
        memcpy(p->schedphaseout, p->ge->synctab + csound->ksmps,
               csound->ksmps*sizeof(MYFLT));
    }
    /* clear first half of sync table to get rid of old sync pulses */
    memset(p->ge->synctab, 0, csound->ksmps*sizeof(MYFLT));
    return OK;
}

static OENTRY localops[] = {
    {
        "partikkel", sizeof(PARTIKKEL), 5,
        "ammmmmmm",
        "xkiakiiikkkkikkiiaikikkkikkkkkiaaaakkkkio",
        (SUBR)partikkel_init,
        (SUBR)NULL,
        (SUBR)partikkel
    },
    {
        "partikkelsync", sizeof(PARTIKKELSYNC), 5,
        "am", "i",
        (SUBR)partikkelsync_init,
        (SUBR)NULL,
        (SUBR)partikkelsync
    }
};

LINKAGE
