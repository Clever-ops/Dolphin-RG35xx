// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>

#include "Common/BitFieldView.h"
#include "Core/HW/WiimoteEmu/Extension/Extension.h"
#include "InputCommon/ControllerEmu/Setting/NumericSetting.h"

namespace ControllerEmu
{
class AnalogStick;
class Buttons;
class ControlGroup;
}  // namespace ControllerEmu

namespace WiimoteEmu
{
enum class DrumsGroup
{
  Buttons,
  Pads,
  Stick
};

// The Drums use the "1st-party" extension encryption scheme.
class Drums : public Extension1stParty
{
public:
  enum class VelocityID : u8
  {
    None = 0b1111111,
    Bass = 0b1011011,
    // TODO: Implement HiHatPedal.
    // HiHatPedal = 0b0011011,
    Red = 0b1011001,
    Yellow = 0b1010001,
    Blue = 0b1001111,
    Orange = 0b1001110,
    Green = 0b1010010,
  };

#pragma pack(push, 1)
  struct DataFormat
  {
    u32 _data1;

    BFVIEW_M(_data1, u8, 0, 6, stick_x);
    // Seemingly random.
    BFVIEW_M(_data1, u8, 6, 2, unk1);

    BFVIEW_M(_data1, u8, 8, 6, stick_y);
    // Seemingly random.
    BFVIEW_M(_data1, u8, 14, 2, unk2);

    // Always 1 with no velocity data and seemingly random otherwise.
    BFVIEW_M(_data1, u8, 16, 1, unk3);
    // For which "pad" the velocity data is for.
    BFVIEW_M(_data1, VelocityID, 17, 7, velocity_id);

    // Always 1 with no velocity data and seemingly random otherwise.
    BFVIEW_M(_data1, u8, 24, 1, unk4);
    // 1 with no velocity data and 0 when velocity data is present.
    BFVIEW_M(_data1, bool, 25, 1, no_velocity_data_1);
    // These two bits seem to always be set. (0b11)
    BFVIEW_M(_data1, u8, 26, 2, unk5);
    // 1 with no velocity data and 0 when velocity data is present.
    BFVIEW_M(_data1, bool, 28, 1, no_velocity_data_2);
    // How "soft" a drum pad has been hit as a range from 0:very-hard to 7:very-soft.
    BFVIEW_M(_data1, u8, 29, 3, softness);

    // Button bits.
    u8 buttons;

    // Drum-pad bits.
    u8 drum_pads;
  };
#pragma pack(pop)
  static_assert(sizeof(DataFormat) == 6, "Wrong size");

  Drums();

  void Update() override;
  void Reset() override;

  ControllerEmu::ControlGroup* GetGroup(DrumsGroup group);

  void DoState(PointerWrap& p) override;

  // FYI: The low/high bits of the button byte are "random" when velocity data is present.
  // static constexpr u8 HAVE_VELOCITY_DATA = 0b10000001;
  static constexpr u8 BUTTON_PLUS = 0x04;
  static constexpr u8 BUTTON_MINUS = 0x10;

  // FYI: The hi-hat pedal sets no bits here.
  static constexpr u8 PAD_BASS = 0x04;
  static constexpr u8 PAD_BLUE = 0x08;
  static constexpr u8 PAD_GREEN = 0x10;
  static constexpr u8 PAD_YELLOW = 0x20;
  static constexpr u8 PAD_RED = 0x40;
  static constexpr u8 PAD_ORANGE = 0x80;

  // Note: My hardware's octagon stick produced the complete range of values (0 - 0x3f)
  // It also had perfect center values of 0x20 with absolutely no "play".
  static constexpr ControlState GATE_RADIUS = 1.0;
  static constexpr u8 STICK_MIN = 0x00;
  static constexpr u8 STICK_CENTER = 0x20;
  static constexpr u8 STICK_MAX = 0x3f;

private:
  ControllerEmu::Buttons* m_buttons;
  ControllerEmu::Buttons* m_pads;
  ControllerEmu::AnalogStick* m_stick;

  ControllerEmu::SettingValue<double> m_hit_strength_setting;

  // Holds previous user input state to watch for "new" hits.
  u8 m_prev_pad_input = 0;
  // Holds new drum pad hits that still need velocity data to be sent.
  u8 m_new_pad_hits = 0;
  // Holds how many more frames to send each drum-pad bit.
  std::array<u8, 6> m_pad_remaining_frames{};
};
}  // namespace WiimoteEmu
