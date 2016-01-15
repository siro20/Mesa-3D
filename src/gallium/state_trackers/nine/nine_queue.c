/*
 * Copyright 2016 Patrick Rudolph <siro@das-labor.org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE. */

#include "util/u_atomic.h"
#include "nine_queue.h"
#include "os/os_thread.h"
#include "util/macros.h"
#include "nine_helpers.h"

#define NINE_QUEUE_SIZE (128)
#define NINE_QUEUE_MASK (NINE_QUEUE_SIZE - 1)

#define NINE_QUEUE_POOL_SIZE (1024 * 1024)
#define NINE_QUEUE_POOL_MASK (NINE_QUEUE_POOL_SIZE - 1)

#define DBG_CHANNEL DBG_DEVICE

#define ENTER_CRITICAL_SECTION() { \
    while (p_atomic_cmpxchg(&ctx->crit_section, 0, 1)) \
    {ERR("waiting on crit section\n");    pthread_yield();} \
}

#define LEAVE_CRITICAL_SECTION() { \
    p_atomic_dec(&ctx->crit_section); \
}

#define WAIT_FOR_FREE_SLOT() { \
    while (p_atomic_read(&ctx->queue_size) == NINE_QUEUE_SIZE) { \
        signal_process_start(ctx); \
        pthread_yield(); } \
}

struct concurrent_queue {
    struct queue_element *buffer;
    unsigned head;
    unsigned tail;
    void *mem_pool;
    volatile unsigned mem_pool_head;
    volatile unsigned mem_pool_tail;
    volatile unsigned crit_section;
    volatile unsigned queue_size;
    pipe_condvar event;
    pipe_mutex mutex;
};

static unsigned get_free_pool_size(struct concurrent_queue* ctx) {
    return p_atomic_read(&ctx->mem_pool_tail) + (NINE_QUEUE_POOL_SIZE - ctx->mem_pool_head);
}

static void nop(struct pipe_context_csmt *a, void *b) { (void)a; (void)b;}

static int max_mem = 0;
struct queue_element*
get_free_slot(struct concurrent_queue* ctx, unsigned memory_size, void **mem) {
    unsigned ticket;
    struct queue_element* element;

    assert(ctx);

    ENTER_CRITICAL_SECTION();

    WAIT_FOR_FREE_SLOT();

    ticket = ctx->head++ & NINE_QUEUE_MASK;
    element = &ctx->buffer[ticket];

    if (memory_size >= (NINE_QUEUE_POOL_SIZE / 4)) {
        /* should never happen */
        *mem = element->mem = malloc(memory_size);
        ERR("have to allocate %d bytes\n", memory_size);
        element->pool_size = 0;
    } else if (memory_size) {
        if (max_mem < memory_size) {
        ERR("memory_size=%d\n", memory_size);
        max_mem = memory_size;
        }
        assert(mem);
        if ((ctx->mem_pool_head + memory_size) > NINE_QUEUE_POOL_SIZE) {
            /* insert nop */
            element->mem = NULL;
            element->processed = NULL;
            element->func = nop;
            element->pool_size = NINE_QUEUE_POOL_SIZE - (ctx->mem_pool_head + memory_size);
            ctx->mem_pool_head = 0;
            element->processed = NULL;
            p_atomic_inc(&ctx->queue_size);

            WAIT_FOR_FREE_SLOT();

            /* get next element */
            ticket = ctx->head++ & NINE_QUEUE_MASK;
            element = &ctx->buffer[ticket];
        }

        /* wait for enough memory */
        while (get_free_pool_size(ctx) < memory_size)
            pthread_yield();

        *mem = ctx->mem_pool + ctx->mem_pool_head;
        element->pool_size = memory_size;
        ctx->mem_pool_head = (ctx->mem_pool_head + element->pool_size) & NINE_QUEUE_POOL_MASK;
        element->mem = NULL;
    } else {
        element->pool_size = 0;
        element->mem = NULL;
    }

    return element;
}

struct queue_element*
wait_slot_ready(struct concurrent_queue* ctx) {
    unsigned ticket = ctx->tail++ & NINE_QUEUE_MASK;
    struct queue_element* element = &ctx->buffer[ticket];

    pipe_mutex_lock(ctx->mutex);
    while (!p_atomic_read(&ctx->queue_size)) {
        pipe_condvar_wait(ctx->event, ctx->mutex);
    }
    pipe_mutex_unlock(ctx->mutex);

    return element;
}

void signal_process_start(struct concurrent_queue* ctx) {
    pipe_mutex_lock(ctx->mutex);
    pipe_condvar_signal(ctx->event);
    pipe_mutex_unlock(ctx->mutex);
    pthread_yield();
}

void set_slot_ready(struct concurrent_queue* ctx, struct queue_element* element) {
    element->processed = NULL;
    p_atomic_inc(&ctx->queue_size);
/*
    pipe_mutex_lock(ctx->mutex);
    pipe_condvar_signal(ctx->event);
    pipe_mutex_unlock(ctx->mutex);*/

    LEAVE_CRITICAL_SECTION();
}

void set_slot_processed(struct concurrent_queue* ctx, struct queue_element* element) {
    if (element->processed)
        p_atomic_inc(element->processed);

    if (element->mem) {
        FREE(element->mem);
    } else if (element->pool_size) {
        ctx->mem_pool_tail = (ctx->mem_pool_tail + element->pool_size) & NINE_QUEUE_POOL_MASK;
    }
    p_atomic_dec(&ctx->queue_size);
}

void set_slot_ready_and_wait(struct concurrent_queue* ctx, struct queue_element* element) {
    unsigned done = FALSE;
    element->processed = &done;

    p_atomic_inc(&ctx->queue_size);

    signal_process_start(ctx);

    LEAVE_CRITICAL_SECTION();

    while (!p_atomic_read(&done)) {
        pthread_yield();
    }
}

struct concurrent_queue *nine_concurrent_queue_create() {
    struct concurrent_queue *ctx;

    ctx = CALLOC_STRUCT(concurrent_queue);
    if (!ctx)
        return NULL;

    ctx->buffer = CALLOC(1, NINE_QUEUE_SIZE * sizeof(struct queue_element));
    if (!ctx->buffer) {
        FREE(ctx);
        return NULL;
    }

    ctx->mem_pool = malloc(NINE_QUEUE_POOL_SIZE);

    if (!ctx->mem_pool) {
        FREE(ctx->buffer);
        FREE(ctx);
        return NULL;
    }

    pipe_condvar_init(ctx->event);
    pipe_mutex_init(ctx->mutex);

    return ctx;
}

void nine_concurrent_queue_delete(struct concurrent_queue *ctx) {
    FREE(ctx->mem_pool);
    FREE(ctx->buffer);
    FREE(ctx);
}
