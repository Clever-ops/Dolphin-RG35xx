// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string_view>

#include "Common/CommonTypes.h"
#include "Common/MathUtil.h"
#include "Common/Matrix.h"

#include "VideoCommon/XFMemory.h"

namespace GraphicsModActionData
{
struct DrawStarted
{
  u32 scissors_x;
  u32 scissors_y;
  float viewport_x;
  float viewport_y;
  float viewport_width;
  float viewport_height;
  bool* skip;
};

struct EFB
{
  u32 texture_width;
  u32 texture_height;
  MathUtil::Rectangle<int> src_rect;
  bool* skip;
  u32* scaled_width;
  u32* scaled_height;
};

struct Projection
{
  ProjectionType projection_type;
  Common::Matrix44* matrix;
};
struct TextureLoad
{
  std::string_view texture_name;
};
}  // namespace GraphicsModActionData
