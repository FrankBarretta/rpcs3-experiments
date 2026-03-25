#pragma once

#ifdef HAVE_D3D9

#include <d3d9.h>

namespace rsx::dx9
{
	class render_targets
	{
		IDirect3DDevice9* m_device = nullptr;
		IDirect3DSurface9* m_color = nullptr;
		IDirect3DSurface9* m_depth = nullptr;

	public:
		~render_targets();

		void bind_device(IDirect3DDevice9* device)
		{
			m_device = device;
		}
		void release();
		bool ensure(u32 width, u32 height, D3DFORMAT color_fmt, D3DFORMAT depth_fmt);
		bool apply();
	};
} // namespace rsx::dx9

#endif
