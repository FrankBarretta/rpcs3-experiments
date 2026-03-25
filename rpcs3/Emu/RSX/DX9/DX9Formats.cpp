#include "stdafx.h"

#ifdef HAVE_D3D9

#include "DX9Formats.h"

namespace rsx::dx9
{
	D3DFORMAT to_color_format(u32)
	{
		// Default safe color format used by the bootstrap backend.
		return D3DFMT_A8R8G8B8;
	}

	D3DFORMAT to_depth_format(u32)
	{
		// Default safe depth format used by the bootstrap backend.
		return D3DFMT_D24S8;
	}
}

#endif
