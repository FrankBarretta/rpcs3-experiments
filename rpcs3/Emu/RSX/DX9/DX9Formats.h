#pragma once

#ifdef HAVE_D3D9

#include <d3d9.h>

namespace rsx::dx9
{
	D3DFORMAT to_color_format(u32 rsx_color_format);
	D3DFORMAT to_depth_format(u32 rsx_depth_format);
}

#endif
