#include "stdafx.h"

#ifdef HAVE_D3D9

#include "DX9TextureCache.h"
#include "DX9Helpers.h"

namespace rsx::dx9
{
	texture_cache::~texture_cache()
	{
		clear();
	}

	void texture_cache::clear()
	{
		for (auto& [_, tex] : m_cache)
		{
			if (tex)
			{
				tex->Release();
			}
		}
		m_cache.clear();
	}

	IDirect3DTexture9* texture_cache::ensure_texture(u32 key, u32 width, u32 height, D3DFORMAT format)
	{
		if (!m_device)
		{
			return nullptr;
		}

		if (auto found = m_cache.find(key); found != m_cache.end())
		{
			return found->second;
		}

		IDirect3DTexture9* texture = nullptr;
		if (!check(m_device->CreateTexture(width, height, 1, D3DUSAGE_DYNAMIC, format, D3DPOOL_DEFAULT, &texture, nullptr), "CreateTexture"))
		{
			return nullptr;
		}

		m_cache.emplace(key, texture);
		return texture;
	}
}

#endif
