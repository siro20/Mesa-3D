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

#include "nine_queue.h"
#include "os/os_thread.h"
#include "util/macros.h"
#include "nine_helpers.h"

#define NINE_QUEUE_SIZE (1024)
#define NINE_QUEUE_MASK (NINE_QUEUE_SIZE - 1)

#define NINE_QUEUE_POOL_SIZE (1024 * 1024 * 4)
#define NINE_QUEUE_POOL_MASK (NINE_QUEUE_POOL_SIZE - 1)

#define DBG_CHANNEL DBG_DEVICE

struct nine_ringqueue {
    unsigned head;
    unsigned tail;
    unsigned tail_end;
    void *mem_pool;
    pipe_condvar event_pop;
    pipe_condvar event_push;
    pipe_mutex mutex_pop;
    pipe_mutex mutex_push;
};

/* RX functions */

/* Gets an element to process. Blocks if none in queue. */
void *nine_ringqueue_get(struct nine_ringqueue* ctx)
{
    pipe_mutex_lock(ctx->mutex_push);
    while(ctx->tail == ctx->head)
    {
        pipe_condvar_wait(ctx->event_push, ctx->mutex_push);
    }
    pipe_mutex_unlock(ctx->mutex_push);

    if (ctx->tail == ctx->tail_end) {
        ctx->tail = 0;
        ctx->tail_end = NINE_QUEUE_POOL_SIZE;
    }

    return ctx->mem_pool + ctx->tail;
}

/* Pops a buffer with size space */
void nine_ringqueue_pop(struct nine_ringqueue* ctx, unsigned space)
{
    pipe_mutex_lock(ctx->mutex_pop);

    ctx->tail += space;

    pipe_condvar_signal(ctx->event_pop);

    pipe_mutex_unlock(ctx->mutex_pop);
}

/* TX functions */

/* gets a buffer with size space. May block if queue is full. */
void *queue_get_free(struct nine_ringqueue* ctx, unsigned space)
{
    if (ctx->head + space > NINE_QUEUE_POOL_SIZE) {

		pipe_mutex_lock(ctx->mutex_pop);
		while (ctx->tail > ctx->head)
		{
			pipe_condvar_wait(ctx->event_pop, ctx->mutex_pop);
		}
		pipe_mutex_unlock(ctx->mutex_pop);

		pipe_mutex_lock(ctx->mutex_pop);
		while(ctx->tail < space)
		{
			pipe_condvar_wait(ctx->event_pop, ctx->mutex_pop);
		}
		pipe_mutex_unlock(ctx->mutex_pop);

        ctx->tail_end = ctx->head;

        return ctx->mem_pool;
    } else if (ctx->head < ctx->tail && ctx->head + space > ctx->tail) {
        pipe_mutex_lock(ctx->mutex_pop);
        while (ctx->head + space > ctx->tail)
        {
            pipe_condvar_wait(ctx->event_pop, ctx->mutex_pop);
        }
        pipe_mutex_unlock(ctx->mutex_pop);
    }

    return ctx->mem_pool + ctx->head;
}

/* pushes a buffer with size space. Doesn't wait. */
void nine_ringqueue_push(struct nine_ringqueue* ctx, unsigned space)
{
    pipe_mutex_lock(ctx->mutex_push);

    if (ctx->head + space > NINE_QUEUE_POOL_SIZE) {
        ctx->head = space;
    } else {
        ctx->head += space;
    }

    pipe_condvar_signal(ctx->event_push);
    pipe_mutex_unlock(ctx->mutex_push);
}

struct nine_ringqueue*
nine_ringqueue_create(void)
{
    struct nine_ringqueue *ctx;

    ctx = CALLOC_STRUCT(nine_ringqueue);
    if (!ctx)
        return NULL;

    ctx->mem_pool = MALLOC(NINE_QUEUE_POOL_SIZE);

    if (!ctx->mem_pool) {
        FREE(ctx);
        return NULL;
    }

    pipe_condvar_init(ctx->event_pop);
    pipe_mutex_init(ctx->mutex_pop);

    pipe_condvar_init(ctx->event_push);
    pipe_mutex_init(ctx->mutex_push);


    ctx->tail_end = NINE_QUEUE_POOL_SIZE;
    return ctx;
}

void
nine_ringqueue_delete(struct nine_ringqueue *ctx)
{
    pipe_mutex_destroy(ctx->mutex_pop);
    pipe_mutex_destroy(ctx->mutex_push);

    FREE(ctx->mem_pool);
    FREE(ctx);
}
