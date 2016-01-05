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

#include "pipe/p_screen.h"
#include "pipe/p_context.h"
#include "os/os_thread.h"
#include "util/u_transfer.h"
#include "util/u_inlines.h"

#include "nine_pipe.h"
#include "nine_queue.h"
#include "nine_csmt_pipe.h"
#include "nine_csmt_structs.h"

#define DBG_CHANNEL DBG_DEVICE

#define DEBUG_CREATE_ON_SEPERATE_CONTEXT 1

struct pipe_csmt_state {
    boolean index_userbuffer;
    boolean vertex_userbuffer;
};

struct pipe_context_csmt {
    struct pipe_context csmt;
    struct pipe_context *pipe;
#ifdef DEBUG_CREATE_ON_SEPERATE_CONTEXT
    struct pipe_context *pipe_creation;
#endif
    pipe_thread render_thread;
    struct concurrent_queue *queue;
    struct pipe_csmt_state state;
    boolean terminate;
};

static void
nine_csmt_destroy_rx(struct pipe_context_csmt *ctx,
                     void* arg) {
    ctx->terminate = TRUE;
}

static void
nine_csmt_destroy_tx(struct pipe_context *pctx) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct queue_element* slot;

    slot = get_free_slot(ctx->queue, 0, NULL);
    slot->func = nine_csmt_destroy_rx;
    set_slot_ready_and_wait(ctx->queue, slot);
}

static void
nine_csmt_draw_vbo_rx(struct pipe_context_csmt *ctx,
                      void *data) {
    struct csmt_draw_vbo* arg = data;

    ctx->pipe->draw_vbo(ctx->pipe, &arg->info);
}

static void
nine_csmt_draw_vbo_tx(struct pipe_context *pctx,
                      const struct pipe_draw_info *info) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct queue_element* slot;
    struct csmt_draw_vbo* arg;

    slot = get_free_slot(ctx->queue, sizeof(struct csmt_draw_vbo), (void **)&arg);
    slot->data = arg;
    slot->func = nine_csmt_draw_vbo_rx;
    memcpy(&arg->info, info, sizeof(struct pipe_draw_info));

    if (ctx->state.vertex_userbuffer || ctx->state.index_userbuffer) {
        set_slot_ready_and_wait(ctx->queue, slot);
    } else {
        set_slot_ready(ctx->queue, slot);
    }
}

static void
nine_csmt_create_query_rx(struct pipe_context_csmt *ctx,
                          void *data) {
    struct csmt_create_query *arg = data;

    arg->query = ctx->pipe->create_query(ctx->pipe, arg->query_type, arg->index);
}

static struct pipe_query *
nine_csmt_create_query_tx(struct pipe_context *pctx,
                          unsigned query_type,
                          unsigned index) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct csmt_create_query arg;
    struct queue_element* slot;

#ifdef DEBUG_CREATE_ON_SEPERATE_CONTEXT
    return ctx->pipe_creation->create_query(ctx->pipe_creation, query_type, index);
#endif
    slot = get_free_slot(ctx->queue, 0, NULL);
    slot->func = nine_csmt_create_query_rx;
    slot->data = &arg;

    arg.index = index;
    arg.query_type = query_type;

    set_slot_ready_and_wait(ctx->queue, slot);
    return arg.query;
}

static void
nine_csmt_destroy_query_rx(struct pipe_context_csmt *ctx,
                           void *data) {
    ctx->pipe->destroy_query(ctx->pipe, data);
}

static void
nine_csmt_destroy_query_tx(struct pipe_context *pctx,
                           struct pipe_query *q) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct queue_element* slot;

    slot = get_free_slot(ctx->queue, 0, NULL);
    slot->func = nine_csmt_destroy_query_rx;
    slot->data = q;

    set_slot_ready(ctx->queue, slot);
}

static void
nine_csmt_begin_query_rx(struct pipe_context_csmt *ctx,
                         void *data) {
    ctx->pipe->begin_query(ctx->pipe, data);
}

static boolean
nine_csmt_begin_query_tx(struct pipe_context *pctx,
                         struct pipe_query *q) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct queue_element* slot;

    slot = get_free_slot(ctx->queue, 0, NULL);
    slot->func = nine_csmt_begin_query_rx;
    slot->data = q;

    set_slot_ready(ctx->queue, slot);
    /* FIXME */
    return TRUE;
}

static void
nine_csmt_end_query_rx(struct pipe_context_csmt *ctx,
                       void *data) {
    ctx->pipe->end_query(ctx->pipe, data);
}

static void
nine_csmt_end_query_tx(struct pipe_context *pctx,
                       struct pipe_query *q) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct queue_element* slot;

    slot = get_free_slot(ctx->queue, 0, NULL);
    slot->func = nine_csmt_end_query_rx;
    slot->data = q;

    set_slot_ready(ctx->queue, slot);
}

static void
nine_csmt_get_query_result_rx(struct pipe_context_csmt *ctx,
                              void *data) {
    struct csmt_query_result* arg = data;
    arg->result_ready = ctx->pipe->get_query_result(
            ctx->pipe, arg->query, arg->wait, arg->result);
}

static boolean
nine_csmt_get_query_result_tx(struct pipe_context *pctx,
                              struct pipe_query *q,
                              boolean wait,
                              union pipe_query_result *result) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct csmt_query_result arg;
    struct queue_element* slot;

    slot = get_free_slot(ctx->queue, 0, NULL);
    slot->func = nine_csmt_get_query_result_rx;
    slot->data = &arg;
    arg.query = q;
    arg.wait = wait;
    arg.result = result;

    set_slot_ready_and_wait(ctx->queue, slot);

    return arg.result_ready;
}

static void
nine_csmt_create_blend_state_rx(struct pipe_context_csmt *ctx,
                                void *data) {
    struct csmt_create_blend_state *arg = data;
    arg->result = ctx->pipe->create_blend_state(ctx->pipe, &arg->state);
}

static void*
nine_csmt_create_blend_state_tx(struct pipe_context *pctx,
                                const struct pipe_blend_state *state) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct queue_element* slot;
    struct csmt_create_blend_state arg;

    DBG("This=%p state=%p\n", ctx, state);

#ifdef DEBUG_CREATE_ON_SEPERATE_CONTEXT
    return ctx->pipe_creation->create_blend_state(ctx->pipe_creation, state);
#endif
    slot = get_free_slot(ctx->queue, 0, NULL);
    slot->func = nine_csmt_create_blend_state_rx;
    slot->data = &arg;

    memcpy(&arg.state, state, sizeof(struct pipe_blend_state));

    set_slot_ready_and_wait(ctx->queue, slot);

    return arg.result;
}

static void
nine_csmt_create_depth_stencil_alpha_state_rx(struct pipe_context_csmt *ctx,
                                              void *data) {
    struct csmt_pipe_depth_stencil_alpha_state *arg = data;
    arg->result = ctx->pipe->create_depth_stencil_alpha_state(ctx->pipe, &arg->state);
}

static void*
nine_csmt_create_depth_stencil_alpha_state_tx(struct pipe_context *pctx,
                                              const struct pipe_depth_stencil_alpha_state *state) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct queue_element* slot;
    struct csmt_pipe_depth_stencil_alpha_state arg;

    DBG("This=%p state=%p\n", ctx, state);

#ifdef DEBUG_CREATE_ON_SEPERATE_CONTEXT
    return ctx->pipe_creation->create_depth_stencil_alpha_state(ctx->pipe_creation, state);
#endif
    slot = get_free_slot(ctx->queue, 0, NULL);
    slot->func = nine_csmt_create_depth_stencil_alpha_state_rx;
    slot->data = &arg;

    memcpy(&arg.state, state, sizeof(struct pipe_depth_stencil_alpha_state));

    set_slot_ready_and_wait(ctx->queue, slot);

    return arg.result;
}

static void
nine_csmt_bind_depth_stencil_alpha_state_rx(struct pipe_context_csmt *ctx,
                                            void *data) {
    ctx->pipe->bind_depth_stencil_alpha_state(ctx->pipe, data);
}

static void
nine_csmt_bind_depth_stencil_alpha_state_tx(struct pipe_context *pctx,
                                            void *state) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct queue_element* slot;

    DBG("This=%p state=%p\n", ctx, state);

    slot = get_free_slot(ctx->queue, 0, NULL);
    slot->func = nine_csmt_bind_depth_stencil_alpha_state_rx;
    slot->data = state;

    set_slot_ready(ctx->queue, slot);
}

static void
nine_csmt_delete_depth_stencil_alpha_state_rx(struct pipe_context_csmt *ctx,
                                              void *data) {
    ctx->pipe->delete_depth_stencil_alpha_state(ctx->pipe, data);
}

static void
nine_csmt_delete_depth_stencil_alpha_state_tx(struct pipe_context *pctx,
                                              void *state) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct queue_element* slot;

    DBG("This=%p state=%p\n", ctx, state);

    slot = get_free_slot(ctx->queue, 0, NULL);
    slot->func = nine_csmt_delete_depth_stencil_alpha_state_rx;
    slot->data = state;

    set_slot_ready(ctx->queue, slot);
}

static void
nine_csmt_bind_blend_state_rx(struct pipe_context_csmt *ctx,
                              void *data) {
    ctx->pipe->bind_blend_state(ctx->pipe, data);
}

static void
nine_csmt_bind_blend_state_tx(struct pipe_context *pctx,
                              void *arg) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct queue_element* slot;

    slot = get_free_slot(ctx->queue, 0, NULL);
    slot->func = nine_csmt_bind_blend_state_rx;
    slot->data = arg;

    set_slot_ready(ctx->queue, slot);
}

static void
nine_csmt_delete_blend_state_rx(struct pipe_context_csmt *ctx,
                                void *data) {
    ctx->pipe->delete_blend_state(ctx->pipe, data);
}

static void
nine_csmt_delete_blend_state_tx(struct pipe_context *pctx,
                                void *arg) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct queue_element* slot;

    slot = get_free_slot(ctx->queue, 0, NULL);
    slot->func = nine_csmt_delete_blend_state_rx;
    slot->data = arg;

    set_slot_ready(ctx->queue, slot);
}

static void
nine_csmt_create_sampler_state_rx(struct pipe_context_csmt *ctx,
                                  void *data) {
    struct csmt_create_sampler_state *arg = data;
    arg->result = ctx->pipe->create_sampler_state(ctx->pipe, &arg->state);
}

static void*
nine_csmt_create_sampler_state_tx(struct pipe_context *pctx,
                                  const struct pipe_sampler_state *state) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct csmt_create_sampler_state arg;
    struct queue_element* slot;

    DBG("This=%p state=%p\n", ctx, state);

#ifdef DEBUG_CREATE_ON_SEPERATE_CONTEXT
    return ctx->pipe_creation->create_sampler_state(ctx->pipe_creation, state);
#endif
    slot = get_free_slot(ctx->queue, 0, NULL);
    slot->func = nine_csmt_create_sampler_state_rx;
    slot->data = &arg;
    memcpy(&arg.state, state, sizeof(struct pipe_sampler_state));

    set_slot_ready_and_wait(ctx->queue, slot);

    return arg.result;
}

static void
nine_csmt_bind_sampler_states_rx(struct pipe_context_csmt *ctx,
                                 void *data) {
    struct csmt_bind_sampler_state *arg = data;

    ctx->pipe->bind_sampler_states(ctx->pipe, arg->shader,
            arg->start_slot, arg->num_samplers,
            arg->samplers_p);
}

static void
nine_csmt_bind_sampler_states_tx(struct pipe_context *pctx,
                                 unsigned shader, unsigned start_slot,
                                 unsigned num_samplers, void **samplers) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct queue_element* slot;
    struct csmt_bind_sampler_state *arg = NULL;

    DBG("This=%p shader=%u startslot=%u num_samplers=%u samplers=%p\n",
            ctx, shader, start_slot, num_samplers, samplers);

    slot = get_free_slot(ctx->queue, sizeof(struct csmt_bind_sampler_state), (void **)&arg);
    slot->func = nine_csmt_bind_sampler_states_rx;
    slot->data = arg;
    arg->shader = shader;
    arg->start_slot = start_slot;
    arg->num_samplers = num_samplers;
    if (samplers) {
        memcpy(&arg->samplers, samplers, sizeof(arg->samplers));
        arg->samplers_p = &arg->samplers;
    } else {
        arg->samplers_p = NULL;
    }

    set_slot_ready(ctx->queue, slot);
}

static void
nine_csmt_delete_sampler_state_rx(struct pipe_context_csmt *ctx,
                                  void *data) {
    ctx->pipe->delete_sampler_state(ctx->pipe, data);
}

static void
nine_csmt_delete_sampler_state_tx(struct pipe_context *pctx,
                                  void *state) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct queue_element* slot;

    slot = get_free_slot(ctx->queue, 0, NULL);
    slot->func = nine_csmt_delete_sampler_state_rx;
    slot->data = state;

    set_slot_ready(ctx->queue, slot);
}

static void
nine_csmt_create_rasterizer_state_rx(struct pipe_context_csmt *ctx,
                                     void *data) {
    struct csmt_create_rasterizer_state *arg = data;

    arg->result = ctx->pipe->create_rasterizer_state(ctx->pipe, &arg->state);
}

static void*
nine_csmt_create_rasterizer_state_tx(struct pipe_context *pctx,
                                     const struct pipe_rasterizer_state *state) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct csmt_create_rasterizer_state arg;
    struct queue_element* slot;

    assert(state);
#ifdef DEBUG_CREATE_ON_SEPERATE_CONTEXT
    return ctx->pipe_creation->create_rasterizer_state(ctx->pipe_creation, state);
#endif
    slot = get_free_slot(ctx->queue, 0, NULL);
    slot->func = nine_csmt_create_rasterizer_state_rx;
    slot->data = &arg;
    memcpy(&arg.state, state, sizeof(struct pipe_rasterizer_state));

    set_slot_ready_and_wait(ctx->queue, slot);

    return arg.result;
}

static void
nine_csmt_bind_rasterizer_state_rx(struct pipe_context_csmt *ctx,
                                   void *data) {
    ctx->pipe->bind_rasterizer_state(ctx->pipe, data);
}

static void
nine_csmt_bind_rasterizer_state_tx(struct pipe_context *pctx,
                                   void *arg) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct queue_element* slot;

    slot = get_free_slot(ctx->queue, 0, NULL);
    slot->func = nine_csmt_bind_rasterizer_state_rx;
    slot->data = arg;

    set_slot_ready(ctx->queue, slot);
}

static void
nine_csmt_delete_rasterizer_state_rx(struct pipe_context_csmt *ctx,
                                     void *data) {
    ctx->pipe->delete_rasterizer_state(ctx->pipe, data);
}

static void
nine_csmt_delete_rasterizer_state_tx(struct pipe_context *pctx,
                                     void *arg) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct queue_element* slot;

    slot = get_free_slot(ctx->queue, 0, NULL);
    slot->func = nine_csmt_delete_rasterizer_state_rx;
    slot->data = arg;

    set_slot_ready(ctx->queue, slot);
}

static void
nine_csmt_create_fs_state_rx(struct pipe_context_csmt *ctx,
                             void *data) {
    struct csmt_create_shader_state *arg = data;

    arg->result = ctx->pipe->create_fs_state(ctx->pipe, &arg->state);
}

static void*
nine_csmt_create_fs_state_tx(struct pipe_context *pctx,
                             const struct pipe_shader_state *state) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct csmt_create_shader_state arg;
    struct queue_element* slot;

    DBG("This=%p, state=%p\n", ctx, state);
#ifdef DEBUG_CREATE_ON_SEPERATE_CONTEXT
    return ctx->pipe_creation->create_fs_state(ctx->pipe_creation, state);
#endif
    slot = get_free_slot(ctx->queue, 0, NULL);
    slot->func = nine_csmt_create_fs_state_rx;
    slot->data = &arg;

    memcpy(&arg.state, state, sizeof(struct pipe_shader_state));

    set_slot_ready_and_wait(ctx->queue, slot);

    return arg.result;
}

static void
nine_csmt_create_vs_state_rx(struct pipe_context_csmt *ctx,
                             void *data) {
    struct csmt_create_shader_state *arg = data;

    arg->result = ctx->pipe->create_vs_state(ctx->pipe, &arg->state);
}

static void*
nine_csmt_create_vs_state_tx(struct pipe_context *pctx,
                             const struct pipe_shader_state *state) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct csmt_create_shader_state arg;
    struct queue_element* slot;

    DBG("This=%p, state=%p\n", ctx, state);
#ifdef DEBUG_CREATE_ON_SEPERATE_CONTEXT
    return ctx->pipe_creation->create_vs_state(ctx->pipe_creation, state);
#endif
    slot = get_free_slot(ctx->queue, 0, NULL);
    slot->func = nine_csmt_create_vs_state_rx;
    slot->data = &arg;

    memcpy(&arg.state, state, sizeof(struct pipe_shader_state));

    set_slot_ready_and_wait(ctx->queue, slot);

    return arg.result;
}

static void
nine_csmt_bind_vs_state_rx(struct pipe_context_csmt *ctx,
                           void *data) {
    ctx->pipe->bind_vs_state(ctx->pipe, data);
}

static void
nine_csmt_bind_vs_state_tx(struct pipe_context *pctx,
                           void *arg) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct queue_element* slot;

    DBG("This=%p arg=%p\n", ctx, &arg);

    slot = get_free_slot(ctx->queue, 0, NULL);
    slot->func = nine_csmt_bind_vs_state_rx;
    slot->data = arg;

    set_slot_ready(ctx->queue, slot);
}

static void
nine_csmt_bind_fs_state_rx(struct pipe_context_csmt *ctx,
                           void *data) {
    ctx->pipe->bind_fs_state(ctx->pipe, data);
}

static void
nine_csmt_bind_fs_state_tx(struct pipe_context *pctx,
                           void *arg) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct queue_element* slot;

    DBG("This=%p arg=%p\n", ctx, &arg);

    slot = get_free_slot(ctx->queue, 0, NULL);
    slot->func = nine_csmt_bind_fs_state_rx;
    slot->data = arg;

    set_slot_ready(ctx->queue, slot);
}

static void
nine_csmt_delete_fs_state_rx(struct pipe_context_csmt *ctx,
                             void *data) {
    ctx->pipe->delete_fs_state(ctx->pipe, data);
}

static void
nine_csmt_delete_fs_state_tx(struct pipe_context *pctx,
                             void *arg) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct queue_element* slot;

    DBG("This=%p arg=%p\n", ctx, &arg);

    slot = get_free_slot(ctx->queue, 0, NULL);
    slot->func = nine_csmt_delete_fs_state_rx;
    slot->data = arg;

    set_slot_ready(ctx->queue, slot);
}

static void
nine_csmt_delete_vs_state_rx(struct pipe_context_csmt *ctx,
                             void *data) {
    ctx->pipe->delete_vs_state(ctx->pipe, data);
}

static void
nine_csmt_delete_vs_state_tx(struct pipe_context *pctx,
                             void *arg) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct queue_element* slot;

    slot = get_free_slot(ctx->queue, 0, NULL);
    slot->func = nine_csmt_delete_vs_state_rx;
    slot->data = arg;

    set_slot_ready(ctx->queue, slot);
}

static void
nine_csmt_create_vertex_elements_state_rx(struct pipe_context_csmt *ctx,
                                          void *data) {
    struct csmt_create_vertex_element *arg = data;

    arg->result = ctx->pipe->create_vertex_elements_state(ctx->pipe,
            arg->num_elements,
            arg->elements);
}

static void*
nine_csmt_create_vertex_elements_state_tx(struct pipe_context *pctx,
                                          unsigned num_elements,
                                          const struct pipe_vertex_element *elements) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct csmt_create_vertex_element arg;
    struct queue_element* slot;

    DBG("This=%p num_elements=%u view=%p\n",
            ctx, num_elements, elements);
#ifdef DEBUG_CREATE_ON_SEPERATE_CONTEXT
    return ctx->pipe_creation->create_vertex_elements_state(ctx->pipe_creation, num_elements, elements);
#endif
    slot = get_free_slot(ctx->queue, 0, NULL);
    slot->func = nine_csmt_create_vertex_elements_state_rx;
    slot->data = &arg;
    arg.num_elements = num_elements;
    memcpy(&arg.elements, elements, sizeof(struct pipe_vertex_element) * num_elements);

    set_slot_ready_and_wait(ctx->queue, slot);

    return arg.result;
}

static void
nine_csmt_bind_vertex_elements_state_rx(struct pipe_context_csmt *ctx,
                                        void *data) {
    ctx->pipe->bind_vertex_elements_state(ctx->pipe, data);
}

static void
nine_csmt_bind_vertex_elements_state_tx(struct pipe_context *pctx,
                                        void *arg) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct queue_element* slot;

    DBG("This=%p arg=%p\n", ctx, &arg);

    slot = get_free_slot(ctx->queue, 0, NULL);
    slot->func = nine_csmt_bind_vertex_elements_state_rx;
    slot->data = arg;

    set_slot_ready(ctx->queue, slot);
}

static void
nine_csmt_delete_vertex_elements_state_rx(struct pipe_context_csmt *ctx,
                                          void *data) {
    ctx->pipe->delete_vertex_elements_state(ctx->pipe, data);
}

static void
nine_csmt_delete_vertex_elements_state_tx(struct pipe_context *pctx,
                                          void *arg) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct queue_element* slot;

    slot = get_free_slot(ctx->queue, 0, NULL);
    slot->func = nine_csmt_delete_vertex_elements_state_rx;
    slot->data = arg;

    set_slot_ready(ctx->queue, slot);
}

static void
nine_csmt_set_blend_color_rx(struct pipe_context_csmt *ctx,
                             void *data) {
    struct csmt_blend_color *arg = data;

    ctx->pipe->set_blend_color(ctx->pipe, &arg->color);
}

static void
nine_csmt_set_blend_color_tx(struct pipe_context *pctx,
                             const struct pipe_blend_color *color) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct queue_element* slot;
    struct csmt_blend_color *arg = NULL;

    slot = get_free_slot(ctx->queue, sizeof(struct csmt_blend_color), (void **)&arg);
    slot->func = nine_csmt_set_blend_color_rx;
    slot->data = arg;
    memcpy(&arg->color, color, sizeof(struct pipe_blend_color));

    set_slot_ready(ctx->queue, slot);
}

static void
nine_csmt_set_stencil_ref_rx(struct pipe_context_csmt *ctx,
                             void *data) {
    struct csmt_pipe_stencil_ref *arg = data;

    ctx->pipe->set_stencil_ref(ctx->pipe, &arg->ref);
}

static void
nine_csmt_set_stencil_ref_tx(struct pipe_context *pctx,
                             const struct pipe_stencil_ref *ref) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct queue_element* slot;
    struct csmt_pipe_stencil_ref *arg = NULL;

    slot = get_free_slot(ctx->queue, sizeof(struct csmt_blend_color), (void **)&arg);
    slot->func = nine_csmt_set_stencil_ref_rx;
    slot->data = arg;
    memcpy(&arg->ref, ref, sizeof(struct pipe_stencil_ref));

    set_slot_ready(ctx->queue, slot);
}

static void
nine_csmt_set_sample_mask_rx(struct pipe_context_csmt *ctx,
                             void *data) {
    struct csmt_set_sample_mask *arg = data;

    ctx->pipe->set_sample_mask(ctx->pipe, arg->sample_mask);
}

static void
nine_csmt_set_sample_mask_tx(struct pipe_context *pctx,
                             unsigned sample_mask) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct queue_element* slot;
    struct csmt_set_sample_mask *arg = NULL;

    slot = get_free_slot(ctx->queue, sizeof(struct csmt_set_sample_mask), (void **)&arg);
    slot->func = nine_csmt_set_sample_mask_rx;
    slot->data = arg;
    arg->sample_mask = sample_mask;

    set_slot_ready(ctx->queue, slot);
}

static void
nine_csmt_set_min_samples_rx(struct pipe_context_csmt *ctx,
                             void *data) {
    struct csmt_set_min_samples *arg = data;

    ctx->pipe->set_min_samples(ctx->pipe, arg->min_samples);
}

static void
nine_csmt_set_min_samples_tx(struct pipe_context *pctx,
                             unsigned min_samples) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct queue_element* slot;
    struct csmt_set_min_samples *arg = NULL;

    slot = get_free_slot(ctx->queue, sizeof(struct csmt_set_min_samples), (void **)&arg);
    slot->func = nine_csmt_set_min_samples_rx;
    slot->data = arg;
    arg->min_samples = min_samples;

    set_slot_ready(ctx->queue, slot);
}

static void
nine_csmt_set_clip_state_rx(struct pipe_context_csmt *ctx,
                            void *data) {
    struct csmt_set_clip_state *arg = data;

    ctx->pipe->set_clip_state(ctx->pipe, &arg->state);
}

static void
nine_csmt_set_clip_state_tx(struct pipe_context *pctx,
                            const struct pipe_clip_state *state) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct queue_element* slot;
    struct csmt_set_clip_state *arg = NULL;

    slot = get_free_slot(ctx->queue, sizeof(struct csmt_set_clip_state), (void **)&arg);
    slot->func = nine_csmt_set_clip_state_rx;
    slot->data = arg;
    memcpy(&arg->state, state, sizeof(struct pipe_clip_state));

    set_slot_ready(ctx->queue, slot);
}

static void
nine_csmt_set_constant_buffer_rx(struct pipe_context_csmt *ctx,
                                 void *data) {
    struct csmt_set_constant_buffer *arg = data;

    ctx->pipe->set_constant_buffer(ctx->pipe, arg->shader, arg->index, arg->buf_p);
    if (arg->buf_p) {
        pipe_resource_reference(&arg->buf.buffer, NULL);
    }
}

static void
nine_csmt_set_constant_buffer_tx(struct pipe_context *pctx,
                                 uint shader,
                                 uint index,
                                 struct pipe_constant_buffer *buf ) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct queue_element* slot;
    struct csmt_set_constant_buffer *arg = NULL;
    unsigned size = sizeof(struct csmt_set_constant_buffer);

    DBG("This=%p shader=%u index=%u buf=%p\n",
            ctx, shader, index, buf);

    /* calculate new size */
    if (buf && buf->user_buffer) {
        size += buf->buffer_size;
    }

    slot = get_free_slot(ctx->queue, size, (void **)&arg);
    slot->func = nine_csmt_set_constant_buffer_rx;
    slot->data = arg;
    arg->shader = shader;
    arg->index = index;
    if (buf) {
        arg->buf.buffer_offset = buf->buffer_offset;
        arg->buf.buffer_size = buf->buffer_size;
        arg->buf.buffer = NULL;
        if (buf->user_buffer) {
            memcpy(arg->user_buffer, buf->user_buffer, buf->buffer_size);
            arg->buf.user_buffer = &arg->user_buffer;
        } else {
            arg->buf.user_buffer = NULL;
            pipe_resource_reference(&arg->buf.buffer, buf->buffer);
        }
        arg->buf_p = &arg->buf;
    } else {
        arg->buf_p = NULL;
    }
    set_slot_ready(ctx->queue, slot);
}

static void
nine_csmt_set_framebuffer_state_rx(struct pipe_context_csmt *ctx,
                                   void *data) {
    struct csmt_set_framebuffer_state *arg = data;
    int i;

    ctx->pipe->set_framebuffer_state(ctx->pipe, &arg->state);
    for (i = 0; i < arg->state.nr_cbufs; i++) {
        arg->state.cbufs[i] = NULL;
        pipe_surface_reference(&arg->state.cbufs[i], NULL);
    }
    if (arg->state.zsbuf)
        pipe_surface_reference(&arg->state.zsbuf, NULL);
}

static void
nine_csmt_set_framebuffer_state_tx(struct pipe_context *pctx,
                                   const struct pipe_framebuffer_state *state) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct queue_element* slot;
    struct csmt_set_framebuffer_state *arg = NULL;
    int i;

    DBG("This=%p state=%p\n", ctx, state);

    slot = get_free_slot(ctx->queue, sizeof(struct csmt_set_framebuffer_state), (void **)&arg);
    slot->func = nine_csmt_set_framebuffer_state_rx;
    slot->data = arg;
    arg->state.height = state->height;
    arg->state.width = state->width;
    arg->state.nr_cbufs = state->nr_cbufs;
    for (i = 0; i < state->nr_cbufs; i++) {
        arg->state.cbufs[i] = NULL;
        pipe_surface_reference(&arg->state.cbufs[i], state->cbufs[i]);
    }
    arg->state.zsbuf = NULL;
    pipe_surface_reference(&arg->state.zsbuf, state->zsbuf);

    set_slot_ready(ctx->queue, slot);
}

static void
nine_csmt_set_polygon_stipple_rx(struct pipe_context_csmt *ctx,
                                 void *data) {
    struct csmt_set_polygon_stipple *arg = data;

    ctx->pipe->set_polygon_stipple(ctx->pipe, &arg->state);
}

static void
nine_csmt_set_polygon_stipple_tx(struct pipe_context *pctx,
                                 const struct pipe_poly_stipple *state) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct queue_element* slot;
    struct csmt_set_polygon_stipple *arg = NULL;

    slot = get_free_slot(ctx->queue, sizeof(struct csmt_set_polygon_stipple), (void **)&arg);
    slot->func = nine_csmt_set_polygon_stipple_rx;
    slot->data = arg;
    memcpy(&arg->state, state, sizeof(struct pipe_framebuffer_state));

    set_slot_ready(ctx->queue, slot);
}

static void
nine_csmt_set_scissor_states_rx(struct pipe_context_csmt *ctx,
                                void *data) {
    struct csmt_set_scissor_states *arg = data;

    ctx->pipe->set_scissor_states(ctx->pipe,
            arg->start_slot,
            arg->num_scissors,
            (struct pipe_scissor_state *)&arg->state);
}

static void
nine_csmt_set_scissor_states_tx(struct pipe_context *pctx,
                                unsigned start_slot,
                                unsigned num_scissors,
                                const struct pipe_scissor_state *state) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct queue_element* slot;
    struct csmt_set_scissor_states *arg = NULL;

    slot = get_free_slot(ctx->queue, sizeof(struct csmt_set_scissor_states), (void **)&arg);
    slot->func = nine_csmt_set_scissor_states_rx;
    slot->data = arg;
    memcpy(&arg->state, state, sizeof(struct pipe_scissor_state) * num_scissors);
    arg->start_slot = start_slot;
    arg->num_scissors = num_scissors;

    set_slot_ready(ctx->queue, slot);
}

static void
nine_csmt_set_viewport_states_rx(struct pipe_context_csmt *ctx,
                                 void *data) {
    struct csmt_set_viewport_states *arg = data;

    ctx->pipe->set_viewport_states(ctx->pipe,
            arg->start_slot,
            arg->num_viewports,
            &arg->state);
}

static void
nine_csmt_set_viewport_states_tx(struct pipe_context *pctx,
                                 unsigned start_slot,
                                 unsigned num_viewports,
                                 const struct pipe_viewport_state *view) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct queue_element* slot;
    struct csmt_set_viewport_states *arg = NULL;

    DBG("This=%p start_slot=%u num_viewports=%u view=%p\n",
            ctx, start_slot, num_viewports, view);

    slot = get_free_slot(ctx->queue, sizeof(struct csmt_set_viewport_states), (void **)&arg);
    slot->func = nine_csmt_set_viewport_states_rx;
    slot->data = arg;
    memcpy(&arg->state, view, sizeof(struct pipe_viewport_state) * num_viewports);
    arg->start_slot = start_slot;
    arg->num_viewports = num_viewports;

    set_slot_ready(ctx->queue, slot);
}

static void
nine_csmt_set_sampler_views_rx(struct pipe_context_csmt *ctx,
                               void *data) {
    struct csmt_set_sampler_views *arg = data;
    int i;

    ctx->pipe->set_sampler_views(ctx->pipe,
            arg->shader,
            arg->start_slot,
            arg->num_views,
            (struct pipe_sampler_view **)&arg->view);
    for(i = 0; i < arg->num_views; i++) {
        pipe_sampler_view_reference(&arg->view[i], NULL);
    }
}

static void
nine_csmt_set_sampler_views_tx(struct pipe_context *pctx,
                                unsigned shader,
                                unsigned start_slot,
                                unsigned num_views,
                                struct pipe_sampler_view **views) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct queue_element* slot;
    struct csmt_set_sampler_views *arg = NULL;
    int i;

    DBG("This=%p shader=%u start_slot=%u num_views=%u views=%p\n",
            ctx, shader, start_slot, num_views, views);

    slot = get_free_slot(ctx->queue, sizeof(struct csmt_set_sampler_views), (void **)&arg);
    slot->func = nine_csmt_set_sampler_views_rx;
    slot->data = arg;
    for(i = 0; i < num_views; i++) {
        arg->view[i] = NULL;
        pipe_sampler_view_reference(&arg->view[i], views[i]);
    }
    arg->start_slot = start_slot;
    arg->num_views = num_views;
    arg->shader = shader;
    set_slot_ready(ctx->queue, slot);
}

static void
nine_csmt_set_vertex_buffers_rx(struct pipe_context_csmt *ctx,
                                void *data) {
    struct csmt_set_vertex_buffers *arg = data;
    int i;

    ctx->pipe->set_vertex_buffers(ctx->pipe,
            arg->start_slot,
            arg->num_buffers,
            arg->buffer_p);
    if (arg->buffer_p) {
        ctx->state.vertex_userbuffer = !!arg->buffer_p->user_buffer;

        for (i = 0; i < arg->num_buffers; i++) {
            pipe_resource_reference(&arg->buffer[i].buffer, NULL);
        }
    } else {
        ctx->state.vertex_userbuffer = FALSE;
    }
}

static void
nine_csmt_set_vertex_buffers_tx(struct pipe_context *pctx,
                                unsigned start_slot,
                                unsigned num_buffers,
                                const struct pipe_vertex_buffer *buf) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct queue_element* slot;
    struct csmt_set_vertex_buffers *arg = NULL;
    int i;

    DBG("This=%p start_slot=%u num_buffers=%u buf=%p\n",
            ctx, start_slot, num_buffers, buf);

    slot = get_free_slot(ctx->queue, sizeof(struct csmt_set_vertex_buffers), (void **)&arg);
    slot->func = nine_csmt_set_vertex_buffers_rx;
    slot->data = arg;
    if (buf) {
        memcpy(&arg->buffer, buf, sizeof(struct pipe_vertex_buffer) * num_buffers);
        for (i = 0; i < num_buffers; i++) {
            arg->buffer[i].buffer = NULL;
            pipe_resource_reference(&arg->buffer[i].buffer, buf[i].buffer);
        }
        arg->buffer_p = &arg->buffer;
    } else {
        arg->buffer_p = NULL;
    }
    arg->start_slot = start_slot;
    arg->num_buffers = num_buffers;

    set_slot_ready(ctx->queue, slot);
}

static void
nine_csmt_set_index_buffer_rx(struct pipe_context_csmt *ctx,
                              void *data) {
    struct csmt_set_index_buffer *arg = data;

    ctx->pipe->set_index_buffer(ctx->pipe, arg->buffer_p);
    if (arg->buffer_p) {
        pipe_resource_reference(&arg->buffer.buffer, NULL);
        ctx->state.index_userbuffer = !!arg->buffer_p->user_buffer;
    } else {
        ctx->state.index_userbuffer = FALSE;
    }
}

static void
nine_csmt_set_index_buffer_tx(struct pipe_context *pctx,
                              const struct pipe_index_buffer *buf) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct queue_element* slot;
    struct csmt_set_index_buffer *arg = NULL;

    DBG("This=%p buf=%p\n", ctx, buf);

    slot = get_free_slot(ctx->queue, sizeof(struct csmt_set_index_buffer), (void **)&arg);
    slot->func = nine_csmt_set_index_buffer_rx;
    slot->data = arg;
    if (buf) {
        memcpy(&arg->buffer, buf, sizeof(struct pipe_index_buffer));
        arg->buffer.buffer = NULL;
        pipe_resource_reference(&arg->buffer.buffer, buf->buffer);
        arg->buffer_p = &arg->buffer;
    } else {
        arg->buffer_p = NULL;
    }

    set_slot_ready(ctx->queue, slot);
}

static void
nine_csmt_create_stream_output_target_rx(struct pipe_context_csmt *ctx,
                                         void *data) {
    struct csmt_create_stream_output_target *arg = data;

    arg->result = ctx->pipe->create_stream_output_target(ctx->pipe,
            arg->resource,
            arg->buffer_offset,
            arg->buffer_size);
    pipe_resource_reference(&arg->resource, NULL);
}

static struct pipe_stream_output_target*
nine_csmt_create_stream_output_target_tx(struct pipe_context *pctx,
                                         struct pipe_resource *resource,
                                         unsigned buffer_offset,
                                         unsigned buffer_size) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct csmt_create_stream_output_target arg;
    struct queue_element* slot;

#ifdef DEBUG_CREATE_ON_SEPERATE_CONTEXT
    return ctx->pipe_creation->create_stream_output_target(ctx->pipe_creation,
            resource, buffer_offset, buffer_size);
#endif
    slot = get_free_slot(ctx->queue, 0, NULL);
    slot->func = nine_csmt_create_stream_output_target_rx;
    slot->data = &arg;
    arg.resource = NULL;
    pipe_resource_reference(&arg.resource, resource);
    arg.buffer_offset = buffer_offset;
    arg.buffer_size = buffer_size;

    set_slot_ready_and_wait(ctx->queue, slot);

    return arg.result;
}

static void
nine_csmt_stream_output_target_destroy_rx(struct pipe_context_csmt *ctx,
                                          void *data) {
    ctx->pipe->stream_output_target_destroy(ctx->pipe, data);
    pipe_so_target_reference((struct pipe_stream_output_target **)&data, NULL);
}

static void
nine_csmt_stream_output_target_destroy_tx(struct pipe_context *pctx,
                                          struct pipe_stream_output_target *arg) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct queue_element* slot;

    slot = get_free_slot(ctx->queue, 0, NULL);
    slot->func = nine_csmt_stream_output_target_destroy_rx;
    slot->data = NULL;
    pipe_so_target_reference((struct pipe_stream_output_target **)&slot->data, arg);

    set_slot_ready(ctx->queue, slot);
}

static void
nine_csmt_set_stream_output_targets_rx(struct pipe_context_csmt *ctx,
                                       void *data) {
    struct csmt_stream_output_targets *arg = data;
    int i;

    ctx->pipe->set_stream_output_targets(ctx->pipe,
            arg->num_targets,
            arg->targets_p,
            arg->offsets_p);
    for (i = 0; i < arg->num_targets; i++) {
        pipe_so_target_reference(&arg->targets[i], NULL);
    }
}

static void
nine_csmt_set_stream_output_targets_tx(struct pipe_context *pctx,
                                       unsigned num_targets,
                                       struct pipe_stream_output_target **targets,
                                       const unsigned *offsets) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct queue_element* slot;
    struct csmt_stream_output_targets *arg = NULL;
    int i;

    slot = get_free_slot(ctx->queue, sizeof(struct csmt_stream_output_targets), (void **)&arg);
    slot->func = nine_csmt_set_stream_output_targets_rx;
    slot->data = arg;
    arg->num_targets = num_targets;

    if (targets) {
        for (i = 0; i < num_targets; i++) {
            arg->targets[i] = NULL;
            pipe_so_target_reference(&arg->targets[i], targets[i]);
        }
        arg->targets_p = &arg->targets;
    } else {
        arg->targets_p = NULL;
    }
    if (offsets) {
        memcpy(&arg->offsets, offsets, sizeof(void *) * num_targets);
        arg->offsets_p = (void *)&arg->offsets;
    } else {
        arg->offsets_p = NULL;
    }

    set_slot_ready(ctx->queue, slot);
}

static void
nine_csmt_resource_copy_region_rx(struct pipe_context_csmt *ctx,
                                  void *data) {
  struct csmt_resource_copy_region *arg = data;

  ctx->pipe->resource_copy_region(ctx->pipe,
        arg->dst,
        arg->dst_level,
        arg->dstx,
        arg->dsty,
        arg->dstz,
        arg->src,
        arg->src_level,
        &arg->src_box
        );
  pipe_resource_reference(&arg->dst, NULL);
  pipe_resource_reference(&arg->src, NULL);
}

static void
nine_csmt_resource_copy_region_tx(struct pipe_context *pctx,
                                  struct pipe_resource *dst,
                                  unsigned dst_level,
                                  unsigned dstx, unsigned dsty, unsigned dstz,
                                  struct pipe_resource *src,
                                  unsigned src_level,
                                  const struct pipe_box *src_box) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct queue_element* slot;
    struct csmt_resource_copy_region *arg = NULL;

    slot = get_free_slot(ctx->queue, sizeof(struct csmt_resource_copy_region), (void **)&arg);
    slot->func = nine_csmt_resource_copy_region_rx;
    slot->data = arg;
    arg->dst = NULL;
    pipe_resource_reference(&arg->dst, dst);
    arg->src = NULL;
    pipe_resource_reference(&arg->src, src);
    arg->dst_level = dst_level;
    arg->dstx = dstx;
    arg->dsty = dsty;
    arg->dstz = dstz;
    arg->src_level = src_level;
    memcpy(&arg->src_box, src_box, sizeof(struct pipe_box));

    set_slot_ready(ctx->queue, slot);
}

static void
nine_csmt_blit_rx(struct pipe_context_csmt *ctx,
                  void *data) {
    struct csmt_blit *arg = data;

    ctx->pipe->blit(ctx->pipe, &arg->info);
    pipe_resource_reference(&arg->info.dst.resource, NULL);
    pipe_resource_reference(&arg->info.src.resource, NULL);
}

static void
nine_csmt_blit_tx(struct pipe_context *pctx,
                  const struct pipe_blit_info *info) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct queue_element* slot;
    struct csmt_blit *arg = NULL;

    slot = get_free_slot(ctx->queue, sizeof(struct csmt_blit), (void **)&arg);
    slot->func = nine_csmt_blit_rx;
    slot->data = arg;
    memcpy(&arg->info, info, sizeof(struct pipe_blit_info));
    arg->info.dst.resource = NULL;
    pipe_resource_reference(&arg->info.dst.resource, info->dst.resource);
    arg->info.src.resource = NULL;
    pipe_resource_reference(&arg->info.src.resource, info->src.resource);

    set_slot_ready(ctx->queue, slot);
}

static void
nine_csmt_clear_rx(struct pipe_context_csmt *ctx,
                   void *data) {
    struct csmt_clear *arg = data;

    ctx->pipe->clear(ctx->pipe,
        arg->buffers,
        &arg->color,
        arg->depth,
        arg->stencil);
}

static void
nine_csmt_clear_tx(struct pipe_context *pctx,
                   unsigned buffers,
                   const union pipe_color_union *color,
                   double depth,
                   unsigned stencil) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct queue_element* slot;
    struct csmt_clear *arg = NULL;

    DBG("This=%p buffers=%u color=%p depth=%f stencil=%u\n",
            ctx, buffers, color, depth, stencil);

    slot = get_free_slot(ctx->queue, sizeof(struct csmt_clear), (void **)&arg);
    slot->func = nine_csmt_clear_rx;
    slot->data = arg;
    arg->buffers = buffers;
    memcpy(&arg->color, color, sizeof(union pipe_color_union));
    arg->depth = depth;
    arg->stencil = stencil;

    set_slot_ready(ctx->queue, slot);
}

static void
nine_csmt_clear_render_target_rx(struct pipe_context_csmt *ctx,
                                 void *data) {
    struct csmt_clear_rendertarget *arg = data;

    ctx->pipe->clear_render_target(ctx->pipe,
        arg->dst,
        &arg->color,
        arg->dstx,
        arg->dsty,
        arg->width,
        arg->height);
    pipe_surface_reference((struct pipe_surface **)&arg->dst, NULL);
}

static void
nine_csmt_clear_render_target_tx(struct pipe_context *pctx,
                                 struct pipe_surface *dst,
                                 const union pipe_color_union *color,
                                 unsigned dstx, unsigned dsty,
                                 unsigned width, unsigned height) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct queue_element* slot;
    struct csmt_clear_rendertarget *arg = NULL;

    DBG("This=%p dst=%p color=%p dstx=%d dsty=%d widht=%d height=%d\n",
            ctx, dst, color, dstx, dsty, width, height);

    slot = get_free_slot(ctx->queue, sizeof(struct csmt_clear_rendertarget), (void **)&arg);
    slot->func = nine_csmt_clear_render_target_rx;
    slot->data = arg;

    arg->dst = NULL;
    pipe_surface_reference((struct pipe_surface **)&arg->dst, dst);

    memcpy(&arg->color, color, sizeof(union pipe_color_union));
    arg->dstx = dstx;
    arg->dsty = dsty;
    arg->width = width;
    arg->height = height;

    set_slot_ready(ctx->queue, slot);
}

static void
nine_csmt_clear_depth_stencil_rx(struct pipe_context_csmt *ctx,
                                 void *data) {
    struct csmt_clear_depthstencil *arg = data;

    ctx->pipe->clear_depth_stencil(ctx->pipe,
            arg->dst,
            arg->clear_flags,
            arg->depth,
            arg->stencil,
            arg->dstx,
            arg->dsty,
            arg->width,
            arg->height);
    pipe_surface_reference(&arg->dst, NULL);
}

static void
nine_csmt_clear_depth_stencil_tx(struct pipe_context *pctx,
                                 struct pipe_surface *dst,
                                 unsigned clear_flags,
                                 double depth,
                                 unsigned stencil,
                                 unsigned dstx, unsigned dsty,
                                 unsigned width, unsigned height) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct queue_element* slot;
    struct csmt_clear_depthstencil *arg = NULL;

    DBG("This=%p dst=%p clear_flags=%u depth=%f stencil=%u dstx=%d dsty=%d widht=%d height=%d\n",
            ctx, dst, clear_flags, depth, stencil, dstx, dsty, width, height);

    slot = get_free_slot(ctx->queue, sizeof(struct csmt_clear_depthstencil), (void **)&arg);
    slot->func = nine_csmt_clear_depth_stencil_rx;
    slot->data = arg;
    arg->dst = NULL;
    pipe_surface_reference(&arg->dst, dst);
    arg->clear_flags = clear_flags;
    arg->depth = depth;
    arg->stencil = stencil;
    arg->dstx = dstx;
    arg->dsty = dsty;
    arg->width = width;
    arg->height = height;

    set_slot_ready(ctx->queue, slot);
}

static void
nine_csmt_flush_rx(struct pipe_context_csmt *ctx,
                   void *data) {
    struct csmt_flush *arg = data;

    ctx->pipe->flush(ctx->pipe, arg->fence, arg->flags);
}

static void
nine_csmt_flush_tx(struct pipe_context *pctx,
                   struct pipe_fence_handle **fence,
                   unsigned flags) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct csmt_flush arg;
    struct queue_element* slot;

    slot = get_free_slot(ctx->queue, 0, NULL);
    slot->func = nine_csmt_flush_rx;
    slot->data = &arg;
    arg.flags = flags;
    arg.fence = fence;

    set_slot_ready_and_wait(ctx->queue, slot);
}

static void
nine_csmt_create_sampler_view_rx(struct pipe_context_csmt *ctx,
                                 void *data) {
    struct csmt_create_sampler_view *arg = data;

    arg->result = ctx->pipe->create_sampler_view(ctx->pipe,
            arg->resource,
            arg->templat);
    pipe_resource_reference(&arg->resource, NULL);
}

static struct pipe_sampler_view*
nine_csmt_create_sampler_view_tx(struct pipe_context *pctx,
                                 struct pipe_resource *resource,
                                 const struct pipe_sampler_view *templat) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct csmt_create_sampler_view arg;
    struct queue_element* slot;

#ifdef DEBUG_CREATE_ON_SEPERATE_CONTEXT
    return ctx->pipe->create_sampler_view(ctx->pipe, resource, templat);
#endif
    slot = get_free_slot(ctx->queue, 0, NULL);
    slot->func = nine_csmt_create_sampler_view_rx;
    slot->data = &arg;
    arg.resource = NULL;
    pipe_resource_reference(&arg.resource, resource);
    arg.templat = templat;

    set_slot_ready_and_wait(ctx->queue, slot);

    return arg.result;
}

static void
nine_csmt_sampler_view_destroy_rx(struct pipe_context_csmt *ctx,
                                  void *data) {
    ctx->pipe->sampler_view_destroy(ctx->pipe, (struct pipe_sampler_view *)data);
    pipe_sampler_view_reference((struct pipe_sampler_view **)&data, NULL);
}

static void
nine_csmt_sampler_view_destroy_tx(struct pipe_context *pctx,
                                  struct pipe_sampler_view *arg) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct queue_element* slot;

    slot = get_free_slot(ctx->queue, 0, NULL);
    slot->func = nine_csmt_sampler_view_destroy_rx;
    slot->data = NULL;
    pipe_sampler_view_reference((struct pipe_sampler_view **)&slot->data, arg);

    set_slot_ready(ctx->queue, slot);
}

static void
nine_csmt_create_surface_rx(struct pipe_context_csmt *ctx,
                            void *data) {
    struct csmt_create_surface *arg = data;

    arg->result = ctx->pipe->create_surface(ctx->pipe,
            arg->resource,
            arg->templat);
    pipe_resource_reference(&arg->resource, NULL);
}

static struct pipe_surface *
nine_csmt_create_surface_tx(struct pipe_context *pctx,
                            struct pipe_resource *resource,
                            const struct pipe_surface *templat) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct csmt_create_surface arg;
    struct queue_element* slot;

    DBG("This=%p resource=%p templat=%p\n", ctx, resource, templat);
#ifdef DEBUG_CREATE_ON_SEPERATE_CONTEXT
    return ctx->pipe_creation->create_surface(ctx->pipe_creation, resource, templat);
#endif
    slot = get_free_slot(ctx->queue, 0, NULL);
    slot->func = nine_csmt_create_surface_rx;
    slot->data = &arg;
    arg.resource = NULL;
    pipe_resource_reference(&arg.resource, resource);
    arg.templat = templat;

    set_slot_ready_and_wait(ctx->queue, slot);

    return arg.result;
}

static void
nine_csmt_surface_destroy_rx(struct pipe_context_csmt *ctx,
                             void *data) {
    ctx->pipe->surface_destroy(ctx->pipe, (struct pipe_surface *)data);
    pipe_surface_reference((struct pipe_surface **)&data, NULL);
}

static void
nine_csmt_surface_destroy_tx(struct pipe_context *pctx,
                             struct pipe_surface *arg) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct queue_element* slot;

    slot = get_free_slot(ctx->queue, 0, NULL);
    slot->func = nine_csmt_surface_destroy_rx;
    slot->data = NULL;
    pipe_surface_reference((struct pipe_surface **)&slot->data, arg);

    set_slot_ready(ctx->queue, slot);
}

static void
nine_csmt_transfer_inline_write_rx(struct pipe_context_csmt *ctx,
                                   void *data) {
    struct csmt_transfer_inline_write *arg = data;

    DBG("resource=%p level=%u usage=%x box=%p data=%p stride=%d layer_stride=%d\n",
            arg->resource,
            arg->level,
            arg->usage,
            &arg->box,
            arg->data,
            arg->stride,
            arg->layer_stride);
    DBG("box: x=%d y=%d z=%d width=%d height=%d depth=%d\n",
            arg->box.x, arg->box.y, arg->box.z, arg->box.width, arg->box.height, arg->box.depth);
    ctx->pipe->transfer_inline_write(ctx->pipe,
            arg->resource,
            arg->level,
            arg->usage,
            &arg->box,
            arg->data,
            arg->stride,
            arg->layer_stride);
    DBG("done, now freeing\n");
    FREE(arg->data);
    DBG("freeing done\n");
    pipe_resource_reference(&arg->resource, NULL);
}

static void
nine_csmt_transfer_inline_write_tx(struct pipe_context *pctx,
                                   struct pipe_resource *resource,
                                   unsigned level,
                                   unsigned usage,
                                   const struct pipe_box *box,
                                   const void *data,
                                   unsigned stride,
                                   unsigned layer_stride) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct queue_element* slot;
    struct csmt_transfer_inline_write *arg = NULL;
    unsigned size;

    DBG("This=%p, resource=%p level=%u usage=%x box=%p data=%p stride=%u layer_stride=%u\n",
            ctx, resource, level, usage,
            box, data, stride, layer_stride);
    DBG("box: x=%d y=%d z=%d width=%d height=%d depth=%d\n",
            box->x, box->y, box->z, box->width, box->height, box->depth);
    DBG("resource->target=%d\n",resource->target);

    slot = get_free_slot(ctx->queue, sizeof(struct csmt_transfer_inline_write), (void **)&arg);
    slot->func = nine_csmt_transfer_inline_write_rx;
    slot->data = arg;
    arg->level = level;
    arg->box = *box;
    arg->layer_stride = layer_stride;
    arg->stride = stride;
    arg->usage = usage;
    arg->data = NULL;
    arg->resource = NULL;
    pipe_resource_reference(&arg->resource, resource);

    if (resource->target == PIPE_BUFFER) {
        arg->data = malloc(box->width);
        memcpy(arg->data, data, box->width);
    } else if (resource->target == PIPE_TEXTURE_2D) {
        size = stride * util_format_get_nblocksy(resource->format, box->height);
        arg->data = malloc(size);
        memcpy(arg->data, data, size);
    } else if (resource->target == PIPE_TEXTURE_CUBE) {
        //size = stride * util_format_get_nblocksy(resource->format, box->height) * 6;
        size = stride * util_format_get_nblocksy(resource->format, box->height) * box->depth;

        arg->data = malloc(size);
        memcpy(arg->data, data, size);
    } else if (resource->target == PIPE_TEXTURE_3D){
        size = layer_stride * box->depth;
        arg->data = malloc(size);
        memcpy(arg->data, data, size);
    } else {
        assert(0);
    }

    set_slot_ready(ctx->queue, slot);
}

static void
nine_csmt_transfer_map_rx(struct pipe_context_csmt *ctx,
                          void *data) {
    struct csmt_transfer_map *arg = data;

    arg->result = ctx->pipe->transfer_map(ctx->pipe,
            arg->resource,
            arg->level,
            arg->usage,
            arg->box,
            arg->out_transfer);
}

static void*
nine_csmt_transfer_map_tx(struct pipe_context *pctx,
                          struct pipe_resource *resource,
                          unsigned level,
                          unsigned usage,
                          const struct pipe_box *box,
                          struct pipe_transfer **out_transfer) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct csmt_transfer_map arg;
    struct queue_element* slot;
    struct csmt_transfer *transfer;
    unsigned stride, layer_stride, size;

    assert(out_transfer);

    DBG("This=%p, resource=%p, level=%u, usage=%x, box=%p, out_transfer=%p\n",
            ctx, resource, level, usage, box, out_transfer);
    DBG("box: x=%d y=%d z=%d width=%d height=%d depth=%d\n",
            box->x, box->y, box->z, box->width, box->height, box->depth);
    DBG("resource->target=%d\n",resource->target);

    transfer = CALLOC_STRUCT(csmt_transfer);
    if (!transfer)
        return NULL;

    /* return wrapper object */
    *out_transfer = (struct pipe_transfer *)transfer;

    if (!(usage & PIPE_TRANSFER_READ)) {
        /* broken */
        if (resource->target == PIPE_BUFFER) {
            size = box->width;

            pipe_resource_reference(&transfer->transfer.resource, resource);
            transfer->transfer.box = *box;
            transfer->transfer.level = level;
            transfer->transfer.usage = usage;
            transfer->transfer.layer_stride = 0;
            transfer->transfer.stride = 0;
            transfer->data = align_malloc(size, 32);

            return transfer->data;
        } else
        if (resource->target == PIPE_TEXTURE_2D) {
            stride = align(util_format_get_stride(resource->format, box->width), 4);
            size = stride * util_format_get_nblocksy(resource->format, box->height);

            pipe_resource_reference(&transfer->transfer.resource, resource);
            transfer->transfer.box = *box;
            transfer->transfer.level = level;
            transfer->transfer.usage = usage;
            transfer->transfer.layer_stride = 0;
            transfer->transfer.stride = stride;
            transfer->data = align_malloc(size, 32);

            return transfer->data;
        } else if (resource->target == PIPE_TEXTURE_3D) {
            stride = align(util_format_get_stride(resource->format, box->width), 4);
            layer_stride = stride * util_format_get_nblocksy(resource->format, box->height);
            size = stride * layer_stride * box->depth;

            pipe_resource_reference(&transfer->transfer.resource, resource);
            transfer->transfer.box = *box;
            transfer->transfer.level = level;
            transfer->transfer.usage = usage;
            transfer->transfer.layer_stride = layer_stride;
            transfer->transfer.stride = stride;
            transfer->data = align_malloc(size, 32);

            return transfer->data;
        } else if (resource->target == PIPE_TEXTURE_CUBE) {
            stride = align(util_format_get_stride(resource->format, box->width), 4);
            layer_stride = stride * util_format_get_nblocksy(resource->format, box->height);
            size = stride * layer_stride * box->depth;

            pipe_resource_reference(&transfer->transfer.resource, resource);
            transfer->transfer.box = *box;
            transfer->transfer.level = level;
            transfer->transfer.usage = usage;
            transfer->transfer.layer_stride = layer_stride;
            transfer->transfer.stride = stride;
            transfer->data = align_malloc(size, 32);

            return transfer->data;
        }
    }

    /* need to wait */
    slot = get_free_slot(ctx->queue, 0, NULL);
    slot->func = nine_csmt_transfer_map_rx;
    slot->data = &arg;
    arg.resource = resource;
    arg.level = level;
    arg.usage = usage;
    arg.box = box;
    arg.out_transfer = &transfer->transfer_p;

    set_slot_ready_and_wait(ctx->queue, slot);

    if (arg.result) {
        memcpy(&transfer->transfer, transfer->transfer_p, sizeof(struct pipe_transfer));
    }
    return arg.result;
}

static void
nine_csmt_transfer_flush_region_rx(struct pipe_context_csmt *ctx,
                                   void *data) {
    struct csmt_transfer_flush_region *arg = data;

    ctx->pipe->transfer_flush_region(ctx->pipe, arg->transfer, &arg->box);
}

static void
nine_csmt_transfer_flush_region_tx(struct pipe_context *pctx,
                                   struct pipe_transfer *transfer,
                                   const struct pipe_box *box) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;

    struct queue_element* slot;
    struct csmt_transfer_flush_region *arg = NULL;

    slot = get_free_slot(ctx->queue, sizeof(struct csmt_transfer_flush_region), (void **)&arg);
    slot->func = nine_csmt_transfer_flush_region_rx;
    slot->data = arg;
    arg->transfer = transfer;
    arg->box = *box;

    set_slot_ready(ctx->queue, slot);
}

static void
nine_csmt_transfer_unmap_rx(struct pipe_context_csmt *ctx,
                            void *data) {
   ctx->pipe->transfer_unmap(ctx->pipe, (struct pipe_transfer *)data);
}

static void
nine_csmt_transfer_unmap_tx(struct pipe_context *pctx,
                            struct pipe_transfer *data) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct queue_element* slot;
    struct csmt_transfer *transfer = data;
    struct csmt_transfer_inline_write *arg = NULL;

    DBG("This=%p, arg=%p\n", ctx, data);

    if (transfer->transfer_p) {
           slot = get_free_slot(ctx->queue, 0, NULL);
           slot->func = nine_csmt_transfer_unmap_rx;
           slot->data = (void *)transfer->transfer_p;
           set_slot_ready(ctx->queue, slot);
    } else {
        slot = get_free_slot(ctx->queue, sizeof(struct csmt_transfer_inline_write), (void **)&arg);
        slot->func = nine_csmt_transfer_inline_write_rx;
        slot->data = arg;

        arg->level = transfer->transfer.level;
        arg->box = transfer->transfer.box;
        arg->data = transfer->data;
        arg->layer_stride = transfer->transfer.layer_stride;
        arg->stride = transfer->transfer.stride;
        arg->usage = transfer->transfer.usage;
        arg->resource = NULL;
        pipe_resource_reference(&arg->resource, transfer->transfer.resource);
        pipe_resource_reference(&transfer->transfer.resource, NULL);

        set_slot_ready(ctx->queue, slot);
    }
    FREE(transfer);
}

static void
nine_csmt_get_timestamp_rx(struct pipe_context_csmt *ctx,
                           void* data) {
    struct csmt_get_timestamp *arg = data;
    arg->timestamp = ctx->pipe->get_timestamp(ctx->pipe);
}

static uint64_t
nine_csmt_get_timestamp_tx(struct pipe_context *pctx) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct csmt_get_timestamp arg;
    struct queue_element* slot;
    slot = get_free_slot(ctx->queue, 0, NULL);
    slot->func = nine_csmt_get_timestamp_rx;
    slot->data = &arg;

    set_slot_ready_and_wait(ctx->queue, slot);

    return arg.timestamp;
}

static void
nine_csmt_flush_resource_rx(struct pipe_context_csmt *ctx,
                            void* data) {
    struct csmt_pipe_resource *arg = data;

    ctx->pipe->flush_resource(ctx->pipe, arg->resource);
    pipe_resource_reference(&arg->resource, NULL);
}

static void
nine_csmt_flush_resource_tx(struct pipe_context *pctx,
                            struct pipe_resource *resource) {
    struct pipe_context_csmt *ctx = (struct pipe_context_csmt *)pctx;
    struct queue_element* slot;
    struct csmt_pipe_resource *arg = NULL;

    slot = get_free_slot(ctx->queue, sizeof(struct csmt_pipe_resource), (void **)&arg);
    slot->func = nine_csmt_flush_resource_rx;
    slot->data = arg;

    arg->resource = NULL;
    pipe_resource_reference(&arg->resource, resource);

    set_slot_ready(ctx->queue, slot);
}

/* TO BE IMPLEMENTED: */
static void
nine_csmt_clear_texture_tx(struct pipe_context *pipe,
                         struct pipe_resource *res,
                         unsigned level,
                         const struct pipe_box *box,
                         const void *data) {
    /* FIXME */
}

static void
nine_csmt_clear_buffer_tx(struct pipe_context *pipe,
                          struct pipe_resource *res,
                          unsigned offset,
                          unsigned size,
                          const void *clear_value,
                          int clear_value_size) {
    /* FIXME */
}

/* dummy functions for CSO */
static void nine_csmt_bind_gs_state_dummy(struct pipe_context *pctx,
                                          void *state) {
}

static void nine_csmt_bind_tcs_state_dummy(struct pipe_context *pctx,
                                           void *state) {
}

static void nine_csmt_bind_tes_state_dummy(struct pipe_context *pctx,
                                           void *state) {
}

static int nine_csmt_worker(void *arg) {
    struct pipe_context_csmt *ctx = arg;
    struct queue_element *slot;

    DBG("csmt worker spawned\n");

    while (!ctx->terminate) {
        slot = wait_slot_ready(ctx->queue);
        /* decode */
        if (slot->func)
            slot->func(ctx, slot->data);
        else
            ERR("slot->func is NULL !\n");
        set_slot_processed(ctx->queue, slot);
    }
    nine_concurrent_queue_delete(ctx->queue);
    ctx->pipe->destroy(ctx->pipe);
#ifdef DEBUG_CREATE_ON_SEPERATE_CONTEXT
    ctx->pipe_creation->destroy(ctx->pipe_creation);
#endif
    FREE(ctx);
    DBG("csmt worker destroyed\n");
    return 0;
}

struct pipe_context *nine_csmt_context_create(struct pipe_screen *screen) {
    struct pipe_context_csmt *ctx;
    ctx = CALLOC_STRUCT(pipe_context_csmt);
    if (!ctx)
        return NULL;

    ctx->csmt.create_query = nine_csmt_create_query_tx;
    ctx->csmt.begin_query = nine_csmt_begin_query_tx;
    ctx->csmt.end_query = nine_csmt_end_query_tx;
    ctx->csmt.destroy_query = nine_csmt_destroy_query_tx;
    ctx->csmt.get_query_result = nine_csmt_get_query_result_tx;
    ctx->csmt.flush = nine_csmt_flush_tx;
    ctx->csmt.flush_resource = nine_csmt_flush_resource_tx;
    ctx->csmt.draw_vbo = nine_csmt_draw_vbo_tx;
    ctx->csmt.destroy = nine_csmt_destroy_tx;
    ctx->csmt.bind_vs_state = nine_csmt_bind_vs_state_tx;
    ctx->csmt.bind_fs_state = nine_csmt_bind_fs_state_tx;
    ctx->csmt.get_timestamp = nine_csmt_get_timestamp_tx;
    ctx->csmt.bind_blend_state = nine_csmt_bind_blend_state_tx;
    ctx->csmt.delete_blend_state = nine_csmt_delete_blend_state_tx;
    ctx->csmt.delete_vs_state = nine_csmt_delete_vs_state_tx;
    ctx->csmt.delete_fs_state = nine_csmt_delete_fs_state_tx;
    ctx->csmt.create_rasterizer_state = nine_csmt_create_rasterizer_state_tx;
    ctx->csmt.bind_rasterizer_state = nine_csmt_bind_rasterizer_state_tx;
    ctx->csmt.delete_rasterizer_state = nine_csmt_delete_rasterizer_state_tx;
    ctx->csmt.transfer_map = nine_csmt_transfer_map_tx;
    ctx->csmt.transfer_unmap = nine_csmt_transfer_unmap_tx;
    ctx->csmt.transfer_flush_region = nine_csmt_transfer_flush_region_tx;
    ctx->csmt.surface_destroy = nine_csmt_surface_destroy_tx;
    ctx->csmt.create_surface = nine_csmt_create_surface_tx;
    ctx->csmt.sampler_view_destroy = nine_csmt_sampler_view_destroy_tx;
    ctx->csmt.create_sampler_view = nine_csmt_create_sampler_view_tx;
    ctx->csmt.bind_vertex_elements_state = nine_csmt_bind_vertex_elements_state_tx;
    ctx->csmt.delete_vertex_elements_state = nine_csmt_delete_vertex_elements_state_tx;
    ctx->csmt.create_vertex_elements_state = nine_csmt_create_vertex_elements_state_tx;
    ctx->csmt.set_stencil_ref = nine_csmt_set_stencil_ref_tx;
    ctx->csmt.create_vs_state = nine_csmt_create_vs_state_tx;
    ctx->csmt.create_fs_state = nine_csmt_create_fs_state_tx;
    ctx->csmt.create_stream_output_target = nine_csmt_create_stream_output_target_tx;
    ctx->csmt.stream_output_target_destroy = nine_csmt_stream_output_target_destroy_tx;
    ctx->csmt.set_blend_color = nine_csmt_set_blend_color_tx;
    ctx->csmt.delete_sampler_state = nine_csmt_delete_sampler_state_tx;
    ctx->csmt.bind_sampler_states = nine_csmt_bind_sampler_states_tx;
    ctx->csmt.create_sampler_state = nine_csmt_create_sampler_state_tx;
    ctx->csmt.transfer_inline_write = nine_csmt_transfer_inline_write_tx;
    ctx->csmt.create_blend_state = nine_csmt_create_blend_state_tx;
    ctx->csmt.set_stream_output_targets = nine_csmt_set_stream_output_targets_tx;
    ctx->csmt.resource_copy_region = nine_csmt_resource_copy_region_tx;
    ctx->csmt.clear_depth_stencil = nine_csmt_clear_depth_stencil_tx;
    ctx->csmt.clear_render_target = nine_csmt_clear_render_target_tx;
    ctx->csmt.clear = nine_csmt_clear_tx;
    ctx->csmt.set_index_buffer = nine_csmt_set_index_buffer_tx;
    ctx->csmt.set_sample_mask = nine_csmt_set_sample_mask_tx;
    ctx->csmt.set_clip_state = nine_csmt_set_clip_state_tx;
    ctx->csmt.set_scissor_states = nine_csmt_set_scissor_states_tx;
    ctx->csmt.set_sampler_views = nine_csmt_set_sampler_views_tx;
    ctx->csmt.set_vertex_buffers = nine_csmt_set_vertex_buffers_tx;
    ctx->csmt.create_depth_stencil_alpha_state = nine_csmt_create_depth_stencil_alpha_state_tx;
    ctx->csmt.bind_depth_stencil_alpha_state = nine_csmt_bind_depth_stencil_alpha_state_tx;
    ctx->csmt.delete_depth_stencil_alpha_state = nine_csmt_delete_depth_stencil_alpha_state_tx;
    ctx->csmt.set_framebuffer_state = nine_csmt_set_framebuffer_state_tx;
    ctx->csmt.set_constant_buffer = nine_csmt_set_constant_buffer_tx;
    ctx->csmt.set_min_samples = nine_csmt_set_min_samples_tx;
    ctx->csmt.blit = nine_csmt_blit_tx;
    ctx->csmt.set_viewport_states = nine_csmt_set_viewport_states_tx;
    ctx->csmt.set_polygon_stipple = nine_csmt_set_polygon_stipple_tx;
    ctx->csmt.bind_gs_state = nine_csmt_bind_gs_state_dummy;
    ctx->csmt.bind_tcs_state = nine_csmt_bind_tcs_state_dummy;
    ctx->csmt.bind_tes_state = nine_csmt_bind_tes_state_dummy;

    ctx->queue = nine_concurrent_queue_create();
    if (!ctx->queue) {
        FREE(ctx);
        return NULL;
    }

    ctx->pipe = screen->context_create(screen, NULL, 0);
    if (!ctx->pipe) {
        nine_concurrent_queue_delete(ctx->queue);
        FREE(ctx);
        return NULL;
    }

#ifdef DEBUG_CREATE_ON_SEPERATE_CONTEXT
    ctx->pipe_creation = screen->context_create(screen, NULL, 0);
    if (!ctx->pipe_creation) {
        ctx->pipe->destroy(ctx->pipe);
        nine_concurrent_queue_delete(ctx->queue);
        FREE(ctx);
        return NULL;
    }
#endif

#ifndef DEBUG_SINGLETHREAD
    ctx->render_thread = pipe_thread_create(nine_csmt_worker, ctx);
    if (!ctx->render_thread) {
        ctx->pipe->destroy(ctx->pipe);
        nine_concurrent_queue_delete(ctx->queue);
        FREE(ctx);
        return NULL;
    }
#else
    gctx = ctx;
#endif

    ctx->csmt.screen = screen;
    ctx->csmt.priv = ctx->pipe->priv;
    ctx->csmt.draw = ctx->pipe->draw;

    DBG("Returning context %p\n", ctx);

    return (struct pipe_context *)ctx;
}
