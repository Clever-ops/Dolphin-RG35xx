// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/LightingShaderGen.h"

#include "Common/Assert.h"
#include "Common/CommonTypes.h"

#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/ShaderGenCommon.h"
#include "VideoCommon/XFMemory.h"

static void GenerateLightShader(ShaderCode& object, const LightingUidData& uid_data, int index,
                                int litchan_index, bool alpha)
{
  const char* swizzle = alpha ? "a" : "rgb";
  const char* swizzle_components = (alpha) ? "" : "3";

  const auto attnfunc =
      static_cast<AttenuationFunc>((uid_data.attnfunc >> (2 * litchan_index)) & 0x3);
  const auto diffusefunc =
      static_cast<DiffuseFunc>((uid_data.diffusefunc >> (2 * litchan_index)) & 0x3);

  switch (attnfunc)
  {
  case AttenuationFunc::None:
  case AttenuationFunc::Dir:
    object.Write("ldir = normalize(" LIGHT_POS ".xyz - pos.xyz);\n", LIGHT_POS_PARAMS(index));
    object.Write("attn = 1.0;\n");
    object.Write("if (length(ldir) == 0.0)\n\t ldir = _normal;\n");
    break;
  case AttenuationFunc::Spec:
    object.Write("ldir = normalize(" LIGHT_POS ".xyz - pos.xyz);\n", LIGHT_POS_PARAMS(index));
    object.Write("attn = (dot(_normal, ldir) >= 0.0) ? max(0.0, dot(_normal, " LIGHT_DIR
                 ".xyz)) : 0.0;\n",
                 LIGHT_DIR_PARAMS(index));
    object.Write("cosAttn = " LIGHT_COSATT ".xyz;\n", LIGHT_COSATT_PARAMS(index));
    object.Write("distAttn = {}(" LIGHT_DISTATT ".xyz);\n",
                 (diffusefunc == DiffuseFunc::None) ? "" : "normalize",
                 LIGHT_DISTATT_PARAMS(index));
    object.Write("attn = max(0.0f, dot(cosAttn, float3(1.0, attn, attn*attn))) / dot(distAttn, "
                 "float3(1.0, attn, attn*attn));\n");
    break;
  case AttenuationFunc::Spot:
    object.Write("ldir = " LIGHT_POS ".xyz - pos.xyz;\n", LIGHT_POS_PARAMS(index));
    object.Write("dist2 = dot(ldir, ldir);\n"
                 "dist = sqrt(dist2);\n"
                 "ldir = ldir / dist;\n"
                 "attn = max(0.0, dot(ldir, " LIGHT_DIR ".xyz));\n",
                 LIGHT_DIR_PARAMS(index));
    // attn*attn may overflow
    object.Write("attn = max(0.0, " LIGHT_COSATT ".x + " LIGHT_COSATT ".y*attn + " LIGHT_COSATT
                 ".z*attn*attn) / dot(" LIGHT_DISTATT ".xyz, float3(1.0,dist,dist2));\n",
                 LIGHT_COSATT_PARAMS(index), LIGHT_COSATT_PARAMS(index), LIGHT_COSATT_PARAMS(index),
                 LIGHT_DISTATT_PARAMS(index));
    break;
  }

  switch (diffusefunc)
  {
  case DiffuseFunc::None:
    object.Write("lacc.{} += int{}(round(attn * float{}(" LIGHT_COL ")));\n", swizzle,
                 swizzle_components, swizzle_components, LIGHT_COL_PARAMS(index, swizzle));
    break;
  case DiffuseFunc::Sign:
  case DiffuseFunc::Clamp:
    object.Write("lacc.{} += int{}(round(attn * {}dot(ldir, _normal)) * float{}(" LIGHT_COL
                 ")));\n",
                 swizzle, swizzle_components, diffusefunc != DiffuseFunc::Sign ? "max(0.0," : "(",
                 swizzle_components, LIGHT_COL_PARAMS(index, swizzle));
    break;
  default:
    ASSERT(false);
  }

  object.Write("\n");
}

// vertex shader
// lights/colors
// materials name is I_MATERIALS in vs and I_PMATERIALS in ps
// inColorName is color in vs and colors_ in ps
// dest is o.colors_ in vs and colors_ in ps
void GenerateLightingShaderCode(ShaderCode& object, const LightingUidData& uid_data,
                                std::string_view in_color_name, std::string_view dest)
{
  for (u32 j = 0; j < NUM_XF_COLOR_CHANNELS; j++)
  {
    object.Write("{{\n");

    const bool colormatsource = !!(uid_data.matsource & (1 << j));
    if (colormatsource)  // from vertex
      object.Write("int4 mat = int4(round({}{} * 255.0));\n", in_color_name, j);
    else  // from color
      object.Write("int4 mat = {}[{}];\n", I_MATERIALS, j + 2);

    if ((uid_data.enablelighting & (1 << j)) != 0)
    {
      if ((uid_data.ambsource & (1 << j)) != 0)  // from vertex
        object.Write("lacc = int4(round({}{} * 255.0));\n", in_color_name, j);
      else  // from color
        object.Write("lacc = {}[{}];\n", I_MATERIALS, j);
    }
    else
    {
      object.Write("lacc = int4(255, 255, 255, 255);\n");
    }

    // check if alpha is different
    const bool alphamatsource = !!(uid_data.matsource & (1 << (j + 2)));
    if (alphamatsource != colormatsource)
    {
      if (alphamatsource)  // from vertex
        object.Write("mat.w = int(round({}{}.w * 255.0));\n", in_color_name, j);
      else  // from color
        object.Write("mat.w = {}[{}].w;\n", I_MATERIALS, j + 2);
    }

    if ((uid_data.enablelighting & (1 << (j + 2))) != 0)
    {
      if ((uid_data.ambsource & (1 << (j + 2))) != 0)  // from vertex
        object.Write("lacc.w = int(round({}{}.w * 255.0));\n", in_color_name, j);
      else  // from color
        object.Write("lacc.w = {}[{}].w;\n", I_MATERIALS, j);
    }
    else
    {
      object.Write("lacc.w = 255;\n");
    }

    if ((uid_data.enablelighting & (1 << j)) != 0)  // Color lights
    {
      for (int i = 0; i < 8; ++i)
      {
        if ((uid_data.light_mask & (1 << (i + 8 * j))) != 0)
          GenerateLightShader(object, uid_data, i, j, false);
      }
    }
    if ((uid_data.enablelighting & (1 << (j + 2))) != 0)  // Alpha lights
    {
      for (int i = 0; i < 8; ++i)
      {
        if ((uid_data.light_mask & (1 << (i + 8 * (j + 2)))) != 0)
          GenerateLightShader(object, uid_data, i, j + 2, true);
      }
    }
    object.Write("lacc = clamp(lacc, 0, 255);\n");
    object.Write("{}{} = float4((mat * (lacc + (lacc >> 7))) >> 8) / 255.0;\n", dest, j);
    object.Write("}}\n");
  }
}

void GetLightingShaderUid(LightingUidData& uid_data)
{
  for (u32 j = 0; j < NUM_XF_COLOR_CHANNELS; j++)
  {
    uid_data.matsource |= static_cast<u32>(xfmem.color[j].matsource.Value()) << j;
    uid_data.matsource |= static_cast<u32>(xfmem.alpha[j].matsource.Value()) << (j + 2);
    uid_data.enablelighting |= xfmem.color[j].enablelighting << j;
    uid_data.enablelighting |= xfmem.alpha[j].enablelighting << (j + 2);

    if ((uid_data.enablelighting & (1 << j)) != 0)  // Color lights
    {
      uid_data.ambsource |= static_cast<u32>(xfmem.color[j].ambsource.Value()) << j;
      uid_data.attnfunc |= static_cast<u32>(xfmem.color[j].attnfunc.Value()) << (2 * j);
      uid_data.diffusefunc |= static_cast<u32>(xfmem.color[j].diffusefunc.Value()) << (2 * j);
      uid_data.light_mask |= xfmem.color[j].GetFullLightMask() << (8 * j);
    }
    if ((uid_data.enablelighting & (1 << (j + 2))) != 0)  // Alpha lights
    {
      uid_data.ambsource |= static_cast<u32>(xfmem.alpha[j].ambsource.Value()) << (j + 2);
      uid_data.attnfunc |= static_cast<u32>(xfmem.alpha[j].attnfunc.Value()) << (2 * (j + 2));
      uid_data.diffusefunc |= static_cast<u32>(xfmem.alpha[j].diffusefunc.Value()) << (2 * (j + 2));
      uid_data.light_mask |= xfmem.alpha[j].GetFullLightMask() << (8 * (j + 2));
    }
  }
}

void GenerateCustomLightingHeaderDetails(ShaderCode* out, u32 enablelighting, u32 light_mask)
{
  u32 light_count = 0;
  for (u32 j = 0; j < NUM_XF_COLOR_CHANNELS; j++)
  {
    if ((enablelighting & (1 << j)) != 0)  // Color lights
    {
      for (int i = 0; i < 8; ++i)
      {
        if ((light_mask & (1 << (i + 8 * j))) != 0)
        {
          light_count++;
        }
      }
    }
    if ((enablelighting & (1 << (j + 2))) != 0)  // Alpha lights
    {
      for (int i = 0; i < 8; ++i)
      {
        if ((light_mask & (1 << (i + 8 * (j + 2)))) != 0)
        {
          light_count++;
        }
      }
    }
  }
  if (light_count > 0)
  {
    out->Write("\tCustomShaderLightData[{}] light;\n", light_count);
  }
  else
  {
    // Cheat so shaders compile
    out->Write("\tCustomShaderLightData[1] light;\n", light_count);
  }
  out->Write("\tint light_count;\n");
}

void GenerateCustomLightingImplementation(ShaderCode* out, const LightingUidData& uid_data,
                                          std::string_view in_color_name)
{
  auto generate_lighting = [](ShaderCode* out, const LightingUidData& uid_data, int index,
                              int litchan_index, u32 custom_light_index, bool alpha) {
    const auto attnfunc =
        static_cast<AttenuationFunc>((uid_data.attnfunc >> (2 * litchan_index)) & 0x3);

    out->Write("\t{{\n");
    out->Write("\t\tcustom_data.lights[{}].direction = " LIGHT_DIR ".xyz;\n", custom_light_index,
               LIGHT_DIR_PARAMS(index));
    out->Write("\t\tcustom_data.lights[{}].position = " LIGHT_POS ".xyz;\n", custom_light_index,
               LIGHT_POS_PARAMS(index));
    out->Write("\t\tcustom_data.lights[{}].cosatt = " LIGHT_COSATT ";\n", custom_light_index,
               LIGHT_COSATT_PARAMS(index));
    out->Write("\t\tcustom_data.lights[{}].distatt = " LIGHT_DISTATT ";\n", custom_light_index,
               LIGHT_DISTATT_PARAMS(index));
    out->Write("\t\tcustom_data.lights[{}].attenuation_type = {};\n", custom_light_index,
               static_cast<u32>(attnfunc));
    if (alpha)
    {
      out->Write("\t\tcustom_data.lights[{}].color = float3(" LIGHT_COL
                 ") / float3(255.0, 255.0, 255.0);\n",
                 custom_light_index, LIGHT_COL_PARAMS(index, alpha ? "a" : "rgb"));
    }
    else
    {
      out->Write("\t\tcustom_data.lights[{}].color = " LIGHT_COL
                 " / float3(255.0, 255.0, 255.0);\n",
                 custom_light_index, LIGHT_COL_PARAMS(index, alpha ? "a" : "rgb"));
    }
    out->Write("\t\tcustom_data.lights[{}].light_type = {};\n", custom_light_index,
               alpha ? "CUSTOM_SHADER_LIGHTING_LIGHT_TYPE_ALPHA" :
                       "CUSTOM_SHADER_LIGHTING_LIGHT_TYPE_COLOR");
    out->Write("\t}}\n");
  };

  u32 light_count = 0;
  for (u32 j = 0; j < NUM_XF_COLOR_CHANNELS; j++)
  {
    const bool colormatsource = !!(uid_data.matsource & (1 << j));
    if (colormatsource)  // from vertex
      out->Write("custom_data.base_material[{}] = {}{};\n", j, in_color_name, j);
    else  // from color
      out->Write("custom_data.base_material[{}] = {}[{}] / 255.0;\n", j, I_MATERIALS, j + 2);

    if ((uid_data.enablelighting & (1 << j)) != 0)
    {
      if ((uid_data.ambsource & (1 << j)) != 0)  // from vertex
        out->Write("custom_data.ambient_lighting[{}] = {}{};\n", j, in_color_name, j);
      else  // from color
        out->Write("custom_data.ambient_lighting[{}] = {}[{}] / 255.0;\n", j, I_MATERIALS, j);
    }
    else
    {
      out->Write("custom_data.ambient_lighting[{}] = float4(1, 1, 1, 1);\n", j);
    }

    // check if alpha is different
    const bool alphamatsource = !!(uid_data.matsource & (1 << (j + 2)));
    if (alphamatsource != colormatsource)
    {
      if (alphamatsource)  // from vertex
        out->Write("custom_data.base_material[{}].w = {}{}.w;\n", j, in_color_name, j);
      else  // from color
        out->Write("custom_data.base_material[{}].w = {}[{}].w / 255.0;\n", j, I_MATERIALS, j + 2);
    }

    if ((uid_data.enablelighting & (1 << (j + 2))) != 0)
    {
      if ((uid_data.ambsource & (1 << (j + 2))) != 0)  // from vertex
        out->Write("custom_data.ambient_lighting[{}].w = {}{}.w;\n", j, in_color_name, j);
      else  // from color
        out->Write("custom_data.ambient_lighting[{}].w = {}[{}].w / 255.0;\n", j, I_MATERIALS, j);
    }
    else
    {
      out->Write("custom_data.ambient_lighting[{}].w = 1;\n", j);
    }

    if ((uid_data.enablelighting & (1 << j)) != 0)  // Color lights
    {
      for (int i = 0; i < 8; ++i)
      {
        if ((uid_data.light_mask & (1 << (i + 8 * j))) != 0)
        {
          generate_lighting(out, uid_data, i, j, light_count, false);
          light_count++;
        }
      }
    }
    if ((uid_data.enablelighting & (1 << (j + 2))) != 0)  // Alpha lights
    {
      for (int i = 0; i < 8; ++i)
      {
        if ((uid_data.light_mask & (1 << (i + 8 * (j + 2)))) != 0)
        {
          generate_lighting(out, uid_data, i, j + 2, light_count, true);
          light_count++;
        }
      }
    }
  }
  for (u32 i = light_count; i < 8; i++)
  {
    out->Write("\tcustom_data.lights[{}].direction = float3(0, 0, 0);\n", i);
    out->Write("\tcustom_data.lights[{}].position = float3(0, 0, 0);\n", i);
    out->Write("\tcustom_data.lights[{}].color = float3(0, 0, 0);\n", i);
    out->Write("\tcustom_data.lights[{}].cosatt = float4(0, 0, 0, 0);\n", i);
    out->Write("\tcustom_data.lights[{}].distatt = float4(0, 0, 0, 0);\n", i);
    out->Write("\tcustom_data.lights[{}].attenuation_type = 0;\n", i);
    out->Write("\tcustom_data.lights[{}].light_type = CUSTOM_SHADER_LIGHTING_LIGHT_TYPE_ALPHA;\n",
               i);
  }
  out->Write("\tcustom_data.light_count = {};\n", light_count);
}
