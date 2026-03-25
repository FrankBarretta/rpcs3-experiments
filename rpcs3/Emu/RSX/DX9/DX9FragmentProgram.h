#pragma once

#ifdef HAVE_D3D9

#include "Emu/RSX/Program/HLSL/HLSL9FragmentProgramDecompiler.h"

#include <d3d9.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

class DX9FragmentProgram
{
public:
	DX9FragmentProgram();
	~DX9FragmentProgram();

	std::string shader_source;
	Microsoft::WRL::ComPtr<ID3DBlob> shader_bytecode;
	IDirect3DPixelShader9* dx9_shader = nullptr;

	std::vector<u32> constant_offsets;

	void Decompile(const RSXFragmentProgram& prog, u32 program_size_hint);
	bool Compile();
	bool CreateDX9Shader(IDirect3DDevice9* device);
	void Delete();
};

#endif
