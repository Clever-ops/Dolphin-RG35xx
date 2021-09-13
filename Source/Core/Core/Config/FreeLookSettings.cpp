// Copyright 2020 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Config/FreeLookSettings.h"
#include "Core/FreeLookConfig.h"

#include <string>

#include "Common/Config/Config.h"

namespace Config
{
// Configuration Information
const Info<bool> FREE_LOOK_ENABLED{{System::FreeLook, "General", "Enabled"}, false};

// FreeLook.Controller1
const Info<FreeLook::ControlType> FL1_CONTROL_TYPE{{System::FreeLook, "Camera1", "ControlType"},
                                                   FreeLook::ControlType::SixAxis};
const Info<std::string> FL1_UDP_ADDRESS{{System::FreeLook, "Camera1", "UDPAddress"}, "127.0.0.1"};
const Info<u16> FL1_UDP_PORT{{System::FreeLook, "Camera1", "UDPPort"}, 36760};

}  // namespace Config
