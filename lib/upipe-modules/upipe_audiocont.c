/*
 * Copyright (C) 2014 OpenHeadend S.A.R.L.
 *
 * Authors: Benjamin Cohen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject
 * to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

 */

/** @file
 * @short Upipe module video continuity
 */

#include <upipe/ubase.h>
#include <upipe/ulist.h>
#include <upipe/uclock.h>
#include <upipe/uprobe.h>
#include <upipe/uref.h>
#include <upipe/uref_clock.h>
#include <upipe/ubuf.h>
#include <upipe/uref_sound.h>
#include <upipe/uref_sound_flow.h>
#include <upipe/upipe.h>
#include <upipe/upipe_helper_upipe.h>
#include <upipe/upipe_helper_urefcount.h>
#include <upipe/upipe_helper_void.h>
#include <upipe/upipe_helper_uref_mgr.h>
#include <upipe/upipe_helper_output.h>
#include <upipe/upipe_helper_subpipe.h>
#include <upipe-modules/upipe_audiocont.h>

#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <assert.h>

/** only accept sound */
#define EXPECTED_FLOW_DEF "sound."
/** default pts tolerance (late packets) */
#define TOLERANCE (UCLOCK_FREQ / 1000)

/** @internal @This is the private context of a ts join pipe. */
struct upipe_audiocont {
    /** refcount management structure */
    struct urefcount urefcount;

    /** pipe acting as output */
    struct upipe *output;
    /** output flow definition packet */
    struct uref *flow_def;
    /** true if the flow definition has already been sent */
    bool flow_def_sent;
    /** number of planes */
    uint8_t planes;
    /** samplerate */
    uint64_t samplerate;

    /** list of input subpipes */
    struct uchain subs;

    /** current input */
    struct upipe *input_cur;
    /** next input */
    char *input_name;

    /** pts tolerance */
    uint64_t tolerance;

    /** manager to create input subpipes */
    struct upipe_mgr sub_mgr;

    /** public upipe structure */
    struct upipe upipe;
};

UPIPE_HELPER_UPIPE(upipe_audiocont, upipe, UPIPE_AUDIOCONT_SIGNATURE)
UPIPE_HELPER_UREFCOUNT(upipe_audiocont, urefcount, upipe_audiocont_free)
UPIPE_HELPER_VOID(upipe_audiocont)
UPIPE_HELPER_OUTPUT(upipe_audiocont, output, flow_def, flow_def_sent)

/** @internal @This is the private context of an input of a videocont pipe. */
struct upipe_audiocont_sub {
    /** refcount management structure */
    struct urefcount urefcount;
    /** structure for double-linked lists */
    struct uchain uchain;

    /** temporary uref storage */
    struct uchain urefs;

    /** input flow definition packet */
    struct uref *flow_def;

    /** public upipe structure */
    struct upipe upipe;
};

UPIPE_HELPER_UPIPE(upipe_audiocont_sub, upipe, UPIPE_AUDIOCONT_INPUT_SIGNATURE)
UPIPE_HELPER_UREFCOUNT(upipe_audiocont_sub, urefcount, upipe_audiocont_sub_dead)
UPIPE_HELPER_VOID(upipe_audiocont_sub)

UPIPE_HELPER_SUBPIPE(upipe_audiocont, upipe_audiocont_sub, sub, sub_mgr,
                     subs, uchain)

/** @internal @This allocates an input subpipe of a videocont pipe.
 *
 * @param mgr common management structure
 * @param uprobe structure used to raise events
 * @param signature signature of the pipe allocator
 * @param args optional arguments
 * @return pointer to upipe or NULL in case of allocation error
 */
static struct upipe *upipe_audiocont_sub_alloc(struct upipe_mgr *mgr,
                                               struct uprobe *uprobe,
                                               uint32_t signature, va_list args)
{
    struct upipe *upipe = upipe_audiocont_sub_alloc_void(mgr,
                                     uprobe, signature, args);
    if (unlikely(upipe == NULL))
        return NULL;

    struct upipe_audiocont_sub *upipe_audiocont_sub =
        upipe_audiocont_sub_from_upipe(upipe);
    upipe_audiocont_sub_init_urefcount(upipe);
    upipe_audiocont_sub_init_sub(upipe);
    ulist_init(&upipe_audiocont_sub->urefs);
    upipe_audiocont_sub->flow_def = NULL;

    upipe_throw_ready(upipe);
    return upipe;
}

/** @internal @This receives data.
 *
 * @param upipe description structure of the pipe
 * @param uref uref structure
 * @param upump_p reference to pump that generated the buffer
 */
static void upipe_audiocont_sub_input(struct upipe *upipe, struct uref *uref,
                                      struct upump **upump_p)
{
    struct upipe_audiocont_sub *upipe_audiocont_sub =
                                upipe_audiocont_sub_from_upipe(upipe);

    uint64_t pts;
    if (unlikely(!ubase_check(uref_clock_get_pts_sys(uref, &pts)))) {
        upipe_warn_va(upipe, "packet without pts");
        uref_free(uref);
        return;
    }
    uint64_t duration;
    if (unlikely(!ubase_check(uref_clock_get_duration(uref, &duration)))) {
        upipe_warn_va(upipe, "packet without duration");
        uref_free(uref);
        return;
    }

    ulist_add(&upipe_audiocont_sub->urefs, uref_to_uchain(uref));
}

/** @internal @This sets the input flow definition.
 *
 * @param upipe description structure of the pipe
 * @param flow_def flow definition packet
 * @return an error code
 */
static enum ubase_err upipe_audiocont_sub_set_flow_def(struct upipe *upipe,
                                                       struct uref *flow_def)
{
    struct upipe_audiocont_sub *upipe_audiocont_sub =
           upipe_audiocont_sub_from_upipe(upipe);
    struct upipe_audiocont *upipe_audiocont =
                            upipe_audiocont_from_sub_mgr(upipe->mgr);

    if (flow_def == NULL) {
        return UBASE_ERR_INVALID;
    }
    UBASE_RETURN(uref_flow_match_def(flow_def, EXPECTED_FLOW_DEF))

    struct uref *flow_def_dup;
    if (unlikely((flow_def_dup = uref_dup(flow_def)) == NULL)) {
        upipe_throw_fatal(upipe, UBASE_ERR_ALLOC);
        return UBASE_ERR_ALLOC;
    }
    if (unlikely(upipe_audiocont_sub->flow_def)) {
        uref_free(upipe_audiocont_sub->flow_def);
    }
    upipe_audiocont_sub->flow_def = flow_def_dup;

    /* check flow against (next) grid input name */
    const char *name = NULL;
    uref_flow_get_name(flow_def, &name);
    if (upipe_audiocont->input_name
        && likely(ubase_check(uref_flow_get_name(flow_def, &name)))
        && !strcmp(upipe_audiocont->input_name, name)) {
        upipe_audiocont->input_cur = upipe;
        upipe_notice_va(upipe, "switched to input \"%s\" (%p)", name, upipe);
    }

    return UBASE_ERR_NONE;
}

/** @internal @This processes control commands on a subpipe of a videocont
 * pipe.
 *
 * @param upipe description structure of the pipe
 * @param command type of command to process
 * @param args arguments of the command
 * @return false in case of error
 */
static int upipe_audiocont_sub_control(struct upipe *upipe,
                                       int command,
                                       va_list args)
{
    switch (command) {
        case UPIPE_SET_FLOW_DEF: {
            struct uref *flow_def = va_arg(args, struct uref *);
            return upipe_audiocont_sub_set_flow_def(upipe, flow_def);
        }
        case UPIPE_SUB_GET_SUPER: {
            struct upipe **p = va_arg(args, struct upipe **);
            return upipe_audiocont_sub_get_super(upipe, p);
        }

        default:
            return UBASE_ERR_UNHANDLED;
    }
}

/** @This marks an input subpipe as dead.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_audiocont_sub_dead(struct upipe *upipe)
{
    struct upipe_audiocont_sub *upipe_audiocont_sub =
                                upipe_audiocont_sub_from_upipe(upipe);
    struct upipe_audiocont *upipe_audiocont =
                            upipe_audiocont_from_sub_mgr(upipe->mgr);

    struct uchain *uchain, *uchain_tmp;
    ulist_delete_foreach (&upipe_audiocont_sub->urefs, uchain, uchain_tmp) {
        struct uref *uref = uref_from_uchain(uchain);
        ulist_delete(uchain);
        uref_free(uref);
    }
    if (upipe == upipe_audiocont->input_cur) {
        upipe_audiocont->input_cur = NULL;
    }

    if (likely(upipe_audiocont_sub->flow_def)) {
        uref_free(upipe_audiocont_sub->flow_def);
    }

    upipe_throw_dead(upipe);
    upipe_audiocont_sub_clean_sub(upipe);
    upipe_audiocont_sub_clean_urefcount(upipe);
    upipe_audiocont_sub_free_void(upipe);
}

/** @internal @This initializes the input manager for a videocont pipe.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_audiocont_init_sub_mgr(struct upipe *upipe)
{
    struct upipe_audiocont *upipe_audiocont = upipe_audiocont_from_upipe(upipe);
    struct upipe_mgr *sub_mgr = &upipe_audiocont->sub_mgr;
    sub_mgr->refcount = upipe_audiocont_to_urefcount(upipe_audiocont);
    sub_mgr->signature = UPIPE_AUDIOCONT_INPUT_SIGNATURE;
    sub_mgr->upipe_alloc = upipe_audiocont_sub_alloc;
    sub_mgr->upipe_input = upipe_audiocont_sub_input;
    sub_mgr->upipe_control = upipe_audiocont_sub_control;
    sub_mgr->upipe_mgr_control = NULL;
}

/** @internal @This allocates a videocont pipe.
 *
 * @param mgr common management structure
 * @param uprobe structure used to raise events
 * @param signature signature of the pipe allocator
 * @param args optional arguments
 * @return pointer to upipe or NULL in case of allocation error
 */
static struct upipe *upipe_audiocont_alloc(struct upipe_mgr *mgr,
                                           struct uprobe *uprobe,
                                           uint32_t signature, va_list args)
{
    struct upipe *upipe = upipe_audiocont_alloc_void(mgr,
                                 uprobe, signature, args);
    if (unlikely(upipe == NULL))
        return NULL;

    upipe_audiocont_init_urefcount(upipe);
    upipe_audiocont_init_output(upipe);
    upipe_audiocont_init_sub_mgr(upipe);
    upipe_audiocont_init_sub_subs(upipe);

    struct upipe_audiocont *upipe_audiocont = upipe_audiocont_from_upipe(upipe);
    upipe_audiocont->input_cur = NULL;
    upipe_audiocont->input_name = NULL;
    upipe_audiocont->tolerance = TOLERANCE;
    upipe_audiocont->planes = 0;
    upipe_audiocont->samplerate = 0;

    upipe_throw_ready(upipe);

    return upipe;
}

static inline int upipe_audiocont_resize_uref(struct uref *uref, size_t offset,
                                              uint64_t samplerate)
{
    uref_sound_resize(uref, offset, -1);
    uint64_t duration = (uint64_t)offset * UCLOCK_FREQ / samplerate;
    uint64_t pts;
    if (ubase_check(uref_clock_get_pts_prog(uref, &pts)))
        uref_clock_set_pts_prog(uref, pts + duration);
    if (ubase_check(uref_clock_get_pts_sys(uref, &pts)))
        uref_clock_set_pts_sys(uref, pts + duration);
    if (ubase_check(uref_clock_get_pts_orig(uref, &pts)))
        uref_clock_set_pts_orig(uref, pts + duration);

    return UBASE_ERR_NONE;
}

/** @internal @This processes reference ("clock") input.
 *
 * @param upipe description structure of the pipe
 * @param uref uref structure
 * @param upump_p reference to pump that generated the buffer
 */
static void upipe_audiocont_input(struct upipe *upipe, struct uref *uref,
                                  struct upump **upump_p)
{
    struct upipe_audiocont *upipe_audiocont = upipe_audiocont_from_upipe(upipe);
    struct uchain *uchain, *uchain_sub, *uchain_tmp;
    uint64_t next_pts = 0, next_duration = 0;

    if (unlikely(!upipe_audiocont->flow_def)) {
        upipe_warn_va(upipe, "need to define flow def first");
        uref_free(uref);
        return;
    }

    if (unlikely(!ubase_check(uref_clock_get_pts_sys(uref, &next_pts)))) {
        upipe_warn_va(upipe, "packet without pts");
        uref_free(uref);
        return;
    }
    if (unlikely(!ubase_check(uref_clock_get_duration(uref, &next_duration)))) {
        upipe_warn_va(upipe, "packet without duration");
        uref_free(uref);
        return;
    }

    size_t ref_size = 0;
    uint8_t ref_sample_size = 0;
    if (unlikely(!ubase_check(uref_sound_size(uref,
                              &ref_size, &ref_sample_size)))) {
        upipe_warn_va(upipe, "invalid ref packet");
        uref_free(uref);
        return;
    }

    /* clean old urefs first */
    int subs = 0;
    ulist_foreach(&upipe_audiocont->subs, uchain_sub) {
        struct upipe_audiocont_sub *sub =
               upipe_audiocont_sub_from_uchain(uchain_sub);
        ulist_delete_foreach(&sub->urefs, uchain, uchain_tmp) {
            uint64_t pts = 0;
            uint64_t duration = 0;
            size_t size = 0;
            struct uref *uref_uchain = uref_from_uchain(uchain);
            uref_clock_get_pts_sys(uref_uchain, &pts);
            uref_clock_get_duration(uref_uchain, &duration);
            uref_sound_size(uref_uchain, &size, NULL);

            if (pts + duration < next_pts) {
                /* packet too old */
                upipe_verbose_va(upipe, "(%d) deleted uref %p (%"PRIu64")",
                                 subs, uref_uchain, pts);
                ulist_delete(uchain);
                uref_free(uref_uchain);
            } else if (pts > next_pts) {
                /* packet in the future */
                break;
            } else {
                /* resize buffer (drop begining of packet) */
                size_t offset = (next_pts - pts)
                                  * upipe_audiocont->samplerate
                                  / UCLOCK_FREQ;
                upipe_verbose_va(upipe,
                    "(%d) %p next_pts %"PRIu64" pts %"PRIu64
                        " samplerate %"PRIu64" size %zu offset %zu",
                    subs, uref_uchain,
                    next_pts, pts, upipe_audiocont->samplerate, size, offset);
                
                if (unlikely(offset > size)) {
                    ulist_delete(uchain);
                    uref_free(uref_uchain);
                } else {
                    upipe_audiocont_resize_uref(uref_uchain, offset,
                                                upipe_audiocont->samplerate);
                    break;
                }
            }
        }
        subs++;
    }

    uint8_t planes = upipe_audiocont->planes;
    uint8_t *ref_buffers[planes];

    if (unlikely(!upipe_audiocont->input_cur)) {
        goto output;
    }
    struct upipe_audiocont_sub *input =
        upipe_audiocont_sub_from_upipe(upipe_audiocont->input_cur);

    if (unlikely(!ubase_check(uref_sound_write_uint8_t(uref, 0,
                                -1, ref_buffers, planes)))) {
        upipe_warn_va(upipe, "could not map ref packet");
        uref_free(uref);
        return;
    }

    /* copy input sound buffer to output stream */
    size_t offset = 0;
    while (offset < ref_size) {
        struct uchain *uchain = ulist_peek(&input->urefs);
        if (unlikely(!uchain)) {
            break;
        }
        struct uref *input_uref = uref_from_uchain(uchain);
        size_t size;
        uref_sound_size(uref, &size, NULL);

        size_t extracted = ((ref_size - offset) < size ) ?
                           (ref_size - offset) : size;
        upipe_verbose_va(upipe, "%p off %zu ext %zu size %zu",
                         input_uref, offset, extracted, size);
        const uint8_t *in_buffers[planes];
        if (unlikely(!ubase_check(uref_sound_read_uint8_t(input_uref, 0,
                                       extracted, in_buffers, planes)))) {
            upipe_warn(upipe, "invalid input buffer");
            uref_free(uref_from_uchain(ulist_pop(&input->urefs)));
            break;
        }
        int i;
        for (i=0; (i < planes) && ref_buffers[i] && in_buffers[i]; i++) {
            memcpy(ref_buffers[i] + offset * ref_sample_size, in_buffers[i],
                   extracted * ref_sample_size);
        }
        uref_sound_unmap(input_uref, 0, extracted, planes);

        offset += extracted;
        if (extracted == size) {
            /* input buffer entirely copied */
            uref_free(uref_from_uchain(ulist_pop(&input->urefs)));
        } else {
            /* resize input buffer (drop copied segment) */
            upipe_audiocont_resize_uref(input_uref, extracted,
                                        upipe_audiocont->samplerate);
        }
    }

    uref_sound_unmap(uref, 0, -1, planes);
    
output:
    upipe_audiocont_output(upipe, uref, upump_p);
}

/** @internal @This sets the input flow definition.
 *
 * @param upipe description structure of the pipe
 * @param flow_def flow definition packet
 * @return an error code
 */
static enum ubase_err upipe_audiocont_set_flow_def(struct upipe *upipe,
                                                   struct uref *flow_def)
{
    struct upipe_audiocont *upipe_audiocont = upipe_audiocont_from_upipe(upipe);
    if (flow_def == NULL)
        return UBASE_ERR_INVALID;
    UBASE_RETURN(uref_flow_match_def(flow_def, EXPECTED_FLOW_DEF))

    uint8_t planes;
    uint64_t rate;
    if (unlikely(!ubase_check(uref_sound_flow_get_planes(flow_def, &planes))
                 || !ubase_check(uref_sound_flow_get_rate(flow_def, &rate)))) {
        return UBASE_ERR_INVALID;
    }

    struct uref *flow_def_dup;
    if (unlikely((flow_def_dup = uref_dup(flow_def)) == NULL)) {
        upipe_throw_fatal(upipe, UBASE_ERR_ALLOC);
        return UBASE_ERR_ALLOC;
    }
    upipe_audiocont_store_flow_def(upipe, flow_def_dup);
    upipe_audiocont->planes = planes;
    upipe_audiocont->samplerate = rate;

    return UBASE_ERR_NONE;
}

/** @internal @This sets the input by name.
 *
 * @param upipe description structure of the pipe
 * @param name input name
 * @return an error code
 */
static enum ubase_err _upipe_audiocont_set_input(struct upipe *upipe,
                                                 const char *name)
{
    struct upipe_audiocont *upipe_audiocont = upipe_audiocont_from_upipe(upipe);
    char *name_dup = NULL;

    if (name) {
        name_dup = strdup(name);
        if (unlikely(!name_dup)) {
            upipe_throw_fatal(upipe, UBASE_ERR_ALLOC);
            return UBASE_ERR_ALLOC;
        }

        struct uchain *uchain;
        ulist_foreach(&upipe_audiocont->subs, uchain) {
            struct upipe_audiocont_sub *sub =
                       upipe_audiocont_sub_from_uchain(uchain);
            const char *flow_name = NULL;
            if (sub->flow_def
                && likely(ubase_check(uref_flow_get_name(sub->flow_def, &flow_name)))
                && !strcmp(name_dup, flow_name)) {
                upipe_audiocont->input_cur = upipe_audiocont_sub_to_upipe(sub);
                upipe_notice_va(upipe, "switched to input \"%s\" (%p)",
                                name_dup, upipe_audiocont->input_cur);
                break;
            }
        }
    }

    free(upipe_audiocont->input_name);
    upipe_audiocont->input_name = name_dup;
    return UBASE_ERR_NONE;
}

/** @This returns the current input name if any.
 *
 * @param upipe description structure of the pipe
 * @param name_p filled with current input name pointer or NULL
 * @return an error code
 */
static inline enum ubase_err _upipe_audiocont_get_current_input(
                       struct upipe *upipe, const char **name_p)
{
    struct upipe_audiocont *upipe_audiocont = upipe_audiocont_from_upipe(upipe);
    if (unlikely(!name_p)) {
        return UBASE_ERR_INVALID;
    }

    *name_p = NULL;
    if (upipe_audiocont->input_cur) {
        struct upipe_audiocont_sub *sub = 
               upipe_audiocont_sub_from_upipe(upipe_audiocont->input_cur);
        if (sub->flow_def) {
            uref_flow_get_name(sub->flow_def, name_p);
        }
    }
    return UBASE_ERR_NONE;
}

/** @internal @This processes control commands.
 *
 * @param upipe description structure of the pipe
 * @param command type of command to process
 * @param args arguments of the command
 * @return an error code
 */
static enum ubase_err _upipe_audiocont_control(struct upipe *upipe,
                                               enum upipe_command command,
                                               va_list args)
{
    struct upipe_audiocont *upipe_audiocont = upipe_audiocont_from_upipe(upipe);
    switch (command) {
        case UPIPE_SET_FLOW_DEF: {
            struct uref *flow_def = va_arg(args, struct uref *);
            return upipe_audiocont_set_flow_def(upipe, flow_def);
        }
        case UPIPE_GET_FLOW_DEF: {
            struct uref **p = va_arg(args, struct uref **);
            return upipe_audiocont_get_flow_def(upipe, p);
        }
        case UPIPE_GET_OUTPUT: {
            struct upipe **p = va_arg(args, struct upipe **);
            return upipe_audiocont_get_output(upipe, p);
        }
        case UPIPE_SET_OUTPUT: {
            struct upipe *output = va_arg(args, struct upipe *);
            return upipe_audiocont_set_output(upipe, output);
        }
        case UPIPE_GET_SUB_MGR: {
            struct upipe_mgr **p = va_arg(args, struct upipe_mgr **);
            return upipe_audiocont_get_sub_mgr(upipe, p);
        }
        case UPIPE_ITERATE_SUB: {
            struct upipe **p = va_arg(args, struct upipe **);
            return upipe_audiocont_iterate_sub(upipe, p);
        }

        case UPIPE_AUDIOCONT_SET_INPUT: {
            assert(va_arg(args, int) == UPIPE_AUDIOCONT_SIGNATURE);
            const char *name = va_arg(args, const char*);
            return _upipe_audiocont_set_input(upipe, name);
        }
        case UPIPE_AUDIOCONT_GET_INPUT: {
            assert(va_arg(args, int) == UPIPE_AUDIOCONT_SIGNATURE);
            *va_arg(args, const char**) = upipe_audiocont->input_name;
            return UBASE_ERR_NONE;
        }
        case UPIPE_AUDIOCONT_SET_TOLERANCE: {
            assert(va_arg(args, int) == UPIPE_AUDIOCONT_SIGNATURE);
            upipe_audiocont->tolerance = va_arg(args, uint64_t);
            return UBASE_ERR_NONE;
        }
        case UPIPE_AUDIOCONT_GET_TOLERANCE: {
            assert(va_arg(args, int) == UPIPE_AUDIOCONT_SIGNATURE);
            *va_arg(args, uint64_t *) = upipe_audiocont->tolerance;
            return UBASE_ERR_NONE;
        }
        case UPIPE_AUDIOCONT_GET_CURRENT_INPUT: {
            assert(va_arg(args, int) == UPIPE_AUDIOCONT_SIGNATURE);
            const char **name_p = va_arg(args, const char **);
            return _upipe_audiocont_get_current_input(upipe, name_p);
        }


        default:
            return UBASE_ERR_UNHANDLED;
    }
}

/** @internal @This processes control commands.
 *
 * @param upipe description structure of the pipe
 * @param command type of command to process
 * @param args arguments of the command
 * @return an error code
 */
static int upipe_audiocont_control(struct upipe *upipe,
                                   int command,
                                   va_list args)
{
    UBASE_RETURN(_upipe_audiocont_control(upipe, command, args))

    return UBASE_ERR_NONE;
}

/** @This frees a upipe.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_audiocont_free(struct upipe *upipe)
{
    struct upipe_audiocont *upipe_audiocont = upipe_audiocont_from_upipe(upipe);
    upipe_throw_dead(upipe);

    free(upipe_audiocont->input_name);

    upipe_audiocont_clean_sub_subs(upipe);
    upipe_audiocont_clean_output(upipe);
    upipe_audiocont_clean_urefcount(upipe);
    upipe_audiocont_free_void(upipe);
}

/** module manager static descriptor */
static struct upipe_mgr upipe_audiocont_mgr = {
    .refcount = NULL,
    .signature = UPIPE_AUDIOCONT_SIGNATURE,

    .upipe_alloc = upipe_audiocont_alloc,
    .upipe_input = upipe_audiocont_input,
    .upipe_control = upipe_audiocont_control,

    .upipe_mgr_control = NULL
};

/** @This returns the management structure for all videocont pipes.
 *
 * @return pointer to manager
 */
struct upipe_mgr *upipe_audiocont_mgr_alloc(void)
{
    return &upipe_audiocont_mgr;
}
