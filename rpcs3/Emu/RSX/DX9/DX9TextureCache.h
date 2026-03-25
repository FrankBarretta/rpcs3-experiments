#pragma once

#ifdef HAVE_D3D9

#include <d3d9.h>
#include <unordered_map>

namespace rsx::dx9
{
	class texture_cache
	{
		IDirect3DDevice9* m_device = nullptr;
		std::unordered_map<u32, IDirect3DTexture9*> m_cache;

	public:
		~texture_cache();

		void bind_device(IDirect3DDevice9* device) { m_device = device; }
		void clear();
		IDirect3DTexture9* ensure_texture(u32 key, u32 width, u32 height, D3DFORMAT format);
	};
}

#endif
