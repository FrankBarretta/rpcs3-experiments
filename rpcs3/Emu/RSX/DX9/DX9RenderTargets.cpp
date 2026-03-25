#include "stdafx.h"

#ifdef HAVE_D3D9

#include "DX9RenderTargets.h"
#include "DX9Helpers.h"

namespace rsx::dx9
{
	render_targets::~render_targets()
	{
		release();
	}

	void render_targets::release()
	{
		if (m_color)
		{
			m_color->Release();
			m_color = nullptr;
		}

		if (m_depth)
		{
			m_depth->Release();
			m_depth = nullptr;
		}
	}

	bool render_targets::ensure(u32 width, u32 height, D3DFORMAT color_fmt, D3DFORMAT depth_fmt)
	{
		if (!m_device)
		{
			return false;
		}

		release();

		if (!check(m_device->CreateRenderTarget(width, height, color_fmt, D3DMULTISAMPLE_NONE, 0, FALSE, &m_color, nullptr), "CreateRenderTarget"))
		{
			return false;
		}

		if (!check(m_device->CreateDepthStencilSurface(width, height, depth_fmt, D3DMULTISAMPLE_NONE, 0, TRUE, &m_depth, nullptr), "CreateDepthStencilSurface"))
		{
			return false;
		}

		return true;
	}

	bool render_targets::apply()
	{
		if (!m_device || !m_color || !m_depth)
		{
			return false;
		}

		if (!check(m_device->SetRenderTarget(0, m_color), "SetRenderTarget"))
		{
			return false;
		}

		return check(m_device->SetDepthStencilSurface(m_depth), "SetDepthStencilSurface");
	}
} // namespace rsx::dx9

#endif
