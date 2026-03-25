#include "stdafx.h"

#ifdef HAVE_D3D9

#include "HLSL9VertexProgramDecompiler.h"
#include "Emu/RSX/gcm_enums.h"

namespace rsx::program::hlsl9
{
	static std::string sanitize_hlsl_initializer(const std::string& type, const std::string& value)
	{
		if (value == type + "(0.)" || value == type + "(0.0)" || value == type + "(0.0f)" || value == type + "(0)")
		{
			return "((" + type + ")0)";
		}

		return value;
	}

	std::string vertex_program_decompiler::getFloatTypeName(usz elementCount)
	{
		switch (elementCount)
		{
		default:
		case 4: return "float4";
		case 3: return "float3";
		case 2: return "float2";
		case 1: return "float";
		}
	}

	std::string vertex_program_decompiler::getIntTypeName(usz elementCount)
	{
		switch (elementCount)
		{
		default:
		case 4: return "int4";
		case 3: return "int3";
		case 2: return "int2";
		case 1: return "int";
		}
	}

	std::string vertex_program_decompiler::getFunction(FUNCTION f)
	{
		switch (f)
		{
		case FUNCTION::DP2:    return "(($Ty)dot($0.xy, $1.xy))";
		case FUNCTION::DP2A:   return "(($Ty)(dot($0.xy, $1.xy) + $2.x))";
		case FUNCTION::DP3:    return "(($Ty)dot($0.xyz, $1.xyz))";
		case FUNCTION::DP4:    return "(($Ty)dot($0, $1))";
		case FUNCTION::DPH:    return "(($Ty)dot(float4($0.xyz, 1.0), $1))";
		case FUNCTION::SFL:    return "(($Ty)0.)";
		case FUNCTION::STR:    return "(($Ty)1.)";
		case FUNCTION::FRACT:  return "frac($0)";
		case FUNCTION::REFL:   return "reflect($0, $1)";
		case FUNCTION::DFDX:   return "ddx($0)";
		case FUNCTION::DFDY:   return "ddy($0)";
		case FUNCTION::VERTEX_TEXTURE_FETCH1D:  return "tex1Dlod($t, float4($0.x, 0, 0, 0))";
		case FUNCTION::VERTEX_TEXTURE_FETCH2D:  return "tex2Dlod($t, float4($0.xy, 0, 0))";
		case FUNCTION::VERTEX_TEXTURE_FETCH3D:
		case FUNCTION::VERTEX_TEXTURE_FETCHCUBE: return "tex3Dlod($t, float4($0.xyz, 0))";
		default: return "(($Ty)0.)";
		}
	}

	std::string vertex_program_decompiler::compareFunction(COMPARE f, const std::string& a, const std::string& b, bool scalar)
	{
		if (scalar)
		{
			switch (f)
			{
			case COMPARE::SEQ: return fmt::format("((%s) == (%s) ? 1.0 : 0.0)", a, b);
			case COMPARE::SGE: return fmt::format("((%s) >= (%s) ? 1.0 : 0.0)", a, b);
			case COMPARE::SGT: return fmt::format("((%s) > (%s) ? 1.0 : 0.0)", a, b);
			case COMPARE::SLE: return fmt::format("((%s) <= (%s) ? 1.0 : 0.0)", a, b);
			case COMPARE::SLT: return fmt::format("((%s) < (%s) ? 1.0 : 0.0)", a, b);
			case COMPARE::SNE: return fmt::format("((%s) != (%s) ? 1.0 : 0.0)", a, b);
			}
		}
		else
		{
			switch (f)
			{
			case COMPARE::SEQ: return "_cmp_eq(" + a + ", " + b + ")";
			case COMPARE::SGE: return "step(" + b + ", " + a + ")";
			case COMPARE::SGT: return "_cmp_gt(" + a + ", " + b + ")";
			case COMPARE::SLE: return "step(" + a + ", " + b + ")";
			case COMPARE::SLT: return "_cmp_lt(" + a + ", " + b + ")";
			case COMPARE::SNE: return "_cmp_ne(" + a + ", " + b + ")";
			}
		}
		return "float4(0,0,0,0)";
	}

	void vertex_program_decompiler::insertHeader(std::stringstream& OS)
	{
		OS << "// DX9 HLSL Vertex Shader (SM 3.0) - Decompiled from RSX\n\n";
		OS << "#define fma(a, b, c) mad((a), (b), (c))\n";

		OS << "float4 _cmp_eq(float4 a, float4 b) { return float4(a.x==b.x?1:0, a.y==b.y?1:0, a.z==b.z?1:0, a.w==b.w?1:0); }\n";
		OS << "float4 _cmp_gt(float4 a, float4 b) { return float4(a.x>b.x?1:0, a.y>b.y?1:0, a.z>b.z?1:0, a.w>b.w?1:0); }\n";
		OS << "float4 _cmp_lt(float4 a, float4 b) { return float4(a.x<b.x?1:0, a.y<b.y?1:0, a.z<b.z?1:0, a.w<b.w?1:0); }\n";
		OS << "float4 _cmp_ne(float4 a, float4 b) { return float4(a.x!=b.x?1:0, a.y!=b.y?1:0, a.z!=b.z?1:0, a.w!=b.w?1:0); }\n";

		OS << "float4 _select(float4 a, float4 b, float4 c) { return float4(c.x?b.x:a.x, c.y?b.y:a.y, c.z?b.z:a.z, c.w?b.w:a.w); }\n";
		OS << "float3 _select(float3 a, float3 b, float3 c) { return float3(c.x?b.x:a.x, c.y?b.y:a.y, c.z?b.z:a.z); }\n";
		OS << "float2 _select(float2 a, float2 b, float2 c) { return float2(c.x?b.x:a.x, c.y?b.y:a.y); }\n";
		OS << "float _select(float a, float b, float c) { return c?b:a; }\n";

		OS << "#define CMP_FIXUP(x) (x)\n\n";

		if (properties.has_lit_op)
		{
			OS << "float4 _lit(float4 src) {\n";
			OS << "\tfloat d = max(src.x, 0.0);\n";
			OS << "\tfloat s = src.x > 0.0 ? pow(max(src.y, 0.0), clamp(src.w, -128.0, 128.0)) : 0.0;\n";
			OS << "\treturn float4(1.0, d, s, 1.0);\n}\n\n";
		}
	}

	static const char* s_input_semantics[16] = {
		"POSITION",
		"BLENDWEIGHT",
		"NORMAL",
		"COLOR0",
		"COLOR1",
		"TEXCOORD5",
		"PSIZE",
		"BLENDINDICES",
		"TEXCOORD0",
		"TEXCOORD1",
		"TEXCOORD2",
		"TEXCOORD3",
		"TEXCOORD4",
		"TEXCOORD6",
		"TEXCOORD7",
		"TANGENT",
	};

	void vertex_program_decompiler::insertInputs(std::stringstream& OS, const std::vector<ParamType>& inputs)
	{
		OS << "struct VS_INPUT\n{\n";
		for (const auto& PT : inputs)
		{
			for (const auto& PI : PT.items)
			{
				const u32 loc = static_cast<u32>(PI.location) & 0xF;
				OS << "\tfloat4 " << PI.name << " : " << s_input_semantics[loc] << ";\n";
			}
		}
		OS << "};\n\n";
	}

	void vertex_program_decompiler::insertConstants(std::stringstream& OS, const std::vector<ParamType>& constants)
	{
		for (const auto& PT : constants)
		{
			for (const auto& PI : PT.items)
			{
				if (PI.name.starts_with("vc["))
				{
					OS << "float4 vc[240] : register(c0);\n\n";
					continue;
				}
				if (PT.type == "sampler2D" || PT.type == "samplerCube" ||
					PT.type == "sampler1D" || PT.type == "sampler3D")
				{
					std::string t = PT.type;
					if (t == "samplerCube") t = "samplerCUBE";
					OS << t << " " << PI.name << ";\n";
					continue;
				}
				OS << PT.type << " " << PI.name << ";\n";
			}
		}
	}

	struct output_entry { const char* name; const char* semantic; const char* src_reg; bool need_declare; bool check_mask; u32 mask_val; };
	static const output_entry s_outputs[] = {
		{ "pos",        "POSITION",  "dst_reg0",  false, false, 0 },
		{ "diff_color", "COLOR0",    "dst_reg1",  true,  true,  CELL_GCM_ATTRIB_OUTPUT_MASK_FRONTDIFFUSE | CELL_GCM_ATTRIB_OUTPUT_MASK_BACKDIFFUSE },
		{ "spec_color", "COLOR1",    "dst_reg2",  true,  true,  CELL_GCM_ATTRIB_OUTPUT_MASK_FRONTSPECULAR | CELL_GCM_ATTRIB_OUTPUT_MASK_BACKSPECULAR },
		{ "tc0",        "TEXCOORD0", "dst_reg7",  true,  true,  CELL_GCM_ATTRIB_OUTPUT_MASK_TEX0 },
		{ "tc1",        "TEXCOORD1", "dst_reg8",  true,  true,  CELL_GCM_ATTRIB_OUTPUT_MASK_TEX1 },
		{ "tc2",        "TEXCOORD2", "dst_reg9",  true,  true,  CELL_GCM_ATTRIB_OUTPUT_MASK_TEX2 },
		{ "tc3",        "TEXCOORD3", "dst_reg10", true,  true,  CELL_GCM_ATTRIB_OUTPUT_MASK_TEX3 },
		{ "tc4",        "TEXCOORD4", "dst_reg11", true,  true,  CELL_GCM_ATTRIB_OUTPUT_MASK_TEX4 },
		{ "tc5",        "TEXCOORD5", "dst_reg12", true,  true,  CELL_GCM_ATTRIB_OUTPUT_MASK_TEX5 },
		{ "tc6",        "TEXCOORD6", "dst_reg13", true,  true,  CELL_GCM_ATTRIB_OUTPUT_MASK_TEX6 },
		{ "tc7",        "TEXCOORD7", "dst_reg14", true,  true,  CELL_GCM_ATTRIB_OUTPUT_MASK_TEX7 },
	};

	void vertex_program_decompiler::insertOutputs(std::stringstream& OS, const std::vector<ParamType>&)
	{
		OS << "struct VS_OUTPUT\n{\n";
		OS << "\tfloat4 pos : POSITION;\n";
		for (const auto& e : s_outputs)
		{
			if (!e.need_declare) continue;
			OS << "\tfloat4 " << e.name << " : " << e.semantic << ";\n";
		}
		OS << "};\n\n";
	}

	void vertex_program_decompiler::insertMainStart(std::stringstream& OS)
	{
		OS << "float4 _fetch_constant(int idx) { return vc[idx]; }\n\n";

		OS << "VS_OUTPUT main(VS_INPUT _input)\n{\n";
		OS << "\tVS_OUTPUT _output = (VS_OUTPUT)0;\n\n";

		if (ParamType* vec4Types = m_parr.SearchParam(PF_PARAM_OUT, "float4"))
		{
			for (const auto& PI : vec4Types->items)
			{
				OS << "\tfloat4 " << PI.name;
				if (!PI.value.empty()) OS << " = " << sanitize_hlsl_initializer("float4", PI.value);
				else OS << " = float4(0.0, 0.0, 0.0, 1.0)";
				OS << ";\n";
			}
		}
		OS << "\n";

		for (const auto& PT : m_parr.params[PF_PARAM_NONE])
		{
			for (const auto& PI : PT.items)
			{
				if (PI.name.starts_with("dst_reg")) continue;
				OS << "\t" << PT.type << " " << PI.name;
				if (!PI.value.empty()) OS << " = " << sanitize_hlsl_initializer(PT.type, PI.value);
				OS << ";\n";
			}
		}
		OS << "\n";

		for (const auto& PT : m_parr.params[PF_PARAM_IN])
		{
			for (const auto& PI : PT.items)
			{
				OS << "\tfloat4 " << PI.name << " = _input." << PI.name << ";\n";
			}
		}
		OS << "\n";
	}

	void vertex_program_decompiler::insertMainEnd(std::stringstream& OS)
	{
		for (const auto& e : s_outputs)
		{
			if (m_parr.HasParam(PF_PARAM_OUT, "float4", e.src_reg))
			{
				if (std::string(e.name) == "pos")
					OS << "\t_output.pos = " << e.src_reg << ";\n";
				else if (e.need_declare)
					OS << "\t_output." << e.name << " = " << e.src_reg << ";\n";
			}
			else if (e.need_declare)
			{
				OS << "\t_output." << e.name << " = float4(0.0, 0.0, 0.0, 1.0);\n";
			}
		}

		// Convert RSX clip Z [-1,1] to D3D9 clip Z [0,1]
		OS << "\n\t_output.pos.z = _output.pos.z * 0.5 + _output.pos.w * 0.5;\n";

		OS << "\n\treturn _output;\n}\n";
	}
}

#endif
