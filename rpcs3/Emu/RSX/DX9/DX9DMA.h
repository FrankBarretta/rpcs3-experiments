#pragma once

#ifdef HAVE_D3D9

#include <d3d9.h>

namespace rsx::dx9
{
	class dma_manager
	{
		IDirect3DDevice9* m_device = nullptr;

	public:
		void bind_device(IDirect3DDevice9* device) { m_device = device; }

		// Placeholder entry point for future RSX host DMA uploads.
		bool upload_to_texture(IDirect3DTexture9* dst, const void* src, u32 src_size);
	};
}

#endif
