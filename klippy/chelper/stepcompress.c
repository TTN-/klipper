// Stepper pulse schedule compression
//
// Copyright (C) 2016,2017  Kevin O'Connor <kevin@koconnor.net>
//
// This file may be distributed under the terms of the GNU GPLv3 license.
//
// The goal of this code is to take a series of scheduled stepper
// pulse times and compress them into a handful of commands that can
// be efficiently transmitted and executed on a microcontroller (mcu).
// The mcu accepts step pulse commands that take interval, count, and
// add parameters such that 'count' pulses occur, with each step event
// calculating the next step event time using:
//  next_wake_time = last_wake_time + interval; interval += add
// This code is written in C (instead of python) for processing
// efficiency - the repetitive integer math is vastly faster in C.

#include <math.h> // sqrt
#include <stddef.h> // offsetof
#include <stdint.h> // uint32_t
#include <stdio.h> // fprintf
#include <stdlib.h> // malloc
#include <string.h> // memset
#include "pyhelper.h" // errorf
#include "serialqueue.h" // struct queue_message

#define CHECK_LINES 1
#define QUEUE_START_SIZE 1024

struct stepcompress {
    // Buffer management
    uint32_t *queue, *queue_end, *queue_pos, *queue_next;
    // Internal tracking
    uint32_t max_error;
    double mcu_time_offset, mcu_freq;
    // Message generation
    uint64_t last_step_clock, homing_clock;
    struct list_head msg_queue;
    uint32_t queue_step_msgid, set_next_step_dir_msgid, oid;
    int sdir, invert_sdir;
};


/****************************************************************
 * Step compression
 ****************************************************************/

#define DIV_UP(n,d) (((n) + (d) - 1) / (d))

static inline int32_t
idiv_up(int32_t n, int32_t d)
{
    return (n>=0) ? DIV_UP(n,d) : (n/d);
}

static inline int32_t
idiv_down(int32_t n, int32_t d)
{
    return (n>=0) ? (n/d) : (n - d + 1) / d;
}

struct points {
    int32_t minp, maxp;
};

// Given a requested step time, return the minimum and maximum
// acceptable times
static inline struct points
minmax_point(struct stepcompress *sc, uint32_t *pos)
{
    uint32_t lsc = sc->last_step_clock, point = *pos - lsc;
    uint32_t prevpoint = pos > sc->queue_pos ? *(pos-1) - lsc : 0;
    uint32_t max_error = (point - prevpoint) / 2;
    if (max_error > sc->max_error)
        max_error = sc->max_error;
    return (struct points){ point - max_error, point };
}

// The maximum add delta between two valid quadratic sequences of the
// form "add*count*(count-1)/2 + interval*count" is "(6 + 4*sqrt(2)) *
// maxerror / (count*count)".  The "6 + 4*sqrt(2)" is 11.65685, but
// using 11 works well in practice.
#define QUADRATIC_DEV 11

struct step_move {
    uint32_t interval;
    uint16_t count;
    int16_t add;
};

// Find a 'step_move' that covers a series of step times
static struct step_move
compress_bisect_add(struct stepcompress *sc)
{
    uint32_t *qlast = sc->queue_next;
    if (qlast > sc->queue_pos + 65535)
        qlast = sc->queue_pos + 65535;
    struct points point = minmax_point(sc, sc->queue_pos);
    int32_t outer_mininterval = point.minp, outer_maxinterval = point.maxp;
    int32_t add = 0, minadd = -0x8000, maxadd = 0x7fff;
    int32_t bestinterval = 0, bestcount = 1, bestadd = 1, bestreach = INT32_MIN;
    int32_t zerointerval = 0, zerocount = 0;

    for (;;) {
        // Find longest valid sequence with the given 'add'
        struct points nextpoint;
        int32_t nextmininterval = outer_mininterval;
        int32_t nextmaxinterval = outer_maxinterval, interval = nextmaxinterval;
        int32_t nextcount = 1;
        for (;;) {
            nextcount++;
            if (&sc->queue_pos[nextcount-1] >= qlast) {
                int32_t count = nextcount - 1;
                return (struct step_move){ interval, count, add };
            }
            nextpoint = minmax_point(sc, sc->queue_pos + nextcount - 1);
            int32_t nextaddfactor = nextcount*(nextcount-1)/2;
            int32_t c = add*nextaddfactor;
            if (nextmininterval*nextcount < nextpoint.minp - c)
                nextmininterval = DIV_UP(nextpoint.minp - c, nextcount);
            if (nextmaxinterval*nextcount > nextpoint.maxp - c)
                nextmaxinterval = (nextpoint.maxp - c) / nextcount;
            if (nextmininterval > nextmaxinterval)
                break;
            interval = nextmaxinterval;
        }

        // Check if this is the best sequence found so far
        int32_t count = nextcount - 1, addfactor = count*(count-1)/2;
        int32_t reach = add*addfactor + interval*count;
        if (reach > bestreach
            || (reach == bestreach && interval > bestinterval)) {
            bestinterval = interval;
            bestcount = count;
            bestadd = add;
            bestreach = reach;
            if (!add) {
                zerointerval = interval;
                zerocount = count;
            }
            if (count > 0x200)
                // No 'add' will improve sequence; avoid integer overflow
                break;
        }

        // Check if a greater or lesser add could extend the sequence
        int32_t nextaddfactor = nextcount*(nextcount-1)/2;
        int32_t nextreach = add*nextaddfactor + interval*nextcount;
        if (nextreach < nextpoint.minp) {
            minadd = add + 1;
            outer_maxinterval = nextmaxinterval;
        } else {
            maxadd = add - 1;
            outer_mininterval = nextmininterval;
        }

        // The maximum valid deviation between two quadratic sequences
        // can be calculated and used to further limit the add range.
        if (count > 1) {
            int32_t errdelta = sc->max_error*QUADRATIC_DEV / (count*count);
            if (minadd < add - errdelta)
                minadd = add - errdelta;
            if (maxadd > add + errdelta)
                maxadd = add + errdelta;
        }

        // See if next point would further limit the add range
        int32_t c = outer_maxinterval * nextcount;
        if (minadd*nextaddfactor < nextpoint.minp - c)
            minadd = idiv_up(nextpoint.minp - c, nextaddfactor);
        c = outer_mininterval * nextcount;
        if (maxadd*nextaddfactor > nextpoint.maxp - c)
            maxadd = idiv_down(nextpoint.maxp - c, nextaddfactor);

        // Bisect valid add range and try again with new 'add'
        if (minadd > maxadd)
            break;
        add = maxadd - (maxadd - minadd) / 4;
    }
    if (zerocount + zerocount/16 >= bestcount)
        // Prefer add=0 if it's similar to the best found sequence
        return (struct step_move){ zerointerval, zerocount, 0 };
    return (struct step_move){ bestinterval, bestcount, bestadd };
}


/****************************************************************
 * Step compress checking
 ****************************************************************/

#define ERROR_RET -989898989

// Verify that a given 'step_move' matches the actual step times
static int
check_line(struct stepcompress *sc, struct step_move move)
{
    if (!CHECK_LINES)
        return 0;
    if (!move.count || (!move.interval && !move.add && move.count > 1)
        || move.interval >= 0x80000000) {
        errorf("stepcompress o=%d i=%d c=%d a=%d: Invalid sequence"
               , sc->oid, move.interval, move.count, move.add);
        return ERROR_RET;
    }
    uint32_t interval = move.interval, p = 0;
    uint16_t i;
    for (i=0; i<move.count; i++) {
        struct points point = minmax_point(sc, sc->queue_pos + i);
        p += interval;
        if (p < point.minp || p > point.maxp) {
            errorf("stepcompress o=%d i=%d c=%d a=%d: Point %d: %d not in %d:%d"
                   , sc->oid, move.interval, move.count, move.add
                   , i+1, p, point.minp, point.maxp);
            return ERROR_RET;
        }
        if (interval >= 0x80000000) {
            errorf("stepcompress o=%d i=%d c=%d a=%d:"
                   " Point %d: interval overflow %d"
                   , sc->oid, move.interval, move.count, move.add
                   , i+1, interval);
            return ERROR_RET;
        }
        interval += move.add;
    }
    return 0;
}


/****************************************************************
 * Step compress interface
 ****************************************************************/

// Allocate a new 'stepcompress' object
struct stepcompress *
stepcompress_alloc(uint32_t max_error, uint32_t queue_step_msgid
                   , uint32_t set_next_step_dir_msgid, uint32_t invert_sdir
                   , uint32_t oid)
{
    struct stepcompress *sc = malloc(sizeof(*sc));
    memset(sc, 0, sizeof(*sc));
    sc->max_error = max_error;
    list_init(&sc->msg_queue);
    sc->queue_step_msgid = queue_step_msgid;
    sc->set_next_step_dir_msgid = set_next_step_dir_msgid;
    sc->oid = oid;
    sc->sdir = -1;
    sc->invert_sdir = !!invert_sdir;
    return sc;
}

// Free memory associated with a 'stepcompress' object
void
stepcompress_free(struct stepcompress *sc)
{
    if (!sc)
        return;
    free(sc->queue);
    message_queue_free(&sc->msg_queue);
    free(sc);
}

// Convert previously scheduled steps into commands for the mcu
static int
stepcompress_flush(struct stepcompress *sc, uint64_t move_clock)
{
    if (sc->queue_pos >= sc->queue_next)
        return 0;
    while (sc->last_step_clock < move_clock) {
        struct step_move move = compress_bisect_add(sc);
        int ret = check_line(sc, move);
        if (ret)
            return ret;

        uint32_t msg[5] = {
            sc->queue_step_msgid, sc->oid, move.interval, move.count, move.add
        };
        struct queue_message *qm = message_alloc_and_encode(msg, 5);
        qm->min_clock = qm->req_clock = sc->last_step_clock;
        int32_t addfactor = move.count*(move.count-1)/2;
        uint32_t ticks = move.add*addfactor + move.interval*move.count;
        sc->last_step_clock += ticks;
        if (sc->homing_clock)
            // When homing, all steps should be sent prior to homing_clock
            qm->min_clock = qm->req_clock = sc->homing_clock;
        list_add_tail(&qm->node, &sc->msg_queue);

        if (sc->queue_pos + move.count >= sc->queue_next) {
            sc->queue_pos = sc->queue_next = sc->queue;
            break;
        }
        sc->queue_pos += move.count;
    }
    return 0;
}

// Generate a queue_step for a step far in the future from the last step
static int
stepcompress_flush_far(struct stepcompress *sc, uint64_t abs_step_clock)
{
    uint32_t msg[5] = {
        sc->queue_step_msgid, sc->oid, abs_step_clock - sc->last_step_clock, 1, 0
    };
    struct queue_message *qm = message_alloc_and_encode(msg, 5);
    qm->min_clock = sc->last_step_clock;
    sc->last_step_clock = qm->req_clock = abs_step_clock;
    if (sc->homing_clock)
        // When homing, all steps should be sent prior to homing_clock
        qm->min_clock = qm->req_clock = sc->homing_clock;
    list_add_tail(&qm->node, &sc->msg_queue);
    return 0;
}

// Send the set_next_step_dir command
static int
set_next_step_dir(struct stepcompress *sc, int sdir)
{
    if (sc->sdir == sdir)
        return 0;
    sc->sdir = sdir;
    int ret = stepcompress_flush(sc, UINT64_MAX);
    if (ret)
        return ret;
    uint32_t msg[3] = {
        sc->set_next_step_dir_msgid, sc->oid, sdir ^ sc->invert_sdir
    };
    struct queue_message *qm = message_alloc_and_encode(msg, 3);
    qm->req_clock = sc->homing_clock ?: sc->last_step_clock;
    list_add_tail(&qm->node, &sc->msg_queue);
    return 0;
}

// Reset the internal state of the stepcompress object
int
stepcompress_reset(struct stepcompress *sc, uint64_t last_step_clock)
{
    int ret = stepcompress_flush(sc, UINT64_MAX);
    if (ret)
        return ret;
    sc->last_step_clock = last_step_clock;
    sc->sdir = -1;
    return 0;
}

// Indicate the stepper is in homing mode (or done homing if zero)
int
stepcompress_set_homing(struct stepcompress *sc, uint64_t homing_clock)
{
    int ret = stepcompress_flush(sc, UINT64_MAX);
    if (ret)
        return ret;
    sc->homing_clock = homing_clock;
    return 0;
}

// Queue an mcu command to go out in order with stepper commands
int
stepcompress_queue_msg(struct stepcompress *sc, uint32_t *data, int len)
{
    int ret = stepcompress_flush(sc, UINT64_MAX);
    if (ret)
        return ret;

    struct queue_message *qm = message_alloc_and_encode(data, len);
    qm->req_clock = sc->homing_clock ?: sc->last_step_clock;
    list_add_tail(&qm->node, &sc->msg_queue);
    return 0;
}

// Set the conversion rate of 'print_time' to mcu clock
static void
stepcompress_set_time(struct stepcompress *sc
                      , double time_offset, double mcu_freq)
{
    sc->mcu_time_offset = time_offset;
    sc->mcu_freq = mcu_freq;
}


/****************************************************************
 * Queue management
 ****************************************************************/

struct queue_append {
    struct stepcompress *sc;
    uint32_t *qnext, *qend, last_step_clock_32;
    double clock_offset;
};

// Maximium clock delta between messages in the queue
#define CLOCK_DIFF_MAX (3<<28)

// Create a cursor for inserting clock times into the queue
static inline struct queue_append
queue_append_start(struct stepcompress *sc, double print_time, double adjust)
{
    double print_clock = (print_time - sc->mcu_time_offset) * sc->mcu_freq;
    return (struct queue_append) {
        .sc = sc, .qnext = sc->queue_next, .qend = sc->queue_end,
        .last_step_clock_32 = sc->last_step_clock,
        .clock_offset = (print_clock - (double)sc->last_step_clock) + adjust };
}

// Finalize a cursor created with queue_append_start()
static inline void
queue_append_finish(struct queue_append qa)
{
    qa.sc->queue_next = qa.qnext;
}

// Slow path for queue_append()
static int
queue_append_slow(struct stepcompress *sc, double rel_sc)
{
    uint64_t abs_step_clock = (uint64_t)rel_sc + sc->last_step_clock;
    if (abs_step_clock >= sc->last_step_clock + CLOCK_DIFF_MAX) {
        // Avoid integer overflow on steps far in the future
        int ret = stepcompress_flush(sc, abs_step_clock - CLOCK_DIFF_MAX + 1);
        if (ret)
            return ret;

        if (abs_step_clock >= sc->last_step_clock + CLOCK_DIFF_MAX)
            return stepcompress_flush_far(sc, abs_step_clock);
    }

    if (sc->queue_next - sc->queue_pos > 65535 + 2000) {
        // No point in keeping more than 64K steps in memory
        uint32_t flush = *(sc->queue_next-65535) - (uint32_t)sc->last_step_clock;
        int ret = stepcompress_flush(sc, sc->last_step_clock + flush);
        if (ret)
            return ret;
    }

    if (sc->queue_next >= sc->queue_end) {
        // Make room in the queue
        int in_use = sc->queue_next - sc->queue_pos;
        if (sc->queue_pos > sc->queue) {
            // Shuffle the internal queue to avoid having to allocate more ram
            memmove(sc->queue, sc->queue_pos, in_use * sizeof(*sc->queue));
        } else {
            // Expand the internal queue of step times
            int alloc = sc->queue_end - sc->queue;
            if (!alloc)
                alloc = QUEUE_START_SIZE;
            while (in_use >= alloc)
                alloc *= 2;
            sc->queue = realloc(sc->queue, alloc * sizeof(*sc->queue));
            sc->queue_end = sc->queue + alloc;
        }
        sc->queue_pos = sc->queue;
        sc->queue_next = sc->queue + in_use;
    }

    *sc->queue_next++ = abs_step_clock;
    return 0;
}

// Add a clock time to the queue (flushing the queue if needed)
static inline int
queue_append(struct queue_append *qa, double step_clock)
{
    double rel_sc = step_clock + qa->clock_offset;
    if (likely(!(qa->qnext >= qa->qend || rel_sc >= (double)CLOCK_DIFF_MAX))) {
        *qa->qnext++ = qa->last_step_clock_32 + (uint32_t)rel_sc;
        return 0;
    }
    // Call queue_append_slow() to handle queue expansion and integer overflow
    struct stepcompress *sc = qa->sc;
    uint64_t old_last_step_clock = sc->last_step_clock;
    sc->queue_next = qa->qnext;
    int ret = queue_append_slow(sc, rel_sc);
    if (ret)
        return ret;
    qa->qnext = sc->queue_next;
    qa->qend = sc->queue_end;
    qa->last_step_clock_32 = sc->last_step_clock;
    qa->clock_offset -= sc->last_step_clock - old_last_step_clock;
    return 0;
}


/****************************************************************
 * Motion to step conversions
 ****************************************************************/

// Common suffixes: _sd is step distance (a unit length the same
// distance the stepper moves on each step), _sv is step velocity (in
// units of step distance per time), _sd2 is step distance squared, _r
// is ratio (scalar usually between 0.0 and 1.0).  Times are in
// seconds and acceleration is in units of step distance per second
// squared.

// Wrapper around sqrt() to handle small negative numbers
static double
_safe_sqrt(double v)
{
    // Due to floating point truncation, it's possible to get a small
    // negative number - treat it as zero.
    if (v < -0.001)
        errorf("safe_sqrt of %.9f", v);
    return 0.;
}
static inline double safe_sqrt(double v) {
    return likely(v >= 0.) ? sqrt(v) : _safe_sqrt(v);
}

// Schedule a step event at the specified step_clock time
int32_t
stepcompress_push(struct stepcompress *sc, double print_time, int32_t sdir)
{
    int ret = set_next_step_dir(sc, !!sdir);
    if (ret)
        return ret;
    struct queue_append qa = queue_append_start(sc, print_time, 0.5);
    ret = queue_append(&qa, 0.);
    if (ret)
        return ret;
    queue_append_finish(qa);
    return sdir ? 1 : -1;
}

// Schedule 'steps' number of steps at constant acceleration. If
// acceleration is zero (ie, constant velocity) it uses the formula:
//  step_time = print_time + step_num/start_sv
// Otherwise it uses the formula:
//  step_time = (print_time + sqrt(2*step_num/accel + (start_sv/accel)**2)
//               - start_sv/accel)
int32_t
stepcompress_push_const(
    struct stepcompress *sc, double print_time
    , double step_offset, double steps, double start_sv, double accel)
{
    // Calculate number of steps to take
    int sdir = 1;
    if (steps < 0) {
        sdir = 0;
        steps = -steps;
        step_offset = -step_offset;
    }
    int count = steps + .5 - step_offset;
    if (count <= 0 || count > 10000000) {
        if (count && steps) {
            errorf("push_const invalid count %d %f %f %f %f %f"
                   , sc->oid, print_time, step_offset, steps
                   , start_sv, accel);
            return ERROR_RET;
        }
        return 0;
    }
    int ret = set_next_step_dir(sc, sdir);
    if (ret)
        return ret;
    int res = sdir ? count : -count;

    // Calculate each step time
    if (!accel) {
        // Move at constant velocity (zero acceleration)
        struct queue_append qa = queue_append_start(sc, print_time, .5);
        double inv_cruise_sv = sc->mcu_freq / start_sv;
        double pos = (step_offset + .5) * inv_cruise_sv;
        while (count--) {
            ret = queue_append(&qa, pos);
            if (ret)
                return ret;
            pos += inv_cruise_sv;
        }
        queue_append_finish(qa);
    } else {
        // Move with constant acceleration
        double inv_accel = 1. / accel;
        double accel_time = start_sv * inv_accel * sc->mcu_freq;
        struct queue_append qa = queue_append_start(
            sc, print_time, 0.5 - accel_time);
        double accel_multiplier = 2. * inv_accel * sc->mcu_freq * sc->mcu_freq;
        double pos = (step_offset + .5)*accel_multiplier + accel_time*accel_time;
        while (count--) {
            double v = safe_sqrt(pos);
            int ret = queue_append(&qa, accel_multiplier >= 0. ? v : -v);
            if (ret)
                return ret;
            pos += accel_multiplier;
        }
        queue_append_finish(qa);
    }
    return res;
}

// Schedule steps using delta kinematics
static int32_t
_stepcompress_push_delta(
    struct stepcompress *sc, int sdir
    , double print_time, double move_sd, double start_sv, double accel
    , double height, double startxy_sd, double arm_sd, double movez_r)
{
    // Calculate number of steps to take
    double movexy_r = movez_r ? sqrt(1. - movez_r*movez_r) : 1.;
    double arm_sd2 = arm_sd * arm_sd;
    double endxy_sd = startxy_sd - movexy_r*move_sd;
    double end_height = safe_sqrt(arm_sd2 - endxy_sd*endxy_sd);
    int count = (end_height + movez_r*move_sd - height) * (sdir ? 1. : -1.) + .5;
    if (count <= 0 || count > 10000000) {
        if (count) {
            errorf("push_delta invalid count %d %d %f %f %f %f %f %f %f %f"
                   , sc->oid, count, print_time, move_sd, start_sv, accel
                   , height, startxy_sd, arm_sd, movez_r);
            return ERROR_RET;
        }
        return 0;
    }
    int ret = set_next_step_dir(sc, sdir);
    if (ret)
        return ret;
    int res = sdir ? count : -count;

    // Calculate each step time
    height += (sdir ? .5 : -.5);
    if (!accel) {
        // Move at constant velocity (zero acceleration)
        struct queue_append qa = queue_append_start(sc, print_time, .5);
        double inv_cruise_sv = sc->mcu_freq / start_sv;
        if (!movez_r) {
            // Optimized case for common XY only moves (no Z movement)
            while (count--) {
                double v = safe_sqrt(arm_sd2 - height*height);
                double pos = startxy_sd + (sdir ? -v : v);
                int ret = queue_append(&qa, pos * inv_cruise_sv);
                if (ret)
                    return ret;
                height += (sdir ? 1. : -1.);
            }
        } else if (!movexy_r) {
            // Optimized case for Z only moves
            double pos = ((sdir ? height-end_height : end_height-height)
                          * inv_cruise_sv);
            while (count--) {
                int ret = queue_append(&qa, pos);
                if (ret)
                    return ret;
                pos += inv_cruise_sv;
            }
        } else {
            // General case (handles XY+Z moves)
            double start_pos = movexy_r*startxy_sd, zoffset = movez_r*startxy_sd;
            while (count--) {
                double relheight = movexy_r*height - zoffset;
                double v = safe_sqrt(arm_sd2 - relheight*relheight);
                double pos = start_pos + movez_r*height + (sdir ? -v : v);
                int ret = queue_append(&qa, pos * inv_cruise_sv);
                if (ret)
                    return ret;
                height += (sdir ? 1. : -1.);
            }
        }
        queue_append_finish(qa);
    } else {
        // Move with constant acceleration
        double start_pos = movexy_r*startxy_sd, zoffset = movez_r*startxy_sd;
        double inv_accel = 1. / accel;
        start_pos += 0.5 * start_sv*start_sv * inv_accel;
        struct queue_append qa = queue_append_start(
            sc, print_time, 0.5 - start_sv * inv_accel * sc->mcu_freq);
        double accel_multiplier = 2. * inv_accel * sc->mcu_freq * sc->mcu_freq;
        while (count--) {
            double relheight = movexy_r*height - zoffset;
            double v = safe_sqrt(arm_sd2 - relheight*relheight);
            double pos = start_pos + movez_r*height + (sdir ? -v : v);
            v = safe_sqrt(pos * accel_multiplier);
            int ret = queue_append(&qa, accel_multiplier >= 0. ? v : -v);
            if (ret)
                return ret;
            height += (sdir ? 1. : -1.);
        }
        queue_append_finish(qa);
    }
    return res;
}

int32_t
stepcompress_push_delta(
    struct stepcompress *sc, double print_time, double move_sd
    , double start_sv, double accel
    , double height, double startxy_sd, double arm_sd, double movez_r)
{
    double reversexy_sd = startxy_sd + arm_sd*movez_r;
    if (reversexy_sd <= 0.)
        // All steps are in down direction
        return _stepcompress_push_delta(
            sc, 0, print_time, move_sd, start_sv, accel
            , height, startxy_sd, arm_sd, movez_r);
    double movexy_r = movez_r ? sqrt(1. - movez_r*movez_r) : 1.;
    if (reversexy_sd >= move_sd * movexy_r)
        // All steps are in up direction
        return _stepcompress_push_delta(
            sc, 1, print_time, move_sd, start_sv, accel
            , height, startxy_sd, arm_sd, movez_r);
    // Steps in both up and down direction
    int res1 = _stepcompress_push_delta(
        sc, 1, print_time, reversexy_sd / movexy_r, start_sv, accel
        , height, startxy_sd, arm_sd, movez_r);
    if (res1 == ERROR_RET)
        return res1;
    int res2 = _stepcompress_push_delta(
        sc, 0, print_time, move_sd, start_sv, accel
        , height + res1, startxy_sd, arm_sd, movez_r);
    if (res2 == ERROR_RET)
        return res2;
    return res1 + res2;
}


/****************************************************************
 * Step compress synchronization
 ****************************************************************/

// The steppersync object is used to synchronize the output of mcu
// step commands.  The mcu can only queue a limited number of step
// commands - this code tracks when items on the mcu step queue become
// free so that new commands can be transmitted.  It also ensures the
// mcu step queue is ordered between steppers so that no stepper
// starves the other steppers of space in the mcu step queue.

struct steppersync {
    // Serial port
    struct serialqueue *sq;
    struct command_queue *cq;
    // Storage for associated stepcompress objects
    struct stepcompress **sc_list;
    int sc_num;
    // Storage for list of pending move clocks
    uint64_t *move_clocks;
    int num_move_clocks;
};

// Allocate a new 'steppersync' object
struct steppersync *
steppersync_alloc(struct serialqueue *sq, struct stepcompress **sc_list
                  , int sc_num, int move_num)
{
    struct steppersync *ss = malloc(sizeof(*ss));
    memset(ss, 0, sizeof(*ss));
    ss->sq = sq;
    ss->cq = serialqueue_alloc_commandqueue();

    ss->sc_list = malloc(sizeof(*sc_list)*sc_num);
    memcpy(ss->sc_list, sc_list, sizeof(*sc_list)*sc_num);
    ss->sc_num = sc_num;

    ss->move_clocks = malloc(sizeof(*ss->move_clocks)*move_num);
    memset(ss->move_clocks, 0, sizeof(*ss->move_clocks)*move_num);
    ss->num_move_clocks = move_num;

    return ss;
}

// Free memory associated with a 'steppersync' object
void
steppersync_free(struct steppersync *ss)
{
    if (!ss)
        return;
    free(ss->sc_list);
    free(ss->move_clocks);
    serialqueue_free_commandqueue(ss->cq);
    free(ss);
}

// Set the conversion rate of 'print_time' to mcu clock
void
steppersync_set_time(struct steppersync *ss, double time_offset, double mcu_freq)
{
    int i;
    for (i=0; i<ss->sc_num; i++) {
        struct stepcompress *sc = ss->sc_list[i];
        stepcompress_set_time(sc, time_offset, mcu_freq);
    }
}

// Implement a binary heap algorithm to track when the next available
// 'struct move' in the mcu will be available
static void
heap_replace(struct steppersync *ss, uint64_t req_clock)
{
    uint64_t *mc = ss->move_clocks;
    int nmc = ss->num_move_clocks, pos = 0;
    for (;;) {
        int child1_pos = 2*pos+1, child2_pos = 2*pos+2;
        uint64_t child2_clock = child2_pos < nmc ? mc[child2_pos] : UINT64_MAX;
        uint64_t child1_clock = child1_pos < nmc ? mc[child1_pos] : UINT64_MAX;
        if (req_clock <= child1_clock && req_clock <= child2_clock) {
            mc[pos] = req_clock;
            break;
        }
        if (child1_clock < child2_clock) {
            mc[pos] = child1_clock;
            pos = child1_pos;
        } else {
            mc[pos] = child2_clock;
            pos = child2_pos;
        }
    }
}

// Find and transmit any scheduled steps prior to the given 'move_clock'
int
steppersync_flush(struct steppersync *ss, uint64_t move_clock)
{
    // Flush each stepcompress to the specified move_clock
    int i;
    for (i=0; i<ss->sc_num; i++) {
        int ret = stepcompress_flush(ss->sc_list[i], move_clock);
        if (ret)
            return ret;
    }

    // Order commands by the reqclock of each pending command
    struct list_head msgs;
    list_init(&msgs);
    for (;;) {
        // Find message with lowest reqclock
        uint64_t req_clock = MAX_CLOCK;
        struct queue_message *qm = NULL;
        for (i=0; i<ss->sc_num; i++) {
            struct stepcompress *sc = ss->sc_list[i];
            if (!list_empty(&sc->msg_queue)) {
                struct queue_message *m = list_first_entry(
                    &sc->msg_queue, struct queue_message, node);
                if (m->req_clock < req_clock) {
                    qm = m;
                    req_clock = m->req_clock;
                }
            }
        }
        if (!qm || (qm->min_clock && req_clock > move_clock))
            break;

        uint64_t next_avail = ss->move_clocks[0];
        if (qm->min_clock)
            // The qm->min_clock field is overloaded to indicate that
            // the command uses the 'move queue' and to store the time
            // that move queue item becomes available.
            heap_replace(ss, qm->min_clock);
        // Reset the min_clock to its normal meaning (minimum transmit time)
        qm->min_clock = next_avail;

        // Batch this command
        list_del(&qm->node);
        list_add_tail(&qm->node, &msgs);
    }

    // Transmit commands
    if (!list_empty(&msgs))
        serialqueue_send_batch(ss->sq, ss->cq, &msgs);
    return 0;
}
