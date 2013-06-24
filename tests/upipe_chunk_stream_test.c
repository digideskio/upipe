/*
 * Copyright (C) 2013 OpenHeadend S.A.R.L.
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
 * @short unit tests for chunk stream module
 */

#undef NDEBUG

#include <upipe/uprobe.h>
#include <upipe/uprobe_stdio.h>
#include <upipe/uprobe_prefix.h>
#include <upipe/uprobe_log.h>
#include <upipe/umem.h>
#include <upipe/umem_alloc.h>
#include <upipe/udict.h>
#include <upipe/udict_inline.h>
#include <upipe/ubuf.h>
#include <upipe/ubuf_block.h>
#include <upipe/ubuf_block_mem.h>
#include <upipe/uref.h>
#include <upipe/uref_flow.h>
#include <upipe/uref_block_flow.h>
#include <upipe/uref_block.h>
#include <upipe/uref_std.h>
#include <upipe/upipe.h>
#include <upipe-modules/upipe_chunk_stream.h>

#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <inttypes.h>
#include <assert.h>

#define UDICT_POOL_DEPTH 10
#define UREF_POOL_DEPTH 10
#define UBUF_POOL_DEPTH 10
#define UPROBE_LOG_LEVEL UPROBE_LOG_DEBUG

#define ITERS 10
#define PACKETS_NUM 45
#define PACKET_SIZE 524
#define MTU 1342
#define ALIGN 3
#define REAL_MTU ((MTU / ALIGN) * ALIGN)

unsigned int nb_packets = 0;

/** definition of our uprobe */
static bool catch(struct uprobe *uprobe, struct upipe *upipe,
                  enum uprobe_event event, va_list args)
{
    switch (event) {
        default:
            assert(0);
            break;
        case UPROBE_READY:
        case UPROBE_DEAD:
        case UPROBE_NEW_FLOW_DEF:
            break;
    }
    return true;
}

/** helper phony pipe to test upipe_chunk_stream */
static struct upipe *chunk_test_alloc(struct upipe_mgr *mgr,
                                          struct uprobe *uprobe,
                                          uint32_t signature, va_list args)
{
    struct upipe *upipe = malloc(sizeof(struct upipe));
    assert(upipe != NULL);
    upipe_init(upipe, mgr, uprobe);
    return upipe;
}

/** helper phony pipe to test upipe_chunk_stream */
static void chunk_test_input(struct upipe *upipe, struct uref *uref,
                          struct upump *upump)
{
    assert(uref != NULL);
    const uint8_t *buffer;
    size_t size = 0;
    int pos = 0, len = -1;
    assert(uref_block_size(uref, &size));
    upipe_dbg_va(upipe, "received packet of size %zu", size);
    nb_packets--;
    if (nb_packets) {
        assert(size == REAL_MTU);
    }

    while (size > 0) {
        assert(uref_block_read(uref, pos, &len, &buffer));
        uref_block_unmap(uref, 0);
        size -= len;
        pos += len;
    }
    uref_free(uref);
    upipe_dbg_va(upipe, "nb_packets %u", nb_packets);
}

/** helper phony pipe to test upipe_chunk_stream */
static void chunk_test_free(struct upipe *upipe)
{
    upipe_clean(upipe);
    free(upipe);
}

/** helper phony pipe to test upipe_chunk_stream */
static struct upipe_mgr chunk_test_mgr = {
    .upipe_alloc = chunk_test_alloc,
    .upipe_input = chunk_test_input,
    .upipe_control = NULL,
    .upipe_free = NULL,

    .upipe_mgr_free = NULL
};

int main(int argc, char *argv[])
{
    struct umem_mgr *umem_mgr = umem_alloc_mgr_alloc();
    assert(umem_mgr != NULL);
    struct udict_mgr *udict_mgr = udict_inline_mgr_alloc(UDICT_POOL_DEPTH,
                                                         umem_mgr, -1, -1);
    assert(udict_mgr != NULL);
    struct uref_mgr *uref_mgr = uref_std_mgr_alloc(UREF_POOL_DEPTH, udict_mgr,
                                                   0);
    assert(uref_mgr != NULL);
    struct ubuf_mgr *ubuf_mgr = ubuf_block_mem_mgr_alloc(UBUF_POOL_DEPTH,
                                                         UBUF_POOL_DEPTH,
                                                         umem_mgr, -1, -1,
                                                         -1, 0);
    assert(ubuf_mgr != NULL);
    struct uprobe uprobe;
    uprobe_init(&uprobe, catch, NULL);
    struct uprobe *uprobe_stdio = uprobe_stdio_alloc(&uprobe, stdout,
                                                     UPROBE_LOG_LEVEL);
    assert(uprobe_stdio != NULL);
    struct uprobe *log = uprobe_log_alloc(uprobe_stdio, UPROBE_LOG_LEVEL);
    assert(log != NULL);

    /* flow def */
    struct uref *uref;
    uref = uref_block_flow_alloc_def(uref_mgr, "foo.");
    assert(uref != NULL);

    struct upipe *upipe_sink = upipe_flow_alloc(&chunk_test_mgr, log, uref);
    assert(upipe_sink != NULL);

    struct upipe_mgr *upipe_chunk_stream_mgr = upipe_chunk_stream_mgr_alloc();
    assert(upipe_chunk_stream_mgr != NULL);
    struct upipe *upipe_chunk_stream = upipe_flow_alloc(upipe_chunk_stream_mgr,
            uprobe_pfx_adhoc_alloc(log, UPROBE_LOG_LEVEL, "chunk"),
            uref);
    assert(upipe_chunk_stream != NULL);
    assert(upipe_set_output(upipe_chunk_stream, upipe_sink));
    uref_free(uref);

    uint8_t *buffer;
    int size, i, j, packet_size;
    unsigned int mtu = 0, align = 0;

    printf("mtu %d align %d real_mtu %d\n", MTU, ALIGN, REAL_MTU);
    assert(upipe_chunk_stream_set_mtu(upipe_chunk_stream, MTU, ALIGN));
    assert(upipe_chunk_stream_get_mtu(upipe_chunk_stream, &mtu, &align));
    assert(mtu == MTU);
    assert(align == ALIGN);

    nb_packets = ((PACKET_SIZE * ITERS * PACKETS_NUM * (ITERS - 1)) / 2 + REAL_MTU - 1) / REAL_MTU;
    for (j=0; j < ITERS; j++) {
        for (i=0; i < PACKETS_NUM; i++) {
            packet_size = j * PACKET_SIZE;
            uref = uref_block_alloc(uref_mgr, ubuf_mgr, packet_size);
            size = -1;
            uref_block_write(uref, 0, &size, &buffer);
            assert(size == packet_size);
            uref_block_unmap(uref, 0);
            upipe_input(upipe_chunk_stream, uref, NULL);
        }
    }

    /* flush */
    upipe_release(upipe_chunk_stream);

    printf("nb_packets: %u\n", nb_packets);
    assert(!nb_packets);

    /* release everything */
    upipe_mgr_release(upipe_chunk_stream_mgr); // nop

    chunk_test_free(upipe_sink);

    uref_mgr_release(uref_mgr);
    ubuf_mgr_release(ubuf_mgr);
    udict_mgr_release(udict_mgr);
    umem_mgr_release(umem_mgr);
    uprobe_log_free(log);
    uprobe_stdio_free(uprobe_stdio);

    return 0;
}
