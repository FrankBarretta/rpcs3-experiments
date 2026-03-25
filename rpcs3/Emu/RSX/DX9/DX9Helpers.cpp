#include "stdafx.h"

#ifdef HAVE_D3D9

#include "DX9Helpers.h"

LOG_CHANNEL(dx9_log, "DX9");

namespace rsx::dx9
{
	bool check(HRESULT hr, const char* what)
	{
		if (SUCCEEDED(hr))
		{
			return true;
		}

		dx9_log.error("%s failed with HRESULT=0x%08x", what, static_cast<u32>(hr));
		return false;
	}

	const char* format_to_string(D3DFORMAT fmt)
	{
		switch (fmt)
		{
		case D3DFMT_A8R8G8B8: return "A8R8G8B8";
		case D3DFMT_X8R8G8B8: return "X8R8G8B8";
		case D3DFMT_D24S8: return "D24S8";
		case D3DFMT_D16: return "D16";
		default: return "Unknown";
		}
	}
}

#endif
