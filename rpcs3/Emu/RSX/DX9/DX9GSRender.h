#pragma once

#ifdef HAVE_D3D9

#include <d3d9.h>
#include <deque>
#include <unordered_map>
#include <vector>

#include "Emu/RSX/GSRender.h"
#include "DX9DMA.h"
#include "DX9FragmentProgram.h"
#include "DX9Present.h"
#include "DX9RenderTargets.h"
#include "DX9ShaderCache.h"
#include "DX9TextureCache.h"
#include "DX9VertexProgram.h"

class DX9GSRender : public GSRender, public ::rsx::reports::ZCULL_control
{
	IDirect3D9Ex* m_d3d = nullptr;
	IDirect3DDevice9Ex* m_device = nullptr;
	D3DPRESENT_PARAMETERS m_pp{};

	rsx::dx9::present_manager m_present;
	rsx::dx9::render_targets m_render_targets;
	rsx::dx9::texture_cache m_texture_cache;
	rsx::dx9::shader_cache m_shader_cache;
	rsx::dx9::dma_manager m_dma;

	IDirect3DSurface9* m_flip_surface = nullptr;
	u32 m_flip_surface_width = 0;
	u32 m_flip_surface_height = 0;
	D3DFORMAT m_flip_surface_format = D3DFMT_UNKNOWN;

	bool m_scene_open = false;
	bool m_frame_needs_clear = true;
	u32 m_frame_draw_index = 0;
	u64 m_bootstrap_emit_count = 0;
	u64 m_last_present_emit_count = 0;

	// Fixed-function bootstrap shaders
	IDirect3DVertexShader9* m_bootstrap_vs = nullptr;
	IDirect3DPixelShader9* m_bootstrap_ps = nullptr;
	IDirect3DPixelShader9* m_ps_tex_vcolor = nullptr;
	IDirect3DPixelShader9* m_ps_tex_const = nullptr;
	IDirect3DPixelShader9* m_ps_solid_vcolor = nullptr;
	IDirect3DPixelShader9* m_ps_solid_const = nullptr;

	// Display buffer blit textures (staging in SYSTEMMEM, gpu in DEFAULT)
	IDirect3DTexture9* m_display_tex_staging = nullptr;
	IDirect3DTexture9* m_display_tex_gpu = nullptr;
	u32 m_display_tex_width = 0;
	u32 m_display_tex_height = 0;

	// Simple per-slot RSX texture cache
	enum class rsx_texture_kind : u8
	{
		texture_2d,
		texture_cubemap,
		texture_volume,
	};

	struct rsx_tex_entry
	{
		IDirect3DBaseTexture9* staging = nullptr; // SYSTEMMEM
		IDirect3DBaseTexture9* gpu = nullptr;     // DEFAULT
		rsx_texture_kind kind = rsx_texture_kind::texture_2d;
		u32 width = 0, height = 0, rsx_format = 0;
		u16 depth = 1;
		u8 layers = 1;
		u16 mipmaps = 1;
		u32 rsx_offset = 0;
		u8 rsx_location = 0;
		u32 rsx_address = 0;
		u32 rsx_length = 0;
		u64 upload_frame = 0;
		u64 content_hash = 0;
	};

	rsx_tex_entry m_rsx_tex[16]{};
	u64 m_frame_number = 0;

	// Lightweight tracking of bound color surfaces
	struct dx9_surface_info
	{
		u32 address = 0;
		u32 pitch = 0;
		u32 width = 0;
		u32 height = 0;
		rsx::surface_color_format color_format{};
		u8 bpp = 0;
		u8 samples = 1;
	};
	dx9_surface_info m_surface_info[4]{};

	// Frame context tracking
	struct frame_context
	{
		u64 sequence_id = 0;
		u64 begin_present_count = 0;
		u64 begin_emit_count = 0;
		u64 emit_count = 0;
		u64 end_present_count = 0;
		bool presented = false;
		bool present_success = false;
		std::vector<IUnknown*> deferred_releases;
	};

	frame_context m_active_frame{};
	std::deque<frame_context> m_frame_context_history;
	bool m_has_active_frame = false;
	u64 m_frame_context_counter = 0;
	static constexpr u32 max_frame_context_history = 8;

	void release_rsx_textures();
	void unbind_all_fragment_textures();
	bool invalidate_rsx_textures_in_range(const utils::address_range32& range);
	void invalidate_all_texture_state();
	void begin_frame_context();
	void end_frame_context();
	void resolve_presented_frame(bool present_success);
	void enqueue_deferred_release(IUnknown* object);
	void clear_frame_context_history();
	void retire_frame_contexts();

	// Vertex program management
	std::unordered_map<u64, std::unique_ptr<DX9VertexProgram>> m_vp_cache;
	DX9VertexProgram* m_current_vp = nullptr;
	IDirect3DVertexDeclaration9* m_current_vdecl = nullptr;
	std::unordered_map<u32, IDirect3DVertexDeclaration9*> m_vdecl_cache;

	DX9VertexProgram* load_vertex_program();
	IDirect3DVertexDeclaration9* get_vertex_declaration(u32 active_attr_mask);
	void upload_vertex_constants(const DX9VertexProgram* vp);
	void upload_fragment_constants(const DX9FragmentProgram* fp);
	void release_vertex_programs();
	void release_vertex_declarations();

	// Fragment program management
	std::unordered_map<u64, std::unique_ptr<DX9FragmentProgram>> m_fp_cache;
	DX9FragmentProgram* m_current_fp = nullptr;
	DX9FragmentProgram* load_fragment_program();
	void release_fragment_programs();

	// Occlusion queries
	std::unordered_map<const rsx::reports::occlusion_query_info*, IDirect3DQuery9*> m_occlusion_queries;

	// Device lifecycle
	bool create_device();
	void destroy_device();
	bool reset_device();

	// Render state
	void configure_render_state();
	void apply_dynamic_state();

	// Display blit helpers
	bool ensure_flip_surface(u32 width, u32 height, D3DFORMAT fmt);
	bool ensure_display_texture(u32 width, u32 height);
	bool blit_framebuffer_surface_memory();
	bool blit_current_render_target();
	bool blit_display_buffer(const rsx::display_flip_info_t& info);

	void release_occlusion_queries();

public:
	u64 get_cycles() final;

	DX9GSRender(utils::serial* ar) noexcept;
	DX9GSRender() noexcept : DX9GSRender(nullptr) {}
	~DX9GSRender() override;

protected:
	void on_init_thread() override;
	void on_exit() override;
	void clear_surface(u32 arg) override;
	void begin() override;
	void end() override;
	void emit_geometry(u32 sub_index) override;
	void flip(const rsx::display_flip_info_t& info) override;
	void do_local_task(rsx::FIFO::state state) override;
	bool on_access_violation(u32 address, bool is_writing) override;
	void on_invalidate_memory_range(const utils::address_range32& range, rsx::invalidation_cause cause) override;
	void notify_tile_unbound(u32 tile) override;
	void on_semaphore_acquire_wait() override;
	void write_barrier(u32 address, u32 range) override;
	bool scaled_image_from_memory(const rsx::blit_src_info& src_info, const rsx::blit_dst_info& dst_info, bool interpolate) override;
	bool release_GCM_label(u32 type, u32 address, u32 value) override;
	bool is_current_program_interpreted() const override;
	void begin_occlusion_query(rsx::reports::occlusion_query_info* query) override;
	void end_occlusion_query(rsx::reports::occlusion_query_info* query) override;
	bool check_occlusion_query_status(rsx::reports::occlusion_query_info* query) override;
	void get_occlusion_query_result(rsx::reports::occlusion_query_info* query) override;
	void discard_occlusion_query(rsx::reports::occlusion_query_info* query) override;
};

#endif
