/* TIMER.C      (C) Copyright Roger Bowler, 1999-2012                */
/*              (C) and others 2013-2021                             */
/*              Timer support functions                              */
/*                                                                   */
/*   Released under "The Q Public License Version 1"                 */
/*   (http://www.hercules-390.org/herclic.html) as modifications to  */
/*   Hercules.                                                       */

/* z/Architecture support - (C) Copyright Jan Jaeger, 1999-2012      */

#include "hstdinc.h"

#define _HENGINE_DLL_

#include "hercules.h"

#include "opcode.h"

#include "feat390.h"
#include "feat370.h"


/*-------------------------------------------------------------------*/
/* Check for timer event                                             */
/*                                                                   */
/* Checks for the following interrupts:                              */
/* [1] Clock comparator                                              */
/* [2] CPU timer                                                     */
/* [3] Interval timer                                                */
/* CPUs with an outstanding interrupt are signalled                  */
/*                                                                   */
/* tod_delta is in hercules internal clock format (>> 8)             */
/*-------------------------------------------------------------------*/
void update_cpu_timer(void)
{
int             cpu;                    /* CPU counter               */
REGS           *regs;                   /* -> CPU register context   */
CPU_BITMAP      intmask = 0;            /* Interrupt CPU mask        */

    /* If no CPUs are available, just return (device server mode) */
    if (!sysblk.hicpu)
      return;

    /* Access the diffent register contexts with the intlock held */
    OBTAIN_INTLOCK(NULL);

    /* Check for [1] clock comparator, [2] cpu timer, and
     * [3] interval timer interrupts for each CPU.
     */
    for (cpu = 0; cpu < sysblk.hicpu; cpu++)
    {
        /* Ignore this CPU if it is not started */
        if (!IS_CPU_ONLINE(cpu)
         || CPUSTATE_STOPPED == sysblk.regs[cpu]->cpustate)
            continue;

        /* Point to the CPU register context */
        regs = sysblk.regs[cpu];

        /*-------------------------------------------*
         * [1] Check for clock comparator interrupt  *
         *-------------------------------------------*/
        if (TOD_CLOCK(regs) > regs->clkc)
        {
            if (!IS_IC_CLKC(regs))
            {
                ON_IC_CLKC(regs);
                intmask |= regs->cpubit;
            }
        }
        else if (IS_IC_CLKC(regs))
            OFF_IC_CLKC(regs);

#if defined(_FEATURE_SIE)
        /* If running under SIE also check the SIE copy */
        if(regs->sie_active)
        {
        /* Signal clock comparator interrupt if needed */
            if(TOD_CLOCK(GUESTREGS) > GUESTREGS->clkc)
            {
                ON_IC_CLKC(GUESTREGS);
                intmask |= regs->cpubit;
            }
            else
                OFF_IC_CLKC(GUESTREGS);
        }
#endif /*defined(_FEATURE_SIE)*/

        /*-------------------------------------------*
         * [2] Decrement the CPU timer for each CPU  *
         *-------------------------------------------*/

        /* Set interrupt flag if the CPU timer is negative */
        if (CPU_TIMER(regs) < 0)
        {
            if (!IS_IC_PTIMER(regs))
            {
                ON_IC_PTIMER(regs);
                intmask |= regs->cpubit;
            }
        }
        else if(IS_IC_PTIMER(regs))
            OFF_IC_PTIMER(regs);

#if defined(_FEATURE_SIE)
        /* When running under SIE also update the SIE copy */
        if(regs->sie_active)
        {
            /* Set interrupt flag if the CPU timer is negative */
            if (CPU_TIMER(GUESTREGS) < 0)
            {
                ON_IC_PTIMER(GUESTREGS);
                intmask |= regs->cpubit;
            }
            else
                OFF_IC_PTIMER(GUESTREGS);
        }
#endif /*defined(_FEATURE_SIE)*/

#if defined(_FEATURE_INTERVAL_TIMER)
        /*-------------------------------------------*
         * [3] Check for interval timer interrupt    *
         *-------------------------------------------*/

        if(regs->arch_mode == ARCH_370_IDX)
        {
            if( chk_int_timer(regs) )
                intmask |= regs->cpubit;
        }


#if defined(_FEATURE_SIE)
        /* When running under SIE also update the SIE copy */
        if(regs->sie_active)
        {
            if(SIE_STATE_BIT_ON(GUESTREGS, M, 370)
              && SIE_STATE_BIT_OFF(GUESTREGS, M, ITMOF))
            {
                if( chk_int_timer(GUESTREGS) )
                    intmask |= regs->cpubit;
            }
        }
#endif /*defined(_FEATURE_SIE)*/

#endif /*defined(_FEATURE_INTERVAL_TIMER)*/

    } /* end for(cpu) */

    /* If a timer interrupt condition was detected for any CPU
       then wake up those CPUs if they are waiting */
    WAKEUP_CPUS_MASK (intmask);

    RELEASE_INTLOCK(NULL);

} /* end function check_timer_event */


/*-------------------------------------------------------------------*/
/* TOD clock and timer thread                                        */
/*                                                                   */
/* This function runs as a separate thread.  It wakes up every       */
/* 1 microsecond, updates the TOD clock, and decrements the          */
/* CPU timer for each CPU.  If any CPU timer goes negative, or       */
/* if the TOD clock exceeds the clock comparator for any CPU,        */
/* it signals any waiting CPUs to wake up and process interrupts.    */
/*-------------------------------------------------------------------*/
void* timer_thread ( void* argp )
{
int     i;                              /* Loop index                */
REGS   *regs;                           /* -> REGS                   */
U64     mipsrate;                       /* Calculated MIPS rate      */
U64     siosrate;                       /* Calculated SIO rate       */
U64     total_mips;                     /* Total MIPS rate           */
U64     total_sios;                     /* Total SIO rate            */

/* Clock times use the top 64-bits of the ETOD clock                 */
U64     now;                            /* Current time of day       */
U64     then;                           /* Previous time of day      */
U64     diff;                           /* Interval                  */
U64     halfdiff;                       /* One-half interval         */
U64     waittime;                       /* Wait time                 */
const U64   period = ETOD_SEC;          /* MIPS calculation period   */
#if defined( _FEATURE_073_TRANSACT_EXEC_FACILITY )
bool    txf_PPA;                        /* true == PPA assist needed */
#endif

#define diffrate(_x,_y) \
        ((((_x) * (_y)) + halfdiff) / diff)

    UNREFERENCED( argp );

    /* Set timer thread priority */
    set_thread_priority( sysblk.todprio );

    // "Thread id "TIDPAT", prio %2d, name %s started"
    LOG_THREAD_BEGIN( TIMER_THREAD_NAME  );

    then = host_tod();

    while (!sysblk.shutfini)
    {
#if defined( _FEATURE_073_TRANSACT_EXEC_FACILITY )
        txf_PPA = false;                /* default until we learn otherwise */
#endif
        /* Update TOD clock and save TOD clock value */
        now = update_tod_clock();

        diff = now - then;

        if (diff >= period)             /* Period expired? */
        {
            halfdiff = diff / 2;        /* One-half interval for rounding */
            then = now;
            total_mips = total_sios = 0;

#if defined( OPTION_SHARED_DEVICES )
            total_sios = sysblk.shrdcount;
            sysblk.shrdcount = 0;
#endif
            for (i=0; i < sysblk.hicpu; i++)
            {
                obtain_lock( &sysblk.cpulock[ i ]);
                {
                    if (!IS_CPU_ONLINE( i ))
                    {
                        release_lock( &sysblk.cpulock[ i ]);
                        continue;
                    }

                    regs = sysblk.regs[i];

                    /* 0% if CPU is STOPPED */
                    if (regs->cpustate == CPUSTATE_STOPPED)
                    {
                        regs->mipsrate = regs->siosrate = regs->cpupct = 0;
                        release_lock( &sysblk.cpulock[ i ]);
                        continue;
                    }

                    /* Calculate instructions per second */
                    mipsrate = regs->instcount;
                    regs->instcount   =  0;
                    regs->prevcount += mipsrate;
                    mipsrate = diffrate(mipsrate, period);
                    regs->mipsrate = mipsrate;
                    total_mips += mipsrate;

                    /* Calculate SIOs per second */
                    siosrate = regs->siocount;
                    regs->siocount = 0;
                    regs->siototal += siosrate;
                    siosrate = diffrate(siosrate, period);
                    regs->siosrate = siosrate;
                    total_sios += siosrate;

                    /* Calculate CPU busy percentage */
                    waittime = regs->waittime;
                    regs->waittime_accumulated += waittime;
                    regs->waittime = 0;

                    if (regs->waittod)
                    {
                        waittime += (now - regs->waittod);
                        regs->waittod = now;
                    }

                    regs->cpupct = min( (diff > waittime) ?
                        diffrate(diff - waittime, 100) : 0, 100 );

#if defined( _FEATURE_073_TRANSACT_EXEC_FACILITY )
                    if (0
                        || (HOSTREGS  && HOSTREGS ->txf_PPA >= PPA_SOME_HELP_THRESHOLD)
                        || (GUESTREGS && GUESTREGS->txf_PPA >= PPA_SOME_HELP_THRESHOLD)
                    )
                        txf_PPA = true;
#endif
                }
                release_lock( &sysblk.cpulock[ i ]);

            } /* end for(cpu) */

            /* Total for ALL CPUs together */
            sysblk.mipsrate = total_mips;
            sysblk.siosrate = total_sios;

            update_maxrates_hwm(); // (update high-water-mark values)

        } /* end if (diff >= period) */

        /* Sleep for another timer update interval... */

#if defined( _FEATURE_073_TRANSACT_EXEC_FACILITY )
        if (txf_PPA)
            usleep( sysblk.txf_timerint );
        else
#endif
            usleep ( sysblk.timerint );

    } /* end while */

    sysblk.todtid = 0;

    // "Thread id "TIDPAT", prio %2d, name %s ended"
    LOG_THREAD_END( TIMER_THREAD_NAME  );

    return NULL;

} /* end function timer_thread */


#if defined( _FEATURE_073_TRANSACT_EXEC_FACILITY )
/*-------------------------------------------------------------------*/
/*              Rubato style TIMERINT modulation                     */
/*-------------------------------------------------------------------*/
/* This function runs as a separate thread. It wakes up every        */
/* interval and calculates a modulation of the TIMERINT setting      */
/* to relax contention during phases of high transactional load.     */
/*-------------------------------------------------------------------*/
void* rubato_thread( void* argp )
{
    int    starting_timerint;           /* Starting sysblk.timerint  */
    int    new_timerint_usecs;          /* Adjusted interval usecs   */
    int    intervals_per_second;        /* Intervals in one second   */
    int    i;                           /* Loop index                */
    U32    count[5] = {0,0,0,0,0};      /* Transactions executed     */
                                        /* during past 5 intervals   */
    U32    max_tps_rate = 0;            /* Transactions per second   */

    UNREFERENCED( argp );

    /* Set our thread priority to be the same as that of CPU threads */
    set_thread_priority( sysblk.cpuprio );

    // "Thread id "TIDPAT", prio %2d, name %s started"
    LOG_THREAD_BEGIN( RUBATO_THREAD_NAME );

    sysblk.txf_counter   = 0;
    starting_timerint    = 0;
    intervals_per_second = MAX_TOD_UPDATE_USECS / sysblk.txf_timerint;

    obtain_lock( &sysblk.rublock );
    {
        while (!sysblk.shutfini && sysblk.rubtid)
        {
            /* Detect change to starting timerint value */
            if (sysblk.timerint != starting_timerint)
            {
                sysblk.txf_timerint = starting_timerint = sysblk.timerint;
                intervals_per_second = MAX_TOD_UPDATE_USECS / sysblk.txf_timerint;
            }

            /* Create room for a new transactions count */
            for (i=1; i < 5; i++)
                count[i-1] = count[i];

            /* Insert new transactions count into array */
            count[4] = sysblk.txf_counter;
            sysblk.txf_counter = 0;

            /* Calculate a maximum transactions-per-second rate
               based on our past transactions count history.
            */
            max_tps_rate = 0;
            for (i=0; i < 5; i++)
                max_tps_rate = MAX( max_tps_rate, count[i] );
            max_tps_rate *= intervals_per_second;

            /* Now adjust the timer interrupt interval correspondingly...
               The goal is to reduce the frequency of timer interrupts
               during periods of high transaction rates by slightly
               increasing the interval between timer interrupts
               (thus causing timer interrupts to occur less frequently)
               or reducing the interval (so that they occur slightly more
               frequently), with the minimum interval (maximum frequency)
               being the original user specified TIMERINT value and the
               maximum interval (minimum frequency) being one second.
            */

            /* Calculate new timer update interval based on past 5 rates */
            new_timerint_usecs = (int) (286000.0 * log( ((double) max_tps_rate + 200.0) / 100.0 ) - 212180.0);

            /* MINMAX(x,y,z): ensure x remains within range y to z */
            MINMAX( new_timerint_usecs, MIN_TOD_UPDATE_USECS, MAX_TOD_UPDATE_USECS );

            sysblk.txf_timerint = new_timerint_usecs;
            intervals_per_second = MAX_TOD_UPDATE_USECS / sysblk.txf_timerint;

            /* Now go back to sleep and wake up a little bit LATER than
               before, or a little bit SOONER than before, before checking
               again to see whether the period of high transaction rate
               has finally subsided or not.
            */
            release_lock( &sysblk.rublock );
            {
                usleep( sysblk.txf_timerint );
            }
            obtain_lock( &sysblk.rublock );
        }

        sysblk.rubtid = 0;
        sysblk.txf_timerint = sysblk.timerint;
    }
    release_lock( &sysblk.rublock );

    // "Thread id "TIDPAT", prio %2d, name %s ended"
    LOG_THREAD_END( RUBATO_THREAD_NAME );

    return NULL;

} /* end function rubato_thread */
#endif /* defined( _FEATURE_073_TRANSACT_EXEC_FACILITY ) */
