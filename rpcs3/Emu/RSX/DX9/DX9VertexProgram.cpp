#include "stdafx.h"

#ifdef HAVE_D3D9

#include "DX9VertexProgram.h"
#include "util/logs.hpp"

#include <set>

LOG_CHANNEL(dx9_vp_log, "DX9VP");

// Semantic table: RSX attr index -> D3D9 decl usage
static const DX9VertexSemanticEntry s_semantic_table[16] =
{
	{ D3DDECLUSAGE_POSITION,     0 },  // v[0]  in_pos
	{ D3DDECLUSAGE_BLENDWEIGHT,  0 },  // v[1]  in_weight
	{ D3DDECLUSAGE_NORMAL,       0 },  // v[2]  in_normal
	{ D3DDECLUSAGE_COLOR,        0 },  // v[3]  in_diff_color
	{ D3DDECLUSAGE_COLOR,        1 },  // v[4]  in_spec_color
	{ D3DDECLUSAGE_TEXCOORD,     5 },  // v[5]  in_fog
	{ D3DDECLUSAGE_PSIZE,        0 },  // v[6]  in_point_size
	{ D3DDECLUSAGE_BLENDINDICES, 0 },  // v[7]  in_7
	{ D3DDECLUSAGE_TEXCOORD,     0 },  // v[8]  in_tc0
	{ D3DDECLUSAGE_TEXCOORD,     1 },  // v[9]  in_tc1
	{ D3DDECLUSAGE_TEXCOORD,     2 },  // v[10] in_tc2
	{ D3DDECLUSAGE_TEXCOORD,     3 },  // v[11] in_tc3
	{ D3DDECLUSAGE_TEXCOORD,     4 },  // v[12] in_tc4
	{ D3DDECLUSAGE_TEXCOORD,     6 },  // v[13] in_tc5
	{ D3DDECLUSAGE_TEXCOORD,     7 },  // v[14] in_tc6
	{ D3DDECLUSAGE_TANGENT,      0 },  // v[15] in_tc7
};

const DX9VertexSemanticEntry& dx9_get_vertex_semantic(u32 rsx_attr_index)
{
	return s_semantic_table[rsx_attr_index & 0xF];
}

DX9VertexProgram::DX9VertexProgram() = default;
DX9VertexProgram::~DX9VertexProgram() { Delete(); }

// Post-process HLSL source to fix GLSL->HLSL incompatibilities.
static void hlsl_fixup_scalar_constructors(std::string& src)
{
	static const char* type_prefixes[] = { "float4(", "float3(", "float2(" };
	static const char* cast_prefixes[] = { "((float4)(", "((float3)(", "((float2)(" };

	for (int t = 0; t < 3; t++)
	{
		const std::string prefix(type_prefixes[t]);
		const std::string cast_prefix(cast_prefixes[t]);
		usz pos = 0;

		while ((pos = src.find(prefix, pos)) != std::string::npos)
		{
			if (pos > 0 && (std::isalnum(src[pos - 1]) || src[pos - 1] == '_'))
			{
				pos += prefix.size();
				continue;
			}

			usz start = pos + prefix.size();
			int depth = 1;
			usz end = start;
			bool has_comma = false;
			while (end < src.size() && depth > 0)
			{
				if (src[end] == '(') depth++;
				else if (src[end] == ')') depth--;
				else if (src[end] == ',' && depth == 1) has_comma = true;
				if (depth > 0) end++;
			}

			if (depth == 0 && !has_comma)
			{
				std::string expr = src.substr(start, end - start);
				std::string replacement = cast_prefix + expr + "))";
				src.replace(pos, end + 1 - pos, replacement);
				pos += replacement.size();
			}
			else
			{
				pos += prefix.size();
			}
		}
	}
}

static constexpr u16 DX9_VP_CONST_ARRAY_SIZE = 240;

static std::unordered_map<u16, u16> hlsl_relocate_high_constants(std::string& src, u16 max_slot = DX9_VP_CONST_ARRAY_SIZE)
{
	std::unordered_map<u16, u16> relocation_map;

	std::set<u16> high_constants;
	const std::string prefix = "_fetch_constant(";
	usz pos = 0;
	while ((pos = src.find(prefix, pos)) != std::string::npos)
	{
		const usz arg_start = pos + prefix.size();
		const usz paren_close = src.find(')', arg_start);
		if (paren_close == std::string::npos) break;

		const std::string arg = src.substr(arg_start, paren_close - arg_start);

		bool is_literal = !arg.empty();
		for (char c : arg)
		{
			if (!std::isdigit(c)) { is_literal = false; break; }
		}

		if (is_literal)
		{
			const u16 idx = static_cast<u16>(std::stoi(arg));
			if (idx >= max_slot)
			{
				high_constants.insert(idx);
			}
		}
		pos = paren_close + 1;
	}

	if (high_constants.empty())
		return relocation_map;

	u16 slot = max_slot - 1;
	std::vector<std::pair<std::string, std::string>> replacements;

	for (auto it = high_constants.rbegin(); it != high_constants.rend(); ++it)
	{
		const u16 high_idx = *it;
		relocation_map[slot] = high_idx;

		replacements.emplace_back(
			"_fetch_constant(" + std::to_string(high_idx) + ")",
			"_fetch_constant(" + std::to_string(slot) + ")");
		slot--;
	}

	for (const auto& [from, to] : replacements)
	{
		usz p = 0;
		while ((p = src.find(from, p)) != std::string::npos)
		{
			src.replace(p, from.size(), to);
			p += to.size();
		}
	}

	return relocation_map;
}

void DX9VertexProgram::Decompile(const RSXVertexProgram& prog)
{
	rsx::program::hlsl9::vertex_program_decompiler decompiler(prog);
	shader_source = decompiler.Decompile();

	hlsl_fixup_scalar_constructors(shader_source);

	has_indexed_constants = decompiler.properties.has_indexed_constants;
	constant_ids = std::vector<u16>(decompiler.m_constant_ids.begin(), decompiler.m_constant_ids.end());

	if (has_indexed_constants)
	{
		high_const_relocation = hlsl_relocate_high_constants(shader_source);
	}
}

bool DX9VertexProgram::Compile()
{
	if (shader_source.empty())
		return false;

	Microsoft::WRL::ComPtr<ID3DBlob> errors;
	HRESULT hr = D3DCompile(
		shader_source.c_str(), shader_source.size(),
		"RSX_VP", nullptr, nullptr,
		"main", "vs_3_0",
		D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_PACK_MATRIX_ROW_MAJOR,
		0, &shader_bytecode, &errors);

	if (FAILED(hr))
	{
		const char* err = errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown";
		dx9_vp_log.error("HLSL compile failed: %s", err);
		dx9_vp_log.error("Source:\n%s", shader_source);
		return false;
	}

	dx9_vp_log.success("Compiled vertex shader (%u bytes, %u constants)",
		static_cast<u32>(shader_bytecode->GetBufferSize()),
		static_cast<u32>(constant_ids.size()));
	return true;
}

bool DX9VertexProgram::CreateDX9Shader(IDirect3DDevice9* device)
{
	if (!shader_bytecode || !device) return false;
	if (dx9_shader) return true;

	HRESULT hr = device->CreateVertexShader(
		static_cast<const DWORD*>(shader_bytecode->GetBufferPointer()),
		&dx9_shader);

	if (FAILED(hr))
	{
		dx9_vp_log.error("CreateVertexShader failed (hr=0x%08x, bytecode=%u bytes, relocations=%u)",
			static_cast<u32>(hr),
			static_cast<u32>(shader_bytecode->GetBufferSize()),
			static_cast<u32>(high_const_relocation.size()));
		dx9_vp_log.error("Source:\n%s", shader_source);
		return false;
	}
	return true;
}

void DX9VertexProgram::Delete()
{
	if (dx9_shader) { dx9_shader->Release(); dx9_shader = nullptr; }
	shader_bytecode.Reset();
	shader_source.clear();
}

#endif
