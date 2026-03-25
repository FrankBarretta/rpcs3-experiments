#include "stdafx.h"

#ifdef HAVE_D3D9

#include "DX9Present.h"
#include "DX9Helpers.h"

namespace rsx::dx9
{
	bool present_manager::present()
	{
		if (!m_device)
		{
			return false;
		}

		return check(m_device->Present(nullptr, nullptr, nullptr, nullptr), "IDirect3DDevice9::Present");
	}
}

#endif
