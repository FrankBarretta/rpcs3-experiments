#include "stdafx.h"

#ifdef HAVE_D3D9

#include "DX9DMA.h"

namespace rsx::dx9
{
	bool dma_manager::upload_to_texture(IDirect3DTexture9* dst, const void* src, u32 src_size)
	{
		if (!m_device || !dst || !src || !src_size)
		{
			return false;
		}

		D3DLOCKED_RECT rect{};
		if (FAILED(dst->LockRect(0, &rect, nullptr, D3DLOCK_DISCARD)))
		{
			return false;
		}

		std::memcpy(rect.pBits, src, src_size);
		dst->UnlockRect(0);
		return true;
	}
}

#endif
