
#include "nine_pipe.h"
#include "nine_state.h"

struct csmt_pipe_resource {
	struct pipe_resource *resource;
};

struct csmt_draw_vbo {
	struct pipe_draw_info info;
};

struct csmt_create_query {
    unsigned query_type;
    unsigned index;
    struct pipe_query *query;
};

struct csmt_query_result {
	struct pipe_query *query;
	boolean wait;
	union pipe_query_result *result;
	boolean result_ready;
};

struct csmt_flush {
    struct pipe_fence_handle **fence;
    unsigned flags;
};

struct csmt_get_timestamp {
	uint64_t timestamp;
};

struct csmt_create_sampler_state {
	struct pipe_sampler_state state;
	void *result;
};

struct csmt_pipe_depth_stencil_alpha_state {
	struct pipe_depth_stencil_alpha_state state;
	void *result;
};

struct csmt_create_blend_state {
	struct pipe_blend_state state;
	void *result;
};

struct csmt_bind_sampler_state {
    unsigned shader;
    unsigned start_slot;
    unsigned num_samplers;
    void *samplers_p;
    void *samplers[PIPE_MAX_SAMPLERS];
};

struct csmt_create_rasterizer_state {
	struct pipe_rasterizer_state state;
	void *result;
};

struct csmt_create_shader_state {
	struct pipe_shader_state state;
	void *result;
};

struct csmt_create_vertex_element {
	unsigned num_elements;
	struct pipe_vertex_element elements[PIPE_MAX_ATTRIBS];
	void *result;
};

struct csmt_blend_color {
	struct pipe_blend_color color;
};

struct csmt_pipe_stencil_ref {
	struct pipe_stencil_ref ref;
};

struct csmt_create_stream_output_target {
	struct pipe_resource *resource;
	unsigned buffer_offset;
	unsigned buffer_size;
	void *result;
};

struct csmt_create_sampler_view {
	struct pipe_resource *resource;
	struct pipe_sampler_view *templat;
	void *result;
};

struct csmt_create_surface {
	struct pipe_resource *resource;
	const struct pipe_surface *templat;
	void *result;
};

struct csmt_transfer_map {
	struct pipe_resource *resource;
	unsigned level;
	unsigned usage;
	const struct pipe_box *box;
	struct pipe_transfer **out_transfer;
	void *result;
};

struct csmt_transfer {
	struct pipe_transfer transfer;
	struct pipe_transfer *transfer_p;
	void *data;
};

struct csmt_transfer_flush_region {
	struct pipe_transfer *transfer;
	struct pipe_box box;
};

struct csmt_clear_rendertarget {
	struct pipe_surface *dst;
	union pipe_color_union color;
	unsigned dstx;
	unsigned dsty;
	unsigned width;
	unsigned height;
};

struct csmt_clear_depthstencil {
	struct pipe_surface *dst;
    unsigned clear_flags;
    double depth;
    unsigned stencil;
	unsigned dstx;
	unsigned dsty;
	unsigned width;
	unsigned height;
};

struct csmt_clear {
	unsigned buffers;
	union pipe_color_union color;
	double depth;
	unsigned stencil;
};

struct csmt_blit {
	struct pipe_blit_info info;
};

struct csmt_resource_copy_region {
	struct pipe_resource *dst;
	unsigned dst_level;
	unsigned dstx;
	unsigned dsty;
	unsigned dstz;
	struct pipe_resource *src;
	unsigned src_level;
	struct pipe_box src_box;
};

struct csmt_stream_output_targets {
	unsigned num_targets;
	struct pipe_stream_output_target *targets[PIPE_MAX_SO_BUFFERS];
	unsigned offsets[PIPE_MAX_SO_BUFFERS];
	struct pipe_stream_output_target **targets_p;
	unsigned *offsets_p;
};

struct csmt_transfer_inline_write {
	struct pipe_resource *resource;
	unsigned level;
	unsigned usage;
	struct pipe_box box;
	void *data;
	unsigned stride;
	unsigned layer_stride;
};

struct csmt_set_sample_mask {
	unsigned sample_mask;
};

struct csmt_set_min_samples {
	unsigned min_samples;
};

struct csmt_set_clip_state {
	struct pipe_clip_state state;
};

struct csmt_set_constant_buffer {
	unsigned shader;
	unsigned index;
	struct pipe_constant_buffer buf;
	struct pipe_constant_buffer *buf_p;
	unsigned user_buffer_size;
	/* should be NINE_MAX_CONST_ALL * 4 */
	unsigned user_buffer[1];
};

struct csmt_set_framebuffer_state {
	struct pipe_framebuffer_state state;
};

struct csmt_set_polygon_stipple {
	struct pipe_poly_stipple state;
};

struct csmt_set_scissor_states {
	unsigned start_slot;
	unsigned num_scissors;
	struct pipe_scissor_state state[PIPE_MAX_SO_BUFFERS];
};

struct csmt_set_viewport_states {
	unsigned start_slot;
	unsigned num_viewports;
	struct pipe_viewport_state state;
};

struct csmt_set_sampler_views {
	unsigned start_slot;
	unsigned num_views;
	unsigned shader;
	struct pipe_sampler_view *view[PIPE_MAX_SHADER_SAMPLER_VIEWS];
};

struct csmt_set_vertex_buffers {
	unsigned start_slot;
	unsigned num_buffers;
	struct pipe_vertex_buffer buffer[PIPE_MAX_SO_BUFFERS];
	struct pipe_vertex_buffer *buffer_p;
};


struct csmt_set_index_buffer {
	struct pipe_index_buffer buffer;
	struct pipe_index_buffer *buffer_p;
};

