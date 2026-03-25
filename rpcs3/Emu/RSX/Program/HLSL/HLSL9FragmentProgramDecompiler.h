#pragma once

#ifdef HAVE_D3D9

#include "Emu/RSX/Program/FragmentProgramDecompiler.h"

namespace rsx::program::hlsl9
{
	class fragment_program_decompiler final : public FragmentProgramDecompiler
	{
	protected:
		std::string getFloatTypeName(usz elementCount) override;
		std::string getHalfTypeName(usz elementCount) override;
		std::string getFunction(FUNCTION) override;
		std::string compareFunction(COMPARE, const std::string& a, const std::string& b) override;
		void insertHeader(std::stringstream& OS) override;
		void insertInputs(std::stringstream& OS) override;
		void insertOutputs(std::stringstream& OS) override;
		void insertConstants(std::stringstream& OS) override;
		void insertGlobalFunctions(std::stringstream& OS) override;
		void insertMainStart(std::stringstream& OS) override;
		void insertMainEnd(std::stringstream& OS) override;

	public:
		fragment_program_decompiler(const RSXFragmentProgram& prog, u32& size)
			: FragmentProgramDecompiler(prog, size)
		{
		}
	};
}

#endif
