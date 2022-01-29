// Copyright 2017 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerEmu/Setting/NumericSetting.h"

namespace ControllerEmu
{
  class PrimeHackMorph : public ControlGroup
  {
  public:
    explicit PrimeHackMorph(const std::string& name, const std::string& default_selection);

    const std::string& GetMorphBallProfileName() const;
    void SetMorphBallProfileName(std::string& val);

  private:
    std::string m_selection_value;
    std::string m_previous_selection;
  };
}  // namespace ControllerEmu
