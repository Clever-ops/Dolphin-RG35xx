// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/Assets/TextureAsset.h"

#include "Common/JsonUtil.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "VideoCommon/BPMemory.h"

namespace VideoCommon
{
namespace
{
bool ParseSampler(const VideoCommon::CustomAssetLibrary::AssetID& asset_id,
                  const picojson::object& json, SamplerState* sampler)
{
  if (!sampler) [[unlikely]]
    return false;

  *sampler = RenderState::GetLinearSamplerState();

  const auto sampler_state_wrap_iter = json.find("wrap_mode");
  if (sampler_state_wrap_iter != json.end())
  {
    if (!sampler_state_wrap_iter->second.is<picojson::object>())
    {
      ERROR_LOG_FMT(VIDEO, "Asset '{}' failed to parse json, 'wrap_mode' is not the right type",
                    asset_id);
      return false;
    }
    const auto sampler_state_wrap_obj = sampler_state_wrap_iter->second.get<picojson::object>();

    for (const auto& uv : std::array<std::string, 2>{"u", "v"})
    {
      auto uv_mode = ReadStringFromJson(sampler_state_wrap_obj, uv).value_or("");
      Common::ToLower(&uv_mode);

      if (uv_mode == "clamp")
      {
        if (uv == "u")
        {
          sampler->tm0.wrap_u = WrapMode::Clamp;
        }
        else
        {
          sampler->tm0.wrap_v = WrapMode::Clamp;
        }
      }
      else if (uv_mode == "repeat")
      {
        if (uv == "u")
        {
          sampler->tm0.wrap_u = WrapMode::Repeat;
        }
        else
        {
          sampler->tm0.wrap_v = WrapMode::Repeat;
        }
      }
      else if (uv_mode == "mirror")
      {
        if (uv == "u")
        {
          sampler->tm0.wrap_u = WrapMode::Mirror;
        }
        else
        {
          sampler->tm0.wrap_v = WrapMode::Mirror;
        }
      }
      else
      {
        ERROR_LOG_FMT(VIDEO,
                      "Asset '{}' failed to parse json, 'wrap_mode[{}]' has an invalid "
                      "value '{}'",
                      asset_id, uv, uv_mode);
        return false;
      }
    }
  }

  const auto sampler_state_filter_iter = json.find("filter_mode");
  if (sampler_state_filter_iter != json.end())
  {
    if (!sampler_state_filter_iter->second.is<picojson::object>())
    {
      ERROR_LOG_FMT(VIDEO, "Asset '{}' failed to parse json, 'filter_mode' is not the right type",
                    asset_id);
      return false;
    }
    const auto sampler_state_filter_obj = sampler_state_filter_iter->second.get<picojson::object>();

    for (const auto& filter_type : std::array<std::string, 3>{"min", "mag", "mipmap"})
    {
      auto filter = ReadStringFromJson(sampler_state_filter_obj, filter_type).value_or("");
      Common::ToLower(&filter);
      if (filter == "linear")
      {
        if (filter_type == "min")
        {
          sampler->tm0.min_filter = FilterMode::Linear;
        }
        else if (filter_type == "mag")
        {
          sampler->tm0.mag_filter = FilterMode::Linear;
        }
        else
        {
          sampler->tm0.mipmap_filter = FilterMode::Linear;
        }
      }
      else if (filter == "near")
      {
        if (filter_type == "min")
        {
          sampler->tm0.min_filter = FilterMode::Near;
        }
        else if (filter_type == "mag")
        {
          sampler->tm0.mag_filter = FilterMode::Near;
        }
        else
        {
          sampler->tm0.mipmap_filter = FilterMode::Near;
        }
      }
      else
      {
        ERROR_LOG_FMT(VIDEO,
                      "Asset '{}' failed to parse json, 'filter_mode[{}]' has an invalid "
                      "value '{}'",
                      asset_id, filter_type, filter);
        return false;
      }
    }
  }

  return true;
}
}  // namespace
bool TextureData::FromJson(const CustomAssetLibrary::AssetID& asset_id,
                           const picojson::object& json, TextureData* data)
{
  const auto type_iter = json.find("type");
  if (type_iter == json.end())
  {
    ERROR_LOG_FMT(VIDEO, "Asset '{}' failed to parse json, property entry 'type' not found",
                  asset_id);
    return false;
  }
  if (!type_iter->second.is<std::string>())
  {
    ERROR_LOG_FMT(VIDEO,
                  "Asset '{}' failed to parse json, property entry 'type' is not "
                  "the right json type",
                  asset_id);
    return false;
  }
  std::string type = type_iter->second.to_str();
  Common::ToLower(&type);

  if (type == "texture2d")
  {
    data->m_type = TextureData::Type::Type_Texture2D;

    if (!ParseSampler(asset_id, json, &data->m_sampler))
    {
      return false;
    }
  }
  else if (type == "texturecube")
  {
    data->m_type = TextureData::Type::Type_TextureCube;
  }
  else
  {
    ERROR_LOG_FMT(VIDEO,
                  "Asset '{}' failed to parse json, texture type '{}' "
                  "an invalid option",
                  asset_id, type);
    return false;
  }

  return true;
}

void TextureData::ToJson(picojson::object* obj, const TextureData& data)
{
  if (!obj) [[unlikely]]
    return;

  auto& json_obj = *obj;
  switch (data.m_type)
  {
  case TextureData::Type::Type_Texture2D:
    json_obj.emplace("type", "texture2d");
    break;
  case TextureData::Type::Type_TextureCube:
    json_obj.emplace("type", "texturecube");
    break;
  case TextureData::Type::Type_Undefined:
    break;
  };

  auto wrap_mode_to_string = [](WrapMode mode) {
    switch (mode)
    {
    case WrapMode::Clamp:
      return "clamp";
    case WrapMode::Mirror:
      return "mirror";
    case WrapMode::Repeat:
      return "repeat";
    };

    return "";
  };
  auto filter_mode_to_string = [](FilterMode mode) {
    switch (mode)
    {
    case FilterMode::Linear:
      return "linear";
    case FilterMode::Near:
      return "near";
    };

    return "";
  };

  picojson::object wrap_mode;
  wrap_mode.emplace("u", wrap_mode_to_string(data.m_sampler.tm0.wrap_u));
  wrap_mode.emplace("v", wrap_mode_to_string(data.m_sampler.tm0.wrap_v));
  json_obj.emplace("wrap_mode", wrap_mode);

  picojson::object filter_mode;
  filter_mode.emplace("min", filter_mode_to_string(data.m_sampler.tm0.min_filter));
  filter_mode.emplace("mag", filter_mode_to_string(data.m_sampler.tm0.mag_filter));
  filter_mode.emplace("mipmap", filter_mode_to_string(data.m_sampler.tm0.mipmap_filter));
  json_obj.emplace("filter_mode", filter_mode);
}

CustomAssetLibrary::LoadInfo GameTextureAsset::LoadImpl(const CustomAssetLibrary::AssetID& asset_id)
{
  auto potential_data = std::make_shared<TextureData>();
  const auto loaded_info = m_owning_library->LoadGameTexture(asset_id, potential_data.get());
  if (loaded_info.m_bytes_loaded == 0)
    return {};
  {
    std::lock_guard lk(m_data_lock);
    m_loaded = true;
    m_data = std::move(potential_data);
  }
  return loaded_info;
}

bool GameTextureAsset::Validate(u32 native_width, u32 native_height) const
{
  std::lock_guard lk(m_data_lock);

  if (!m_loaded)
  {
    ERROR_LOG_FMT(VIDEO,
                  "Game texture can't be validated for asset '{}' because it is not loaded yet.",
                  GetAssetId());
    return false;
  }

  if (m_data->m_texture.m_slices.empty())
  {
    ERROR_LOG_FMT(VIDEO,
                  "Game texture can't be validated for asset '{}' because no data was available.",
                  GetAssetId());
    return false;
  }

  if (m_data->m_texture.m_slices.size() > 1)
  {
    ERROR_LOG_FMT(
        VIDEO,
        "Game texture can't be validated for asset '{}' because it has more slices than expected.",
        GetAssetId());
    return false;
  }

  const auto& slice = m_data->m_texture.m_slices[0];
  if (slice.m_levels.empty())
  {
    ERROR_LOG_FMT(
        VIDEO,
        "Game texture can't be validated for asset '{}' because first slice has no data available.",
        GetAssetId());
    return false;
  }

  // Verify that the aspect ratio of the texture hasn't changed, as this could have
  // side-effects.
  const VideoCommon::CustomTextureData::ArraySlice::Level& first_mip = slice.m_levels[0];
  if (first_mip.width * native_height != first_mip.height * native_width)
  {
    // Note: this feels like this should return an error but
    // for legacy reasons this is only a notice that something *could*
    // go wrong
    WARN_LOG_FMT(
        VIDEO,
        "Invalid custom texture size {}x{} for game texture asset '{}'. The aspect differs "
        "from the native size {}x{}.",
        first_mip.width, first_mip.height, GetAssetId(), native_width, native_height);
  }

  // Same deal if the custom texture isn't a multiple of the native size.
  if (native_width != 0 && native_height != 0 &&
      (first_mip.width % native_width || first_mip.height % native_height))
  {
    // Note: this feels like this should return an error but
    // for legacy reasons this is only a notice that something *could*
    // go wrong
    WARN_LOG_FMT(
        VIDEO,
        "Invalid custom texture size {}x{} for game texture asset '{}'. Please use an integer "
        "upscaling factor based on the native size {}x{}.",
        first_mip.width, first_mip.height, GetAssetId(), native_width, native_height);
  }

  return true;
}
}  // namespace VideoCommon
