#include "stdafx.h"

#ifdef HAVE_D3D9

#include "DX9ShaderCache.h"
#include "DX9Helpers.h"

namespace rsx::dx9
{
	shader_cache::~shader_cache()
	{
		clear();
	}

	void shader_cache::clear_vs()
	{
		for (auto& [_, shader] : m_vs)
		{
			if (shader)
			{
				shader->Release();
			}
		}
		m_vs.clear();
	}

	void shader_cache::clear_ps()
	{
		for (auto& [_, shader] : m_ps)
		{
			if (shader)
			{
				shader->Release();
			}
		}
		m_ps.clear();
	}

	void shader_cache::clear()
	{
		clear_vs();
		clear_ps();
	}

	IDirect3DVertexShader9* shader_cache::compile_vertex(const std::string& key, const std::string& source, const char* entry)
	{
		if (!m_device)
		{
			return nullptr;
		}

		if (auto found = m_vs.find(key); found != m_vs.end())
		{
			return found->second;
		}

		ID3DBlob* bytecode = nullptr;
		ID3DBlob* errors = nullptr;

		const HRESULT hr = D3DCompile(source.data(), source.size(), key.c_str(), nullptr, nullptr, entry, "vs_3_0", 0, 0, &bytecode, &errors);
		if (FAILED(hr))
		{
			if (errors)
			{
				rsx_log.error("DX9 vertex shader compile error (%s): %s", key, static_cast<const char*>(errors->GetBufferPointer()));
				errors->Release();
			}
			return nullptr;
		}
		if (errors)
		{
			errors->Release();
		}

		IDirect3DVertexShader9* shader = nullptr;
		if (!check(m_device->CreateVertexShader(static_cast<const DWORD*>(bytecode->GetBufferPointer()), &shader), "CreateVertexShader"))
		{
			bytecode->Release();
			return nullptr;
		}

		bytecode->Release();
		m_vs.emplace(key, shader);
		return shader;
	}

	IDirect3DPixelShader9* shader_cache::compile_pixel(const std::string& key, const std::string& source, const char* entry)
	{
		if (!m_device)
		{
			return nullptr;
		}

		if (auto found = m_ps.find(key); found != m_ps.end())
		{
			return found->second;
		}

		ID3DBlob* bytecode = nullptr;
		ID3DBlob* errors = nullptr;

		const HRESULT hr = D3DCompile(source.data(), source.size(), key.c_str(), nullptr, nullptr, entry, "ps_3_0", 0, 0, &bytecode, &errors);
		if (FAILED(hr))
		{
			if (errors)
			{
				rsx_log.error("DX9 pixel shader compile error (%s): %s", key, static_cast<const char*>(errors->GetBufferPointer()));
				errors->Release();
			}
			return nullptr;
		}
		if (errors)
		{
			errors->Release();
		}

		IDirect3DPixelShader9* shader = nullptr;
		if (!check(m_device->CreatePixelShader(static_cast<const DWORD*>(bytecode->GetBufferPointer()), &shader), "CreatePixelShader"))
		{
			bytecode->Release();
			return nullptr;
		}

		bytecode->Release();
		m_ps.emplace(key, shader);
		return shader;
	}
}

#endif
