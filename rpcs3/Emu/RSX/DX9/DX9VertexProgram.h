#pragma once

#ifdef HAVE_D3D9

#include "Emu/RSX/Program/HLSL/HLSL9VertexProgramDecompiler.h"
#include "Emu/RSX/Program/program_util.h"

#include <d3d9.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <unordered_map>

/**
 * Compiled DX9 vertex program.
 * Uses rsx::program::hlsl9::vertex_program_decompiler for RSX->HLSL translation,
 * then compiles to D3D9 bytecode via D3DCompile.
 */
class DX9VertexProgram : public rsx::VertexProgramBase
{
public:
	DX9VertexProgram();
	~DX9VertexProgram();

	std::string shader_source;
	Microsoft::WRL::ComPtr<ID3DBlob> shader_bytecode;
	IDirect3DVertexShader9* dx9_shader = nullptr;

	// Map of relocated high-index constants: relocation_slot -> original_rsx_index.
	// D3D9 SM3.0 supports max 256 float4 constants, but RSX uses up to 468.
	// We reserve c0-c239 for vc[] (240 slots) and leave c240-c255 for the HLSL
	// compiler's 'def' constants. Literal accesses >= 240 are remapped here.
	std::unordered_map<u16, u16> high_const_relocation;

	void Decompile(const RSXVertexProgram& prog);
	bool Compile();
	bool CreateDX9Shader(IDirect3DDevice9* device);
	void Delete();
};

// Semantic mapping for RSX vertex attributes v[0]-v[15]
struct DX9VertexSemanticEntry
{
	D3DDECLUSAGE usage;
	BYTE usage_index;
};

const DX9VertexSemanticEntry& dx9_get_vertex_semantic(u32 rsx_attr_index);

#endif
