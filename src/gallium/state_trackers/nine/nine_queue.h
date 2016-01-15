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

struct concurrent_queue;

struct pipe_context_csmt;

struct queue_element {
	void (*func)(struct pipe_context_csmt*, void*);
	void *data;
	unsigned *processed;
	void *mem;
	unsigned pool_size;
};

struct queue_element* get_free_slot(struct concurrent_queue* ctx, unsigned memory_size, void **mem);
struct queue_element* wait_slot_ready(struct concurrent_queue* ctx);
void set_slot_ready(struct concurrent_queue* ctx, struct queue_element* element);
void set_slot_processed(struct concurrent_queue* ctx, struct queue_element* element);
void set_slot_ready_and_wait(struct concurrent_queue* ctx, struct queue_element* element);
void signal_process_start(struct concurrent_queue* ctx);

struct concurrent_queue *nine_concurrent_queue_create(void);
void nine_concurrent_queue_delete(struct concurrent_queue *ctx);
