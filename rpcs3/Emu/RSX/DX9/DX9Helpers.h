#pragma once

#ifdef HAVE_D3D9

#include <d3d9.h>

#include "util/logs.hpp"
#include "util/types.hpp"

namespace rsx::dx9
{
	bool check(HRESULT hr, const char* what);

	const char* format_to_string(D3DFORMAT fmt);
}

#endif
