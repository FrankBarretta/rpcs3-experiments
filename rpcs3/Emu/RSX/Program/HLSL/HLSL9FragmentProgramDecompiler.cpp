#include "stdafx.h"

#ifdef HAVE_D3D9

#include "HLSL9FragmentProgramDecompiler.h"

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

	static const char* map_input_semantic(const std::string& name)
	{
		if (name == "wpos")
			return "POSITION0";
		if (name == "diff_color")
			return "COLOR0";
		if (name == "spec_color")
			return "COLOR1";
		if (name == "fogc")
			return "FOG";
		if (name == "tc0")
			return "TEXCOORD0";
		if (name == "tc1")
			return "TEXCOORD1";
		if (name == "tc2")
			return "TEXCOORD2";
		if (name == "tc3")
			return "TEXCOORD3";
		if (name == "tc4")
			return "TEXCOORD4";
		if (name == "tc5")
			return "TEXCOORD5";
		if (name == "tc6")
			return "TEXCOORD6";
		if (name == "tc7")
			return "TEXCOORD7";
		if (name == "tc8")
			return "TEXCOORD8";
		if (name == "tc9")
			return "TEXCOORD9";
		if (name == "ssa")
			return "TEXCOORD9";
		return "TEXCOORD0";
	}

	std::string fragment_program_decompiler::getFloatTypeName(usz elementCount)
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

	std::string fragment_program_decompiler::getHalfTypeName(usz elementCount)
	{
		switch (elementCount)
		{
		default:
		case 4: return "half4";
		case 3: return "half3";
		case 2: return "half2";
		case 1: return "half";
		}
	}

	std::string fragment_program_decompiler::getFunction(FUNCTION f)
	{
		switch (f)
		{
		default:
			return "$Ty(0.)";
		case FUNCTION::DP2: return "(($Ty)dot($0.xy, $1.xy))";
		case FUNCTION::DP2A: return "(($Ty)(dot($0.xy, $1.xy) + $2.x))";
		case FUNCTION::DP3: return "(($Ty)dot($0.xyz, $1.xyz))";
		case FUNCTION::DP4: return "(($Ty)dot($0, $1))";
		case FUNCTION::DPH: return "(($Ty)dot(float4($0.xyz, 1.0), $1))";
		case FUNCTION::SFL: return "(($Ty)0.)";
		case FUNCTION::STR: return "(($Ty)1.)";
		case FUNCTION::FRACT: return "frac($0)";
		case FUNCTION::DFDX: return "ddx($0)";
		case FUNCTION::DFDY: return "ddy($0)";
		case FUNCTION::REFL: return "reflect($0, $1)";

		case FUNCTION::TEXTURE_SAMPLE1D:
		case FUNCTION::TEXTURE_SAMPLE1D_DEPTH_RGBA:
			return "tex1D($t, $0.x)";
		case FUNCTION::TEXTURE_SAMPLE2D:
		case FUNCTION::TEXTURE_SAMPLE2D_DEPTH_RGBA:
		case FUNCTION::TEXTURE_SAMPLE2DMS:
		case FUNCTION::TEXTURE_SAMPLE2DMS_DEPTH_RGBA:
			return "tex2D($t, $0.xy)";
		case FUNCTION::TEXTURE_SAMPLE3D:
		case FUNCTION::TEXTURE_SAMPLE3D_DEPTH_RGBA:
			return "tex3D($t, $0.xyz)";

		case FUNCTION::TEXTURE_SAMPLE1D_PROJ:
		case FUNCTION::TEXTURE_SAMPLE1D_DEPTH_RGBA_PROJ:
		case FUNCTION::TEXTURE_SAMPLE1D_SHADOW_PROJ:
			return "tex1D($t, ($0.x / $0.w))";
		case FUNCTION::TEXTURE_SAMPLE2D_PROJ:
		case FUNCTION::TEXTURE_SAMPLE2DMS_PROJ:
		case FUNCTION::TEXTURE_SAMPLE2D_DEPTH_RGBA_PROJ:
		case FUNCTION::TEXTURE_SAMPLE2DMS_DEPTH_RGBA_PROJ:
		case FUNCTION::TEXTURE_SAMPLE2D_SHADOW_PROJ:
			return "tex2Dproj($t, $0)";
		case FUNCTION::TEXTURE_SAMPLE3D_PROJ:
		case FUNCTION::TEXTURE_SAMPLE3D_DEPTH_RGBA_PROJ:
		case FUNCTION::TEXTURE_SAMPLE3D_SHADOW_PROJ:
			return "tex3D($t, ($0.xyz / $0.w))";
		case FUNCTION::TEXTURE_SAMPLE2DMS_SHADOW_PROJ:
			return "tex2Dproj($t, $0)";

		case FUNCTION::TEXTURE_SAMPLE1D_BIAS:
			return "tex1Dbias($t, float4($0.x, 0, 0, $1.x))";
		case FUNCTION::TEXTURE_SAMPLE2D_BIAS:
		case FUNCTION::TEXTURE_SAMPLE2DMS_BIAS:
			return "tex2Dbias($t, float4($0.xy, 0, $1.x))";
		case FUNCTION::TEXTURE_SAMPLE3D_BIAS:
			return "tex3Dbias($t, float4($0.xyz, $1.x))";

		case FUNCTION::TEXTURE_SAMPLE1D_LOD:
			return "tex1Dlod($t, float4($0.x, 0, 0, $1.x))";
		case FUNCTION::TEXTURE_SAMPLE2D_LOD:
		case FUNCTION::TEXTURE_SAMPLE2DMS_LOD:
			return "tex2Dlod($t, float4($0.xy, 0, $1.x))";
		case FUNCTION::TEXTURE_SAMPLE3D_LOD:
			return "tex3Dlod($t, float4($0.xyz, $1.x))";

		case FUNCTION::TEXTURE_SAMPLE1D_GRAD:
			return "tex1Dgrad($t, $0.x, $1.x, $2.x)";
		case FUNCTION::TEXTURE_SAMPLE2D_GRAD:
		case FUNCTION::TEXTURE_SAMPLE2DMS_GRAD:
			return "tex2Dgrad($t, $0.xy, $1.xy, $2.xy)";
		case FUNCTION::TEXTURE_SAMPLE3D_GRAD:
			return "tex3Dgrad($t, $0.xyz, $1.xyz, $2.xyz)";

		case FUNCTION::TEXTURE_SAMPLE1D_SHADOW:
			return "tex1D($t, $0.x)";
		case FUNCTION::TEXTURE_SAMPLE2D_SHADOW:
		case FUNCTION::TEXTURE_SAMPLE2DMS_SHADOW:
			return "tex2D($t, $0.xy)";
		case FUNCTION::TEXTURE_SAMPLE3D_SHADOW:
			return "tex3D($t, $0.xyz)";
		}
	}

	std::string fragment_program_decompiler::compareFunction(COMPARE f, const std::string& a, const std::string& b)
	{
		switch (f)
		{
		case COMPARE::SEQ: return "_cmp_eq(" + a + ", " + b + ")";
		case COMPARE::SGE: return "_cmp_ge(" + a + ", " + b + ")";
		case COMPARE::SGT: return "_cmp_gt(" + a + ", " + b + ")";
		case COMPARE::SLE: return "_cmp_le(" + a + ", " + b + ")";
		case COMPARE::SLT: return "_cmp_lt(" + a + ", " + b + ")";
		case COMPARE::SNE: return "_cmp_ne(" + a + ", " + b + ")";
		}
		return "float4(0, 0, 0, 0)";
	}

	void fragment_program_decompiler::insertHeader(std::stringstream& OS)
	{
		OS << "// Auto-generated RSX fragment shader for HLSL9\n";
		OS << "#define fma(a, b, c) mad((a), (b), (c))\n";
		OS << "#define _saturate(x) saturate(x)\n";
	}

	void fragment_program_decompiler::insertInputs(std::stringstream& OS)
	{
		OS << "struct PSIn\n{\n";
		for (const auto& PT : m_parr.params[PF_PARAM_IN])
		{
			for (const auto& PI : PT.items)
			{
				OS << "\t" << PT.type << " " << PI.name << " : " << map_input_semantic(PI.name) << ";\n";
			}
		}
		OS << "};\n";
	}

	void fragment_program_decompiler::insertOutputs(std::stringstream& OS)
	{
		OS << "struct PSOut\n{\n";
		OS << "\tfloat4 color : COLOR0;\n";
		if (m_prog.ctrl & CELL_GCM_SHADER_CONTROL_DEPTH_EXPORT)
		{
			OS << "\tfloat depth : DEPTH0;\n";
		}
		OS << "};\n";
	}

	void fragment_program_decompiler::insertConstants(std::stringstream& OS)
	{
		for (const auto& list : {PF_PARAM_CONST, PF_PARAM_UNIFORM})
		{
			for (const auto& PT : m_parr.params[list])
			{
				for (const auto& PI : PT.items)
				{
					std::string type = PT.type;
					if (type == "samplerCube")
					{
						type = "samplerCUBE";
					}

					if (PI.name.starts_with("fc["))
					{
						OS << type << " " << PI.name << " : register(c0);\n";
						continue;
					}

					OS << type << " " << PI.name << ";\n";
				}
			}
		}
	}

	void fragment_program_decompiler::insertGlobalFunctions(std::stringstream& OS)
	{
		OS << "float4 rsx_saturate(float4 v) { return saturate(v); }\n";
		OS << "float4 _cmp_eq(float4 a, float4 b) { return float4(a.x==b.x?1:0, a.y==b.y?1:0, a.z==b.z?1:0, a.w==b.w?1:0); }\n";
		OS << "float4 _cmp_ge(float4 a, float4 b) { return float4(a.x>=b.x?1:0, a.y>=b.y?1:0, a.z>=b.z?1:0, a.w>=b.w?1:0); }\n";
		OS << "float4 _cmp_gt(float4 a, float4 b) { return float4(a.x>b.x?1:0, a.y>b.y?1:0, a.z>b.z?1:0, a.w>b.w?1:0); }\n";
		OS << "float4 _cmp_le(float4 a, float4 b) { return float4(a.x<=b.x?1:0, a.y<=b.y?1:0, a.z<=b.z?1:0, a.w<=b.w?1:0); }\n";
		OS << "float4 _cmp_lt(float4 a, float4 b) { return float4(a.x<b.x?1:0, a.y<b.y?1:0, a.z<b.z?1:0, a.w<b.w?1:0); }\n";
		OS << "float4 _cmp_ne(float4 a, float4 b) { return float4(a.x!=b.x?1:0, a.y!=b.y?1:0, a.z!=b.z?1:0, a.w!=b.w?1:0); }\n";

		OS << "float4 _select(float4 a, float4 b, float4 c) { return float4(c.x?b.x:a.x, c.y?b.y:a.y, c.z?b.z:a.z, c.w?b.w:a.w); }\n";
		OS << "float3 _select(float3 a, float3 b, float3 c) { return float3(c.x?b.x:a.x, c.y?b.y:a.y, c.z?b.z:a.z); }\n";
		OS << "float2 _select(float2 a, float2 b, float2 c) { return float2(c.x?b.x:a.x, c.y?b.y:a.y); }\n";
		OS << "float _select(float a, float b, float c) { return c?b:a; }\n";

		OS << "void _kill() { clip(-1); }\n";

		bool has_fc_array = false;
		for (const auto& PT : m_parr.params[PF_PARAM_CONST])
		{
			for (const auto& PI : PT.items)
			{
				if (PI.name.starts_with("fc["))
				{
					has_fc_array = true;
					break;
				}
			}
			if (has_fc_array)
			{
				break;
			}
		}
		if (has_fc_array)
		{
			OS << "float4 _fetch_constant(int idx) { return fc[idx]; }\n";
		}
	}

	void fragment_program_decompiler::insertMainStart(std::stringstream& OS)
	{
		OS << "PSOut main(PSIn input)\n{\n";
		OS << "\tPSOut o = (PSOut)0;\n";

		for (const auto& PT : m_parr.params[PF_PARAM_NONE])
		{
			for (const auto& PI : PT.items)
			{
				OS << "\t" << PT.type << " " << PI.name;
				if (!PI.value.empty())
				{
					OS << " = " << sanitize_hlsl_initializer(PT.type, PI.value);
				}
				OS << ";\n";
			}
		}

		for (const auto& PT : m_parr.params[PF_PARAM_IN])
		{
			for (const auto& PI : PT.items)
			{
				OS << "\t" << PT.type << " " << PI.name << " = input." << PI.name << ";\n";
			}
		}
	}

	void fragment_program_decompiler::insertMainEnd(std::stringstream& OS)
	{
		if (m_parr.HasParam(PF_PARAM_NONE, "float4", "r0"))
		{
			OS << "\to.color = r0;\n";
		}
		else if (m_parr.HasParam(PF_PARAM_NONE, "half4", "h0"))
		{
			OS << "\to.color = h0;\n";
		}
		else
		{
			OS << "\to.color = float4(0.0, 0.0, 0.0, 1.0);\n";
		}

		if (m_prog.ctrl & CELL_GCM_SHADER_CONTROL_DEPTH_EXPORT)
		{
			if (m_parr.HasParam(PF_PARAM_NONE, "float4", "r1"))
			{
				OS << "\to.depth = r1.z;\n";
			}
			else
			{
				OS << "\to.depth = 0.0;\n";
			}
		}

		OS << "\treturn o;\n}\n";
	}
} // namespace rsx::program::hlsl9

#endif
