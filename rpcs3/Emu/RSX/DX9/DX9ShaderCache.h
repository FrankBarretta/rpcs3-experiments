#pragma once

#ifdef HAVE_D3D9

#include <d3d9.h>
#include <d3dcompiler.h>
#include <string>
#include <unordered_map>

namespace rsx::dx9
{
	class shader_cache
	{
		IDirect3DDevice9* m_device = nullptr;
		std::unordered_map<std::string, IDirect3DVertexShader9*> m_vs;
		std::unordered_map<std::string, IDirect3DPixelShader9*> m_ps;

		void clear_vs();
		void clear_ps();

	public:
		~shader_cache();
		void bind_device(IDirect3DDevice9* device) { m_device = device; }
		void clear();
		IDirect3DVertexShader9* compile_vertex(const std::string& key, const std::string& source, const char* entry = "main");
		IDirect3DPixelShader9* compile_pixel(const std::string& key, const std::string& source, const char* entry = "main");
	};
}

#endif
