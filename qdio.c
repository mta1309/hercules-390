/* QDIO.C       (c) Copyright Jan Jaeger, 2003-2011                  */
/*              (C) Copyright Harold Grovesteen, 2011                */
/*              Queued Direct Input Output                           */
/*                                                                   */

// $Id$

/*      This module contains the Signal Adapter instruction          */
/*      and QEBSM EQBS and SQBS instructions                         */

#include "hstdinc.h"

#if !defined(_HENGINE_DLL_)
#define _HENGINE_DLL_
#endif

#if !defined(_QDIO_C_)
#define _QDIO_C_
#endif

#include "hercules.h"

#include "opcode.h"

#include "qeth.h"

#include "inline.h"

#undef PTIO
 #define PTIO(_class, _name) \
 PTT(PTT_CL_ ## _class,_name,regs->GR_L(1),(U32)(effective_addr2 & 0xffffffff),regs->psw.IA_L)

#if defined(FEATURE_QUEUED_DIRECT_IO)

/*-------------------------------------------------------------------*/
/* B274 SIGA  - Signal Adapter                                   [S] */
/*-------------------------------------------------------------------*/
DEF_INST(signal_adapter)
{
int     b2;
RADR    effective_addr2;
DEVBLK *dev;                            /* -> device block           */

    S(inst, regs, b2, effective_addr2);

//  ARCH_DEP(display_inst) (regs, inst);

    PRIV_CHECK(regs);

#if defined(_FEATURE_SIE)
    if(SIE_STATNB(regs, EC3, SIGAA))
        longjmp(regs->progjmp, SIE_INTERCEPT_INST);
#endif /*defined(_FEATURE_SIE)*/

    PTIO(IO,"SIGA");

    /* Specification exception if invalid function code */
    if((regs->GR_L(0)
#if defined(FEATURE_QEBSM)
                      & ~(FACILITY_ENABLED(QEBSM,regs) ? SIGA_TOKEN : 0)
#endif /*defined(FEATURE_QEBSM)*/
                      ) > SIGA_FC_MAX)
        ARCH_DEP(program_interrupt) (regs, PGM_SPECIFICATION_EXCEPTION);

    /* Locate the device block for this subchannel */
#if defined(FEATURE_QEBSM)
    if(FACILITY_ENABLED(QEBSM, regs) 
      && (regs->GR_L(0) & SIGA_TOKEN))
        dev = find_device_by_subchan (TKN2IOID(regs->GR_G(1)));
    else
#endif /*defined(FEATURE_QEBSM)*/
    {
        /* Program check if the ssid including lcss is invalid */
        SSID_CHECK(regs);
        dev = find_device_by_subchan (regs->GR_L(1));
    }

    /* Condition code 3 if subchannel does not exist,
       is not valid, or is not enabled or is not a QDIO subchannel */
    if (dev == NULL
        || (dev->pmcw.flag5 & PMCW5_V) == 0
        || (dev->pmcw.flag5 & PMCW5_E) == 0
        || (dev->pmcw.flag4 & PMCW4_Q) == 0)
    {
        PTIO(ERR,"*SIGA");
#if defined(_FEATURE_QUEUED_DIRECT_IO_ASSIST)
        SIE_INTERCEPT(regs);
#endif
        regs->psw.cc = 3;
        return;
    }

    /* Obtain the device lock */
    obtain_lock (&dev->lock);

    /* Check that device is QDIO active */
    if ((dev->scsw.flag2 & SCSW2_Q) == 0)
    {
        PTIO(ERR,"*SIGA");
        release_lock (&dev->lock);
        regs->psw.cc = 1;
        return;
    }

    switch(
           regs->GR_L(0)
#if defined(FEATURE_QEBSM)
                         & ~(FACILITY_ENABLED(QEBSM,regs) ? SIGA_TOKEN : 0)
#endif /*defined(FEATURE_QEBSM)*/
                         ) {

    case SIGA_FC_R:
        if(dev->hnd->siga_r)
            regs->psw.cc = (dev->hnd->siga_r) (dev, regs->GR_L(2) );
        else
        {
            PTIO(ERR,"*SIGA");
            regs->psw.cc = 3;
        }
        break;

    case SIGA_FC_W:
        if(dev->hnd->siga_w)
            regs->psw.cc = (dev->hnd->siga_w) (dev, regs->GR_L(2) );
        else
        {
            PTIO(ERR,"*SIGA");
            regs->psw.cc = 3;
        }
        break;

    case SIGA_FC_S:
#if SIGA_FC_MAX >= SIGA_FC_S
        if(dev->hnd->siga_s)
            regs->psw.cc = (dev->hnd->siga_s) (dev, regs->GR_L(2) );
        else
        {
            PTIO(ERR,"*SIGA");
            regs->psw.cc = 3;
        }
#else
        /* No signalling required for sync requests as we emulate
           a real machine */
        regs->psw.cc = 0;
#endif
        break;

#if SIGA_FC_MAX >= SIGA_FC_M
    case SIGA_FC_M:
        if(dev->hnd->siga_m)
            regs->psw.cc = (dev->hnd->siga_m) (dev, regs->GR_L(2) );
        else
        {
            PTIO(ERR,"*SIGA");
            regs->psw.cc = 3;
        }
        break;
#endif

    default:
        PTIO(ERR,"*SIGA");

    }

    release_lock (&dev->lock);

}


#if defined(FEATURE_QEBSM)
/*-------------------------------------------------------------------*/
/* EB8A SQBS  - Set Queue Buffer State                         [RSY] */
/*-------------------------------------------------------------------*/
DEF_INST(set_queue_buffer_state)
{
int     r1, r3;                /* Register numbers                   */
int     b2;                    /* effective address base             */
VADR    effective_addr2;       /* effective address                  */

DEVBLK  *dev;                  /* Data device DEVBLK                 */
OSA_GRP *grp;                  /* OSA Group device structure         */

BYTE    state;                 /* Target buffer state                */
BYTE    oldstate;              /* Current buffer state               */
BYTE    nextstate;             /* Next buffer's state                */
U32     queues;                /* Total number of queues             */
U32     count;                 /* Number of buffers to set           */
U32     qidx;                  /* Queue index                        */
U32     bidx;                  /* SLSB buffer state index            */
U64     slsba;                 /* Storage list state block address   */

    RSY(inst, regs, r1, r3, b2, effective_addr2);

    FACILITY_CHECK(QEBSM, regs);

//  ARCH_DEP(display_inst) (regs, inst);

    PRIV_CHECK(regs);

#if defined(_FEATURE_SIE)
    if(SIE_STATNB(regs, EC3, SIGAA))
        longjmp(regs->progjmp, SIE_INTERCEPT_INST);
#endif /*defined(_FEATURE_SIE)*/

    PTIO(INF,"SQBS");

    dev = find_device_by_subchan(TKN2IOID(regs->GR_G(1)));

    /* Condition code 3 if subchannel does not exist,
       is not valid, or is not enabled or is not a QDIO subchannel */
    if (dev == NULL
        || (dev->pmcw.flag5 & PMCW5_V) == 0
        || (dev->pmcw.flag5 & PMCW5_E) == 0
        || (dev->pmcw.flag4 & PMCW4_Q) == 0)
    {
        PTIO(ERR,"*SQBS");
#if defined(_FEATURE_QUEUED_DIRECT_IO_ASSIST)
        SIE_INTERCEPT(regs);
#endif
        regs->GR_H(r3) = 1;       /* Activation error */
        regs->psw.cc = 3;         /* Guess */
        return;
    }

#if 0
    /* Check that device is QDIO active */
    if ((dev->scsw.flag2 & SCSW2_Q) == 0)
    {
        PTIO(ERR,"*SQBS");
        regs->GR_H(r3) = 1;       /* Activation error */
        regs->psw.cc = 1;
        return;
    }
#endif

    /* Locate the group device block */
    grp = (OSA_GRP*)dev->group->grp_data;

    qidx  = regs->GR_H(r1);       /* Queue index */
    bidx  = regs->GR_L(r1);       /* Buffer index */
    count = regs->GR_L(r3) < 128 ? regs->GR_L(r3) : 128; /* Number of buffers */
    state = effective_addr2 & 0xFF;

    queues = (U32)(grp->i_qcnt + grp->o_qcnt);

    if ( (qidx >= queues) || (bidx > 127) )
        ARCH_DEP(program_interrupt) (regs, PGM_SPECIFICATION_EXCEPTION);

    if (qidx < (U32)grp->i_qcnt)
        slsba = grp->i_slsbla[qidx];
    else
        slsba = grp->o_slsbla[qidx - grp->i_qcnt];

    oldstate = nextstate = ARCH_DEP(wfetchb)((VADR)(slsba+bidx), USE_REAL_ADDR, regs);

    while (count && oldstate == nextstate)
    {
        ARCH_DEP(wstoreb)(state, (VADR)(slsba+bidx), USE_REAL_ADDR, regs);

        bidx++; bidx &= 0x7F;              /* Advance and wrap index */
        count--;

        if(count)
            nextstate = ARCH_DEP(wfetchb)((VADR)(slsba+bidx), USE_REAL_ADDR, regs);
    }


    regs->GR_L(r1) = bidx;              /* Return buffer state index */
    regs->GR_L(r3) = count;    /* Return number of unchanged buffers */
    regs->GR_H(r3) = !count || oldstate == nextstate ? 0 : 0x20;
    regs->psw.cc = 0;

    return;
}


/*-------------------------------------------------------------------*/
/* B99C EQBS  - Extract Queue Buffer State                     [RRF] */
/*-------------------------------------------------------------------*/
DEF_INST(extract_queue_buffer_state)
{
int     r1, r2, r3, m4;       /* Register numbers                    */
VADR    effective_addr2 = 0;  /* effective address                   */

DEVBLK  *dev;                 /* Data device DEVBLK                  */
OSA_GRP *grp;                 /* OSA Group device structure          */

int     autoack;              /* Flag for auto-acknowkledgement      */
BYTE    state;                /* State extracted from first buffer   */
BYTE    nextstate;            /*    next buffer                      */
U32     queues;               /* Total number of queues              */
U32     count;                /* Number of buffers to set            */
U32     qidx;                 /* Queue index                         */
U32     bidx;                 /* SLSB buffer state index             */
U64     slsba;                /* Storage list state block address    */

    RRF_RM(inst, regs, r1, r2, r3, m4);

    FACILITY_CHECK(QEBSM, regs);

//  ARCH_DEP(display_inst) (regs, inst);

    PRIV_CHECK(regs);

#if defined(_FEATURE_SIE)
    if(SIE_STATNB(regs, EC3, SIGAA))
        longjmp(regs->progjmp, SIE_INTERCEPT_INST);
#endif /*defined(_FEATURE_SIE)*/

    PTIO(INF,"EQBS");

    dev = find_device_by_subchan(TKN2IOID(regs->GR_G(1)));

    /* Condition code 3 if subchannel does not exist,
       is not valid, or is not enabled or is not a QDIO subchannel */
    if (dev == NULL
        || (dev->pmcw.flag5 & PMCW5_V) == 0
        || (dev->pmcw.flag5 & PMCW5_E) == 0
        || (dev->pmcw.flag4 & PMCW4_Q) == 0)
    {
        PTIO(ERR,"*EQBS");
#if defined(_FEATURE_QUEUED_DIRECT_IO_ASSIST)
        SIE_INTERCEPT(regs);
#endif
        regs->GR_H(r3) = 1;    /* Activation error */
        regs->psw.cc = 3;      /* Guess */
        return;
    }

#if 0
    /* Check that device is QDIO active */
    if ((dev->scsw.flag2 & SCSW2_Q) == 0)
    {
        PTIO(ERR,"*EQBS");
        regs->GR_H(r3) = 1;    /* Activation error */
        regs->psw.cc = 1;
        return;
    }
#endif

    /* Locate the group device block */
    grp = (OSA_GRP*)dev->group->grp_data;

    qidx  = regs->GR_H(r1);                           /* Queue index */
    bidx  = regs->GR_L(r1);                          /* Buffer index */
    autoack = (regs->GR_H(r2) & 0x80000000);
    count = regs->GR_L(r3) < 128 ? regs->GR_L(r3) : 128; /* Number of buffers */

    queues = (U32)(grp->i_qcnt + grp->o_qcnt);

    if ( (qidx >= queues) || (bidx > 127) )
        ARCH_DEP(program_interrupt) (regs, PGM_SPECIFICATION_EXCEPTION);

    if (qidx < (U32)grp->i_qcnt)
        slsba = grp->i_slsbla[qidx];
    else
        slsba = grp->o_slsbla[qidx - grp->i_qcnt];

    state = nextstate = ARCH_DEP(wfetchb)((VADR)(slsba+bidx), USE_REAL_ADDR, regs);

    while(count && state == nextstate)
    {
        if (autoack)
            switch(nextstate) {

                case SLSBE_INPUT_COMPLETED:
                    ARCH_DEP(wstoreb)
                        (SLSBE_INPUT_ACKED, (VADR)(slsba+bidx), USE_REAL_ADDR, regs);
                    break;

#if 0
                case SLSBE_OUTPUT_COMPLETED:
                    ARCH_DEP(wstoreb)
                        (SLSBE_OUTPUT_PRIMED, (VADR)(slsba+bidx), USE_REAL_ADDR, regs);
                    break;
#endif

            }

        bidx++; bidx &= 0x7F;              /* Advance and wrap index */
        count--;

        if(count)
            nextstate = ARCH_DEP(wfetchb)((VADR)(slsba+bidx), USE_REAL_ADDR, regs);
    }

    regs->GR_L(r1) = bidx;
    regs->GR_LHLCL(r2) = state;
    regs->GR_L(r3) = count;
    regs->GR_H(r3) = !count || state == nextstate ? 0 : 0x20;
    regs->psw.cc = 0;

    return;

}
#endif /*defined(FEATURE_QEBSM)*/

#if defined(FEATURE_SVS)
/*-------------------------------------------------------------------*/
/* B265 SVS   - Set Vector Summary                             [RRE] */
/*-------------------------------------------------------------------*/
DEF_INST(set_vector_summary)
{
int     r1, unused;            /* Register numbers                   */

    RRE0(inst, regs, r1, unused);

//  ARCH_DEP(display_inst) (regs, inst);

    FACILITY_CHECK(SVS, regs);

    PRIV_CHECK(regs);

    SIE_INTERCEPT(regs);

    ODD_CHECK(r1, regs);

    switch(regs->GR_L(1)) {

        case 3:   // Clear Global Summary
            regs->GR(r1+1) = 0;
            break;

        default:
            ARCH_DEP(program_interrupt) (regs, PGM_SPECIFICATION_EXCEPTION);
     }
}
#endif /*defined(FEATURE_SVS)*/

#endif /*defined(FEATURE_QUEUED_DIRECT_IO)*/


#if !defined(_GEN_ARCH)

#if defined(_ARCHMODE2)
 #define  _GEN_ARCH _ARCHMODE2
 #include "qdio.c"
#endif

#if defined(_ARCHMODE3)
 #undef   _GEN_ARCH
 #define  _GEN_ARCH _ARCHMODE3
 #include "qdio.c"
#endif

#endif /*!defined(_GEN_ARCH)*/
