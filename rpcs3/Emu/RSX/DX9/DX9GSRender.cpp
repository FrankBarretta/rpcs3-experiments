#include "stdafx.h"

#ifdef HAVE_D3D9

#include "DX9GSRender.h"

#include "DX9Formats.h"
#include "DX9Helpers.h"
#include "Emu/Cell/Modules/cellVideoOut.h"
#include "Emu/RSX/Common/tiled_dma_copy.hpp"
#include "Emu/RSX/Common/TextureUtils.h"
#include "Emu/RSX/Host/MM.h"
#include "Emu/RSX/Program/ProgramStateCache.h"
#include "Emu/RSX/RSXThread.h"
#include "Emu/RSX/rsx_methods.h"
#include "Emu/RSX/rsx_utils.h"

LOG_CHANNEL(dx9_log, "DX9");

namespace
{
	constexpr bool g_dx9_force_direct_backbuffer_bootstrap = true;
	constexpr bool g_dx9_force_prepresent_overlay = false;
	constexpr bool g_dx9_texture_debug = false;

	enum class display_decode_mode : u8
	{
		swap_x8r8g8b8,
		swap_x8b8g8r8,
		native_x8r8g8b8,
		native_x8b8g8r8
	};

	u32 convert_display_pixel_to_d3d9_argb(u32 src_word, display_decode_mode mode)
	{
		const bool do_swap = (mode == display_decode_mode::swap_x8r8g8b8 || mode == display_decode_mode::swap_x8b8g8r8);
		const bool is_xbgr = (mode == display_decode_mode::swap_x8b8g8r8 || mode == display_decode_mode::native_x8b8g8r8);
		const u32 native = do_swap ? _byteswap_ulong(src_word) : src_word;

		if (!is_xbgr)
		{
			return 0xFF000000u | (native & 0x00FFFFFFu);
		}

		const u32 r = (native & 0x000000FFu) << 16;
		const u32 g = (native & 0x0000FF00u);
		const u32 b = (native & 0x00FF0000u) >> 16;
		return 0xFF000000u | r | g | b;
	}

	u32 colorfulness_score(u32 argb)
	{
		const s32 r = static_cast<s32>((argb >> 16) & 0xFFu);
		const s32 g = static_cast<s32>((argb >> 8) & 0xFFu);
		const s32 b = static_cast<s32>(argb & 0xFFu);
		return static_cast<u32>(std::abs(r - g) + std::abs(g - b) + std::abs(b - r));
	}

	display_decode_mode choose_display_decode_mode(const u8* src, u32 width, u32 height, u32 pitch, u32 av_out_format)
	{
		const bool is_xbgr = (av_out_format == CELL_VIDEO_OUT_BUFFER_COLOR_FORMAT_X8B8G8R8);
		const auto mode_a = is_xbgr ? display_decode_mode::swap_x8b8g8r8 : display_decode_mode::swap_x8r8g8b8;
		const auto mode_b = is_xbgr ? display_decode_mode::native_x8b8g8r8 : display_decode_mode::native_x8r8g8b8;

		if (!src || !width || !height || pitch < 4u)
		{
			return mode_a;
		}

		const u32 step_x = std::max<u32>(1u, width / 8u);
		const u32 step_y = std::max<u32>(1u, height / 8u);
		u64 score_a = 0;
		u64 score_b = 0;
		u32 samples = 0;

		for (u32 y = 0; y < height; y += step_y)
		{
			const auto* row = reinterpret_cast<const u32*>(src + static_cast<usz>(y) * pitch);
			for (u32 x = 0; x < width; x += step_x)
			{
				score_a += colorfulness_score(convert_display_pixel_to_d3d9_argb(row[x], mode_a));
				score_b += colorfulness_score(convert_display_pixel_to_d3d9_argb(row[x], mode_b));
				++samples;
			}
		}

		if (!samples)
		{
			return mode_a;
		}

		return (score_b > (score_a + (score_a / 8u) + 32u)) ? mode_b : mode_a;
	}

	D3DCMPFUNC to_d3d_cmp(rsx::comparison_function f)
	{
		switch (f)
		{
		case rsx::comparison_function::never: return D3DCMP_NEVER;
		case rsx::comparison_function::less: return D3DCMP_LESS;
		case rsx::comparison_function::equal: return D3DCMP_EQUAL;
		case rsx::comparison_function::less_or_equal: return D3DCMP_LESSEQUAL;
		case rsx::comparison_function::greater: return D3DCMP_GREATER;
		case rsx::comparison_function::not_equal: return D3DCMP_NOTEQUAL;
		case rsx::comparison_function::greater_or_equal: return D3DCMP_GREATEREQUAL;
		case rsx::comparison_function::always: return D3DCMP_ALWAYS;
		default: return D3DCMP_ALWAYS;
		}
	}

	D3DSTENCILOP to_d3d_stencil(rsx::stencil_op op)
	{
		switch (op)
		{
		case rsx::stencil_op::keep: return D3DSTENCILOP_KEEP;
		case rsx::stencil_op::zero: return D3DSTENCILOP_ZERO;
		case rsx::stencil_op::replace: return D3DSTENCILOP_REPLACE;
		case rsx::stencil_op::incr: return D3DSTENCILOP_INCRSAT;
		case rsx::stencil_op::decr: return D3DSTENCILOP_DECRSAT;
		case rsx::stencil_op::invert: return D3DSTENCILOP_INVERT;
		case rsx::stencil_op::incr_wrap: return D3DSTENCILOP_INCR;
		case rsx::stencil_op::decr_wrap: return D3DSTENCILOP_DECR;
		default: return D3DSTENCILOP_KEEP;
		}
	}

	D3DBLEND to_d3d_blend(rsx::blend_factor f)
	{
		switch (f)
		{
		case rsx::blend_factor::zero: return D3DBLEND_ZERO;
		case rsx::blend_factor::one: return D3DBLEND_ONE;
		case rsx::blend_factor::src_color: return D3DBLEND_SRCCOLOR;
		case rsx::blend_factor::one_minus_src_color: return D3DBLEND_INVSRCCOLOR;
		case rsx::blend_factor::dst_color: return D3DBLEND_DESTCOLOR;
		case rsx::blend_factor::one_minus_dst_color: return D3DBLEND_INVDESTCOLOR;
		case rsx::blend_factor::src_alpha: return D3DBLEND_SRCALPHA;
		case rsx::blend_factor::one_minus_src_alpha: return D3DBLEND_INVSRCALPHA;
		case rsx::blend_factor::dst_alpha: return D3DBLEND_DESTALPHA;
		case rsx::blend_factor::one_minus_dst_alpha: return D3DBLEND_INVDESTALPHA;
		case rsx::blend_factor::src_alpha_saturate: return D3DBLEND_SRCALPHASAT;
		case rsx::blend_factor::constant_color: return D3DBLEND_BLENDFACTOR;
		case rsx::blend_factor::one_minus_constant_color: return D3DBLEND_INVBLENDFACTOR;
		case rsx::blend_factor::constant_alpha: return D3DBLEND_BLENDFACTOR;
		case rsx::blend_factor::one_minus_constant_alpha: return D3DBLEND_INVBLENDFACTOR;
		default: return D3DBLEND_ONE;
		}
	}

	D3DBLENDOP to_d3d_blend_op(rsx::blend_equation eq)
	{
		switch (eq)
		{
		case rsx::blend_equation::add:
		case rsx::blend_equation::add_signed:
		case rsx::blend_equation::reverse_add_signed:
			return D3DBLENDOP_ADD;
		case rsx::blend_equation::subtract:
			return D3DBLENDOP_SUBTRACT;
		case rsx::blend_equation::reverse_subtract:
		case rsx::blend_equation::reverse_subtract_signed:
			return D3DBLENDOP_REVSUBTRACT;
		case rsx::blend_equation::min:
			return D3DBLENDOP_MIN;
		case rsx::blend_equation::max:
			return D3DBLENDOP_MAX;
		default:
			return D3DBLENDOP_ADD;
		}
	}

	D3DCULL to_d3d_cull(rsx::cull_face face)
	{
		switch (face)
		{
		case rsx::cull_face::back: return D3DCULL_CCW;
		case rsx::cull_face::front: return D3DCULL_CW;
		case rsx::cull_face::front_and_back: return D3DCULL_NONE;
		default: return D3DCULL_NONE;
		}
	}

	D3DTEXTUREADDRESS to_d3d_address(rsx::texture_wrap_mode mode)
	{
		switch (mode)
		{
		case rsx::texture_wrap_mode::wrap:
			return D3DTADDRESS_WRAP;
		case rsx::texture_wrap_mode::mirror:
			return D3DTADDRESS_MIRROR;
		case rsx::texture_wrap_mode::clamp_to_edge:
		case rsx::texture_wrap_mode::clamp:
			return D3DTADDRESS_CLAMP;
		case rsx::texture_wrap_mode::border:
		case rsx::texture_wrap_mode::mirror_once_border:
			return D3DTADDRESS_BORDER;
		case rsx::texture_wrap_mode::mirror_once_clamp_to_edge:
		case rsx::texture_wrap_mode::mirror_once_clamp:
			return D3DTADDRESS_MIRRORONCE;
		default:
			return D3DTADDRESS_CLAMP;
		}
	}

	D3DTEXTUREFILTERTYPE to_d3d_min_filter(rsx::texture_minify_filter f)
	{
		switch (f)
		{
		case rsx::texture_minify_filter::nearest:
		case rsx::texture_minify_filter::nearest_nearest:
		case rsx::texture_minify_filter::nearest_linear:
			return D3DTEXF_POINT;
		case rsx::texture_minify_filter::linear:
		case rsx::texture_minify_filter::linear_nearest:
		case rsx::texture_minify_filter::linear_linear:
		case rsx::texture_minify_filter::convolution_min:
			return D3DTEXF_LINEAR;
		default:
			return D3DTEXF_LINEAR;
		}
	}

	D3DTEXTUREFILTERTYPE to_d3d_mag_filter(rsx::texture_magnify_filter f)
	{
		switch (f)
		{
		case rsx::texture_magnify_filter::nearest:
			return D3DTEXF_POINT;
		case rsx::texture_magnify_filter::linear:
		case rsx::texture_magnify_filter::convolution_mag:
			return D3DTEXF_LINEAR;
		default:
			return D3DTEXF_LINEAR;
		}
	}

	D3DTEXTUREFILTERTYPE to_d3d_mip_filter(rsx::texture_minify_filter f, u16 mipmap_count)
	{
		if (mipmap_count <= 1)
		{
			return D3DTEXF_NONE;
		}

		switch (f)
		{
		case rsx::texture_minify_filter::nearest_nearest:
		case rsx::texture_minify_filter::linear_nearest:
			return D3DTEXF_POINT;
		case rsx::texture_minify_filter::nearest_linear:
		case rsx::texture_minify_filter::linear_linear:
		case rsx::texture_minify_filter::convolution_min:
			return D3DTEXF_LINEAR;
		default:
			return D3DTEXF_NONE;
		}
	}

	AVPixelFormat to_ffmpeg_src_format(rsx::blit_engine::transfer_source_format format)
	{
		switch (format)
		{
		case rsx::blit_engine::transfer_source_format::r5g6b5:
			return AV_PIX_FMT_RGB565BE;
		case rsx::blit_engine::transfer_source_format::a1r5g5b5:
		case rsx::blit_engine::transfer_source_format::x1r5g5b5:
			return AV_PIX_FMT_RGB555BE;
		case rsx::blit_engine::transfer_source_format::a8r8g8b8:
		case rsx::blit_engine::transfer_source_format::x8r8g8b8:
			return AV_PIX_FMT_ARGB;
		case rsx::blit_engine::transfer_source_format::a8b8g8r8:
		case rsx::blit_engine::transfer_source_format::x8b8g8r8:
			return AV_PIX_FMT_ABGR;
		case rsx::blit_engine::transfer_source_format::y8:
			return AV_PIX_FMT_GRAY8;
		case rsx::blit_engine::transfer_source_format::ay8:
#ifdef AV_PIX_FMT_YA8
			return AV_PIX_FMT_YA8;
#else
			return AV_PIX_FMT_GRAY8;
#endif
		case rsx::blit_engine::transfer_source_format::yb8cr8ya8cb8:
		case rsx::blit_engine::transfer_source_format::eyb8ecr8eya8ecb8:
			return AV_PIX_FMT_YUYV422;
		case rsx::blit_engine::transfer_source_format::cr8yb8cb8ya8:
		case rsx::blit_engine::transfer_source_format::ecr8eyb8ecb8eya8:
			return AV_PIX_FMT_UYVY422;
		default:
			return AV_PIX_FMT_ARGB;
		}
	}

	AVPixelFormat to_ffmpeg_dst_format(rsx::blit_engine::transfer_destination_format format)
	{
		switch (format)
		{
		case rsx::blit_engine::transfer_destination_format::r5g6b5:
			return AV_PIX_FMT_RGB565BE;
		case rsx::blit_engine::transfer_destination_format::a8r8g8b8:
			return AV_PIX_FMT_ARGB;
		case rsx::blit_engine::transfer_destination_format::y32:
			return AV_PIX_FMT_ARGB;
		default:
			return AV_PIX_FMT_ARGB;
		}
	}

} // namespace

u64 DX9GSRender::get_cycles()
{
	return thread_ctrl::get_cycles(static_cast<named_thread<DX9GSRender>&>(*this));
}

DX9GSRender::DX9GSRender(utils::serial* ar) noexcept
	: GSRender(ar)
{
	backend_config.supports_multidraw = false;
	backend_config.supports_hw_a2c = false;
	backend_config.supports_hw_a2c_1spp = false;
	backend_config.supports_hw_renormalization = false;
	backend_config.supports_hw_msaa = true;
	backend_config.supports_hw_a2one = false;
	backend_config.supports_hw_conditional_render = false;
	backend_config.supports_passthrough_dma = false;
	backend_config.supports_asynchronous_compute = false;
	backend_config.supports_host_gpu_labels = false;
	backend_config.supports_normalized_barycentrics = false;
}

DX9GSRender::~DX9GSRender()
{
	destroy_device();
}

bool DX9GSRender::create_device()
{
	dx9_log.notice("DX9: create_device() requested");

	if (!m_frame)
	{
		dx9_log.error("DX9: cannot create device because render frame is null");
		return false;
	}

	const auto hwnd = static_cast<HWND>(m_frame->handle());
	if (!hwnd)
	{
		dx9_log.error("DX9: invalid window handle");
		return false;
	}

	if (!rsx::dx9::check(Direct3DCreate9Ex(D3D_SDK_VERSION, &m_d3d), "Direct3DCreate9Ex"))
	{
		return false;
	}

	dx9_log.notice("DX9: frame hwnd=%p client=%ux%u", hwnd, m_frame->client_width(), m_frame->client_height());

	std::memset(&m_pp, 0, sizeof(m_pp));
	m_pp.Windowed = TRUE;
	m_pp.hDeviceWindow = hwnd;
	m_pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
	m_pp.BackBufferFormat = D3DFMT_A8R8G8B8;
	m_pp.EnableAutoDepthStencil = TRUE;
	m_pp.AutoDepthStencilFormat = D3DFMT_D24S8;
	m_pp.BackBufferWidth = static_cast<UINT>(std::max(1, m_frame->client_width()));
	m_pp.BackBufferHeight = static_cast<UINT>(std::max(1, m_frame->client_height()));
	m_pp.PresentationInterval = (g_cfg.video.vsync.get() != vsync_mode::off) ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;

	dx9_log.notice("DX9: CreateDeviceEx backbuffer=%ux%u depth=%s vsync=%s",
		m_pp.BackBufferWidth, m_pp.BackBufferHeight,
		rsx::dx9::format_to_string(m_pp.AutoDepthStencilFormat),
		(g_cfg.video.vsync.get() != vsync_mode::off) ? "on" : "off");

	if (!rsx::dx9::check(m_d3d->CreateDeviceEx(
							 D3DADAPTER_DEFAULT,
							 D3DDEVTYPE_HAL,
							 hwnd,
							 D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE | D3DCREATE_MULTITHREADED,
							 &m_pp,
							 nullptr,
							 &m_device),
			"IDirect3D9Ex::CreateDeviceEx"))
	{
		destroy_device();
		return false;
	}

	{
		D3DCAPS9 caps{};
		if (SUCCEEDED(m_device->GetDeviceCaps(&caps)))
		{
			dx9_log.notice("DX9 device caps: MaxVertexShaderConst=%u, VS%u.%u, PS%u.%u",
				caps.MaxVertexShaderConst,
				D3DSHADER_VERSION_MAJOR(caps.VertexShaderVersion),
				D3DSHADER_VERSION_MINOR(caps.VertexShaderVersion),
				D3DSHADER_VERSION_MAJOR(caps.PixelShaderVersion),
				D3DSHADER_VERSION_MINOR(caps.PixelShaderVersion));
		}
	}

	m_present.bind(m_device);
	m_render_targets.bind_device(m_device);
	m_texture_cache.bind_device(m_device);
	m_shader_cache.bind_device(m_device);
	m_dma.bind_device(m_device);

	m_render_targets.ensure(m_pp.BackBufferWidth, m_pp.BackBufferHeight, rsx::dx9::to_color_format(0), rsx::dx9::to_depth_format(0));
	m_render_targets.apply();

	static constexpr auto s_passthrough_vs = R"(
struct VSIn { float4 pos : POSITION0; };
struct VSOut { float4 pos : POSITION0; };
VSOut main(VSIn i) { VSOut o; o.pos = i.pos; return o; }
)";
	static constexpr auto s_passthrough_ps = R"(
float4 main() : COLOR0 { return float4(0.0, 0.0, 0.0, 1.0); }
)";
	static constexpr auto s_tex_vcolor_ps = R"(
sampler2D s0 : register(s0);
float4 main(float4 color : COLOR0, float2 uv : TEXCOORD0) : COLOR0
{
	return tex2D(s0, uv) * color;
}
)";
	static constexpr auto s_tex_const_ps = R"(
sampler2D s0 : register(s0);
float4 c0 : register(c0);
float4 main(float2 uv : TEXCOORD0) : COLOR0
{
	return tex2D(s0, uv) * c0;
}
)";
	static constexpr auto s_solid_vcolor_ps = R"(
float4 main(float4 color : COLOR0) : COLOR0
{
	return color;
}
)";
	static constexpr auto s_solid_const_ps = R"(
float4 c0 : register(c0);
float4 main() : COLOR0
{
	return c0;
}
)";
	m_bootstrap_vs = m_shader_cache.compile_vertex("dx9.bootstrap.vs", s_passthrough_vs);
	m_bootstrap_ps = m_shader_cache.compile_pixel("dx9.bootstrap.ps", s_passthrough_ps);
	m_ps_tex_vcolor = m_shader_cache.compile_pixel("dx9.ps.tex_vcolor", s_tex_vcolor_ps);
	m_ps_tex_const = m_shader_cache.compile_pixel("dx9.ps.tex_const", s_tex_const_ps);
	m_ps_solid_vcolor = m_shader_cache.compile_pixel("dx9.ps.solid_vcolor", s_solid_vcolor_ps);
	m_ps_solid_const = m_shader_cache.compile_pixel("dx9.ps.solid_const", s_solid_const_ps);
	configure_render_state();

	dx9_log.success("DX9: initialized %ux%u", m_pp.BackBufferWidth, m_pp.BackBufferHeight);
	return true;
}

void DX9GSRender::destroy_device()
{
	m_scene_open = false;
	clear_frame_context_history();
	m_texture_cache.clear();
	m_shader_cache.clear();
	m_render_targets.release();
	release_occlusion_queries();
	if (m_flip_surface)
	{
		m_flip_surface->Release();
		m_flip_surface = nullptr;
	}
	m_flip_surface_width = 0;
	m_flip_surface_height = 0;
	m_flip_surface_format = D3DFMT_UNKNOWN;
	if (m_display_tex_staging)
	{
		m_display_tex_staging->Release();
		m_display_tex_staging = nullptr;
	}
	if (m_display_tex_gpu)
	{
		m_display_tex_gpu->Release();
		m_display_tex_gpu = nullptr;
	}
	m_display_tex_width = 0;
	m_display_tex_height = 0;
	release_rsx_textures();
	release_vertex_programs();
	release_fragment_programs();
	release_vertex_declarations();
	m_bootstrap_vs = nullptr;
	m_bootstrap_ps = nullptr;
	m_ps_tex_vcolor = nullptr;
	m_ps_tex_const = nullptr;
	m_ps_solid_vcolor = nullptr;
	m_ps_solid_const = nullptr;

	if (m_device)
	{
		m_device->Release();
		m_device = nullptr;
	}

	if (m_d3d)
	{
		m_d3d->Release();
		m_d3d = nullptr;
	}
}

bool DX9GSRender::reset_device()
{
	if (!m_device || !m_frame)
	{
		dx9_log.error("DX9: reset_device() skipped (device=%p frame=%p)", m_device, m_frame);
		return false;
	}

	m_pp.BackBufferWidth = static_cast<UINT>(std::max(1, m_frame->client_width()));
	m_pp.BackBufferHeight = static_cast<UINT>(std::max(1, m_frame->client_height()));
	m_pp.PresentationInterval = (g_cfg.video.vsync.get() != vsync_mode::off) ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;

	if (!rsx::dx9::check(m_device->ResetEx(&m_pp, nullptr), "IDirect3DDevice9Ex::ResetEx"))
	{
		return false;
	}
	m_scene_open = false;
	clear_frame_context_history();

	dx9_log.notice("DX9: reset_device() completed (%ux%u)", m_pp.BackBufferWidth, m_pp.BackBufferHeight);

	m_render_targets.release();
	m_render_targets.ensure(m_pp.BackBufferWidth, m_pp.BackBufferHeight, rsx::dx9::to_color_format(0), rsx::dx9::to_depth_format(0));
	m_render_targets.apply();
	configure_render_state();
	return true;
}

void DX9GSRender::configure_render_state()
{
	if (!m_device)
	{
		return;
	}

	m_device->SetRenderState(D3DRS_ZENABLE, TRUE);
	m_device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
	m_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	m_device->SetRenderState(D3DRS_LIGHTING, FALSE);
	m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
	m_device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
	m_device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
	m_device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
	m_device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
}

void DX9GSRender::apply_dynamic_state()
{
	if (!m_device)
	{
		return;
	}

	const auto [clip_width, clip_height] = rsx::apply_resolution_scale<true>(
		rsx::method_registers.surface_clip_width(), rsx::method_registers.surface_clip_height());

	D3DVIEWPORT9 vp{};
	vp.X = 0;
	vp.Y = 0;
	vp.Width = std::max<u32>(1, clip_width);
	vp.Height = std::max<u32>(1, clip_height);
	vp.MinZ = std::clamp(rsx::method_registers.clip_min(), 0.0f, 1.0f);
	vp.MaxZ = std::clamp(rsx::method_registers.clip_max(), 0.0f, 1.0f);
	m_device->SetViewport(&vp);

	areau scissor;
	if (get_scissor(scissor, true))
	{
		const RECT r{
			static_cast<LONG>(scissor.x1),
			static_cast<LONG>(scissor.y1),
			static_cast<LONG>(scissor.x1 + scissor.width()),
			static_cast<LONG>(scissor.y1 + scissor.height())};
		m_device->SetScissorRect(&r);
		m_device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
	}
	else
	{
		m_device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
	}

	m_device->SetRenderState(D3DRS_ZENABLE, rsx::method_registers.depth_test_enabled() ? D3DZB_TRUE : D3DZB_FALSE);
	m_device->SetRenderState(D3DRS_ZWRITEENABLE, rsx::method_registers.depth_write_enabled());
	m_device->SetRenderState(D3DRS_ZFUNC, to_d3d_cmp(rsx::method_registers.depth_func()));

	const bool stencil_enabled = rsx::method_registers.stencil_test_enabled();
	m_device->SetRenderState(D3DRS_STENCILENABLE, stencil_enabled);
	if (stencil_enabled)
	{
		m_device->SetRenderState(D3DRS_STENCILMASK, rsx::method_registers.stencil_mask());
		m_device->SetRenderState(D3DRS_STENCILWRITEMASK, rsx::method_registers.stencil_mask());
		m_device->SetRenderState(D3DRS_STENCILFUNC, to_d3d_cmp(rsx::method_registers.stencil_func()));
		m_device->SetRenderState(D3DRS_STENCILREF, rsx::method_registers.stencil_func_ref());
		m_device->SetRenderState(D3DRS_STENCILFAIL, to_d3d_stencil(rsx::method_registers.stencil_op_fail()));
		m_device->SetRenderState(D3DRS_STENCILZFAIL, to_d3d_stencil(rsx::method_registers.stencil_op_zfail()));
		m_device->SetRenderState(D3DRS_STENCILPASS, to_d3d_stencil(rsx::method_registers.stencil_op_zpass()));

		const bool two_sided = rsx::method_registers.two_sided_stencil_test_enabled();
		m_device->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE, two_sided);
		if (two_sided)
		{
			m_device->SetRenderState(D3DRS_CCW_STENCILFUNC, to_d3d_cmp(rsx::method_registers.back_stencil_func()));
			m_device->SetRenderState(D3DRS_CCW_STENCILFAIL, to_d3d_stencil(rsx::method_registers.back_stencil_op_fail()));
			m_device->SetRenderState(D3DRS_CCW_STENCILZFAIL, to_d3d_stencil(rsx::method_registers.back_stencil_op_zfail()));
			m_device->SetRenderState(D3DRS_CCW_STENCILPASS, to_d3d_stencil(rsx::method_registers.back_stencil_op_zpass()));
		}
	}

	const bool blend_enabled = rsx::method_registers.blend_enabled();
	m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, blend_enabled);
	if (blend_enabled)
	{
		m_device->SetRenderState(D3DRS_SRCBLEND, to_d3d_blend(rsx::method_registers.blend_func_sfactor_rgb()));
		m_device->SetRenderState(D3DRS_DESTBLEND, to_d3d_blend(rsx::method_registers.blend_func_dfactor_rgb()));
		m_device->SetRenderState(D3DRS_SRCBLENDALPHA, to_d3d_blend(rsx::method_registers.blend_func_sfactor_a()));
		m_device->SetRenderState(D3DRS_DESTBLENDALPHA, to_d3d_blend(rsx::method_registers.blend_func_dfactor_a()));
		m_device->SetRenderState(D3DRS_BLENDOP, to_d3d_blend_op(rsx::method_registers.blend_equation_rgb()));
		m_device->SetRenderState(D3DRS_BLENDOPALPHA, to_d3d_blend_op(rsx::method_registers.blend_equation_a()));

		const auto blend = rsx::get_constant_blend_colors();
		const DWORD factor = D3DCOLOR_COLORVALUE(blend[0], blend[1], blend[2], blend[3]);
		m_device->SetRenderState(D3DRS_BLENDFACTOR, factor);
	}

	const bool alpha_test_enabled = rsx::method_registers.alpha_test_enabled();
	m_device->SetRenderState(D3DRS_ALPHATESTENABLE, alpha_test_enabled);
	if (alpha_test_enabled)
	{
		m_device->SetRenderState(D3DRS_ALPHAFUNC, to_d3d_cmp(rsx::method_registers.alpha_func()));
		const f32 alpha_ref = std::clamp(rsx::method_registers.alpha_ref(), 0.0f, 1.0f);
		const DWORD alpha_ref_u8 = static_cast<DWORD>(alpha_ref * 255.f + 0.5f);
		m_device->SetRenderState(D3DRS_ALPHAREF, alpha_ref_u8);
	}

	if (rsx::method_registers.cull_face_enabled())
	{
		m_device->SetRenderState(D3DRS_CULLMODE, to_d3d_cull(rsx::method_registers.cull_face_mode()));
	}
	else
	{
		m_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	}

	DWORD color_mask = 0;
	if (rsx::method_registers.color_mask_r(0))
		color_mask |= D3DCOLORWRITEENABLE_RED;
	if (rsx::method_registers.color_mask_g(0))
		color_mask |= D3DCOLORWRITEENABLE_GREEN;
	if (rsx::method_registers.color_mask_b(0))
		color_mask |= D3DCOLORWRITEENABLE_BLUE;
	if (rsx::method_registers.color_mask_a(0))
		color_mask |= D3DCOLORWRITEENABLE_ALPHA;
	m_device->SetRenderState(D3DRS_COLORWRITEENABLE, color_mask ? color_mask : (D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA));
}

void DX9GSRender::release_rsx_textures()
{
	for (auto& e : m_rsx_tex)
	{
		if (e.staging)
		{
			enqueue_deferred_release(e.staging);
			e.staging = nullptr;
		}
		if (e.gpu)
		{
			enqueue_deferred_release(e.gpu);
			e.gpu = nullptr;
		}
		e = {};
	}
}

void DX9GSRender::unbind_all_fragment_textures()
{
	if (!m_device)
	{
		return;
	}

	for (u32 i = 0; i < 16; ++i)
	{
		m_device->SetTexture(i, nullptr);
	}
}

bool DX9GSRender::invalidate_rsx_textures_in_range(const utils::address_range32& range)
{
	bool invalidated = false;

	for (u32 i = 0; i < 16; ++i)
	{
		auto& entry = m_rsx_tex[i];
		if (!entry.staging && !entry.gpu)
		{
			continue;
		}

		const bool has_known_range = entry.rsx_address && entry.rsx_length;
		const bool should_invalidate = !has_known_range ||
		                               range.overlaps(utils::address_range32::start_length(entry.rsx_address, entry.rsx_length));

		if (!should_invalidate)
		{
			continue;
		}

		if (m_device)
		{
			m_device->SetTexture(i, nullptr);
		}

		if (entry.staging)
		{
			enqueue_deferred_release(entry.staging);
		}

		if (entry.gpu)
		{
			enqueue_deferred_release(entry.gpu);
		}

		entry = {};
		invalidated = true;
	}

	return invalidated;
}

void DX9GSRender::invalidate_all_texture_state()
{
	unbind_all_fragment_textures();
	m_texture_cache.clear();
	release_rsx_textures();
	m_frame_needs_clear = true;
}

void DX9GSRender::release_vertex_programs()
{
	m_current_vp = nullptr;
	m_vp_cache.clear();
}

void DX9GSRender::release_fragment_programs()
{
	m_current_fp = nullptr;
	m_fp_cache.clear();
}

void DX9GSRender::release_vertex_declarations()
{
	m_current_vdecl = nullptr;
	for (auto& [_, decl] : m_vdecl_cache)
	{
		if (decl)
			decl->Release();
	}
	m_vdecl_cache.clear();
}

DX9VertexProgram* DX9GSRender::load_vertex_program()
{
	prefetch_vertex_program();

	const auto& rsx_vp = current_vertex_program;

	if (rsx_vp.data.empty())
		return nullptr;

	u64 hash = rsx_vp.output_mask;
	for (u32 i = 0; i < rsx_vp.data.size(); i++)
	{
		hash ^= static_cast<u64>(rsx_vp.data[i]) * 0x9e3779b97f4a7c15ULL;
		hash = (hash << 13) | (hash >> 51);
	}

	auto it = m_vp_cache.find(hash);
	if (it != m_vp_cache.end())
	{
		auto* cached = it->second.get();
		if (cached->dx9_shader || cached->CreateDX9Shader(m_device))
			return cached;
		return nullptr;
	}

	auto prog = std::make_unique<DX9VertexProgram>();
	prog->Decompile(rsx_vp);

	if (!prog->Compile())
	{
		m_vp_cache[hash] = std::move(prog);
		return nullptr;
	}

	if (!prog->CreateDX9Shader(m_device))
	{
		m_vp_cache[hash] = std::move(prog);
		return nullptr;
	}

	auto* ptr = prog.get();
	m_vp_cache[hash] = std::move(prog);
	return ptr;
}

DX9FragmentProgram* DX9GSRender::load_fragment_program()
{
	if (!current_fragment_program.valid || !current_fragment_program.get_data() || !current_fragment_program.ucode_length)
	{
		return nullptr;
	}

	u64 hash = program_hash_util::fragment_program_utils::get_fragment_program_ucode_hash(current_fragment_program);
	hash ^= static_cast<u64>(current_fragment_program.ctrl) << 32;
	hash ^= static_cast<u64>(current_fp_metadata.referenced_textures_mask) << 16;

	if (auto it = m_fp_cache.find(hash); it != m_fp_cache.end())
	{
		auto* cached = it->second.get();
		if (cached->dx9_shader || cached->CreateDX9Shader(m_device))
		{
			return cached;
		}
		return nullptr;
	}

	auto prog = std::make_unique<DX9FragmentProgram>();
	prog->Decompile(current_fragment_program, current_fp_metadata.program_ucode_length);

	if (!prog->Compile())
	{
		m_fp_cache[hash] = std::move(prog);
		return nullptr;
	}

	if (!prog->CreateDX9Shader(m_device))
	{
		m_fp_cache[hash] = std::move(prog);
		return nullptr;
	}

	auto* ptr = prog.get();
	m_fp_cache[hash] = std::move(prog);
	return ptr;
}

IDirect3DVertexDeclaration9* DX9GSRender::get_vertex_declaration(u32 active_attr_mask)
{
	auto it = m_vdecl_cache.find(active_attr_mask);
	if (it != m_vdecl_cache.end())
		return it->second;

	std::vector<D3DVERTEXELEMENT9> elements;
	WORD offset = 0;
	for (u32 i = 0; i < 16; i++)
	{
		if (!(active_attr_mask & (1u << i)))
			continue;

		const auto& sem = dx9_get_vertex_semantic(i);
		D3DVERTEXELEMENT9 elem{};
		elem.Stream = 0;
		elem.Offset = offset;
		elem.Type = D3DDECLTYPE_FLOAT4;
		elem.Method = D3DDECLMETHOD_DEFAULT;
		elem.Usage = static_cast<BYTE>(sem.usage);
		elem.UsageIndex = sem.usage_index;
		elements.push_back(elem);
		offset += 16;
	}

	D3DVERTEXELEMENT9 end_elem = D3DDECL_END();
	elements.push_back(end_elem);

	IDirect3DVertexDeclaration9* decl = nullptr;
	HRESULT hr = m_device->CreateVertexDeclaration(elements.data(), &decl);
	if (FAILED(hr))
	{
		dx9_log.error("DX9: CreateVertexDeclaration failed (mask=0x%x hr=0x%08x)", active_attr_mask, static_cast<u32>(hr));
		m_vdecl_cache[active_attr_mask] = nullptr;
		return nullptr;
	}

	m_vdecl_cache[active_attr_mask] = decl;
	return decl;
}

void DX9GSRender::upload_vertex_constants(const DX9VertexProgram* vp)
{
	if (!vp || !m_device)
		return;

	const auto& tc = rsx::method_registers.transform_constants;

	if (vp->has_indexed_constants)
	{
		constexpr u32 dx9_const_count = 240;
		float buf[dx9_const_count * 4];
		for (u32 i = 0; i < dx9_const_count; i++)
		{
			std::memcpy(&buf[i * 4], tc[i], 16);
		}

		for (const auto& [slot, original_idx] : vp->high_const_relocation)
		{
			if (slot < dx9_const_count && original_idx < 512)
			{
				std::memcpy(&buf[slot * 4], tc[original_idx], 16);
			}
		}

		m_device->SetVertexShaderConstantF(0, buf, dx9_const_count);
	}
	else
	{
		const u32 count = static_cast<u32>(vp->constant_ids.size());
		if (count == 0)
			return;

		std::vector<float> buf(count * 4);
		for (u32 i = 0; i < count; i++)
		{
			const u16 rsx_id = vp->constant_ids[i];
			if (rsx_id < 512)
				std::memcpy(&buf[i * 4], tc[rsx_id], 16);
		}
		m_device->SetVertexShaderConstantF(0, buf.data(), count);
	}
}

void DX9GSRender::release_occlusion_queries()
{
	for (auto& [_, query] : m_occlusion_queries)
	{
		if (query)
		{
			enqueue_deferred_release(query);
		}
	}
	m_occlusion_queries.clear();
}

void DX9GSRender::on_init_thread()
{
	GSRender::on_init_thread();
	zcull_ctrl.reset(static_cast<::rsx::reports::ZCULL_control*>(this));

	if (!create_device())
	{
		fmt::throw_exception("DX9: failed to initialize D3D9Ex device. Check the DX9 log channel for details.");
	}
}

void DX9GSRender::on_exit()
{
	zcull_ctrl.release();
	destroy_device();
	GSRender::on_exit();
}

void DX9GSRender::clear_surface(u32 arg)
{
	if (!m_device)
	{
		return;
	}

	if (g_dx9_force_direct_backbuffer_bootstrap)
	{
		static bool s_logged_clear_skip = false;
		if (!s_logged_clear_skip)
		{
			dx9_log.notice("DX9: clear_surface skipped in direct bootstrap mode");
			s_logged_clear_skip = true;
		}
		return;
	}

	if (!rsx::method_registers.stencil_mask())
	{
		arg &= ~RSX_GCM_CLEAR_STENCIL_BIT;
	}

	if ((arg & RSX_GCM_CLEAR_ANY_MASK) == 0)
	{
		return;
	}

	DWORD flags = 0;
	if (arg & RSX_GCM_CLEAR_COLOR_RGBA_MASK)
		flags |= D3DCLEAR_TARGET;
	if (arg & RSX_GCM_CLEAR_DEPTH_BIT)
		flags |= D3DCLEAR_ZBUFFER;
	if (arg & RSX_GCM_CLEAR_STENCIL_BIT)
		flags |= D3DCLEAR_STENCIL;

	const D3DCOLOR clear_color = D3DCOLOR_ARGB(
		rsx::method_registers.clear_color_a(),
		rsx::method_registers.clear_color_r(),
		rsx::method_registers.clear_color_g(),
		rsx::method_registers.clear_color_b());

	const f32 clear_depth = static_cast<f32>(rsx::method_registers.z_clear_value(true)) / static_cast<f32>(0x00FFFFFF);
	const DWORD clear_stencil = rsx::method_registers.stencil_clear_value();
	m_device->Clear(0, nullptr, flags, clear_color, clear_depth, clear_stencil);
}

void DX9GSRender::begin_frame_context()
{
	if (m_has_active_frame)
	{
		end_frame_context();
	}

	m_active_frame = {};
	m_active_frame.sequence_id = ++m_frame_context_counter;
	m_active_frame.begin_present_count = m_frame_number;
	m_active_frame.begin_emit_count = m_bootstrap_emit_count;
	m_has_active_frame = true;
}

void DX9GSRender::end_frame_context()
{
	if (!m_has_active_frame)
	{
		return;
	}

	m_active_frame.emit_count = m_bootstrap_emit_count - m_active_frame.begin_emit_count;
	m_active_frame.end_present_count = m_frame_number;
	m_frame_context_history.emplace_back(std::move(m_active_frame));
	m_has_active_frame = false;
	m_active_frame = {};
	retire_frame_contexts();
}

void DX9GSRender::resolve_presented_frame(bool present_success)
{
	for (auto& frame : m_frame_context_history)
	{
		if (!frame.presented)
		{
			frame.presented = true;
			frame.present_success = present_success;
			frame.end_present_count = m_frame_number + 1;
			return;
		}
	}
}

void DX9GSRender::enqueue_deferred_release(IUnknown* object)
{
	if (!object)
	{
		return;
	}

	if (m_has_active_frame)
	{
		m_active_frame.deferred_releases.emplace_back(object);
		return;
	}

	object->Release();
}

void DX9GSRender::clear_frame_context_history()
{
	for (auto& object : m_active_frame.deferred_releases)
	{
		if (object)
		{
			object->Release();
		}
	}
	m_active_frame.deferred_releases.clear();
	m_active_frame = {};
	m_has_active_frame = false;

	for (auto& frame : m_frame_context_history)
	{
		for (auto& object : frame.deferred_releases)
		{
			if (object)
			{
				object->Release();
			}
		}
		frame.deferred_releases.clear();
	}
	m_frame_context_history.clear();
}

void DX9GSRender::retire_frame_contexts()
{
	while (m_frame_context_history.size() > max_frame_context_history)
	{
		auto it = m_frame_context_history.begin();
		for (; it != m_frame_context_history.end(); ++it)
		{
			if (it->presented)
			{
				break;
			}
		}

		if (it == m_frame_context_history.end())
		{
			it = m_frame_context_history.begin();
		}

		for (auto& object : it->deferred_releases)
		{
			if (object)
			{
				object->Release();
			}
		}
		m_frame_context_history.erase(it);
	}
}

void DX9GSRender::begin()
{
	if (m_device)
	{
		begin_frame_context();

		if (m_frame && (m_pp.BackBufferWidth != static_cast<UINT>(std::max(1, m_frame->client_width())) ||
						   m_pp.BackBufferHeight != static_cast<UINT>(std::max(1, m_frame->client_height()))))
		{
			reset_device();
		}

		get_framebuffer_layout(rsx::framebuffer_creation_context::context_draw, m_framebuffer_layout);
		if (m_graphics_state.test(rsx::rtt_config_valid))
		{
			const auto color_bpp = [&]() -> u8
			{
				switch (m_framebuffer_layout.color_format)
				{
				case rsx::surface_color_format::a8r8g8b8:
				case rsx::surface_color_format::a8b8g8r8:
				case rsx::surface_color_format::x8r8g8b8_o8r8g8b8:
				case rsx::surface_color_format::x8r8g8b8_z8r8g8b8:
				case rsx::surface_color_format::x8b8g8r8_o8b8g8r8:
				case rsx::surface_color_format::x8b8g8r8_z8b8g8r8:
				case rsx::surface_color_format::x32:
					return 4;
				case rsx::surface_color_format::r5g6b5:
				case rsx::surface_color_format::x1r5g5b5_o1r5g5b5:
				case rsx::surface_color_format::x1r5g5b5_z1r5g5b5:
				case rsx::surface_color_format::g8b8:
					return 2;
				case rsx::surface_color_format::b8:
					return 1;
				case rsx::surface_color_format::w16z16y16x16:
					return 8;
				case rsx::surface_color_format::w32z32y32x32:
					return 16;
				default:
					return 0;
				}
			}();

			for (u32 i = 0; i < static_cast<u32>(std::size(m_surface_info)); ++i)
			{
				if (m_framebuffer_layout.color_addresses[i] && m_framebuffer_layout.actual_color_pitch[i] && m_framebuffer_layout.width && m_framebuffer_layout.height)
				{
					m_surface_info[i].address = m_framebuffer_layout.color_addresses[i];
					m_surface_info[i].pitch = m_framebuffer_layout.actual_color_pitch[i];
					m_surface_info[i].width = m_framebuffer_layout.width;
					m_surface_info[i].height = m_framebuffer_layout.height;
					m_surface_info[i].color_format = m_framebuffer_layout.color_format;
					m_surface_info[i].bpp = color_bpp;
					m_surface_info[i].samples = rsx::get_format_sample_count(m_framebuffer_layout.aa_mode);
				}
				else
				{
					m_surface_info[i] = {};
				}
			}
		}
		else
		{
			for (auto& s : m_surface_info)
			{
				s = {};
			}
		}

		m_render_targets.apply();
		apply_dynamic_state();
		m_device->SetVertexShader(m_bootstrap_vs);
		m_device->SetPixelShader(m_bootstrap_ps);
		m_scene_open = rsx::dx9::check(m_device->BeginScene(), "IDirect3DDevice9::BeginScene");
		if (!m_scene_open)
		{
			static bool s_logged_begin_scene_fail = false;
			if (!s_logged_begin_scene_fail)
			{
				dx9_log.error("DX9: BeginScene failed; bootstrap draws will be skipped");
				s_logged_begin_scene_fail = true;
			}
		}
	}
	else
	{
		clear_frame_context_history();

		static bool s_logged_missing_device = false;
		if (!s_logged_missing_device)
		{
			dx9_log.error("DX9: begin() called without a valid device");
			s_logged_missing_device = true;
		}
	}

	rsx::thread::begin();
}

void DX9GSRender::end()
{
	auto& draw_call = rsx::method_registers.current_draw_clause;
	if (!draw_call.empty())
	{
		draw_call.begin();
		u32 subdraw = 0;
		do
		{
			emit_geometry(subdraw++);
			if (draw_call.is_trivial_instanced_draw)
			{
				draw_call.end();
			}
		} while (draw_call.next());
	}

	if (m_device)
	{
		if (m_scene_open)
		{
			rsx::dx9::check(m_device->EndScene(), "IDirect3DDevice9::EndScene");
			m_scene_open = false;
		}
	}
	end_frame_context();

	rsx::thread::end();
}

void DX9GSRender::emit_geometry(u32 sub_index)
{
	if (!m_device || !m_scene_open)
	{
		return;
	}

	auto& clause = rsx::method_registers.current_draw_clause;
	if (clause.empty() || clause.command == rsx::draw_command::inlined_array)
	{
		return;
	}

	clause.execute_pipeline_dependencies(m_ctx);

	if (g_dx9_force_direct_backbuffer_bootstrap)
	{
		IDirect3DSurface9* backbuffer = nullptr;
		if (rsx::dx9::check(m_device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer), "GetBackBuffer(emit)"))
		{
			m_device->SetRenderTarget(0, backbuffer);
			backbuffer->Release();
		}

		if (m_frame_needs_clear)
		{
			m_device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, D3DCOLOR_ARGB(255, 8, 8, 24), 1.0f, 0);
			m_frame_needs_clear = false;
			m_frame_draw_index = 0;
		}
	}

	apply_dynamic_state();

	m_device->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
	m_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	m_device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
	m_device->SetRenderState(D3DRS_ZENABLE, rsx::method_registers.depth_test_enabled() ? D3DZB_TRUE : D3DZB_FALSE);
	m_device->SetRenderState(D3DRS_ZWRITEENABLE, rsx::method_registers.depth_write_enabled());
	m_device->SetRenderState(D3DRS_ZFUNC, to_d3d_cmp(rsx::method_registers.depth_func()));
	m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
	m_device->SetRenderState(D3DRS_LIGHTING, FALSE);
	m_device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
	m_device->SetVertexShader(nullptr);
	for (u32 i = 0; i < 16; ++i)
	{
		m_device->SetTexture(i, nullptr);
	}

	D3DMATRIX identity{};
	identity._11 = identity._22 = identity._33 = identity._44 = 1.0f;
	m_device->SetTransform(D3DTS_VIEW, &identity);

	struct PosColorVertex
	{
		float x, y, z;
		DWORD color;
		float u, v;
	};

	const int clip_w = rsx::method_registers.surface_clip_width();
	const int clip_h = rsx::method_registers.surface_clip_height();
	const float half_w = (clip_w > 0) ? (clip_w / 2.f) : 1.f;
	const float half_h = (clip_h > 0) ? (clip_h / 2.f) : 1.f;

	float scale_x = rsx::method_registers.viewport_scale_x() / half_w;
	float offset_x = (rsx::method_registers.viewport_offset_x() - half_w) / half_w;

	float scale_y = rsx::method_registers.viewport_scale_y() / half_h;
	float offset_y = (rsx::method_registers.viewport_offset_y() - half_h) / half_h;
	scale_y *= -1.f;
	offset_y *= -1.f;

	const float scale_z = rsx::method_registers.viewport_scale_z();
	const float offset_z = rsx::method_registers.viewport_offset_z();

	D3DMATRIX proj_scale_offset{};
	proj_scale_offset._11 = scale_x;
	proj_scale_offset._14 = offset_x;
	proj_scale_offset._22 = scale_y;
	proj_scale_offset._24 = offset_y;
	proj_scale_offset._33 = scale_z;
	proj_scale_offset._34 = offset_z;
	proj_scale_offset._44 = 1.f;

	auto set_draw_transform = [&](const PosColorVertex*, u32)
	{
		m_device->SetTransform(D3DTS_WORLD, &identity);
		m_device->SetTransform(D3DTS_PROJECTION, &proj_scale_offset);
	};

	auto read_position = [](const u8* ptr, u32 comp) -> PosColorVertex
	{
		const u32* raw = reinterpret_cast<const u32*>(ptr);
		PosColorVertex v{};
		u32 rx = _byteswap_ulong(raw[0]);
		u32 ry = comp >= 2 ? _byteswap_ulong(raw[1]) : 0;
		u32 rz = comp >= 3 ? _byteswap_ulong(raw[2]) : 0;
		std::memcpy(&v.x, &rx, 4);
		std::memcpy(&v.y, &ry, 4);
		std::memcpy(&v.z, &rz, 4);
		v.color = 0xFFFFFFFF;
		v.u = 0.f;
		v.v = 0.f;
		return v;
	};

	auto f16_to_f32 = [](u16 raw_be) -> float
	{
		const u16 h = _byteswap_ushort(raw_be);
		const u32 raw =
			((static_cast<u32>(h) & 0x8000u) << 16) |
			(((static_cast<u32>(h) & 0x7c00u) + 0x1C000u) << 13) |
			((static_cast<u32>(h) & 0x03FFu) << 13);
		float out;
		std::memcpy(&out, &raw, 4);
		return out;
	};

	auto read_texcoord = [&](const u8* ptr, u32 comp, rsx::vertex_base_type type, float& ou, float& ov)
	{
		ou = 0.f;
		ov = 0.f;
		const u32 n = std::min<u32>(comp, 2);

		for (u32 c = 0; c < n; ++c)
		{
			float v = 0.f;
			if (type == rsx::vertex_base_type::f)
			{
				u32 raw;
				std::memcpy(&raw, ptr + c * 4, 4);
				raw = _byteswap_ulong(raw);
				std::memcpy(&v, &raw, 4);
			}
			else if (type == rsx::vertex_base_type::sf)
			{
				u16 raw;
				std::memcpy(&raw, ptr + c * 2, 2);
				v = f16_to_f32(raw);
			}
			else if (type == rsx::vertex_base_type::s1)
			{
				u16 raw;
				std::memcpy(&raw, ptr + c * 2, 2);
				const s16 sv = static_cast<s16>(_byteswap_ushort(raw));
				v = static_cast<float>(sv) / 32767.f;
			}
			else if (type == rsx::vertex_base_type::s32k)
			{
				u16 raw;
				std::memcpy(&raw, ptr + c * 2, 2);
				const s16 sv = static_cast<s16>(_byteswap_ushort(raw));
				v = static_cast<float>(sv);
			}
			else if (type == rsx::vertex_base_type::ub || type == rsx::vertex_base_type::ub256)
			{
				v = static_cast<float>(ptr[c]) / 255.f;
			}

			if (c == 0)
				ou = v;
			else
				ov = v;
		}

		if (!std::isfinite(ou) || std::abs(ou) > 1e6f)
			ou = 0.f;
		if (!std::isfinite(ov) || std::abs(ov) > 1e6f)
			ov = 0.f;
	};

	auto is_addr_safe = [](u32 addr, u32 size) -> bool
	{
		return vm::check_addr(addr, vm::page_readable, size);
	};

	auto read_color_ub = [](const u8* ptr, u32 comp) -> DWORD
	{
		const u8 r = ptr[0];
		const u8 g = comp >= 2 ? ptr[1] : 0;
		const u8 b = comp >= 3 ? ptr[2] : 0;
		const u8 a = comp >= 4 ? ptr[3] : 255;
		return D3DCOLOR_ARGB(a, r, g, b);
	};

	auto read_color_float = [](const u8* ptr, u32 comp) -> DWORD
	{
		auto rd = [](const u8* p) -> float
		{
			u32 be;
			std::memcpy(&be, p, 4);
			u32 le = _byteswap_ulong(be);
			float f;
			std::memcpy(&f, &le, 4);
			return f;
		};
		float r = rd(ptr);
		float g = comp >= 2 ? rd(ptr + 4) : 0.f;
		float b = comp >= 3 ? rd(ptr + 8) : 0.f;
		float a = comp >= 4 ? rd(ptr + 12) : 1.f;
		return D3DCOLOR_ARGB(
			static_cast<u8>(std::clamp(a, 0.f, 1.f) * 255.f),
			static_cast<u8>(std::clamp(r, 0.f, 1.f) * 255.f),
			static_cast<u8>(std::clamp(g, 0.f, 1.f) * 255.f),
			static_cast<u8>(std::clamp(b, 0.f, 1.f) * 255.f));
	};

	auto map_primitive = [](rsx::primitive_type prim, u32 count, D3DPRIMITIVETYPE& out_prim, u32& out_count, bool& out_quad) -> bool
	{
		out_quad = false;
		switch (prim)
		{
		case rsx::primitive_type::triangles:
			out_prim = D3DPT_TRIANGLELIST;
			out_count = count / 3;
			break;
		case rsx::primitive_type::triangle_strip:
			out_prim = D3DPT_TRIANGLESTRIP;
			out_count = count >= 3 ? count - 2 : 0;
			break;
		case rsx::primitive_type::triangle_fan:
			out_prim = D3DPT_TRIANGLEFAN;
			out_count = count >= 3 ? count - 2 : 0;
			break;
		case rsx::primitive_type::quads:
			out_prim = D3DPT_TRIANGLELIST;
			out_count = (count / 4) * 2;
			out_quad = true;
			break;
		case rsx::primitive_type::lines:
			out_prim = D3DPT_LINELIST;
			out_count = count / 2;
			break;
		case rsx::primitive_type::line_strip:
			out_prim = D3DPT_LINESTRIP;
			out_count = count >= 2 ? count - 1 : 0;
			break;
		case rsx::primitive_type::points:
			out_prim = D3DPT_POINTLIST;
			out_count = count;
			break;
		default: return false;
		}
		return out_count > 0;
	};

	auto expand_quads = [](const std::vector<PosColorVertex>& src, std::vector<PosColorVertex>& dst)
	{
		const u32 num_quads = static_cast<u32>(src.size()) / 4;
		dst.resize(num_quads * 6);
		for (u32 q = 0; q < num_quads; ++q)
		{
			dst[q * 6 + 0] = src[q * 4 + 0];
			dst[q * 6 + 1] = src[q * 4 + 1];
			dst[q * 6 + 2] = src[q * 4 + 2];
			dst[q * 6 + 3] = src[q * 4 + 2];
			dst[q * 6 + 4] = src[q * 4 + 3];
			dst[q * 6 + 5] = src[q * 4 + 0];
		}
	};

	// Find position attribute
	s32 pos_attr = -1;
	for (u32 i = 0; i < 16; ++i)
	{
		const auto& info = rsx::method_registers.vertex_arrays_info[i];
		if (info.size() >= 2 && info.stride() > 0 && info.type() == rsx::vertex_base_type::f)
		{
			pos_attr = static_cast<s32>(i);
			break;
		}
	}

	// Find color attribute
	s32 col_attr = -1;
	if (pos_attr >= 0)
	{
		for (u32 i = 0; i < 16; ++i)
		{
			if (static_cast<s32>(i) == pos_attr)
				continue;
			const auto& info = rsx::method_registers.vertex_arrays_info[i];
			if (info.size() >= 3 && info.stride() > 0 &&
				(info.type() == rsx::vertex_base_type::ub || info.type() == rsx::vertex_base_type::ub256))
			{
				col_attr = static_cast<s32>(i);
				break;
			}
		}
	}

	// Find texcoord attribute
	s32 tc_attr = -1;
	if (pos_attr >= 0)
	{
		for (u32 i = 0; i < 16; ++i)
		{
			if (static_cast<s32>(i) == pos_attr || static_cast<s32>(i) == col_attr)
				continue;
			const auto& info = rsx::method_registers.vertex_arrays_info[i];
			const auto type = info.type();
			const bool supported_tc_type =
				(type == rsx::vertex_base_type::f) ||
				(type == rsx::vertex_base_type::sf) ||
				(type == rsx::vertex_base_type::s1) ||
				(type == rsx::vertex_base_type::s32k) ||
				(type == rsx::vertex_base_type::ub) ||
				(type == rsx::vertex_base_type::ub256);
			if (info.size() >= 2 && info.stride() > 0 && supported_tc_type)
			{
				tc_attr = static_cast<s32>(i);
				break;
			}
		}
	}

	// Register vertex color
	DWORD constant_color = 0xFFFFFFFF;
	bool has_register_color = false;
	if (col_attr < 0)
	{
		auto be_to_f = [](u32 raw) -> float
		{
			u32 le = _byteswap_ulong(raw);
			float f;
			std::memcpy(&f, &le, 4);
			return f;
		};

		for (u32 i : {2u, 3u, 4u, 9u})
		{
			if (static_cast<s32>(i) == pos_attr)
				continue;
			const auto& ri = rsx::method_registers.register_vertex_info[i];
			if (ri.size < 3)
				continue;

			float r = 1.f, g = 1.f, b = 1.f, a = 1.f;
			if (ri.type == rsx::vertex_base_type::f)
			{
				r = be_to_f(ri.data[0]);
				g = be_to_f(ri.data[1]);
				b = be_to_f(ri.data[2]);
				a = ri.size >= 4 ? be_to_f(ri.data[3]) : 1.0f;
			}
			else if (ri.type == rsx::vertex_base_type::sf)
			{
				auto be_to_sf = [](u32 raw) -> float
				{
					u32 le = _byteswap_ulong(raw);
					s16 val = static_cast<s16>(le & 0xFFFFu);
					return std::clamp(static_cast<float>(val) / 32767.f, 0.f, 1.f);
				};
				r = be_to_sf(ri.data[0]);
				g = be_to_sf(ri.data[1]);
				b = be_to_sf(ri.data[2]);
				a = ri.size >= 4 ? be_to_sf(ri.data[3]) : 1.0f;
			}
			else if (ri.type == rsx::vertex_base_type::ub)
			{
				r = static_cast<float>(ri.data[0] & 0xFFu) / 255.f;
				g = static_cast<float>(ri.data[1] & 0xFFu) / 255.f;
				b = static_cast<float>(ri.data[2] & 0xFFu) / 255.f;
				a = ri.size >= 4 ? static_cast<float>(ri.data[3] & 0xFFu) / 255.f : 1.f;
			}
			else
			{
				continue;
			}

			const DWORD candidate = D3DCOLOR_ARGB(
				static_cast<u8>(std::clamp(a, 0.f, 1.f) * 255.f),
				static_cast<u8>(std::clamp(r, 0.f, 1.f) * 255.f),
				static_cast<u8>(std::clamp(g, 0.f, 1.f) * 255.f),
				static_cast<u8>(std::clamp(b, 0.f, 1.f) * 255.f));
			if ((candidate & 0xFF000000u) == 0)
				continue;
			constant_color = candidate;
			has_register_color = true;
			break;
		}
	}

	// Try to upload RSX fragment textures
	prefetch_fragment_program();
	const bool fp_valid = current_fragment_program.valid;
	u32 sampled_tex_unit = 0;
	if (fp_valid && current_fp_metadata.referenced_textures_mask)
	{
		for (u32 i = 0; i < 16; ++i)
		{
			if ((current_fp_metadata.referenced_textures_mask & (1u << i)) &&
				rsx::method_registers.fragment_textures[i].enabled())
			{
				sampled_tex_unit = i;
				break;
			}
		}
	}
	else if (rsx::method_registers.fragment_textures[0].enabled())
	{
		sampled_tex_unit = 0;
	}
	else
	{
		for (u32 i = 0; i < 16; ++i)
		{
			if (rsx::method_registers.fragment_textures[i].enabled())
			{
				sampled_tex_unit = i;
				break;
			}
		}
	}
	DX9FragmentProgram* fragment_prog = load_fragment_program();
	m_current_fp = fragment_prog;

	auto upload_fragment_texture = [&](u32 tex_unit) -> bool
	{
		const auto& ftex0 = rsx::method_registers.fragment_textures[tex_unit];
		if (!ftex0.enabled() || !m_device)
		{
			return false;
		}

		bool uploaded_or_cached = false;
		const u32 tex_w = ftex0.width();
		const u32 tex_h = ftex0.height();
		const u32 tex_fmt = ftex0.format();
		const u32 tex_off = ftex0.offset();
		const u8 tex_loc = ftex0.location();
		const u32 base_fmt = tex_fmt & 0x1Fu;
		const bool is_swizzled = !(tex_fmt & CELL_GCM_TEXTURE_LN);
		const u16 mipmap_count = std::max<u16>(1, ftex0.get_exact_mipmap_count());
		const auto tex_dimension = ftex0.get_extended_texture_dimension();
		const bool is_cubemap = (tex_dimension == rsx::texture_dimension_extended::texture_dimension_cubemap) || ftex0.cubemap();
		const bool is_volume = (tex_dimension == rsx::texture_dimension_extended::texture_dimension_3d) || (ftex0.depth() > 1);
		const u16 tex_depth = std::max<u16>(1, ftex0.depth());
		const u8 layer_count = is_cubemap ? 6 : 1;
		const rsx_texture_kind texture_kind = is_cubemap ? rsx_texture_kind::texture_cubemap : (is_volume ? rsx_texture_kind::texture_volume : rsx_texture_kind::texture_2d);
		auto& entry = m_rsx_tex[tex_unit];

		D3DFORMAT d3d_fmt = D3DFMT_UNKNOWN;
		u32 bpp = 0;
		u32 block_edge = 1;
		bool is_compressed = false;

		switch (base_fmt)
		{
		case CELL_GCM_TEXTURE_COMPRESSED_DXT45 & 0x1Fu:
			d3d_fmt = D3DFMT_DXT5; bpp = 16; block_edge = 4; is_compressed = true; break;
		case CELL_GCM_TEXTURE_COMPRESSED_DXT23 & 0x1Fu:
			d3d_fmt = D3DFMT_DXT3; bpp = 16; block_edge = 4; is_compressed = true; break;
		case CELL_GCM_TEXTURE_COMPRESSED_DXT1 & 0x1Fu:
			d3d_fmt = D3DFMT_DXT1; bpp = 8; block_edge = 4; is_compressed = true; break;
		case CELL_GCM_TEXTURE_A8R8G8B8 & 0x1Fu:
			d3d_fmt = D3DFMT_A8R8G8B8; bpp = 4; break;
		case CELL_GCM_TEXTURE_D8R8G8B8 & 0x1Fu:
			d3d_fmt = D3DFMT_X8R8G8B8; bpp = 4; break;
		case CELL_GCM_TEXTURE_R5G6B5 & 0x1Fu:
			d3d_fmt = D3DFMT_R5G6B5; bpp = 2; break;
		case CELL_GCM_TEXTURE_A1R5G5B5 & 0x1Fu:
			d3d_fmt = D3DFMT_A1R5G5B5; bpp = 2; break;
		case CELL_GCM_TEXTURE_A4R4G4B4 & 0x1Fu:
			d3d_fmt = D3DFMT_A4R4G4B4; bpp = 2; break;
		case CELL_GCM_TEXTURE_B8 & 0x1Fu:
			d3d_fmt = D3DFMT_L8; bpp = 1; break;
		case CELL_GCM_TEXTURE_W16_Z16_Y16_X16_FLOAT & 0x1Fu:
			d3d_fmt = D3DFMT_A16B16G16R16F; bpp = 8; break;
		case CELL_GCM_TEXTURE_W32_Z32_Y32_X32_FLOAT & 0x1Fu:
			d3d_fmt = D3DFMT_A32B32G32R32F; bpp = 16; break;
		case CELL_GCM_TEXTURE_D1R5G5B5 & 0x1Fu:
			d3d_fmt = D3DFMT_A1R5G5B5; bpp = 2; break;
		case CELL_GCM_TEXTURE_G8B8 & 0x1Fu:
			d3d_fmt = D3DFMT_V8U8; bpp = 2; break;
		case CELL_GCM_TEXTURE_X16 & 0x1Fu:
			d3d_fmt = D3DFMT_L16; bpp = 2; break;
		case CELL_GCM_TEXTURE_Y16_X16 & 0x1Fu:
			d3d_fmt = D3DFMT_G16R16; bpp = 4; break;
		case CELL_GCM_TEXTURE_Y16_X16_FLOAT & 0x1Fu:
			d3d_fmt = D3DFMT_G16R16F; bpp = 4; break;
		case CELL_GCM_TEXTURE_X32_FLOAT & 0x1Fu:
			d3d_fmt = D3DFMT_R32F; bpp = 4; break;
		case CELL_GCM_TEXTURE_COMPRESSED_HILO8 & 0x1Fu:
		case CELL_GCM_TEXTURE_COMPRESSED_HILO_S8 & 0x1Fu:
			d3d_fmt = D3DFMT_V8U8; bpp = 2; break;
		default: break;
		}

		const u32 create_w = (block_edge > 1) ? ((tex_w + block_edge - 1) & ~(block_edge - 1)) : tex_w;
		const u32 create_h = (block_edge > 1) ? ((tex_h + block_edge - 1) & ~(block_edge - 1)) : tex_h;

		if (d3d_fmt != D3DFMT_UNKNOWN && create_w > 0 && create_h > 0)
		{
			const auto subresources = rsx::get_subresources_layout(ftex0);
			if (subresources.empty())
			{
				return false;
			}
			const auto gcm_upload_format = tex_fmt & ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN);

			if (entry.width != create_w || entry.height != create_h || entry.rsx_format != tex_fmt || entry.rsx_offset != tex_off || entry.rsx_location != tex_loc || entry.mipmaps != mipmap_count || entry.kind != texture_kind || entry.depth != tex_depth || entry.layers != layer_count)
			{
				if (entry.staging) { enqueue_deferred_release(entry.staging); entry.staging = nullptr; }
				if (entry.gpu) { enqueue_deferred_release(entry.gpu); entry.gpu = nullptr; }
				entry = {};
			}

			if (!entry.staging)
			{
				HRESULT hr1 = E_FAIL;
				HRESULT hr2 = E_FAIL;

				if (texture_kind == rsx_texture_kind::texture_2d)
				{
					IDirect3DTexture9* staging = nullptr;
					IDirect3DTexture9* gpu = nullptr;
					hr1 = m_device->CreateTexture(create_w, create_h, mipmap_count, 0, d3d_fmt, D3DPOOL_SYSTEMMEM, &staging, nullptr);
					hr2 = m_device->CreateTexture(create_w, create_h, mipmap_count, 0, d3d_fmt, D3DPOOL_DEFAULT, &gpu, nullptr);
					entry.staging = staging;
					entry.gpu = gpu;
				}
				else if (texture_kind == rsx_texture_kind::texture_cubemap)
				{
					IDirect3DCubeTexture9* staging = nullptr;
					IDirect3DCubeTexture9* gpu = nullptr;
					hr1 = m_device->CreateCubeTexture(create_w, mipmap_count, 0, d3d_fmt, D3DPOOL_SYSTEMMEM, &staging, nullptr);
					hr2 = m_device->CreateCubeTexture(create_w, mipmap_count, 0, d3d_fmt, D3DPOOL_DEFAULT, &gpu, nullptr);
					entry.staging = staging;
					entry.gpu = gpu;
				}
				else
				{
					IDirect3DVolumeTexture9* staging = nullptr;
					IDirect3DVolumeTexture9* gpu = nullptr;
					hr1 = m_device->CreateVolumeTexture(create_w, create_h, tex_depth, mipmap_count, 0, d3d_fmt, D3DPOOL_SYSTEMMEM, &staging, nullptr);
					hr2 = m_device->CreateVolumeTexture(create_w, create_h, tex_depth, mipmap_count, 0, d3d_fmt, D3DPOOL_DEFAULT, &gpu, nullptr);
					entry.staging = staging;
					entry.gpu = gpu;
				}

				if (FAILED(hr1) || FAILED(hr2))
				{
					if (entry.staging) { enqueue_deferred_release(entry.staging); entry.staging = nullptr; }
					if (entry.gpu) { enqueue_deferred_release(entry.gpu); entry.gpu = nullptr; }
					static bool s_logged_tex_fail = false;
					if (!s_logged_tex_fail)
					{
						dx9_log.error("DX9: failed to create texture %ux%u mips=%u fmt=0x%x d3dfmt=%d", create_w, create_h, mipmap_count, tex_fmt, static_cast<int>(d3d_fmt));
						s_logged_tex_fail = true;
					}
				}
				else
				{
					entry.width = create_w;
					entry.height = create_h;
					entry.rsx_format = tex_fmt;
					entry.kind = texture_kind;
					entry.depth = tex_depth;
					entry.layers = layer_count;
					entry.mipmaps = mipmap_count;
					entry.rsx_offset = tex_off;
					entry.rsx_location = tex_loc;
				}
			}

			const u32 addr = rsx::get_address(tex_off, tex_loc);
			const usz storage_size = rsx::get_texture_size(ftex0);
			const u32 tex_length = static_cast<u32>(std::min<usz>(storage_size, std::numeric_limits<u32>::max()));
			const bool addr_safe = tex_length ? is_addr_safe(addr, tex_length) : false;
			entry.rsx_address = addr;
			entry.rsx_length = tex_length;

			u64 content_hash = 0;
			if (addr_safe)
			{
				for (const auto& subresource : subresources)
				{
					if (subresource.level >= mipmap_count) continue;
					if (is_cubemap && subresource.layer >= layer_count) continue;
					if (!is_cubemap && subresource.layer != 0) continue;

					const auto* bytes = reinterpret_cast<const u8*>(subresource.data.data());
					const usz sample_size = std::min<usz>(subresource.data.size(), 64);
					for (usz i = 0; i < sample_size; ++i)
					{
						content_hash = (content_hash * 131ull) ^ bytes[i];
					}
				}
			}

			const bool needs_upload = entry.staging && entry.gpu &&
			                          (entry.upload_frame == 0 || entry.content_hash != content_hash);

			if (needs_upload && addr_safe)
			{
				bool upload_valid = true;

				for (const auto& subresource : subresources)
				{
					if (subresource.level >= mipmap_count) continue;
					if (is_cubemap && subresource.layer >= layer_count) continue;
					if (!is_cubemap && subresource.layer != 0) continue;

					const auto* src_pixels = reinterpret_cast<const u8*>(subresource.data.data());
					if (!src_pixels) { upload_valid = false; break; }

					const u32 mip_w = std::max<u32>(subresource.width_in_texel, 1);
					const u32 mip_h = std::max<u32>(subresource.height_in_texel, 1);
					const u32 bw = std::max<u32>(subresource.width_in_block, 1);
					const u32 bh = std::max<u32>(subresource.height_in_block, 1);
					const u32 mip_d = std::max<u32>(subresource.depth, 1);
					const u32 row_bytes = bw * bpp;
					const u32 src_pitch = subresource.pitch_in_block * bpp;
					const u32 src_slice = src_pitch * bh;

					u8* dst_bits = nullptr;
					u32 dst_pitch = 0;
					u32 dst_slice = 0;
					bool locked_ok = false;

					D3DLOCKED_RECT locked{};
					D3DLOCKED_BOX locked_box{};
					if (texture_kind == rsx_texture_kind::texture_2d)
					{
						auto* tex2d = static_cast<IDirect3DTexture9*>(entry.staging);
						locked_ok = SUCCEEDED(tex2d->LockRect(subresource.level, &locked, nullptr, 0));
						if (locked_ok) { dst_bits = static_cast<u8*>(locked.pBits); dst_pitch = static_cast<u32>(locked.Pitch); dst_slice = dst_pitch * bh; }
					}
					else if (texture_kind == rsx_texture_kind::texture_cubemap)
					{
						if (subresource.layer >= 6) { upload_valid = false; break; }
						auto* cube = static_cast<IDirect3DCubeTexture9*>(entry.staging);
						const auto face = static_cast<D3DCUBEMAP_FACES>(subresource.layer);
						locked_ok = SUCCEEDED(cube->LockRect(face, subresource.level, &locked, nullptr, 0));
						if (locked_ok) { dst_bits = static_cast<u8*>(locked.pBits); dst_pitch = static_cast<u32>(locked.Pitch); dst_slice = dst_pitch * bh; }
					}
					else
					{
						auto* volume = static_cast<IDirect3DVolumeTexture9*>(entry.staging);
						locked_ok = SUCCEEDED(volume->LockBox(subresource.level, &locked_box, nullptr, 0));
						if (locked_ok) { dst_bits = static_cast<u8*>(locked_box.pBits); dst_pitch = static_cast<u32>(locked_box.RowPitch); dst_slice = static_cast<u32>(locked_box.SlicePitch); }
					}

					if (!locked_ok || !dst_bits || !dst_pitch) { upload_valid = false; break; }

					if (is_swizzled)
					{
						rsx::texture_uploader_capabilities caps{
							.supports_byteswap = true,
							.supports_vtc_decoding = false,
							.supports_hw_deswizzle = false,
							.supports_zero_copy = false,
							.supports_dxt = true,
							.alignment = 4};

						const u32 decoded_pitch = static_cast<u32>(utils::align(row_bytes, caps.alignment));
						const usz min_size = static_cast<usz>(decoded_pitch) * bh * mip_d;
						std::vector<u8> decoded(std::max<usz>(min_size, subresource.data.size()));
						rsx::io_buffer io_buf(decoded.data(), decoded.size());
						const auto op = rsx::upload_texture_subresource(io_buf, subresource, gcm_upload_format, true, caps);

						if (io_buf.empty() || op.require_upload)
						{
							upload_valid = false;
						}
						else
						{
							for (u32 z = 0; z < mip_d; ++z)
							{
								for (u32 by = 0; by < bh; ++by)
								{
									std::memcpy(
										dst_bits + static_cast<usz>(z) * dst_slice + static_cast<usz>(by) * dst_pitch,
										decoded.data() + static_cast<usz>(z) * decoded_pitch * bh + static_cast<usz>(by) * decoded_pitch,
										row_bytes);
								}
							}
						}
					}
					else if (is_compressed)
					{
						for (u32 z = 0; z < mip_d; ++z)
							for (u32 by = 0; by < bh; ++by)
								std::memcpy(dst_bits + static_cast<usz>(z) * dst_slice + static_cast<usz>(by) * dst_pitch,
									src_pixels + static_cast<usz>(z) * src_slice + static_cast<usz>(by) * src_pitch, row_bytes);
					}
					else if (bpp == 4)
					{
						for (u32 z = 0; z < mip_d; ++z)
							for (u32 y = 0; y < mip_h; ++y)
							{
								const u32* src_row = reinterpret_cast<const u32*>(src_pixels + static_cast<usz>(z) * src_slice + static_cast<usz>(y) * src_pitch);
								auto* dst_row = reinterpret_cast<u32*>(dst_bits + static_cast<usz>(z) * dst_slice + static_cast<usz>(y) * dst_pitch);
								for (u32 x = 0; x < mip_w; ++x) dst_row[x] = _byteswap_ulong(src_row[x]);
							}
					}
					else if (bpp == 2)
					{
						for (u32 z = 0; z < mip_d; ++z)
							for (u32 y = 0; y < mip_h; ++y)
							{
								const u16* src_row = reinterpret_cast<const u16*>(src_pixels + static_cast<usz>(z) * src_slice + static_cast<usz>(y) * src_pitch);
								auto* dst_row = reinterpret_cast<u16*>(dst_bits + static_cast<usz>(z) * dst_slice + static_cast<usz>(y) * dst_pitch);
								for (u32 x = 0; x < mip_w; ++x) dst_row[x] = _byteswap_ushort(src_row[x]);
							}
					}
					else if (bpp == 8)
					{
						for (u32 z = 0; z < mip_d; ++z)
							for (u32 y = 0; y < mip_h; ++y)
							{
								const u16* src_row = reinterpret_cast<const u16*>(src_pixels + static_cast<usz>(z) * src_slice + static_cast<usz>(y) * src_pitch);
								auto* dst_row = reinterpret_cast<u16*>(dst_bits + static_cast<usz>(z) * dst_slice + static_cast<usz>(y) * dst_pitch);
								for (u32 x = 0; x < mip_w * 4; ++x) dst_row[x] = _byteswap_ushort(src_row[x]);
							}
					}
					else if (bpp == 16)
					{
						for (u32 z = 0; z < mip_d; ++z)
							for (u32 y = 0; y < mip_h; ++y)
							{
								const u32* src_row = reinterpret_cast<const u32*>(src_pixels + static_cast<usz>(z) * src_slice + static_cast<usz>(y) * src_pitch);
								auto* dst_row = reinterpret_cast<u32*>(dst_bits + static_cast<usz>(z) * dst_slice + static_cast<usz>(y) * dst_pitch);
								for (u32 x = 0; x < mip_w * 4; ++x) dst_row[x] = _byteswap_ulong(src_row[x]);
							}
					}
					else
					{
						for (u32 z = 0; z < mip_d; ++z)
							for (u32 by = 0; by < bh; ++by)
								std::memcpy(dst_bits + static_cast<usz>(z) * dst_slice + static_cast<usz>(by) * dst_pitch,
									src_pixels + static_cast<usz>(z) * src_slice + static_cast<usz>(by) * src_pitch, row_bytes);
					}

					if (texture_kind == rsx_texture_kind::texture_2d)
						static_cast<IDirect3DTexture9*>(entry.staging)->UnlockRect(subresource.level);
					else if (texture_kind == rsx_texture_kind::texture_cubemap)
						static_cast<IDirect3DCubeTexture9*>(entry.staging)->UnlockRect(static_cast<D3DCUBEMAP_FACES>(subresource.layer), subresource.level);
					else
						static_cast<IDirect3DVolumeTexture9*>(entry.staging)->UnlockBox(subresource.level);
				}

				if (upload_valid && SUCCEEDED(m_device->UpdateTexture(entry.staging, entry.gpu)))
				{
					uploaded_or_cached = true;
					entry.upload_frame = m_frame_number;
					entry.content_hash = content_hash;
				}
			}
			else if (entry.gpu && entry.upload_frame > 0)
			{
				uploaded_or_cached = true;
			}
		}
		return uploaded_or_cached;
	};

	bool has_texture = false;
	for (u32 i = 0; i < 16; ++i)
	{
		if (upload_fragment_texture(i))
		{
			has_texture = true;
		}
	}

	if (fp_valid && current_fp_metadata.referenced_textures_mask == 0)
	{
		bool any_enabled_texture = false;
		for (u32 i = 0; i < 16; ++i)
		{
			if (rsx::method_registers.fragment_textures[i].enabled())
			{
				any_enabled_texture = true;
				break;
			}
		}
		if (!any_enabled_texture)
		{
			has_texture = false;
		}
	}

	// Load vertex program
	DX9VertexProgram* vertex_prog = load_vertex_program();
	m_current_vp = vertex_prog;

	bool using_vp = false;
	if (vertex_prog && vertex_prog->dx9_shader)
	{
		m_device->SetVertexShader(vertex_prog->dx9_shader);
		upload_vertex_constants(vertex_prog);
		using_vp = true;
	}

	if (!using_vp)
	{
		m_device->SetVertexShader(nullptr);
		m_device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1);
	}

	// Set up fragment shader and textures
	if (has_texture && m_rsx_tex[sampled_tex_unit].gpu)
	{
		const bool use_const_color = (using_vp || col_attr < 0);
		const bool use_compiled_fp = fragment_prog && fragment_prog->dx9_shader && !current_fp_metadata.is_nop_shader;

		if (use_compiled_fp)
		{
			for (u32 i = 0; i < 16; ++i)
			{
				IDirect3DBaseTexture9* tex = nullptr;
				if (rsx::method_registers.fragment_textures[i].enabled() && m_rsx_tex[i].gpu)
					tex = m_rsx_tex[i].gpu;

				const auto& rsx_tex = rsx::method_registers.fragment_textures[i];
				const auto mip_count = m_rsx_tex[i].mipmaps;
				m_device->SetTexture(i, tex);
				m_device->SetSamplerState(i, D3DSAMP_MINFILTER, to_d3d_min_filter(rsx_tex.min_filter()));
				m_device->SetSamplerState(i, D3DSAMP_MAGFILTER, to_d3d_mag_filter(rsx_tex.mag_filter()));
				m_device->SetSamplerState(i, D3DSAMP_MIPFILTER, to_d3d_mip_filter(rsx_tex.min_filter(), mip_count));
				m_device->SetSamplerState(i, D3DSAMP_MAXMIPLEVEL, 0);
				const float lod_bias = rsx_tex.bias();
				DWORD lod_bias_bits = 0;
				std::memcpy(&lod_bias_bits, &lod_bias, sizeof(lod_bias_bits));
				m_device->SetSamplerState(i, D3DSAMP_MIPMAPLODBIAS, lod_bias_bits);
				m_device->SetSamplerState(i, D3DSAMP_ADDRESSU, to_d3d_address(rsx_tex.wrap_s()));
				m_device->SetSamplerState(i, D3DSAMP_ADDRESSV, to_d3d_address(rsx_tex.wrap_t()));
				m_device->SetSamplerState(i, D3DSAMP_ADDRESSW, to_d3d_address(rsx_tex.wrap_r()));
			}
			m_device->SetPixelShader(fragment_prog->dx9_shader);
			upload_fragment_constants(fragment_prog);
		}
		else
		{
			m_device->SetTexture(0, m_rsx_tex[sampled_tex_unit].gpu);
			const auto& rsx_tex = rsx::method_registers.fragment_textures[sampled_tex_unit];
			m_device->SetSamplerState(0, D3DSAMP_MINFILTER, to_d3d_min_filter(rsx_tex.min_filter()));
			m_device->SetSamplerState(0, D3DSAMP_MAGFILTER, to_d3d_mag_filter(rsx_tex.mag_filter()));
			m_device->SetSamplerState(0, D3DSAMP_MIPFILTER, to_d3d_mip_filter(rsx_tex.min_filter(), m_rsx_tex[sampled_tex_unit].mipmaps));
			m_device->SetSamplerState(0, D3DSAMP_ADDRESSU, to_d3d_address(rsx_tex.wrap_s()));
			m_device->SetSamplerState(0, D3DSAMP_ADDRESSV, to_d3d_address(rsx_tex.wrap_t()));

			if (use_const_color)
			{
				m_device->SetPixelShader(m_ps_tex_const ? m_ps_tex_const : m_bootstrap_ps);
				const float c[4] = {
					static_cast<float>((constant_color >> 16) & 0xFFu) / 255.f,
					static_cast<float>((constant_color >> 8) & 0xFFu) / 255.f,
					static_cast<float>(constant_color & 0xFFu) / 255.f,
					static_cast<float>((constant_color >> 24) & 0xFFu) / 255.f};
				m_device->SetPixelShaderConstantF(0, c, 1);
			}
			else
			{
				m_device->SetPixelShader(m_ps_tex_vcolor ? m_ps_tex_vcolor : m_bootstrap_ps);
			}
		}

		m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
		const auto rsx_sfactor = rsx::method_registers.blend_func_sfactor_rgb();
		if (rsx::method_registers.blend_enabled() && (rsx_sfactor == rsx::blend_factor::one || rsx_sfactor == rsx::blend_factor::src_alpha || rsx_sfactor == rsx::blend_factor::one_minus_src_alpha))
		{
			m_device->SetRenderState(D3DRS_SRCBLEND, to_d3d_blend(rsx_sfactor));
			m_device->SetRenderState(D3DRS_DESTBLEND, to_d3d_blend(rsx::method_registers.blend_func_dfactor_rgb()));
			m_device->SetRenderState(D3DRS_BLENDOP, to_d3d_blend_op(rsx::method_registers.blend_equation_rgb()));
		}
		else
		{
			m_device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
			m_device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
		}
	}
	else
	{
		m_device->SetTexture(0, nullptr);
		if (fragment_prog && fragment_prog->dx9_shader && !current_fp_metadata.is_nop_shader)
		{
			m_device->SetPixelShader(fragment_prog->dx9_shader);
			upload_fragment_constants(fragment_prog);
		}
		else if (!using_vp && col_attr >= 0)
		{
			m_device->SetPixelShader(m_ps_solid_vcolor ? m_ps_solid_vcolor : nullptr);
		}
		else
		{
			m_device->SetPixelShader(m_ps_solid_const ? m_ps_solid_const : m_bootstrap_ps);
			const float c[4] = {
				static_cast<float>((constant_color >> 16) & 0xFFu) / 255.f,
				static_cast<float>((constant_color >> 8) & 0xFFu) / 255.f,
				static_cast<float>(constant_color & 0xFFu) / 255.f,
				static_cast<float>((constant_color >> 24) & 0xFFu) / 255.f};
			m_device->SetPixelShaderConstantF(0, c, 1);
		}
	}

	// Prepare attribute data pointers
	const u8* cdata = nullptr;
	u32 col_stride = 0, col_comp = 0;
	rsx::vertex_base_type col_type = rsx::vertex_base_type::ub;
	if (col_attr >= 0)
	{
		const auto& col_info = rsx::method_registers.vertex_arrays_info[col_attr];
		const u32 base_offset = rsx::method_registers.vertex_data_base_offset();
		const u32 col_array_off = col_info.offset() & 0x7FFFFFFFu;
		const u32 col_mem_loc = col_info.offset() >> 31;
		const u32 col_address = rsx::get_address(rsx::get_vertex_offset_from_base(base_offset, col_array_off), col_mem_loc);
		if (is_addr_safe(col_address, 16))
		{
			cdata = static_cast<const u8*>(vm::base(col_address));
			col_stride = col_info.stride();
			col_comp = col_info.size();
			col_type = col_info.type();
		}
	}

	const u8* tdata = nullptr;
	u32 tc_stride = 0, tc_comp = 0;
	rsx::vertex_base_type tc_type = rsx::vertex_base_type::f;
	if (tc_attr >= 0)
	{
		const auto& tc_info = rsx::method_registers.vertex_arrays_info[tc_attr];
		const u32 base_offset = rsx::method_registers.vertex_data_base_offset();
		const u32 tc_array_off = tc_info.offset() & 0x7FFFFFFFu;
		const u32 tc_mem_loc = tc_info.offset() >> 31;
		const u32 tc_address = rsx::get_address(rsx::get_vertex_offset_from_base(base_offset, tc_array_off), tc_mem_loc);
		if (is_addr_safe(tc_address, 16))
		{
			tdata = static_cast<const u8*>(vm::base(tc_address));
			tc_stride = tc_info.stride();
			tc_comp = tc_info.size();
			tc_type = tc_info.type();
		}
	}

	auto get_vertex_color = [&](u32 vertex_index) -> DWORD
	{
		if (!cdata || col_stride == 0) return constant_color;
		const u8* cptr = cdata + static_cast<usz>(vertex_index) * col_stride;
		if (col_type == rsx::vertex_base_type::ub || col_type == rsx::vertex_base_type::ub256)
			return read_color_ub(cptr, col_comp);
		if (col_type == rsx::vertex_base_type::f)
			return read_color_float(cptr, col_comp);
		return constant_color;
	};

	auto get_vertex_texcoord = [&](u32 vertex_index, float& ou, float& ov)
	{
		if (!tdata || tc_stride == 0) { ou = 0.f; ov = 0.f; return; }
		read_texcoord(tdata + static_cast<usz>(vertex_index) * tc_stride, tc_comp, tc_type, ou, ov);
	};

	bool drew_rsx = false;
	const auto& range = clause.get_range();

	// VP draw path
	const bool use_vp_draw = using_vp && range.count >= 2 &&
	                         (clause.command == rsx::draw_command::array || clause.command == rsx::draw_command::indexed);

	if (use_vp_draw)
	{
		D3DPRIMITIVETYPE d3d_prim;
		u32 prim_count;
		bool need_quad;

		if (map_primitive(clause.primitive, range.count, d3d_prim, prim_count, need_quad))
		{
			u32 active_attrs = 0;
			struct attr_source
			{
				const u8* array_data = nullptr;
				u32 array_stride = 0, array_comp = 0;
				rsx::vertex_base_type array_type = rsx::vertex_base_type::f;
				float reg_data[4] = {0.f, 0.f, 0.f, 1.f};
				bool from_array = false;
			};
			attr_source sources[16]{};

			const u32 base_offset = rsx::method_registers.vertex_data_base_offset();

			for (u32 i = 0; i < 16; i++)
			{
				const auto& info = rsx::method_registers.vertex_arrays_info[i];
				if (info.stride() > 0 && info.size() > 0)
				{
					const u32 arr_off = info.offset() & 0x7FFFFFFFu;
					const u32 mem_loc = info.offset() >> 31;
					const u32 addr = rsx::get_address(rsx::get_vertex_offset_from_base(base_offset, arr_off), mem_loc);
					const u32 last_vtx = range.first + range.count - 1;
					const u32 needed = static_cast<u32>(static_cast<usz>(last_vtx) * info.stride() + info.size() * 4);

					if (is_addr_safe(addr, needed))
					{
						sources[i].array_data = static_cast<const u8*>(vm::base(addr));
						sources[i].array_stride = info.stride();
						sources[i].array_comp = info.size();
						sources[i].array_type = info.type();
						sources[i].from_array = true;
						active_attrs |= (1u << i);
					}
				}
			}

			for (u32 i = 0; i < 16; i++)
			{
				if (active_attrs & (1u << i)) continue;
				const auto& ri = rsx::method_registers.register_vertex_info[i];
				if (ri.size > 0)
				{
					sources[i].reg_data[0] = 0.f; sources[i].reg_data[1] = 0.f;
					sources[i].reg_data[2] = 0.f; sources[i].reg_data[3] = 1.f;
					if (ri.type == rsx::vertex_base_type::f)
					{
						const u32* raw = reinterpret_cast<const u32*>(ri.data.data());
						for (u32 c = 0; c < std::min<u32>(ri.size, 4); c++)
						{
							const u32 swapped = _byteswap_ulong(raw[c]);
							std::memcpy(&sources[i].reg_data[c], &swapped, 4);
						}
					}
					sources[i].from_array = false;
					active_attrs |= (1u << i);
				}
			}

			if (active_attrs != 0)
			{
				IDirect3DVertexDeclaration9* vp_decl = get_vertex_declaration(active_attrs);

				u32 attrs_count = 0;
				for (u32 i = 0; i < 16; i++)
					if (active_attrs & (1u << i)) attrs_count++;

				const u32 vertex_floats = attrs_count * 4;
				const u32 vertex_stride_bytes = attrs_count * 16;

				auto read_array_float4 = [&](const attr_source& src, u32 vertex_index, float* out)
				{
					out[0] = 0.f; out[1] = 0.f; out[2] = 0.f; out[3] = 1.f;
					const u8* ptr = src.array_data + static_cast<usz>(vertex_index) * src.array_stride;
					if (src.array_type == rsx::vertex_base_type::f)
					{
						for (u32 c = 0; c < std::min<u32>(src.array_comp, 4); c++)
						{
							u32 raw; std::memcpy(&raw, ptr + c * 4, 4);
							raw = _byteswap_ulong(raw); std::memcpy(&out[c], &raw, 4);
						}
					}
					else if (src.array_type == rsx::vertex_base_type::sf)
					{
						for (u32 c = 0; c < std::min<u32>(src.array_comp, 4); c++)
						{
							u16 raw; std::memcpy(&raw, ptr + c * 2, 2);
							out[c] = f16_to_f32(raw);
						}
					}
					else if (src.array_type == rsx::vertex_base_type::ub)
					{
						for (u32 c = 0; c < std::min<u32>(src.array_comp, 4); c++)
							out[c] = static_cast<float>(ptr[c]) / 255.f;
					}
					else if (src.array_type == rsx::vertex_base_type::ub256)
					{
						for (u32 c = 0; c < std::min<u32>(src.array_comp, 4); c++)
							out[c] = static_cast<float>(ptr[c]);
					}
					else if (src.array_type == rsx::vertex_base_type::s1)
					{
						for (u32 c = 0; c < std::min<u32>(src.array_comp, 4); c++)
						{
							u16 raw; std::memcpy(&raw, ptr + c * 2, 2);
							raw = _byteswap_ushort(raw);
							out[c] = std::max(static_cast<float>(static_cast<s16>(raw)) / 32767.f, -1.f);
						}
					}
					else if (src.array_type == rsx::vertex_base_type::s32k)
					{
						for (u32 c = 0; c < std::min<u32>(src.array_comp, 4); c++)
						{
							u16 raw; std::memcpy(&raw, ptr + c * 2, 2);
							out[c] = static_cast<float>(static_cast<s16>(_byteswap_ushort(raw)));
						}
					}
				};

				const u32 idx_addr = (clause.command == rsx::draw_command::indexed) ? rsx::get_address(rsx::method_registers.index_array_address(), rsx::method_registers.index_array_location()) : 0u;
				const bool is_u16 = rsx::method_registers.index_type() == rsx::index_array_type::u16;
				const u32 idx_base = rsx::method_registers.vertex_data_base_index();
				const u8* idx_data = nullptr;
				if (clause.command == rsx::draw_command::indexed && idx_addr)
				{
					const u32 idx_elem_size = is_u16 ? 2u : 4u;
					const u32 idx_needed = (range.first + range.count) * idx_elem_size;
					if (is_addr_safe(idx_addr, idx_needed))
						idx_data = static_cast<const u8*>(vm::base(idx_addr));
				}

				auto resolve_vertex_index = [&](u32 draw_index) -> u32
				{
					if (clause.command == rsx::draw_command::array)
						return range.first + draw_index;
					if (!idx_data) return 0;
					u32 idx;
					if (is_u16) { u16 raw; std::memcpy(&raw, idx_data + (range.first + draw_index) * 2, 2); idx = _byteswap_ushort(raw); }
					else { u32 raw; std::memcpy(&raw, idx_data + (range.first + draw_index) * 4, 4); idx = _byteswap_ulong(raw); }
					return (idx + idx_base) & 0x000FFFFFu;
				};

				std::vector<float> vbuf(range.count * vertex_floats);
				for (u32 v = 0; v < range.count; v++)
				{
					const u32 vi = resolve_vertex_index(v);
					u32 attr_offset = 0;
					for (u32 i = 0; i < 16; i++)
					{
						if (!(active_attrs & (1u << i))) continue;
						float* dst = &vbuf[(v * attrs_count + attr_offset) * 4];
						if (sources[i].from_array) read_array_float4(sources[i], vi, dst);
						else std::memcpy(dst, sources[i].reg_data, 16);
						attr_offset++;
					}
				}

				if (need_quad)
				{
					const u32 quad_count = range.count / 4;
					const u32 tri_verts = quad_count * 6;
					std::vector<float> expanded(tri_verts * vertex_floats);
					for (u32 q = 0; q < quad_count; q++)
					{
						const u32 src_base = q * 4 * vertex_floats;
						const u32 dst_base = q * 6 * vertex_floats;
						std::memcpy(&expanded[dst_base + 0 * vertex_floats], &vbuf[src_base + 0 * vertex_floats], vertex_floats * 4);
						std::memcpy(&expanded[dst_base + 1 * vertex_floats], &vbuf[src_base + 1 * vertex_floats], vertex_floats * 4);
						std::memcpy(&expanded[dst_base + 2 * vertex_floats], &vbuf[src_base + 2 * vertex_floats], vertex_floats * 4);
						std::memcpy(&expanded[dst_base + 3 * vertex_floats], &vbuf[src_base + 0 * vertex_floats], vertex_floats * 4);
						std::memcpy(&expanded[dst_base + 4 * vertex_floats], &vbuf[src_base + 2 * vertex_floats], vertex_floats * 4);
						std::memcpy(&expanded[dst_base + 5 * vertex_floats], &vbuf[src_base + 3 * vertex_floats], vertex_floats * 4);
					}
					prim_count = quad_count * 2;
					if (vp_decl)
					{
						m_device->SetVertexDeclaration(vp_decl);
						drew_rsx = rsx::dx9::check(m_device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, prim_count, expanded.data(), vertex_stride_bytes), "DrawPrimitiveUP(vp_quad)");
					}
				}
				else if (vp_decl)
				{
					m_device->SetVertexDeclaration(vp_decl);
					drew_rsx = rsx::dx9::check(m_device->DrawPrimitiveUP(d3d_prim, prim_count, vbuf.data(), vertex_stride_bytes), "DrawPrimitiveUP(vp_draw)");
				}
			}
		}
	}

	// Fallback (fixed-function) draw path
	if (!drew_rsx && pos_attr >= 0 && range.count >= 2)
	{
		if (using_vp)
		{
			m_device->SetVertexShader(nullptr);
			m_device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1);
		}
		const auto& pos_info = rsx::method_registers.vertex_arrays_info[pos_attr];
		const u32 base_offset = rsx::method_registers.vertex_data_base_offset();
		const u32 array_offset = pos_info.offset() & 0x7FFFFFFFu;
		const u32 memory_location = pos_info.offset() >> 31;
		const u32 address = rsx::get_address(rsx::get_vertex_offset_from_base(base_offset, array_offset), memory_location);
		const u32 stride = pos_info.stride();
		const u32 comp = pos_info.size();
		const u32 last_vertex = range.first + range.count - 1;
		const u32 needed_bytes = static_cast<u32>(static_cast<usz>(last_vertex) * stride + comp * 4);
		const bool pos_addr_safe = is_addr_safe(address, needed_bytes);
		const u8* vdata = pos_addr_safe ? static_cast<const u8*>(vm::base(address)) : nullptr;

		D3DPRIMITIVETYPE d3d_prim;
		u32 prim_count;
		bool need_quad;

		if (vdata && stride > 0 && map_primitive(clause.primitive, range.count, d3d_prim, prim_count, need_quad))
		{
			if (clause.command == rsx::draw_command::array)
			{
				std::vector<PosColorVertex> raw_verts(range.count);
				for (u32 v = 0; v < range.count; ++v)
				{
					const u32 vi = range.first + v;
					raw_verts[v] = read_position(vdata + static_cast<usz>(vi) * stride, comp);
					raw_verts[v].color = get_vertex_color(vi);
					get_vertex_texcoord(vi, raw_verts[v].u, raw_verts[v].v);
				}

				set_draw_transform(raw_verts.data(), range.count);

				std::vector<PosColorVertex> expanded;
				const PosColorVertex* draw_ptr = raw_verts.data();
				if (need_quad) { expand_quads(raw_verts, expanded); draw_ptr = expanded.data(); }

				drew_rsx = rsx::dx9::check(m_device->DrawPrimitiveUP(d3d_prim, prim_count, draw_ptr, sizeof(PosColorVertex)), "DrawPrimitiveUP(rsx_array)");
			}
			else if (clause.command == rsx::draw_command::indexed)
			{
				const u32 idx_addr = rsx::get_address(rsx::method_registers.index_array_address(), rsx::method_registers.index_array_location());
				const bool is_u16 = rsx::method_registers.index_type() == rsx::index_array_type::u16;
				const u32 idx_base = rsx::method_registers.vertex_data_base_index();
				const u32 idx_elem_size = is_u16 ? 2u : 4u;
				const u32 idx_needed = (range.first + range.count) * idx_elem_size;
				const bool idx_addr_safe = is_addr_safe(idx_addr, idx_needed);
				const u8* idx_data = idx_addr_safe ? static_cast<const u8*>(vm::base(idx_addr)) : nullptr;

				if (idx_data)
				{
					std::vector<PosColorVertex> indexed_verts(range.count);
					for (u32 i = 0; i < range.count; ++i)
					{
						u32 idx;
						if (is_u16) { u16 raw_idx; std::memcpy(&raw_idx, idx_data + (range.first + i) * 2, 2); idx = static_cast<u32>(_byteswap_ushort(raw_idx)); }
						else { u32 raw_idx; std::memcpy(&raw_idx, idx_data + (range.first + i) * 4, 4); idx = _byteswap_ulong(raw_idx); }
						idx = (idx + idx_base) & 0x000FFFFFu;

						const u32 vtx_off = static_cast<u32>(static_cast<usz>(idx) * stride);
						if (!is_addr_safe(address + vtx_off, comp * 4)) { indexed_verts[i] = PosColorVertex{}; continue; }
						indexed_verts[i] = read_position(vdata + static_cast<usz>(idx) * stride, comp);
						indexed_verts[i].color = get_vertex_color(idx);
						get_vertex_texcoord(idx, indexed_verts[i].u, indexed_verts[i].v);
					}

					set_draw_transform(indexed_verts.data(), range.count);

					std::vector<PosColorVertex> expanded;
					const PosColorVertex* draw_ptr = indexed_verts.data();
					if (need_quad) { expand_quads(indexed_verts, expanded); draw_ptr = expanded.data(); }

					drew_rsx = rsx::dx9::check(m_device->DrawPrimitiveUP(d3d_prim, prim_count, draw_ptr, sizeof(PosColorVertex)), "DrawPrimitiveUP(rsx_indexed)");
				}
			}
		}

		if (using_vp && vertex_prog && vertex_prog->dx9_shader)
		{
			m_device->SetVertexShader(vertex_prog->dx9_shader);
		}
	}

	if (using_vp)
	{
		m_device->SetVertexShader(nullptr);
		m_device->SetPixelShader(nullptr);
		m_device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1);
	}

	++m_bootstrap_emit_count;
}

bool DX9GSRender::ensure_flip_surface(u32 width, u32 height, D3DFORMAT fmt)
{
	if (!m_device || !width || !height) return false;

	if (m_flip_surface && m_flip_surface_width == width && m_flip_surface_height == height && m_flip_surface_format == fmt)
		return true;

	if (m_flip_surface) { enqueue_deferred_release(m_flip_surface); m_flip_surface = nullptr; }

	if (!rsx::dx9::check(m_device->CreateOffscreenPlainSurface(width, height, fmt, D3DPOOL_SYSTEMMEM, &m_flip_surface, nullptr), "CreateOffscreenPlainSurface"))
	{
		m_flip_surface_width = 0; m_flip_surface_height = 0; m_flip_surface_format = D3DFMT_UNKNOWN;
		return false;
	}

	m_flip_surface_width = width; m_flip_surface_height = height; m_flip_surface_format = fmt;
	return true;
}

bool DX9GSRender::ensure_display_texture(u32 width, u32 height)
{
	if (!m_device || !width || !height) return false;

	if (m_display_tex_staging && m_display_tex_gpu && m_display_tex_width == width && m_display_tex_height == height)
		return true;

	if (m_display_tex_staging) { enqueue_deferred_release(m_display_tex_staging); m_display_tex_staging = nullptr; }
	if (m_display_tex_gpu) { enqueue_deferred_release(m_display_tex_gpu); m_display_tex_gpu = nullptr; }
	m_display_tex_width = 0; m_display_tex_height = 0;

	if (!rsx::dx9::check(m_device->CreateTexture(width, height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &m_display_tex_staging, nullptr), "CreateTexture(display_staging)"))
		return false;

	if (!rsx::dx9::check(m_device->CreateTexture(width, height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &m_display_tex_gpu, nullptr), "CreateTexture(display_gpu)"))
	{
		m_display_tex_staging->Release(); m_display_tex_staging = nullptr;
		return false;
	}

	m_display_tex_width = width; m_display_tex_height = height;
	return true;
}

bool DX9GSRender::blit_display_buffer(const rsx::display_flip_info_t& info)
{
	if (!m_device || info.buffer >= display_buffers_count)
		return false;

	const auto& db = display_buffers[info.buffer];
	u32 width = db.width;
	u32 height = db.height;
	u32 pitch = db.pitch;
	const auto& avconfig = g_fxo->get<rsx::avconf>();
	const u32 av_out_format = avconfig.state ? avconfig.format : CELL_VIDEO_OUT_BUFFER_COLOR_FORMAT_X8R8G8B8;
	const u32 av_format = avconfig.state ? avconfig.get_compatible_gcm_format() : CELL_GCM_TEXTURE_A8R8G8B8;
	const u32 src_bpp = avconfig.state ? avconfig.get_bpp() : 4u;

	if (!width || !height) { width = avconfig.resolution_x; height = avconfig.resolution_y; if (!pitch) pitch = width * src_bpp; }
	else if (!pitch) { pitch = width * src_bpp; }
	if (!width || !height || !pitch) return false;
	if (av_format != CELL_GCM_TEXTURE_A8R8G8B8 || src_bpp != 4u) return false;
	if (!ensure_flip_surface(width, height, D3DFMT_A8R8G8B8)) return false;

	const u32 addr = rsx::get_address(db.offset, CELL_GCM_LOCATION_LOCAL);
	auto* src = static_cast<const u8*>(vm::base(addr));
	if (!src) return false;

	D3DLOCKED_RECT locked{};
	if (!rsx::dx9::check(m_flip_surface->LockRect(&locked, nullptr, 0), "IDirect3DSurface9::LockRect(flip_surface)"))
		return false;

	const display_decode_mode decode_mode = choose_display_decode_mode(src, width, height, pitch, av_out_format);
	for (u32 y = 0; y < height; ++y)
	{
		const auto* src_row = reinterpret_cast<const u32*>(src + (y * pitch));
		auto* dst_row = reinterpret_cast<u32*>(static_cast<u8*>(locked.pBits) + (y * locked.Pitch));
		for (u32 x = 0; x < width; ++x)
			dst_row[x] = convert_display_pixel_to_d3d9_argb(src_row[x], decode_mode);
	}
	m_flip_surface->UnlockRect();

	IDirect3DSurface9* backbuffer = nullptr;
	if (!rsx::dx9::check(m_device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer), "IDirect3DDevice9::GetBackBuffer"))
		return false;

	const bool updated = rsx::dx9::check(m_device->UpdateSurface(m_flip_surface, nullptr, backbuffer, nullptr), "IDirect3DDevice9::UpdateSurface");
	backbuffer->Release();
	return updated;
}

bool DX9GSRender::blit_framebuffer_surface_memory()
{
	if (!m_device) return false;

	u32 index = umax;
	u32 width = 0, height = 0, pitch = 0, addr = 0;

	for (u32 i = 0; i < static_cast<u32>(std::size(m_surface_info)); ++i)
	{
		const auto& s = m_surface_info[i];
		if (s.address && s.pitch && s.width && s.height && s.bpp == 4)
		{
			index = i; width = s.width; height = s.height; pitch = s.pitch; addr = s.address;
			break;
		}
	}

	if (index == umax)
	{
		const auto layout_bpp = [&]() -> u32
		{
			switch (m_framebuffer_layout.color_format)
			{
			case rsx::surface_color_format::a8r8g8b8:
			case rsx::surface_color_format::a8b8g8r8:
			case rsx::surface_color_format::x8r8g8b8_o8r8g8b8:
			case rsx::surface_color_format::x8r8g8b8_z8r8g8b8:
			case rsx::surface_color_format::x8b8g8r8_o8b8g8r8:
			case rsx::surface_color_format::x8b8g8r8_z8b8g8r8:
			case rsx::surface_color_format::x32:
				return 4;
			default: return 0;
			}
		}();

		if (layout_bpp != 4) return false;

		for (u32 i = 0; i < static_cast<u32>(m_framebuffer_layout.color_addresses.size()); ++i)
		{
			const u32 layout_addr = m_framebuffer_layout.color_addresses[i];
			const u32 layout_pitch = m_framebuffer_layout.actual_color_pitch[i] ? m_framebuffer_layout.actual_color_pitch[i] : m_framebuffer_layout.color_pitch[i];
			if (layout_addr && layout_pitch && m_framebuffer_layout.width && m_framebuffer_layout.height)
			{
				index = i; width = m_framebuffer_layout.width; height = m_framebuffer_layout.height;
				pitch = layout_pitch; addr = layout_addr;
				break;
			}
		}

		if (index == umax) return false;
	}

	if (!ensure_flip_surface(width, height, D3DFMT_A8R8G8B8)) return false;

	const auto* src = static_cast<const u8*>(vm::base(addr));
	if (!src) return false;

	D3DLOCKED_RECT locked{};
	if (!rsx::dx9::check(m_flip_surface->LockRect(&locked, nullptr, 0), "IDirect3DSurface9::LockRect(framebuffer_surface)"))
		return false;

	for (u32 y = 0; y < height; ++y)
		std::memcpy(static_cast<u8*>(locked.pBits) + (y * locked.Pitch), src + (y * pitch), std::min<u32>(pitch, width * 4u));
	m_flip_surface->UnlockRect();

	IDirect3DSurface9* backbuffer = nullptr;
	if (!rsx::dx9::check(m_device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer), "IDirect3DDevice9::GetBackBuffer"))
		return false;

	const bool updated = rsx::dx9::check(m_device->UpdateSurface(m_flip_surface, nullptr, backbuffer, nullptr), "IDirect3DDevice9::UpdateSurface(framebuffer_surface)");
	backbuffer->Release();
	return updated;
}

bool DX9GSRender::blit_current_render_target()
{
	if (!m_device) return false;

	IDirect3DSurface9* src_rt = nullptr;
	if (!rsx::dx9::check(m_device->GetRenderTarget(0, &src_rt), "IDirect3DDevice9::GetRenderTarget"))
		return false;

	IDirect3DSurface9* backbuffer = nullptr;
	if (!rsx::dx9::check(m_device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer), "IDirect3DDevice9::GetBackBuffer"))
	{
		src_rt->Release();
		return false;
	}

	const bool copied = rsx::dx9::check(m_device->StretchRect(src_rt, nullptr, backbuffer, nullptr, D3DTEXF_NONE), "IDirect3DDevice9::StretchRect(rt->backbuffer)");
	backbuffer->Release();
	src_rt->Release();
	return copied;
}

void DX9GSRender::flip(const rsx::display_flip_info_t& info)
{
	if (m_device)
	{
		const bool should_try_blit = !info.skip_frame;
		if (should_try_blit && !g_dx9_force_direct_backbuffer_bootstrap)
		{
			blit_current_render_target() || blit_framebuffer_surface_memory() || blit_display_buffer(info);
		}
		else if (g_dx9_force_direct_backbuffer_bootstrap)
		{
			const bool new_emits = (m_bootstrap_emit_count > m_last_present_emit_count);

			IDirect3DSurface9* backbuffer = nullptr;
			if (rsx::dx9::check(m_device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer), "IDirect3DDevice9::GetBackBuffer(flip_blit)"))
			{
				rsx::dx9::check(m_device->SetRenderTarget(0, backbuffer), "IDirect3DDevice9::SetRenderTarget(flip_blit)");
				m_device->SetDepthStencilSurface(nullptr);
				backbuffer->Release();

				bool blit_ok = false;
				if (!info.skip_frame && info.buffer < display_buffers_count && !new_emits)
				{
					const auto& db = display_buffers[info.buffer];
					const auto& avconfig = g_fxo->get<rsx::avconf>();
					const u32 av_out_format = avconfig.state ? avconfig.format : CELL_VIDEO_OUT_BUFFER_COLOR_FORMAT_X8R8G8B8;
					u32 src_width = db.width ? static_cast<u32>(db.width) : avconfig.resolution_x;
					u32 src_height = db.height ? static_cast<u32>(db.height) : avconfig.resolution_y;
					u32 src_pitch = db.pitch ? static_cast<u32>(db.pitch) : (src_width * std::max<u32>(1u, avconfig.get_bpp()));

					if (src_width && src_height && src_pitch && ensure_display_texture(src_width, src_height))
					{
						const u32 addr = rsx::get_address(db.offset, CELL_GCM_LOCATION_LOCAL);
						const auto* src = static_cast<const u8*>(vm::base(addr));

						bool has_content = false;
						if (src)
						{
							const u32 step_y = std::max<u32>(1u, src_height / 8u);
							const u32 step_x = std::max<u32>(1u, src_width / 8u);
							for (u32 sy = 0; sy < src_height && !has_content; sy += step_y)
							{
								const auto* row = reinterpret_cast<const u32*>(src + sy * src_pitch);
								for (u32 sx = 0; sx < src_width && !has_content; sx += step_x)
									if (row[sx] != 0) has_content = true;
							}
						}

						D3DLOCKED_RECT locked{};
						if (has_content && src && SUCCEEDED(m_display_tex_staging->LockRect(0, &locked, nullptr, D3DLOCK_DISCARD)))
						{
							const display_decode_mode decode_mode = choose_display_decode_mode(src, src_width, src_height, src_pitch, av_out_format);
							for (u32 y = 0; y < src_height; ++y)
							{
								const auto* src_row = reinterpret_cast<const u32*>(src + y * src_pitch);
								auto* dst_row = reinterpret_cast<u32*>(static_cast<u8*>(locked.pBits) + y * locked.Pitch);
								for (u32 x = 0; x < src_width; ++x)
									dst_row[x] = convert_display_pixel_to_d3d9_argb(src_row[x], decode_mode);
							}
							m_display_tex_staging->UnlockRect(0);

							if (SUCCEEDED(m_device->UpdateTexture(m_display_tex_staging, m_display_tex_gpu)))
							{
								rsx::dx9::check(m_device->BeginScene(), "IDirect3DDevice9::BeginScene(flip_blit)");

								D3DVIEWPORT9 vp{}; vp.Width = m_pp.BackBufferWidth; vp.Height = m_pp.BackBufferHeight; vp.MaxZ = 1.0f;
								m_device->SetViewport(&vp);
								m_device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
								m_device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
								m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
								m_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
								m_device->SetVertexShader(nullptr);
								m_device->SetPixelShader(nullptr);
								m_device->SetTexture(0, m_display_tex_gpu);
								m_device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
								m_device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
								m_device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
								m_device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
								m_device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
								m_device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
								m_device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
								m_device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
								m_device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

								m_device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
								struct tv { float x, y, z, w, u, v; };
								const float fw = static_cast<float>(m_pp.BackBufferWidth);
								const float fh = static_cast<float>(m_pp.BackBufferHeight);
								const tv quad[4] = {
									{-0.5f, -0.5f, 0.5f, 1.0f, 0.0f, 0.0f},
									{fw - 0.5f, -0.5f, 0.5f, 1.0f, 1.0f, 0.0f},
									{-0.5f, fh - 0.5f, 0.5f, 1.0f, 0.0f, 1.0f},
									{fw - 0.5f, fh - 0.5f, 0.5f, 1.0f, 1.0f, 1.0f}};
								rsx::dx9::check(m_device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(tv)), "DrawPrimitiveUP(flip_display_blit)");
								rsx::dx9::check(m_device->EndScene(), "IDirect3DDevice9::EndScene(flip_blit)");
								blit_ok = true;
							}
						}
					}
				}

				if (!blit_ok && !new_emits)
				{
					rsx::dx9::check(m_device->BeginScene(), "IDirect3DDevice9::BeginScene(flip_fallback)");
					D3DVIEWPORT9 vp{}; vp.Width = m_pp.BackBufferWidth; vp.Height = m_pp.BackBufferHeight; vp.MaxZ = 1.0f;
					m_device->SetViewport(&vp);
					m_device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
					m_device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
					m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
					m_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
					m_device->SetVertexShader(nullptr); m_device->SetPixelShader(nullptr);
					m_device->SetTexture(0, nullptr);
					m_device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
					m_device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
					m_device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);

					static u64 s_flip_count = 0;
					const u8 pulse = static_cast<u8>((++s_flip_count * 3u) & 0xFFu);
					m_device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 255, pulse, static_cast<u8>(255u - pulse)), 1.0f, 0);

					struct bv { float x, y, z, w; u32 c; };
					const f32 fw = static_cast<f32>(m_pp.BackBufferWidth);
					const f32 fh = static_cast<f32>(m_pp.BackBufferHeight);
					const bv tri[3] = {
						{fw * 0.5f - 0.5f, fh * 0.1f - 0.5f, 0.5f, 1.0f, 0xFFFF0000},
						{fw * 0.9f - 0.5f, fh * 0.9f - 0.5f, 0.5f, 1.0f, 0xFF00FF00},
						{fw * 0.1f - 0.5f, fh * 0.9f - 0.5f, 0.5f, 1.0f, 0xFF0000FF}};
					rsx::dx9::check(m_device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(bv)), "DrawPrimitiveUP(flip_fallback)");
					rsx::dx9::check(m_device->EndScene(), "IDirect3DDevice9::EndScene(flip_fallback)");
				}
			}

			m_last_present_emit_count = m_bootstrap_emit_count;
		}

		const bool present_ok = m_present.present();
		if (!present_ok)
		{
			reset_device();
		}
		resolve_presented_frame(present_ok);
		m_frame_needs_clear = true;
		++m_frame_number;
	}

	GSRender::flip(info);
}

void DX9GSRender::do_local_task(rsx::FIFO::state state)
{
	if (!m_device || !m_frame)
	{
		rsx::thread::do_local_task(state);
		return;
	}

	const u32 w = static_cast<u32>(std::max(1, m_frame->client_width()));
	const u32 h = std::max(1, m_frame->client_height());
	if (m_pp.BackBufferWidth != w || m_pp.BackBufferHeight != h)
	{
		reset_device();
	}

	rsx::thread::do_local_task(state);
}

bool DX9GSRender::on_access_violation(u32 address, bool is_writing)
{
	(void)is_writing;
	rsx::mm_flush(address);
	invalidate_rsx_textures_in_range(utils::address_range32::start_length(address, 1));
	m_texture_cache.clear();
	m_frame_needs_clear = true;
	if (zcull_ctrl) return zcull_ctrl->on_access_violation(address);
	return true;
}

void DX9GSRender::on_invalidate_memory_range(const utils::address_range32& range, rsx::invalidation_cause cause)
{
	const bool invalidated = invalidate_rsx_textures_in_range(range);
	if (!cause.valid() || !cause.is_read()) m_texture_cache.clear();
	if (invalidated) m_frame_needs_clear = true;
}

void DX9GSRender::notify_tile_unbound(u32 tile)
{
	const u32 address = rsx::get_address(tiles[tile].offset, tiles[tile].location);
	const u32 length = std::max<u32>(tiles[tile].size, 1);
	if (!invalidate_rsx_textures_in_range(utils::address_range32::start_length(address, length)))
		invalidate_all_texture_state();
}

void DX9GSRender::on_semaphore_acquire_wait()
{
	do_local_task(rsx::FIFO::state::lock_wait);
}

void DX9GSRender::write_barrier(u32 address, u32 range)
{
	if (!range) return;
	on_invalidate_memory_range(utils::address_range32::start_length(address, range), rsx::invalidation_cause::write);
}

bool DX9GSRender::scaled_image_from_memory(const rsx::blit_src_info& src, const rsx::blit_dst_info& dst, bool interpolate)
{
	if (!src.pixels || !dst.pixels || !src.pitch || !dst.pitch || !src.width || !src.height || !dst.width || !dst.height)
		return false;
	if (src.bpp != 1 && src.bpp != 2 && src.bpp != 4)
		return false;

	std::vector<u8> mirror_tmp;
	std::vector<u8> detile_tmp;
	const u8* src_pixels = src.pixels;
	u32 src_pitch = src.pitch;
	bool src_is_temp = false;
	u8* dst_pixels_out = dst.pixels;
	u8* real_dst_pixels = dst.pixels;

	const u32 write_length = dst.pitch * dst.clip_height;
	const auto tiled_region = get_tiled_memory_region(utils::address_range32::start_length(dst.rsx_address, write_length));
	std::vector<u8> tiled_tmp;
	if (tiled_region)
	{
		if (dst.bpp != 2 && dst.bpp != 4) return false;
		tiled_tmp.resize(tiled_region.tile->size);
		dst_pixels_out = tiled_tmp.data();
	}

	const u32 read_length = src.pitch * src.height;
	const auto src_tiled_region = get_tiled_memory_region(utils::address_range32::start_length(src.rsx_address, read_length));
	if (src_tiled_region)
	{
		if (src.bpp != 2 && src.bpp != 4) return false;
		detile_tmp.resize(static_cast<usz>(src.pitch) * src.height);
		const auto detile_func = (src.bpp == 4) ? rsx::detile_texel_data32 : rsx::detile_texel_data16;
		detile_func(detile_tmp.data(), src_pixels, src_tiled_region.base_address,
			src.rsx_address - src_tiled_region.base_address, src_tiled_region.tile->size,
			src_tiled_region.tile->bank, src_tiled_region.tile->pitch, src.width, src.height);
		src_pixels = detile_tmp.data();
		src_pitch = src.pitch;
		src_is_temp = true;
	}

	if (dst.scale_x < 0.f || dst.scale_y < 0.f)
	{
		const bool flip_x = (dst.scale_x < 0.f);
		const bool flip_y = (dst.scale_y < 0.f);
		const u32 packed_pitch = src.width * src.bpp;
		mirror_tmp.resize(static_cast<usz>(packed_pitch) * src.height);

		for (u32 y = 0; y < src.height; ++y)
		{
			const u32 sy = flip_y ? (src.height - 1 - y) : y;
			const u8* src_row = src.pixels + static_cast<usz>(sy) * src.pitch;
			u8* dst_row = mirror_tmp.data() + static_cast<usz>(y) * packed_pitch;
			if (!flip_x) { std::memcpy(dst_row, src_row, packed_pitch); continue; }
			for (u32 x = 0; x < src.width; ++x)
				std::memcpy(dst_row + static_cast<usz>(x) * src.bpp, src_row + static_cast<usz>(src.width - 1 - x) * src.bpp, src.bpp);
		}
		src_pixels = mirror_tmp.data();
		src_pitch = packed_pitch;
		src_is_temp = true;
	}

	const AVPixelFormat ffmpeg_src_format = to_ffmpeg_src_format(src.format);
	const AVPixelFormat ffmpeg_dst_format = to_ffmpeg_dst_format(dst.format);

	const bool need_clip = dst.clip_width != dst.width || dst.clip_height != dst.height || dst.clip_x > 0 || dst.clip_y > 0;
	const bool need_convert = ffmpeg_dst_format != ffmpeg_src_format ||
		!rsx::fcmp(std::abs(dst.scale_x), 1.f) || !rsx::fcmp(std::abs(dst.scale_y), 1.f) ||
		src.width != dst.width || src.height != dst.height;

	const f32 abs_scale_y = std::max(std::abs(dst.scale_y), 0.00001f);
	const u32 slice_h = static_cast<u32>(std::ceil(static_cast<f32>(dst.clip_height + dst.clip_y) / abs_scale_y));

	std::vector<u8> temp;
	std::vector<u8> swz_temp;

	if (dst.swizzled)
	{
		const u8* linear_pixels = src_pixels;
		u32 linear_pitch = src_pitch;
		u16 swizzle_w = dst.width, swizzle_h = dst.height;

		if (need_convert)
		{
			temp.resize(static_cast<usz>(dst.pitch) * std::max<u32>(dst.height, dst.clip_height));
			rsx::convert_scale_image(temp.data(), ffmpeg_dst_format, dst.width, dst.height, dst.pitch,
				src_pixels, ffmpeg_src_format, src.width, src.height, src_pitch, slice_h, interpolate);
			linear_pixels = temp.data(); linear_pitch = dst.pitch;
		}

		if (need_clip)
		{
			swz_temp.resize(static_cast<usz>(dst.pitch) * dst.clip_height);
			rsx::clip_image(swz_temp.data(), linear_pixels, dst.clip_x, dst.clip_y, dst.clip_width, dst.clip_height, dst.bpp, linear_pitch, dst.pitch);
			linear_pixels = swz_temp.data(); linear_pitch = dst.pitch;
			swizzle_w = dst.clip_width; swizzle_h = dst.clip_height;
		}

		if (!swizzle_w || !swizzle_h) return false;

		u32 sw_width = rsx::next_pow2(swizzle_w);
		u32 sw_height = rsx::next_pow2(swizzle_h);
		std::vector<u8> padded;

		if (sw_width != swizzle_w || sw_height != swizzle_h)
		{
			padded.resize(static_cast<usz>(dst.bpp) * sw_width * sw_height);
			switch (dst.bpp)
			{
			case 1: rsx::pad_texture<u8>(linear_pixels, padded.data(), swizzle_w, swizzle_h, sw_width, sw_height); break;
			case 2: rsx::pad_texture<u16>(linear_pixels, padded.data(), swizzle_w, swizzle_h, sw_width, sw_height); break;
			case 4: rsx::pad_texture<u32>(linear_pixels, padded.data(), swizzle_w, swizzle_h, sw_width, sw_height); break;
			default: return false;
			}
			linear_pixels = padded.data(); linear_pitch = sw_width * dst.bpp;
		}

		switch (dst.bpp)
		{
		case 1: rsx::convert_linear_swizzle<u8, false>(linear_pixels, dst_pixels_out, sw_width, sw_height, linear_pitch); break;
		case 2: rsx::convert_linear_swizzle<u16, false>(linear_pixels, dst_pixels_out, sw_width, sw_height, linear_pitch); break;
		case 4: rsx::convert_linear_swizzle<u32, false>(linear_pixels, dst_pixels_out, sw_width, sw_height, linear_pitch); break;
		default: return false;
		}

		if (tiled_region)
		{
			const auto tile_func = (dst.bpp == 4) ? rsx::tile_texel_data32 : rsx::tile_texel_data16;
			tile_func(real_dst_pixels, dst_pixels_out, tiled_region.base_address,
				dst.rsx_address - tiled_region.base_address, tiled_region.tile->size,
				tiled_region.tile->bank, tiled_region.tile->pitch, dst.clip_width, dst.clip_height);
		}

		rsx::mm_flush();
		return true;
	}

	if (!need_convert)
	{
		const bool is_overlapping = !src_is_temp && (dst.dma == src.dma) && [&]() -> bool
		{
			const auto src_range = utils::address_range32::start_length(src.rsx_address, src.pitch * (src.height - 1) + (src.bpp * src.width));
			const auto dst_range = utils::address_range32::start_length(dst.rsx_address, dst.pitch * (dst.clip_height - 1) + (dst.bpp * dst.clip_width));
			return src_range.overlaps(dst_range);
		}();

		if (is_overlapping)
		{
			if (need_clip)
			{
				temp.resize(static_cast<usz>(dst.pitch) * dst.clip_height);
				rsx::clip_image_may_overlap(dst_pixels_out, src_pixels, dst.clip_x, dst.clip_y, dst.clip_width, dst.clip_height, dst.bpp, src_pitch, dst.pitch, temp.data());
			}
			else if (dst.pitch != src_pitch || dst.pitch != dst.bpp * dst.width)
			{
				const u32 buffer_pitch = dst.bpp * dst.width;
				temp.resize(static_cast<usz>(buffer_pitch) * dst.height);
				u8* buf = temp.data(); const u8* pixels = src_pixels;
				for (u32 y = 0; y < dst.height; ++y) { std::memcpy(buf, pixels, buffer_pitch); pixels += src_pitch; buf += buffer_pitch; }
				buf = temp.data(); u8* dst_ptr = dst_pixels_out;
				for (u32 y = 0; y < dst.height; ++y) { std::memcpy(dst_ptr, buf, buffer_pitch); dst_ptr += dst.pitch; buf += buffer_pitch; }
			}
			else
			{
				std::memmove(dst_pixels_out, src_pixels, static_cast<usz>(dst.pitch) * dst.height);
			}
		}
		else if (need_clip)
		{
			rsx::clip_image(dst_pixels_out, src_pixels, dst.clip_x, dst.clip_y, dst.clip_width, dst.clip_height, dst.bpp, src_pitch, dst.pitch);
		}
		else if (dst.pitch != src_pitch || dst.pitch != dst.bpp * dst.width)
		{
			u8* dst_p = dst_pixels_out; const u8* src_row = src_pixels;
			for (u32 y = 0; y < dst.height; ++y) { std::memcpy(dst_p, src_row, static_cast<usz>(dst.width) * dst.bpp); dst_p += dst.pitch; src_row += src_pitch; }
		}
		else
		{
			std::memcpy(dst_pixels_out, src_pixels, static_cast<usz>(dst.pitch) * dst.height);
		}
	}
	else
	{
		if (need_clip)
		{
			temp.resize(static_cast<usz>(dst.pitch) * std::max<u32>(dst.height, dst.clip_height));
			rsx::convert_scale_image(temp.data(), ffmpeg_dst_format, dst.width, dst.height, dst.pitch,
				src_pixels, ffmpeg_src_format, src.width, src.height, src_pitch, slice_h, interpolate);
			rsx::clip_image(dst_pixels_out, temp.data(), dst.clip_x, dst.clip_y, dst.clip_width, dst.clip_height, dst.bpp, dst.pitch, dst.pitch);
		}
		else
		{
			rsx::convert_scale_image(dst_pixels_out, ffmpeg_dst_format, dst.width, dst.height, dst.pitch,
				src_pixels, ffmpeg_src_format, src.width, src.height, src_pitch, slice_h, interpolate);
		}
	}

	if (tiled_region)
	{
		const auto tile_func = (dst.bpp == 4) ? rsx::tile_texel_data32 : rsx::tile_texel_data16;
		tile_func(real_dst_pixels, dst_pixels_out, tiled_region.base_address,
			dst.rsx_address - tiled_region.base_address, tiled_region.tile->size,
			tiled_region.tile->bank, tiled_region.tile->pitch, dst.clip_width, dst.clip_height);
	}

	rsx::mm_flush();
	return true;
}

bool DX9GSRender::release_GCM_label(u32, u32, u32)
{
	return false;
}

bool DX9GSRender::is_current_program_interpreted() const
{
	return current_fp_metadata.has_branch_instructions || current_fp_metadata.has_pack_instructions;
}

void DX9GSRender::begin_occlusion_query(rsx::reports::occlusion_query_info* query)
{
	if (!m_device || !query) return;

	IDirect3DQuery9* d3d_query = nullptr;
	if (auto found = m_occlusion_queries.find(query); found != m_occlusion_queries.end())
		d3d_query = found->second;
	else if (rsx::dx9::check(m_device->CreateQuery(D3DQUERYTYPE_OCCLUSION, &d3d_query), "CreateQuery(OCCLUSION)") && d3d_query)
		m_occlusion_queries.emplace(query, d3d_query);

	if (d3d_query) d3d_query->Issue(D3DISSUE_BEGIN);
}

void DX9GSRender::end_occlusion_query(rsx::reports::occlusion_query_info* query)
{
	if (!query) return;
	if (auto found = m_occlusion_queries.find(query); found != m_occlusion_queries.end() && found->second)
		found->second->Issue(D3DISSUE_END);
}

bool DX9GSRender::check_occlusion_query_status(rsx::reports::occlusion_query_info* query)
{
	if (!query) return true;
	if (auto found = m_occlusion_queries.find(query); found != m_occlusion_queries.end() && found->second)
	{
		DWORD visible = 0;
		const HRESULT hr = found->second->GetData(&visible, sizeof(visible), D3DGETDATA_FLUSH);
		return hr == S_OK || hr == D3DERR_INVALIDCALL;
	}
	return true;
}

void DX9GSRender::get_occlusion_query_result(rsx::reports::occlusion_query_info* query)
{
	if (!query) return;
	if (auto found = m_occlusion_queries.find(query); found != m_occlusion_queries.end() && found->second)
	{
		DWORD visible = 0;
		found->second->GetData(&visible, sizeof(visible), D3DGETDATA_FLUSH);
	}
}

void DX9GSRender::discard_occlusion_query(rsx::reports::occlusion_query_info* query)
{
	if (!query) return;
	if (auto found = m_occlusion_queries.find(query); found != m_occlusion_queries.end())
	{
		if (found->second) enqueue_deferred_release(found->second);
		m_occlusion_queries.erase(found);
	}
}

void DX9GSRender::upload_fragment_constants(const DX9FragmentProgram* fp)
{
	if (!fp || !m_device) return;
	const u32 count = static_cast<u32>(fp->constant_offsets.size());
	if (count == 0) return;

	std::vector<f32> constants(count * 4);
	rsx::write_fragment_constants_to_buffer(std::span<f32>(constants), current_fragment_program, fp->constant_offsets);

	m_device->SetPixelShaderConstantF(0, constants.data(), count);
}

#endif
