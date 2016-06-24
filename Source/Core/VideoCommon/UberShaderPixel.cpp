// Copyright 2015 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "VideoCommon/BPMemory.h"
#include "VideoCommon/UberShaderPixel.h"

namespace UberShader {
template <typename T>
std::string BitfieldExtract(const std::string &source, T type) {
  return StringFromFormat("bitfieldExtract(%s, %u, %u)", source.c_str(),
                          type.offset, type.size);
}

PixelShaderUid GetPixelShaderUid(DSTALPHA_MODE dstAlphaMode) {
  PixelShaderUid out;
  pixel_ubershader_uid_data *uid = out.GetUidData<pixel_ubershader_uid_data>();
  uid->numTexgens = xfmem.numTexGen.numTexGens;
  uid->EarlyDepth = bpmem.zcontrol.early_ztest && !bpmem.genMode.zfreeze;

  return out;
}

ShaderCode GenPixelShader(DSTALPHA_MODE dstAlphaMode, API_TYPE ApiType,
                          bool per_pixel_depth, bool msaa, bool ssaa) {
  ShaderCode out;

  out.Write("// Pixel UberShader\n");
  WritePixelShaderCommonHeader(out, ApiType);

  // Bad
  u32 numTexgen = xfmem.numTexGen.numTexGens;

  // TODO: This is variable based on number of texcoord gens
  out.Write("struct VS_OUTPUT {\n");
  GenerateVSOutputMembers(out, ApiType, numTexgen, false, "");
  out.Write("};\n");

  // TEV constants
  if (ApiType == API_OPENGL)
    out.Write("layout(std140, binding = 4) uniform UBERBlock {\n");
  else
    out.Write("cbuffer UBERBlock : register(b1) {\n");
  out.Write(
      "	uint	bpmem_genmode;\n"
      "	uint	bpmem_alphaTest;\n"
      "	uint	bpmem_fogParam3;\n"
      "	uint	bpmem_fogRangeBase;\n"
      "	uint	bpmem_dstalpha;\n"
      "	uint	bpmem_ztex2;\n"    // TODO: We only use two bits out of this
      "	uint	bpmem_zcontrol;\n" // TODO: We only use one bit out of this
      "	uint	xfmem_projection;\n"
      "	uint	bpmem_tevorder[8];\n"
      "	uint2	bpmem_combiners[16];\n"
      "	uint	bpmem_tevksel[8];\n"
      "	uint4	bpmem_iref;\n"
      "	uint	bpmem_tevind[16];\n"
      "	int4	konstLookup[32];\n"
      "	float4  debug;\n"
      "};\n");

  // TODO: Per pixel lighting (not really needed)

  // TODO: early depth tests (we will need multiple shaders)

  // ==============================================
  //  BitfieldExtract for APIs which don't have it
  // ==============================================

  if (!g_ActiveConfig.backend_info.bSupportsBitfield) {
    out.Write("uint bitfieldExtract(uint val, int off, int size) {\n"
              "	// This built-in function is only support in OpenGL "
              "4.0+ and ES 3.1+\n"
              "	// Microsoft's HLSL compiler automatically optimises "
              "this to a bitfield extract "
              "instruction.\n"
              "	uint mask = uint((1 << size) - 1);\n"
              "	return uint(val >> off) & mask;\n"
              "}\n\n");
  }

  // =====================
  //   Texture Sampling
  // =====================

  if (g_ActiveConfig.backend_info.bSupportsDynamicSamplerIndexing) {
    // Doesn't look like directx supports this. Oh well the code path is here
    // just incase it
    // supports this in the future.
    out.Write("int4 sampleTexture(uint sampler_num, float2 uv) {\n");
    if (ApiType == API_OPENGL)
      out.Write("	return iround(texture(samp[sampler_num], float3(uv, "
                "0.0)) * 255.0);\n");
    else if (ApiType == API_D3D)
      out.Write("	return "
                "iround(Tex[sampler_num].Sample(samp[sampler_num], float3(uv, "
                "0.0)) * 255.0);\n");
    out.Write("}\n\n");
  } else {
    out.Write("int4 sampleTexture(uint sampler_num, float2 uv) {\n"
              "	// This is messy, but DirectX, OpenGl 3.3 and Opengl ES "
              "3.0 doesn't support "
              "dynamic indexing of the sampler array\n"
              "	// With any luck the shader compiler will optimise this "
              "if the hardware supports "
              "dynamic indexing.\n"
              "	switch(sampler_num) {\n");
    for (int i = 0; i < 8; i++) {
      if (ApiType == API_OPENGL)
        out.Write("	case %du: return int4(texture(samp[%d], float3(uv, "
                  "0.0)) * 255.0);\n",
                  i, i);
      else if (ApiType == API_D3D)
        out.Write("	case %du: return int4(Tex[%d].Sample(samp[%d], "
                  "float3(uv, 0.0)) * 255.0);\n",
                  i, i, i);
    }
    out.Write("	}\n"
              "}\n\n");
  }

  // ======================
  //   Arbatary Swizzling
  // ======================

  out.Write("int4 Swizzle(uint s, int4 color) {\n"
            "	// AKA: Color Channel Swapping\n"
            "\n"
            "	int4 ret;\n");
  out.Write("	ret.r = color[%s];\n",
            BitfieldExtract("bpmem_tevksel[s * 2u]", TevKSel().swap1).c_str());
  out.Write("	ret.g = color[%s];\n",
            BitfieldExtract("bpmem_tevksel[s * 2u]", TevKSel().swap2).c_str());
  out.Write(
      "	ret.b = color[%s];\n",
      BitfieldExtract("bpmem_tevksel[s * 2u + 1u]", TevKSel().swap1).c_str());
  out.Write(
      "	ret.a = color[%s];\n",
      BitfieldExtract("bpmem_tevksel[s * 2u + 1u]", TevKSel().swap2).c_str());
  out.Write("	return ret;\n"
            "}\n\n");

  // ======================
  //   Indirect Wrappping
  // ======================
  out.Write("int Wrap(int coord, uint mode) {\n"
            "	if (mode == 0u) // ITW_OFF\n"
            "		return coord;\n"
            "	else if (mode < 6u) // ITW_256 to ITW_16\n"
            "		return coord & (0xfffe >> mode);\n"
            "	else // ITW_0\n"
            "		return 0;\n"
            "}\n\n");

  // ======================
  //   TEV's Special Lerp
  // ======================

  out.Write(
      "// One channel worth of TEV's Linear Interpolate, plus bias, "
      "add/subtract and scale\n"
      "int tevLerp(int A, int B, int C, int D, uint bias, bool op, uint shift) "
      "{\n"
      "	C += C >> 7; // Scale C from 0..255 to 0..256\n"
      "	int lerp = (A << 8) + (B - A)*C;\n"
      "	if (shift != 3u) {\n"
      "		lerp = lerp << shift;\n"
      "		lerp = lerp + (op ? 127 : 128);\n"
      "		D = D << shift;\n"
      "	}\n"
      "	int result = lerp >> 8;\n"
      "\n"
      "	// Add/Subtract D (and bias)\n"
      "	if (bias == 1u) result += 128;\n"
      "	else if (bias == 2u) result -= 128;\n"
      "	if(op) // Subtract\n"
      "		result = D - result;\n"
      "	else // Add\n"
      "		result = D + result;\n"
      "\n"
      "	// Most of the Shift was moved inside the lerp for improved percision\n"
      "	// But we still do the divide by 2 here\n"
      "	if (shift == 3u)\n"
      "		result = result >> 1;\n"
      "	return result;\n"
      "}\n\n");

  // =======================
  //   TEV's Color Compare
  // =======================

  out.Write(
      "// Implements operations 0-5 of tev's compare mode,\n"
      "// which are common to both color and alpha channels\n"
      "bool tevCompare(uint op, int3 color_A, int3 color_B) {\n"
      "	switch (op) {\n"
      "	case 0u: // TEVCMP_R8_GT\n"
      "		return (color_A.r > color_B.r);\n"
      "	case 1u: // TEVCMP_R8_EQ\n"
      "		return (color_A.r == color_B.r);\n"
      "	case 2u: // TEVCMP_GR16_GT\n"
      "		int A_16 = (color_A.r | (color_A.g << 8));\n"
      "		int B_16 = (color_B.r | (color_B.g << 8));\n"
      "		return A_16 > B_16;\n"
      "	case 3u: // TEVCMP_GR16_EQ\n"
      "		return (color_A.r == color_B.r && color_A.g == color_B.g);\n"
      "	case 4u: // TEVCMP_BGR24_GT\n"
      "		int A_24 = (color_A.r | (color_A.g << 8) | (color_A.b "
      "<< 16));\n"
      "		int B_24 = (color_B.r | (color_B.g << 8) | (color_B.b "
      "<< 16));\n"
      "		return A_24 > B_24;\n"
      "	case 5u: // TEVCMP_BGR24_EQ\n"
      "		return (color_A.r == color_B.r && color_A.g == "
      "color_B.g && color_A.b == color_B.b);\n"
      "	default:\n"
      "		return false;\n"
      "	}\n"
      "}\n\n");

  // =================
  //   Alpha Compare
  // =================

  out.Write("// Helper function for Alpha Test\n"
            "bool alphaCompare(int a, int b, uint compare) {\n"
            "	switch (compare) {\n"
            "	case 0u: // NEVER\n"
            "		return false;\n"
            "	case 1u: // LESS\n"
            "		return a < b;\n"
            "	case 2u: // EQUAL\n"
            "		return a == b;\n"
            "	case 3u: // LEQUAL\n"
            "		return a <= b;\n"
            "	case 4u: // GREATER\n"
            "		return a > b;\n"
            "	case 5u: // NEQUAL;\n"
            "		return a != b;\n"
            "	case 6u: // GEQUAL\n"
            "		return a >= b;\n"
            "	case 7u: // ALWAYS\n"
            "		return true;\n"
            "	}\n"
            "}\n\n");

  // =================
  //   Input Selects
  // =================

  out.Write("struct State {\n"
            "	int4 Reg[4];\n"
            "	int4 RasColor;\n"
            "	int4 TexColor;\n"
            "	int4 KonstColor;\n"
            "};\n"
            "\n"
            "int3 selectColorInput(State s, uint index) {\n"
            "	switch (index) {\n"
            "	case 0u: // prev.rgb\n"
            "		return s.Reg[0].rgb;\n"
            "	case 1u: // prev.aaa\n"
            "		return s.Reg[0].aaa;\n"
            "	case 2u: // c0.rgb\n"
            "		return s.Reg[1].rgb;\n"
            "	case 3u: // c0.aaa\n"
            "		return s.Reg[1].aaa;\n"
            "	case 4u: // c1.rgb\n"
            "		return s.Reg[2].rgb;\n"
            "	case 5u: // c1.aaa\n"
            "		return s.Reg[2].aaa;\n"
            "	case 6u: // c2.rgb\n"
            "		return s.Reg[3].rgb;\n"
            "	case 7u: // c2.aaa\n"
            "		return s.Reg[3].aaa;\n"
            "	case 8u:\n"
            "		return s.TexColor.rgb;\n"
            "	case 9u:\n"
            "		return s.TexColor.aaa;\n"
            "	case 10u:\n"
            "		return s.RasColor.rgb;\n"
            "	case 11u:\n"
            "		return s.RasColor.aaa;\n"
            "	case 12u: // One\n"
            "		return int3(255, 255, 255);\n"
            "	case 13u: // Half\n"
            "		return int3(128, 128, 128);\n"
            "	case 14u:\n"
            "		return s.KonstColor.rgb;\n"
            "	case 15u: // Zero\n"
            "		return int3(0, 0, 0);\n"
            "	}\n"
            "}\n"
            "int selectAlphaInput(State s, uint index) {\n"
            "	switch (index) {\n"
            "	case 0u: // prev.a\n"
            "		return s.Reg[0].a;\n"
            "	case 1u: // c0.a\n"
            "		return s.Reg[1].a;\n"
            "	case 2u: // c1.a\n"
            "		return s.Reg[2].a;\n"
            "	case 3u: // c2.a\n"
            "		return s.Reg[3].a;\n"
            "	case 4u:\n"
            "		return s.TexColor.a;\n"
            "	case 5u:\n"
            "		return s.RasColor.a;\n"
            "	case 6u:\n"
            "		return s.KonstColor.a;\n"
            "	case 7u: // Zero\n"
            "		return 0;\n"
            "	}\n"
            "}\n"
            "\n"
            "void setRegColor(inout State s, uint index, int3 color) {\n"
            "	switch (index) {\n"
            "	case 0u: // prev\n"
            "		s.Reg[0].rgb = color;\n"
            "		break;\n"
            "	case 1u: // c0\n"
            "		s.Reg[1].rgb = color;\n"
            "		break;\n"
            "	case 2u: // c1\n"
            "		s.Reg[2].rgb = color;\n"
            "		break;\n"
            "	case 3u: // c2\n"
            "		s.Reg[3].rgb = color;\n"
            "		break;\n"
            "	}\n"
            "}\n"
            "\n"
            "void setRegAlpha(inout State s, uint index, int alpha) {\n"
            "	switch (index) {\n"
            "	case 0u: // prev\n"
            "		s.Reg[0].a = alpha;\n"
            "		break;\n"
            "	case 1u: // c0\n"
            "		s.Reg[1].a = alpha;\n"
            "		break;\n"
            "	case 2u: // c1\n"
            "		s.Reg[2].a = alpha;\n"
            "		break;\n"
            "	case 3u: // c2\n"
            "		s.Reg[3].a = alpha;\n"
            "		break;\n"
            "	}\n"
            "}\n"
            "\n");

  bool early_depth = bpmem.zcontrol.early_ztest == 1 && !bpmem.genMode.zfreeze;
  if (early_depth && g_ActiveConfig.backend_info.bSupportsEarlyZ) {
    if (ApiType == API_OPENGL)
      out.Write("FORCE_EARLY_Z;\n");
    else
      out.Write("[earlydepthstencil]\n");
  }

  if (ApiType == API_OPENGL) {
    out.Write("out vec4 ocol0;\n"
              "out vec4 ocol1;\n");

    if (!early_depth)
      out.Write("#define depth gl_FragDepth\n");
    out.Write("in VertexData {\n");
    GenerateVSOutputMembers(out, ApiType, numTexgen, false,
                            GetInterpolationQualifier(msaa, ssaa));

    // TODO: Stereo Mode

    out.Write("};\n\n");

    // TODO: Add support for OpenGL without geometery shaders back in.

    out.Write("void main()\n{\n");

    out.Write("\tfloat4 rawpos = gl_FragCoord;\n");
  } else // D3D
  {
    out.Write("void main(\n"
              "	out float4 ocol0 : SV_Target0,\n"
              "	out float4 ocol1 : SV_Target1,\n"
              "	%s\n",
              !early_depth ? "\n  out float depth : SV_Depth," : "");
    out.Write("	in float4 rawpos : SV_Position,\n");

    out.Write("  in %s float4 colors_0 : COLOR0,\n",
              GetInterpolationQualifier(msaa, ssaa));
    out.Write("  in %s float4 colors_1 : COLOR1\n",
              GetInterpolationQualifier(msaa, ssaa));

    // compute window position if needed because binding semantic WPOS is not
    // widely supported
    if (numTexgen > 0)
      out.Write(",\n  in %s float3 tex[%d] : TEXCOORD0",
                GetInterpolationQualifier(msaa, ssaa), numTexgen);
    out.Write(",\n  in %s float4 clipPos : TEXCOORD%d",
              GetInterpolationQualifier(msaa, ssaa), numTexgen);
    if (g_ActiveConfig.bEnablePixelLighting) {
      out.Write(",\n  in %s float3 Normal : TEXCOORD%d",
                GetInterpolationQualifier(msaa, ssaa), numTexgen + 1);
      out.Write(",\n  in %s float3 WorldPos : TEXCOORD%d",
                GetInterpolationQualifier(msaa, ssaa), numTexgen + 2);
    }
    if (g_ActiveConfig.iStereoMode > 0)
      out.Write(",\n  in uint layer : SV_RenderTargetArrayIndex\n");
    out.Write("        ) {\n");
  }

  out.Write("	int AlphaBump = 0;\n"
            "	int3 tevcoord = int3(0, 0, 0);\n"
            "	int4 icolors_0 = iround(colors_0 * 255.0);\n"
            "	int4 icolors_1 = iround(colors_1 * 255.0);\n"
            "	int4 TevResult;\n"
            "	State s;\n"
            "	s.TexColor = int4(0, 0, 0, 0);\n"
            "	s.RasColor = int4(0, 0, 0, 0);\n"
            "	s.KonstColor = int4(0, 0, 0, 0);\n"
            "\n");
  for (int i = 0; i < 4; i++)
    out.Write("	s.Reg[%d] = " I_COLORS "[%d];\n", i, i);

  if (numTexgen != 0) {
    out.Write( // TODO: Skip preload on Nvidia and other GPUs which can't handle
               // dynamic indexed
               // arrays?
        "\n"
        "	int3 indtex[4];\n"
        "	// Pre-sample indirect textures\n"
        "	for(uint i = 0u; i < 4u; i++)\n"
        "	{\n"
        "		uint iref = bpmem_iref[i];\n"
        "		if ( iref != 0u)\n"
        "		{\n"
        "			uint texcoord = bitfieldExtract(iref, 0, 3);\n"
        "			uint texmap = bitfieldExtract(iref, 8, 3);\n"
        "			int2 fixedPoint_uv; \n"
        "			if ((xfmem_projection & (1u << texcoord)) != "
        "0u) // Optional Perspective divide\n"
        "				fixedPoint_uv = "
        "itrunc((tex[texcoord].xy / tex[texcoord].z) * " I_TEXDIMS
        "[texcoord].zw);\n"
        "			else\n"
        "				fixedPoint_uv = "
        "itrunc(tex[texcoord].xy "
        "* " I_TEXDIMS "[texcoord].zw);\n"
        "\n"
        "			if ((i & 1u) == 0u)\n"
        "				fixedPoint_uv = fixedPoint_uv "
        ">> " I_INDTEXSCALE "[i >> 1].xy;\n"
        "			else\n"
        "				fixedPoint_uv = fixedPoint_uv "
        ">> " I_INDTEXSCALE "[i >> 1].zw;\n"
        "\n"
        "			indtex[i] = sampleTexture(texmap, "
        "float2(fixedPoint_uv) * " I_TEXDIMS "[texmap].xy).abg;\n"
        "		}\n"
        "		else\n"
        "		{\n"
        "			indtex[i] = int3(0, 0, 0);\n"
        "		}\n"
        "	}\n"
        "\n");
  }

  out.Write(
      "	uint num_stages = %s;\n\n",
      BitfieldExtract("bpmem_genmode", bpmem.genMode.numtevstages).c_str());

  out.Write("	// Main tev loop\n");
  if (ApiType == API_D3D)
    out.Write("	[loop]\n"); // Tell DirectX we don't want this loop unrolled (it
                            // crashes if it tries to)
  out.Write("	for(uint stage = 0u; stage <= num_stages; stage++)\n"
            "	{\n"
            "		uint cc = bpmem_combiners[stage].x;\n"
            "		uint ac = bpmem_combiners[stage].y;\n"
            "		uint order = bpmem_tevorder[stage>>1];\n"
            "		if ((stage & 1u) == 1u)\n"
            "			order = order >> %d;\n\n",
            TwoTevStageOrders().enable1.offset -
                TwoTevStageOrders().enable0.offset);

  // Disable texturing when there are no texgens (for now)
  if (numTexgen != 0) {
    out.Write("		uint tex_coord = %s;\n",
              BitfieldExtract("order", TwoTevStageOrders().texcoord0).c_str());
    out.Write("		int2 fixedPoint_uv;\n"
              "		if ((xfmem_projection & (1u << tex_coord)) != "
              "0u) // Optional Perspective divide\n"
              "			fixedPoint_uv = "
              "itrunc((tex[tex_coord].xy / tex[tex_coord].z) * " I_TEXDIMS
              "[tex_coord].zw);\n"
              "		else\n"
              "			fixedPoint_uv = "
              "itrunc(tex[tex_coord].xy * " I_TEXDIMS "[tex_coord].zw);\n"
              "\n"
              "		bool texture_enabled = (order & %du) != 0u;\n",
              1 << TwoTevStageOrders().enable0.offset);
    out.Write("\n"
              "		// Indirect textures\n"
              "		uint tevind = bpmem_tevind[stage];\n"
              "		if (tevind != 0u)\n"
              "		{\n"
              "			uint bs = %s;\n",
              BitfieldExtract("tevind", TevStageIndirect().bs).c_str());
    out.Write("			uint fmt = %s;\n",
              BitfieldExtract("tevind", TevStageIndirect().fmt).c_str());
    out.Write("			uint bias = %s;\n",
              BitfieldExtract("tevind", TevStageIndirect().bias).c_str());
    out.Write("			uint bt = %s;\n",
              BitfieldExtract("tevind", TevStageIndirect().bt).c_str());
    out.Write("			uint mid = %s;\n",
              BitfieldExtract("tevind", TevStageIndirect().mid).c_str());
    out.Write("\n"

              "			int3 indcoord = indtex[bt];\n"
              "			if (bs != 0u)\n"
              "				AlphaBump = indcoord[bs - 1u];\n"
              "			switch(fmt)\n"
              "			{\n"
              "			case %iu:\n",
              ITF_8);
    out.Write("				indcoord.x = indcoord.x + "
              "((bias & 1u) != 0u ? -128 : 0);\n"
              "				indcoord.y = indcoord.y + "
              "((bias & 2u) != 0u ? -128 : 0);\n"
              "				indcoord.z = indcoord.z + "
              "((bias & 4u) != 0u ? -128 : 0);\n"
              "				AlphaBump = AlphaBump & 0xf8;\n"
              "				break;\n"
              "			case %iu:\n",
              ITF_5);
    out.Write("				indcoord.x = (indcoord.x & "
              "0x1f) + ((bias & 1u) != 0u ? 1 : 0);\n"
              "				indcoord.y = (indcoord.y & "
              "0x1f) + ((bias & 2u) != 0u ? 1 : 0);\n"
              "				indcoord.z = (indcoord.z & "
              "0x1f) + ((bias & 4u) != 0u ? 1 : 0);\n"
              "				AlphaBump = AlphaBump & 0xe0;\n"
              "				break;\n"
              "			case %iu:\n",
              ITF_4);
    out.Write("				indcoord.x = (indcoord.x & "
              "0x0f) + ((bias & 1u) != 0u ? 1 : 0);\n"
              "				indcoord.y = (indcoord.y & "
              "0x0f) + ((bias & 2u) != 0u ? 1 : 0);\n"
              "				indcoord.z = (indcoord.z & "
              "0x0f) + ((bias & 4u) != 0u ? 1 : 0);\n"
              "				AlphaBump = AlphaBump & 0xf0;\n"
              "				break;\n"
              "			case %iu:\n",
              ITF_3);
    out.Write("				indcoord.x = (indcoord.x & "
              "0x07) + ((bias & 1u) != 0u ? 1 : 0);\n"
              "				indcoord.y = (indcoord.y & "
              "0x07) + ((bias & 2u) != 0u ? 1 : 0);\n"
              "				indcoord.z = (indcoord.z & "
              "0x07) + ((bias & 4u) != 0u ? 1 : 0);\n"
              "				AlphaBump = AlphaBump & 0xf8;\n"
              "				break;\n"
              "			}\n"
              "\n"
              "			// Matrix multiply\n"
              "			int2 indtevtrans = int2(0, 0);\n"
              "			if ((mid & 3u) != 0u)\n"
              "			{\n"
              "				uint mtxidx = 2u * ((mid & 3u) - 1u);\n"
              "				int shift = " I_INDTEXMTX
              "[mtxidx].w;\n"
              "\n"
              "				switch (mid >> 2)\n"
              "				{\n"
              "				case 0u: // 3x2 S0.10 matrix\n"
              "					indtevtrans = "
              "int2(idot(" I_INDTEXMTX
              "[mtxidx].xyz, indcoord), idot(" I_INDTEXMTX
              "[mtxidx + 1u].xyz, indcoord));\n"
              "					shift = shift + 3;\n"
              "					break;\n"
              "				case 1u: // S matrix, S17.7 format\n"
              "					indtevtrans = "
              "fixedPoint_uv * indcoord.xx;\n"
              "					shift = shift + 8;\n"
              "					break;\n"
              "				case 2u: // T matrix, S17.7 format\n"
              "					indtevtrans = "
              "fixedPoint_uv * indcoord.yy;\n"
              "					shift = shift + 8;\n"
              "					break;\n"
              "				}\n"
              "\n"
              "				if (shift >= 0)\n"
              "					indtevtrans = "
              "indtevtrans >> shift;\n"
              "				else\n"
              "					indtevtrans = "
              "indtevtrans << ((-shift) & 31);\n"
              "			}\n"
              "\n"
              "			// Wrapping\n"
              "			uint sw = %s;\n",
              BitfieldExtract("tevind", TevStageIndirect().sw).c_str());
    out.Write("			uint tw = %s; \n",
              BitfieldExtract("tevind", TevStageIndirect().tw).c_str());
    out.Write("			int2 wrapped_coord = "
              "int2(Wrap(fixedPoint_uv.x, sw), Wrap(fixedPoint_uv.y, tw));\n"
              "\n"
              "			if ((tevind & %du) != 0u) // add "
              "previous tevcoord\n",
              1 << TevStageIndirect().fb_addprev.offset);
    out.Write("				tevcoord.xy += wrapped_coord + "
              "indtevtrans;\n"
              "			else\n"
              "				tevcoord.xy = wrapped_coord + "
              "indtevtrans;\n"
              "\n"
              "			// Emulate s24 overflows\n"
              "			tevcoord.xy = (tevcoord.xy << 8) >> 8;\n"
              "		}\n"
              "		else if (texture_enabled)\n"
              "		{\n"
              "			tevcoord.xy = fixedPoint_uv;\n"
              "		}\n"
              "\n"
              "		// Sample texture for stage\n"
              "		if(texture_enabled) {\n"
              "			uint sampler_num = %s;\n",
              BitfieldExtract("order", TwoTevStageOrders().texmap0).c_str());
    out.Write("\n"
              "			float2 uv = (float2(tevcoord.xy)) * " I_TEXDIMS
              "[sampler_num].xy;\n"
              "\n"
              "			int4 color = sampleTexture(sampler_num, uv);\n"
              "\n"
              "			uint swap = %s;\n",
              BitfieldExtract("ac", TevStageCombiner().alphaC.tswap).c_str());
    out.Write("			s.TexColor = Swizzle(swap, color);\n");
    out.Write("		} else {\n"
              "			// Texture is disabled\n"
              "			s.TexColor = int4(255, 255, 255, 255);\n"
              "		}\n"
              "\n");
  }

  out.Write("		// Select Konst for stage\n"
            "		// TODO: a switch case might be better here than an "
            "dynamically indexed uniform lookup\n"
            "		uint tevksel = bpmem_tevksel[stage>>1];\n"
            "		if ((stage & 1u) == 0u)\n"
            "			s.KonstColor = int4(konstLookup[%s].rgb, "
            "konstLookup[%s].a);\n",
            BitfieldExtract("tevksel", bpmem.tevksel[0].kcsel0).c_str(),
            BitfieldExtract("tevksel", bpmem.tevksel[0].kasel0).c_str());
  out.Write("		else\n"
            "			s.KonstColor = int4(konstLookup[%s].rgb, "
            "konstLookup[%s].a);\n\n",
            BitfieldExtract("tevksel", bpmem.tevksel[0].kcsel1).c_str(),
            BitfieldExtract("tevksel", bpmem.tevksel[0].kasel1).c_str());
  out.Write("\n");

  out.Write("		// Select Ras for stage\n"
            "		uint ras = %s;\n",
            BitfieldExtract("order", TwoTevStageOrders().colorchan0).c_str());
  out.Write(
      "		if (ras < 2u) { // Lighting Channel 0 or 1\n"
      "			int4 color = (ras == 0u) ? icolors_0 : icolors_1;\n"
      "			uint swap = %s;\n",
      BitfieldExtract("ac", TevStageCombiner().alphaC.rswap).c_str());
  out.Write("			s.RasColor = Swizzle(swap, color);\n");
  out.Write("		} else if (ras == 5u) { // Alpha Bumb\n"
            "			s.RasColor = int4(AlphaBump, AlphaBump, "
            "AlphaBump, AlphaBump);\n"
            "		} else if (ras == 6u) { // Normalzied Alpha Bump\n"
            "			int normalized = AlphaBump | AlphaBump >> 5;\n"
            "			s.RasColor = int4(normalized, normalized, "
            "normalized, normalized);\n"
            "		} else {\n"
            "			s.RasColor = int4(0, 0, 0, 0);\n"
            "		}\n"
            "\n");

  out.Write("		// This is the Meat of TEV\n"
            "		{\n"
            "			// Color Combiner\n");
  out.Write("\t\t\tuint color_a = %s;\n",
            BitfieldExtract("cc", TevStageCombiner().colorC.a).c_str());
  out.Write("\t\t\tuint color_b = %s;\n",
            BitfieldExtract("cc", TevStageCombiner().colorC.b).c_str());
  out.Write("\t\t\tuint color_c = %s;\n",
            BitfieldExtract("cc", TevStageCombiner().colorC.c).c_str());
  out.Write("\t\t\tuint color_d = %s;\n",
            BitfieldExtract("cc", TevStageCombiner().colorC.d).c_str());

  out.Write("\t\t\tuint color_bias = %s;\n",
            BitfieldExtract("cc", TevStageCombiner().colorC.bias).c_str());
  out.Write("\t\t\tbool color_op = bool(%s);\n",
            BitfieldExtract("cc", TevStageCombiner().colorC.op).c_str());
  out.Write("\t\t\tbool color_clamp = bool(%s);\n",
            BitfieldExtract("cc", TevStageCombiner().colorC.clamp).c_str());
  out.Write("\t\t\tuint color_shift = %s;\n",
            BitfieldExtract("cc", TevStageCombiner().colorC.shift).c_str());
  out.Write("\t\t\tuint color_dest = %s;\n",
            BitfieldExtract("cc", TevStageCombiner().colorC.dest).c_str());

  out.Write(
      "			uint color_compare_op = color_shift << 1 | "
      "uint(color_op);\n"
      "\n"
      "			int3 color_A = selectColorInput(s, color_a) & "
      "int3(255, 255, 255);\n"
      "			int3 color_B = selectColorInput(s, color_b) & "
      "int3(255, 255, 255);\n"
      "			int3 color_C = selectColorInput(s, color_c) & "
      "int3(255, 255, 255);\n"
      "			int3 color_D = selectColorInput(s, color_d);  "
      "// 10 bits + sign\n" // TODO: do we need
                            // to sign extend?
      "\n"
      "			int3 color;\n"
      "			if(color_bias != 3u) { // Normal mode\n"
      "				color.r = tevLerp(color_A.r, color_B.r, "
      "color_C.r, color_D.r, color_bias, color_op, "
      "color_shift);\n"
      "				color.g = tevLerp(color_A.g, color_B.g, "
      "color_C.g, color_D.g, color_bias, color_op, "
      "color_shift);\n"
      "				color.b = tevLerp(color_A.b, color_B.b, "
      "color_C.b, color_D.b, color_bias, color_op, "
      "color_shift);\n"
      "			} else { // Compare mode\n"
      "				// op 6 and 7 do a select per color channel\n"
      "				if (color_compare_op == 6u) {\n"
      "					// TEVCMP_RGB8_GT\n"
      "					color.r = (color_A.r > "
      "color_B.r) ? color_C.r : 0;\n"
      "					color.g = (color_A.g > "
      "color_B.g) ? color_C.g : 0;\n"
      "					color.b = (color_A.b > "
      "color_B.b) ? color_C.b : 0;\n"
      "				} else if (color_compare_op == 7u) {\n"
      "					// TEVCMP_RGB8_EQ\n"
      "					color.r = (color_A.r == "
      "color_B.r) ? color_C.r : 0;\n"
      "					color.g = (color_A.g == "
      "color_B.g) ? color_C.g : 0;\n"
      "					color.b = (color_A.b == "
      "color_B.b) ? color_C.b : 0;\n"
      "				} else {\n"
      "					// The remaining ops do one "
      "compare which selects all 3 channels\n"
      "					color = "
      "tevCompare(color_compare_op, color_A, color_B) ? color_C : int3(0, 0, "
      "0);\n"
      "				}\n"
      "				color = color_D + color;\n"
      "			}\n"
      "\n"
      "			// Clamp result\n"
      "			if (color_clamp)\n"
      "				color = clamp(color, 0, 255);\n"
      "			else\n"
      "				color = clamp(color, -1024, 1023);\n"
      "\n"
      "			if (stage == num_stages) { // If this is the "
      "last stage\n"
      "				// Write result to output\n"
      "				TevResult.rgb = color;\n"
      "			} else {\n"
      "				// Write result to the correct input "
      "register of the next stage\n"
      "				setRegColor(s, color_dest, color);\n"
      "			}\n"
      "\n");

  // Alpha combiner
  out.Write("		// Alpha Combiner\n");
  out.Write("\t\t\tuint alpha_a = %s;\n",
            BitfieldExtract("ac", TevStageCombiner().alphaC.a).c_str());
  out.Write("\t\t\tuint alpha_b = %s;\n",
            BitfieldExtract("ac", TevStageCombiner().alphaC.b).c_str());
  out.Write("\t\t\tuint alpha_c = %s;\n",
            BitfieldExtract("ac", TevStageCombiner().alphaC.c).c_str());
  out.Write("\t\t\tuint alpha_d = %s;\n",
            BitfieldExtract("ac", TevStageCombiner().alphaC.d).c_str());

  out.Write("\t\t\tuint alpha_bias = %s;\n",
            BitfieldExtract("ac", TevStageCombiner().alphaC.bias).c_str());
  out.Write("\t\t\tbool alpha_op = bool(%s);\n",
            BitfieldExtract("ac", TevStageCombiner().alphaC.op).c_str());
  out.Write("\t\t\tbool alpha_clamp = bool(%s);\n",
            BitfieldExtract("ac", TevStageCombiner().alphaC.clamp).c_str());
  out.Write("\t\t\tuint alpha_shift = %s;\n",
            BitfieldExtract("ac", TevStageCombiner().alphaC.shift).c_str());
  out.Write("\t\t\tuint alpha_dest = %s;\n",
            BitfieldExtract("ac", TevStageCombiner().alphaC.dest).c_str());

  out.Write(
      "			uint alpha_compare_op = alpha_shift << 1 | "
      "uint(alpha_op);\n"
      "\n"
      "			int alpha_A;\n"
      "			int alpha_B;\n"
      "			if (alpha_bias != 3u || alpha_compare_op > 5u) {\n"
      "				// Small optimisation here: alpha_A and "
      "alpha_B are unused by compare ops 0-5\n"
      "				alpha_A = selectAlphaInput(s, alpha_a) & 255;\n"
      "				alpha_B = selectAlphaInput(s, alpha_b) & 255;\n"
      "			};\n"
      "			int alpha_C = selectAlphaInput(s, alpha_c) & 255;\n"
      "			int alpha_D = selectAlphaInput(s, alpha_d); // "
      "10 bits + sign\n" // TODO: do we
                         // need to sign
                         // extend?
      "\n"
      "			int alpha;\n"
      "			if(alpha_bias != 3u) { // Normal mode\n"
      "				alpha = tevLerp(alpha_A, alpha_B, "
      "alpha_C, alpha_D, alpha_bias, alpha_op, "
      "alpha_shift);\n"
      "			} else { // Compare mode\n"
      "				if (alpha_compare_op == 6u) {\n"
      "					// TEVCMP_A8_GT\n"
      "					alpha = (alpha_A > alpha_B) ? "
      "alpha_C : 0;\n"
      "				} else if (alpha_compare_op == 7u) {\n"
      "					// TEVCMP_A8_EQ\n"
      "					alpha = (alpha_A == alpha_B) ? "
      "alpha_C : 0;\n"
      "				} else {\n"
      "					// All remaining alpha compare "
      "ops actually compare the color channels\n"
      "					alpha = "
      "tevCompare(alpha_compare_op, color_A, color_B) ? alpha_C : 0;\n"
      "				}\n"
      "				alpha = alpha_D + alpha;\n"
      "			}\n"
      "\n"
      "			// Clamp result\n"
      "			if (alpha_clamp)\n"
      "				alpha = clamp(alpha, 0, 255);\n"
      "			else\n"
      "				alpha = clamp(alpha, -1024, 1023);\n"
      "\n"
      "			if (stage == num_stages) { // If this is the "
      "last stage\n"
      "				// Write result to output\n"
      "				TevResult.a = alpha;\n"
      "				break;\n"
      "			} else {\n"
      "				// Write result to the correct input "
      "register of the next stage\n"
      "				setRegAlpha(s, alpha_dest, alpha);\n"
      "			}\n"
      "		}\n");

  out.Write("	} // Main tev loop\n"
            "\n");

  out.Write("	// Alpha Test\n"
            "	if (bpmem_alphaTest != 0u) {\n"
            "		bool comp0 = alphaCompare(TevResult.a, " I_ALPHA
            ".r, %s);\n",
            BitfieldExtract("bpmem_alphaTest", AlphaTest().comp0).c_str());
  out.Write("		bool comp1 = alphaCompare(TevResult.a, " I_ALPHA
            ".g, %s);\n",
            BitfieldExtract("bpmem_alphaTest", AlphaTest().comp1).c_str());
  out.Write("\n"
            "		// These if statements are written weirdly to work "
            "around intel and qualcom bugs "
            "with handling booleans.\n"
            "		switch (%s) {\n",
            BitfieldExtract("bpmem_alphaTest", AlphaTest().logic).c_str());
  out.Write(
      "		case 0u: // AND\n"
      "			if (comp0 && comp1) break; else discard; break;\n"
      "		case 1u: // OR\n"
      "			if (comp0 || comp1) break; else discard; break;\n"
      "		case 2u: // XOR\n"
      "			if (comp0 != comp1) break; else discard; break;\n"
      "		case 3u: // XNOR\n"
      "			if (comp0 == comp1) break; else discard; break;\n"
      "		}\n"
      "	}\n"
      "\n");

  out.Write("	// TODO: zCoord is hardcoded to fast depth with no zfreeze\n");
  if (ApiType == API_D3D)
    out.Write("	int zCoord = int((1.0 - rawpos.z) * 16777216.0);\n");
  else
    out.Write("	int zCoord = int(rawpos.z * 16777216.0);\n");
  out.Write("	zCoord = clamp(zCoord, 0, 0xFFFFFF);\n"
            "\n");

  // ===========
  //   ZFreeze
  // ===========

  if (!early_depth) { // Zfreeze forces early depth off
    out.Write("	// ZFreeze\n"
              "	if ((bpmem_genmode & %du) != 0u) {\n",
              1 << GenMode().zfreeze.offset);
    out.Write("		float2 screenpos = rawpos.xy * " I_EFBSCALE ".xy;\n");
    if (ApiType == API_OPENGL)
      out.Write("		// Opengl has reversed vertical screenspace "
                "coordiantes\n"
                "		screenpos.y = 528.0 - screenpos.y;\n");

    out.Write("		zCoord = int(" I_ZSLOPE ".z + " I_ZSLOPE
              ".x * screenpos.x + " I_ZSLOPE ".y * screenpos.y);\n"
              "\n"
              "		// If early depth is enabled, write to zbuffer "
              "before depth textures\n"
              "		if ((bpmem_zcontrol & %du) != 0u)\n",
              1 << PEControl().early_ztest.offset);
    if (ApiType == API_D3D)
      out.Write("	depth = 1.0 - float(zCoord) / 16777216.0;\n");
    else
      out.Write("	depth = float(zCoord) / 16777216.0;\n");
    out.Write("	}\n"
              "\n");
  }

  // =================
  //   Depth Texture
  // =================

  out.Write("	// Depth Texture\n"
            "	uint ztex_op = %s;\n",
            BitfieldExtract("bpmem_ztex2", ZTex2().op).c_str());
  out.Write(
      "	if (ztex_op != 0u) {\n"
      "		int ztex = int(" I_ZBIAS "[1].w); // fixed bias\n"
      "\n"
      "		// Whatever texture was in our last stage, it's now our "
      "depth texture\n"
      "		ztex += idot(s.TexColor.xyzw, " I_ZBIAS "[0].xyzw);\n"
      "		if (ztex_op == 1u)\n"
      "			ztex += zCoord;\n"
      "		zCoord = ztex & 0xFFFFFF;\n"
      "	}\n"
      "\n");

  if (!early_depth) {
    out.Write("	// If early depth isn't enabled, we write to the zbuffer here\n"
              "	if ((bpmem_zcontrol & %du) == 0u)\n",
              1 << PEControl().early_ztest.offset);
    if (ApiType == API_D3D)
      out.Write("		depth = 1.0 - float(zCoord) / 16777216.0;\n");
    else
      out.Write("		depth = float(zCoord) / 16777216.0;\n");
  }

  // =========
  //    Fog
  // =========

  // FIXME: Fog is implemented the same as ShaderGen, but ShaderGen's fog is all
  // hacks.
  //        Should be fixed point, and should not make guesses about Range-Based
  //        adjustments.
  out.Write("	// Fog\n"
            "	uint fog_function = %s;\n",
            BitfieldExtract("bpmem_fogParam3", FogParam3().fsel).c_str());
  out.Write("	if (fog_function != 0u) {\n"
            "		// TODO: This all needs to be converted from float to "
            "fixed point\n"
            "		float ze;\n"
            "		if (%s == 0u) {\n",
            BitfieldExtract("bpmem_fogParam3", FogParam3().proj).c_str());
  out.Write(
      "			// perspective\n"
      "			// ze = A/(B - (Zs >> B_SHF)\n"
      "			ze = (" I_FOGF "[1].x * 16777216.0) / float(" I_FOGI
      ".y - (zCoord >> " I_FOGI ".w));\n"
      "		} else {\n"
      "			// orthographic\n"
      "			// ze = a*Zs    (here, no B_SHF)\n"
      "			ze = " I_FOGF "[1].x * float(zCoord) / 16777216.0;\n"
      "		}\n"
      "\n"
      "		if (bool(%s)) {\n",
      BitfieldExtract("bpmem_fogRangeBase", FogRangeParams::RangeBase().Enabled)
          .c_str());
  out.Write(
      "			// x_adjust = sqrt((x-center)^2 + k^2)/k\n"
      "			// ze *= x_adjust\n"
      "			// TODO Instead of this theoretical "
      "calculation, we should use the\n"
      "			//      coefficient table given in the fog "
      "range BP registers!\n"
      "			float x_adjust = (2.0 * (rawpos.x / " I_FOGF
      "[0].y)) - 1.0 - " I_FOGF "[0].x; \n"
      "			x_adjust = sqrt(x_adjust * x_adjust + " I_FOGF
      "[0].z * " I_FOGF "[0].z) / " I_FOGF "[0].z;\n"
      "			ze *= x_adjust;\n"
      "		}\n"
      "\n"
      "		float fog = clamp(ze - " I_FOGF "[1].z, 0.0, 1.0);\n"
      "\n"
      "		if (fog_function > 3u) {\n"
      "			switch (fog_function) {\n"
      "			case 4u:\n"
      "				fog = 1.0 - exp2(-8.0 * fog);\n"
      "				break;\n"
      "			case 5u:\n"
      "				fog = 1.0 - exp2(-8.0 * fog * fog);\n"
      "				break;\n"
      "			case 6u:\n"
      "				fog = exp2(-8.0 * (1.0 - fog));\n"
      "				break;\n"
      "			case 7u:\n"
      "				fog = 1.0 - fog;\n"
      "				fog = exp2(-8.0 * fog * fog);\n"
      "				break;\n"
      "			}\n"
      "		}\n"
      "\n"
      "		int ifog = iround(fog * 256.0);\n"
      "		TevResult.rgb = (TevResult.rgb * (256 - ifog) + " I_FOGCOLOR
      ".rgb * ifog) >> 8;\n"
      "	}\n"
      "\n");

  // TODO: Do we still want to support two pass alpha blending?
  out.Write("	ocol0 = float4(TevResult) / 255.0;\n"
            "\n"
            "	// Dest alpha override (dual source blening)\n"
            "	// Colors will be blended against the alpha from ocol1 and\n"
            "	// the alpha from ocol0 will be written to the framebuffer.\n"
            "	ocol1 = float4(TevResult) / 255.0; \n"
            "	if (bpmem_dstalpha != 0u) {\n"
            "		ocol0.a = float(%s) / 255.0;\n",
            BitfieldExtract("bpmem_dstalpha", ConstantAlpha().alpha).c_str());
  out.Write("	}\n"
            "\n");

  out.Write("}");

  return out;
}
}
