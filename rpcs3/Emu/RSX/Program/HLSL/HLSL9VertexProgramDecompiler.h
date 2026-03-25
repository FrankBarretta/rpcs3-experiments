#pragma once

#ifdef HAVE_D3D9

#include "Emu/RSX/Program/VertexProgramDecompiler.h"

namespace rsx::program::hlsl9
{
	class vertex_program_decompiler final : public VertexProgramDecompiler
	{
	protected:
		std::string getFloatTypeName(usz elementCount) override;
		std::string getIntTypeName(usz elementCount) override;
		std::string getFunction(FUNCTION) override;
		std::string compareFunction(COMPARE, const std::string& a, const std::string& b, bool scalar = false) override;
		void insertHeader(std::stringstream& OS) override;
		void insertInputs(std::stringstream& OS, const std::vector<ParamType>& inputs) override;
		void insertConstants(std::stringstream& OS, const std::vector<ParamType>& constants) override;
		void insertOutputs(std::stringstream& OS, const std::vector<ParamType>& outputs) override;
		void insertMainStart(std::stringstream& OS) override;
		void insertMainEnd(std::stringstream& OS) override;

	public:
		explicit vertex_program_decompiler(const RSXVertexProgram& prog)
			: VertexProgramDecompiler(prog)
		{
		}
	};
}

#endif
