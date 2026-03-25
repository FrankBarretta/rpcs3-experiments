#pragma once

#ifdef HAVE_D3D9

#include <d3d9.h>

namespace rsx::dx9
{
	class present_manager
	{
		IDirect3DDevice9* m_device = nullptr;

	public:
		void bind(IDirect3DDevice9* device)
		{
			m_device = device;
		}

		bool present();
	};
}

#endif
