// Copyright 2015 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Null/NullVertexManager.h"

namespace Null
{
VertexManager::VertexManager() = default;

VertexManager::~VertexManager() = default;

u32 VertexManager::DrawCurrentBatch(u32 base_index, u32 num_indices, u32 base_vertex)
{
  return 0;
}

}  // namespace Null
