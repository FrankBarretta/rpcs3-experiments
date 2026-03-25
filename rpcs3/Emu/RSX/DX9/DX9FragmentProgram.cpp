#include "stdafx.h"

#ifdef HAVE_D3D9

#include "DX9FragmentProgram.h"
#include "util/logs.hpp"

LOG_CHANNEL(dx9_fp_log, "DX9FP");

DX9FragmentProgram::DX9FragmentProgram() = default;
DX9FragmentProgram::~DX9FragmentProgram()
{
	Delete();
}

void DX9FragmentProgram::Decompile(const RSXFragmentProgram& prog, u32 program_size_hint)
{
	u32 size = program_size_hint ? program_size_hint : prog.ucode_length;
	rsx::program::hlsl9::fragment_program_decompiler decompiler(prog, size);
	shader_source = decompiler.Decompile();
	constant_offsets = decompiler.properties.constant_offsets;
}

bool DX9FragmentProgram::Compile()
{
	if (shader_source.empty())
	{
		return false;
	}

	Microsoft::WRL::ComPtr<ID3DBlob> errors;
	const HRESULT hr = D3DCompile(
		shader_source.c_str(), shader_source.size(),
		"RSX_FP", nullptr, nullptr,
		"main", "ps_3_0",
		D3DCOMPILE_OPTIMIZATION_LEVEL3,
		0, &shader_bytecode, &errors);

	if (FAILED(hr))
	{
		const char* err = errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown";
		dx9_fp_log.error("HLSL fragment compile failed: %s", err);
		std::stringstream numbered;
		u32 line = 1;
		numbered << line << ":\t";
		for (const char c : shader_source)
		{
			numbered << c;
			if (c == '\n')
			{
				++line;
				numbered << line << ":\t";
			}
		}
		dx9_fp_log.error("HLSL source dump:\n%s", numbered.str());
		return false;
	}

	return true;
}

bool DX9FragmentProgram::CreateDX9Shader(IDirect3DDevice9* device)
{
	if (!shader_bytecode || !device)
	{
		return false;
	}

	if (dx9_shader)
	{
		return true;
	}

	const HRESULT hr = device->CreatePixelShader(
		static_cast<const DWORD*>(shader_bytecode->GetBufferPointer()),
		&dx9_shader);

	if (FAILED(hr))
	{
		dx9_fp_log.error("CreatePixelShader failed (hr=0x%08x)", static_cast<u32>(hr));
		return false;
	}

	return true;
}

void DX9FragmentProgram::Delete()
{
	if (dx9_shader)
	{
		dx9_shader->Release();
		dx9_shader = nullptr;
	}

	shader_bytecode.Reset();
	shader_source.clear();
}

#endif
