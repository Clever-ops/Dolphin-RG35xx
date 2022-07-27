// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/WII_IPC.h"

#include <array>
#include <string_view>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Core/CoreTiming.h"
#include "Core/HW/DVD/DVDInterface.h"
#include "Core/HW/MMIO.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/IOS/IOS.h"
#include "Core/System.h"

// This is the intercommunication between ARM and PPC. Currently only PPC actually uses it, because
// of the IOS HLE
// How IOS uses IPC:
// X1 Execute command: a new pointer is available in HW_IPC_PPCCTRL
// X2 Reload (a new IOS is being loaded, old one doesn't need to reply anymore)
// Y1 Command executed and reply available in HW_IPC_ARMMSG
// Y2 Command acknowledge
// m_ppc_msg is a pointer to 0x40byte command structure
// m_arm_msg is, similarly, starlet's response buffer*

namespace IOS
{
enum
{
  IPC_PPCMSG = 0x00,
  IPC_PPCCTRL = 0x04,
  IPC_ARMMSG = 0x08,
  IPC_ARMCTRL = 0x0c,

  PPCSPEED = 0x18,
  VISOLID = 0x24,

  PPC_IRQFLAG = 0x30,
  PPC_IRQMASK = 0x34,
  ARM_IRQFLAG = 0x38,
  ARM_IRQMASK = 0x3c,

  GPIOB_OUT = 0xc0,
  GPIOB_DIR = 0xc4,
  GPIOB_IN = 0xc8,

  GPIO_OUT = 0xe0,
  GPIO_DIR = 0xe4,
  GPIO_IN = 0xe8,

  HW_RESETS = 0x194,

  UNK_180 = 0x180,
  UNK_1CC = 0x1cc,
  UNK_1D0 = 0x1d0,
};

// Indicates which pins are accessible by broadway.  Writable by starlet only.
static constexpr Common::Flags<GPIO> gpio_owner = {GPIO::SLOT_LED, GPIO::SLOT_IN, GPIO::SENSOR_BAR,
                                                   GPIO::DO_EJECT, GPIO::AVE_SCL, GPIO::AVE_SDA};

static u32 resets;

struct I2CState
{
  bool active;
  u8 bit_counter;
  bool read_i2c_address;
  bool is_correct_i2c_address;
  bool is_read;
  bool read_ave_address;
  bool acknowledge;
  u8 current_byte;
  u8 current_address;
};
I2CState i2c_state;
#pragma pack(1)
struct AVEState
{
  // See https://wiibrew.org/wiki/Hardware/AV_Encoder#Registers_description
  // (note that the code snippet indicates that values are big-endian)
  u8 timings;                             // 0x00
  u8 video_output_config;                 // 0x01
  u8 vertical_blanking_interval_control;  // 0x02
  u8 composite_trap_filter_control;       // 0x03
  u8 audio_video_output_control;          // 0x04
  u8 cgms_high, cgms_low;                 // 0x05-0x06
  u8 pad1;                                // 0x07
  u8 wss_high, wss_low;                   // 0x08-0x09, Widescreen signaling?
  u8 rgb_color_output_control;            // 0x0A, only used when video_output_config is DEBUG (3)?
  std::array<u8, 5> pad2;                 // 0x0B-0x0F
  std::array<u8, 33> gamma_coefficients;  // 0x10-0x30
  std::array<u8, 15> pad3;                // 0x31-0x3F
  std::array<u8, 26> macrovision_code;    // 0x40-0x59, analog copy protection
  std::array<u8, 8> pad4;                 // 0x5A-0x61
  u8 rgb_switch;                          // 0x62, swap blue and red channels
  std::array<u8, 2> pad5;                 // 0x63-0x64
  u8 color_dac;                           // 0x65
  u8 pad6;                                // 0x66
  u8 color_test;                          // 0x67, display a color test pattern
  std::array<u8, 2> pad7;                 // 0x68-0x69
  u8 ccsel;                               // 0x6A
  std::array<u8, 2> pad8;                 // 0x6B-0x6C
  u8 mute;                                // 0x6D
  u8 rgb_output_filter;                   // 0x6E
  std::array<u8, 2> pad9;                 // 0x6F-0x70
  u8 right_volume;                        // 0x71
  u8 left_volume;                         // 0x72
  std::array<u8, 7> pad10;                // 0x73-0x79
  std::array<u8, 4> closed_captioning;    // 0x7A-0x7D
  std::array<u8, 130> pad11;              // 0x7E-0xFF
};
#pragma pack()
static_assert(sizeof(AVEState) == 0x100);
static AVEState ave_state;

static CoreTiming::EventType* updateInterrupts;

WiiIPC::WiiIPC(Core::System& system) : m_system(system)
{
}

WiiIPC::~WiiIPC() = default;

void WiiIPC::DoState(PointerWrap& p)
{
  p.Do(m_ppc_msg);
  p.Do(m_arm_msg);
  p.Do(m_ctrl);
  p.Do(m_ppc_irq_flags);
  p.Do(m_ppc_irq_masks);
  p.Do(m_arm_irq_flags);
  p.Do(m_arm_irq_masks);
  p.Do(m_gpio_dir);
  p.Do(m_gpio_out);
  p.Do(m_resets);
}

void WiiIPC::InitState()
{
  m_ctrl = CtrlRegister();
  m_ppc_msg = 0;
  m_arm_msg = 0;

  m_ppc_irq_flags = 0;
  m_ppc_irq_masks = 0;
  m_arm_irq_flags = 0;
  m_arm_irq_masks = 0;

  // The only inputs are POWER, EJECT_BTN, SLOT_IN, EEP_MISO, and sometimes AVE_SCL and AVE_SDA;
  // Broadway only has access to SLOT_IN, AVE_SCL, and AVE_SDA.
  m_gpio_dir = {
      GPIO::POWER,      GPIO::SHUTDOWN, GPIO::FAN,    GPIO::DC_DC,   GPIO::DI_SPIN,  GPIO::SLOT_LED,
      GPIO::SENSOR_BAR, GPIO::DO_EJECT, GPIO::EEP_CS, GPIO::EEP_CLK, GPIO::EEP_MOSI, GPIO::AVE_SCL,
      GPIO::AVE_SDA,    GPIO::DEBUG0,   GPIO::DEBUG1, GPIO::DEBUG2,  GPIO::DEBUG3,   GPIO::DEBUG4,
      GPIO::DEBUG5,     GPIO::DEBUG6,   GPIO::DEBUG7,
  };
  m_gpio_out = {};

  // A cleared bit indicates the device is reset/off, so set everything to 1 (this may not exactly
  // match hardware)
  m_resets = 0xffffffff;

  m_ppc_irq_masks |= INT_CAUSE_IPC_BROADWAY;

  i2c_state = {};
  ave_state = {};
}

void WiiIPC::Init()
{
  InitState();
  m_event_type_update_interrupts =
      m_system.GetCoreTiming().RegisterEvent("IPCInterrupt", UpdateInterruptsCallback);
}

void WiiIPC::Reset()
{
  INFO_LOG_FMT(WII_IPC, "Resetting ...");
  InitState();
}

void WiiIPC::Shutdown()
{
}

static std::string_view GetAVERegisterName(u8 address)
{
  if (address == 0x00)
    return "A/V Timings";
  else if (address == 0x01)
    return "Video Output configuration";
  else if (address == 0x02)
    return "Vertical blanking interval (VBI) control";
  else if (address == 0x03)
    return "Composite Video Trap Filter control";
  else if (address == 0x04)
    return "A/V output control";
  else if (address == 0x05 || address == 0x06)
    return "CGMS protection";
  else if (address == 0x08 || address == 0x09)
    return "WSS (Widescreen signaling)";
  else if (address == 0x0A)
    return "RGB color output control";
  else if (address >= 0x10 && address <= 0x30)
    return "Gamma coefficients";
  else if (address >= 0x40 && address <= 0x59)
    return "Macrovision code";
  else if (address == 0x62)
    return "RGB switch control";
  else if (address == 0x65)
    return "Color DAC control";
  else if (address == 0x67)
    return "Color Test";
  else if (address == 0x6A)
    return "CCSEL";
  else if (address == 0x6D)
    return "Audio mute control";
  else if (address == 0x6E)
    return "RGB output filter";
  else if (address == 0x71)
    return "Audio stereo output control - right volume";
  else if (address == 0x72)
    return "Audio stereo output control - right volume";
  else if (address >= 0x7a && address <= 0x7d)
    return "Closed Captioning control";
  else
    return fmt::format("Unknown ({:02x})", address);
}

static u32 ReadGPIOIn(Core::System& system)
{
  Common::Flags<GPIO> gpio_in;
  gpio_in[GPIO::SLOT_IN] = system.GetDVDInterface().IsDiscInside();
  // Note: This doesn't implement the direction logic currently (are bits not included in the
  // direction treated as clear?)
  if (i2c_state.bit_counter == 9 && i2c_state.acknowledge)
    gpio_in[GPIO::AVE_SDA] = false;  // pull low
  else
    gpio_in[GPIO::AVE_SDA] = true;  // passive pullup
  return gpio_in.m_hex;
}

void WiiIPC::WriteGPIOOut(Core::System& system, bool broadway, u32 value)
{
  Common::Flags<GPIO> old_value = m_gpio_out;

  if (broadway)
    m_gpio_out.m_hex = (value & gpio_owner.m_hex) | (m_gpio_out.m_hex & ~gpio_owner.m_hex);
  else
    m_gpio_out.m_hex = (value & ~gpio_owner.m_hex) | (m_gpio_out.m_hex & gpio_owner.m_hex);

  if (m_gpio_out[GPIO::DO_EJECT])
  {
    INFO_LOG_FMT(WII_IPC, "Ejecting disc due to GPIO write");
    system.GetDVDInterface().EjectDisc(DVD::EjectCause::Software);
  }

  // I²C logic for the audio/video encoder (AVE)
  if (m_gpio_dir[GPIO::AVE_SCL])
  {
    if (old_value[GPIO::AVE_SCL] && m_gpio_out[GPIO::AVE_SCL])
    {
      // Check for changes to SDA while the clock is high. This only makes sense if the SDA pin is
      // outbound.
      if (m_gpio_dir[GPIO::AVE_SDA])
      {
        if (old_value[GPIO::AVE_SDA] && !m_gpio_out[GPIO::AVE_SDA])
        {
          DEBUG_LOG_FMT(WII_IPC, "AVE: Start I2C");
          // SDA falling edge (now pulled low) while SCL is high indicates I²C start
          i2c_state.active = true;
          i2c_state.acknowledge = false;
          i2c_state.bit_counter = 0;
          i2c_state.read_i2c_address = false;
          i2c_state.is_correct_i2c_address = false;
          i2c_state.read_ave_address = false;
        }
        else if (!old_value[GPIO::AVE_SDA] && m_gpio_out[GPIO::AVE_SDA])
        {
          DEBUG_LOG_FMT(WII_IPC, "AVE: Stop I2C");
          // SDA rising edge (now passive pullup) while SCL is high indicates I²C stop
          i2c_state.active = false;
          i2c_state.bit_counter = 0;
        }
      }
    }
    else if (!old_value[GPIO::AVE_SCL] && m_gpio_out[GPIO::AVE_SCL])
    {
      // Clock changed from low to high; transfer a new bit.
      if (i2c_state.active && (!i2c_state.read_i2c_address || i2c_state.is_correct_i2c_address))
      {
        if (i2c_state.bit_counter == 9)
        {
          // Note: 9 not 8, as an extra clock is spent for acknowleding
          i2c_state.acknowledge = false;
          i2c_state.current_byte = 0;
          i2c_state.bit_counter = 0;
        }

        // Rising edge: a new bit
        if (i2c_state.bit_counter < 8)
        {
          i2c_state.current_byte <<= 1;
          if (m_gpio_out[GPIO::AVE_SDA])
            i2c_state.current_byte |= 1;
        }

        if (i2c_state.bit_counter == 8)
        {
          i2c_state.acknowledge = true;

          DEBUG_LOG_FMT(WII_IPC, "AVE: New byte: {:02x}", i2c_state.current_byte);

          if (!i2c_state.read_i2c_address)
          {
            i2c_state.read_i2c_address = true;
            if ((i2c_state.current_byte >> 1) == 0x70)
            {
              i2c_state.is_correct_i2c_address = true;
            }
            else
            {
              WARN_LOG_FMT(WII_IPC, "AVE: Wrong I2C address: {:02x}", i2c_state.current_byte >> 1);
              i2c_state.acknowledge = false;
              i2c_state.is_correct_i2c_address = false;
            }

            if ((i2c_state.current_byte & 1) == 0)
            {
              i2c_state.is_read = false;
            }
            else
            {
              WARN_LOG_FMT(WII_IPC, "AVE: Reads aren't implemented yet");
              i2c_state.is_read = true;
              i2c_state.acknowledge = false;  // until reads are implemented
            }
          }
          else if (!i2c_state.read_ave_address)
          {
            i2c_state.read_ave_address = true;
            i2c_state.current_address = i2c_state.current_byte;
            DEBUG_LOG_FMT(WII_IPC, "AVE address: {:02x} ({})", i2c_state.current_address,
                          GetAVERegisterName(i2c_state.current_address));
          }
          else
          {
            // This is always inbounds, as we're indexing with a u8 and the struct is 0x100 bytes
            const u8 old_ave_value = reinterpret_cast<u8*>(&ave_state)[i2c_state.current_address];
            reinterpret_cast<u8*>(&ave_state)[i2c_state.current_address] = i2c_state.current_byte;
            if (old_ave_value != i2c_state.current_byte)
            {
              INFO_LOG_FMT(WII_IPC, "AVE: Wrote {:02x} to {:02x} ({})", i2c_state.current_byte,
                           i2c_state.current_address,
                           GetAVERegisterName(i2c_state.current_address));
            }
            else
            {
              DEBUG_LOG_FMT(WII_IPC, "AVE: Wrote {:02x} to {:02x} ({})", i2c_state.current_byte,
                            i2c_state.current_address,
                            GetAVERegisterName(i2c_state.current_address));
            }
            i2c_state.current_address++;
          }
        }

        i2c_state.bit_counter++;
      }
    }
  }

  // SENSOR_BAR is checked by WiimoteEmu::CameraLogic
  // TODO: SLOT_LED
}

void WiiIPC::RegisterMMIO(MMIO::Mapping* mmio, u32 base)
{
  mmio->Register(base | IPC_PPCMSG, MMIO::InvalidRead<u32>(), MMIO::DirectWrite<u32>(&m_ppc_msg));

  mmio->Register(base | IPC_PPCCTRL, MMIO::ComplexRead<u32>([](Core::System& system, u32) {
                   auto& wii_ipc = system.GetWiiIPC();
                   return wii_ipc.m_ctrl.ppc();
                 }),
                 MMIO::ComplexWrite<u32>([](Core::System& system, u32, u32 val) {
                   auto& wii_ipc = system.GetWiiIPC();
                   wii_ipc.m_ctrl.ppc(val);
                   // The IPC interrupt is triggered when IY1/IY2 is set and
                   // Y1/Y2 is written to -- even when this results in clearing the bit.
                   if ((val >> 2 & 1 && wii_ipc.m_ctrl.IY1) || (val >> 1 & 1 && wii_ipc.m_ctrl.IY2))
                     wii_ipc.m_ppc_irq_flags |= INT_CAUSE_IPC_BROADWAY;
                   if (wii_ipc.m_ctrl.X1)
                     HLE::GetIOS()->EnqueueIPCRequest(wii_ipc.m_ppc_msg);
                   HLE::GetIOS()->UpdateIPC();
                   system.GetCoreTiming().ScheduleEvent(0, wii_ipc.m_event_type_update_interrupts,
                                                        0);
                 }));

  mmio->Register(base | IPC_ARMMSG, MMIO::DirectRead<u32>(&m_arm_msg), MMIO::InvalidWrite<u32>());

  mmio->Register(base | PPC_IRQFLAG, MMIO::InvalidRead<u32>(),
                 MMIO::ComplexWrite<u32>([](Core::System& system, u32, u32 val) {
                   auto& wii_ipc = system.GetWiiIPC();
                   wii_ipc.m_ppc_irq_flags &= ~val;
                   HLE::GetIOS()->UpdateIPC();
                   system.GetCoreTiming().ScheduleEvent(0, wii_ipc.m_event_type_update_interrupts,
                                                        0);
                 }));

  mmio->Register(base | PPC_IRQMASK, MMIO::InvalidRead<u32>(),
                 MMIO::ComplexWrite<u32>([](Core::System& system, u32, u32 val) {
                   auto& wii_ipc = system.GetWiiIPC();
                   wii_ipc.m_ppc_irq_masks = val;
                   if (wii_ipc.m_ppc_irq_masks & INT_CAUSE_IPC_BROADWAY)  // wtf?
                     wii_ipc.Reset();
                   HLE::GetIOS()->UpdateIPC();
                   system.GetCoreTiming().ScheduleEvent(0, wii_ipc.m_event_type_update_interrupts,
                                                        0);
                 }));

  mmio->Register(base | GPIOB_OUT, MMIO::DirectRead<u32>(&m_gpio_out.m_hex),
                 MMIO::ComplexWrite<u32>([](Core::System& system, u32, u32 val) {
                   auto& wii_ipc = system.GetWiiIPC();
                   wii_ipc.WriteGPIOOut(system, true, val);
                 }));
  mmio->Register(base | GPIOB_DIR, MMIO::DirectRead<u32>(&m_gpio_dir.m_hex),
                 MMIO::ComplexWrite<u32>([](Core::System& system, u32, u32 val) {
                   auto& wii_ipc = system.GetWiiIPC();
                   wii_ipc.m_gpio_dir.m_hex =
                       (val & gpio_owner.m_hex) | (wii_ipc.m_gpio_dir.m_hex & ~gpio_owner.m_hex);
                 }));
  mmio->Register(base | GPIOB_IN, MMIO::ComplexRead<u32>([](Core::System& system, u32) {
                   return ReadGPIOIn(system);
                 }),
                 MMIO::Nop<u32>());
  // Starlet GPIO registers, not normally accessible by PPC (but they can be depending on how
  // AHBPROT is set up).  We just always allow access, since some homebrew uses them.

  // Note from WiiBrew: When switching owners, copying of the data is not necessary. For example, if
  // pin 0 has certain configuration in the HW_GPIO registers, and that bit is then set in the
  // HW_GPIO_OWNER register, those settings will immediately be visible in the HW_GPIOB registers.
  // There is only one set of data registers, and the HW_GPIO_OWNER register just controls the
  // access that the HW_GPIOB registers have to that data.
  // Also: The HW_GPIO registers always have read access to all pins, but any writes (changes) must
  // go through the HW_GPIOB registers if the corresponding bit is set in the HW_GPIO_OWNER
  // register.
  mmio->Register(base | GPIO_OUT, MMIO::DirectRead<u32>(&m_gpio_out.m_hex),
                 MMIO::ComplexWrite<u32>([](Core::System& system, u32, u32 val) {
                   auto& wii_ipc = system.GetWiiIPC();
                   wii_ipc.WriteGPIOOut(system, false, val);
                 }));
  mmio->Register(base | GPIO_DIR, MMIO::DirectRead<u32>(&m_gpio_dir.m_hex),
                 MMIO::ComplexWrite<u32>([](Core::System& system, u32, u32 val) {
                   auto& wii_ipc = system.GetWiiIPC();
                   wii_ipc.m_gpio_dir.m_hex =
                       (wii_ipc.m_gpio_dir.m_hex & gpio_owner.m_hex) | (val & ~gpio_owner.m_hex);
                 }));
  mmio->Register(base | GPIO_IN, MMIO::ComplexRead<u32>([](Core::System& system, u32) {
                   return ReadGPIOIn(system);
                 }),
                 MMIO::Nop<u32>());

  mmio->Register(base | HW_RESETS, MMIO::DirectRead<u32>(&m_resets),
                 MMIO::ComplexWrite<u32>([](Core::System& system, u32, u32 val) {
                   // A reset occurs when the corresponding bit is cleared
                   auto& wii_ipc = system.GetWiiIPC();
                   const bool di_reset_triggered = (wii_ipc.m_resets & 0x400) && !(val & 0x400);
                   wii_ipc.m_resets = val;
                   if (di_reset_triggered)
                   {
                     // The GPIO *disables* spinning up the drive
                     const bool spinup = !wii_ipc.m_gpio_out[GPIO::DI_SPIN];
                     INFO_LOG_FMT(WII_IPC, "Resetting DI {} spinup", spinup ? "with" : "without");
                     system.GetDVDInterface().ResetDrive(spinup);
                   }
                 }));

  // Register some stubbed/unknown MMIOs required to make Wii games work.
  mmio->Register(base | PPCSPEED, MMIO::InvalidRead<u32>(), MMIO::Nop<u32>());
  mmio->Register(base | VISOLID, MMIO::InvalidRead<u32>(), MMIO::Nop<u32>());
  mmio->Register(base | UNK_180, MMIO::Constant<u32>(0), MMIO::Nop<u32>());
  mmio->Register(base | UNK_1CC, MMIO::Constant<u32>(0), MMIO::Nop<u32>());
  mmio->Register(base | UNK_1D0, MMIO::Constant<u32>(0), MMIO::Nop<u32>());
}

void WiiIPC::UpdateInterruptsCallback(Core::System& system, u64 userdata, s64 cycles_late)
{
  system.GetWiiIPC().UpdateInterrupts();
}

void WiiIPC::UpdateInterrupts()
{
  if ((m_ctrl.Y1 & m_ctrl.IY1) || (m_ctrl.Y2 & m_ctrl.IY2))
  {
    m_ppc_irq_flags |= INT_CAUSE_IPC_BROADWAY;
  }

  if ((m_ctrl.X1 & m_ctrl.IX1) || (m_ctrl.X2 & m_ctrl.IX2))
  {
    m_ppc_irq_flags |= INT_CAUSE_IPC_STARLET;
  }

  // Generate interrupt on PI if any of the devices behind starlet have an interrupt and mask is set
  m_system.GetProcessorInterface().SetInterrupt(ProcessorInterface::INT_CAUSE_WII_IPC,
                                                !!(m_ppc_irq_flags & m_ppc_irq_masks));
}

void WiiIPC::ClearX1()
{
  m_ctrl.X1 = 0;
}

void WiiIPC::GenerateAck(u32 address)
{
  m_ctrl.Y2 = 1;
  DEBUG_LOG_FMT(WII_IPC, "GenerateAck: {:08x} | {:08x} [R:{} A:{} E:{}]", m_ppc_msg, address,
                m_ctrl.Y1, m_ctrl.Y2, m_ctrl.X1);
  // Based on a hardware test, the IPC interrupt takes approximately 100 TB ticks to fire
  // after Y2 is seen in the control register.
  m_system.GetCoreTiming().ScheduleEvent(100 * SystemTimers::TIMER_RATIO,
                                         m_event_type_update_interrupts);
}

void WiiIPC::GenerateReply(u32 address)
{
  m_arm_msg = address;
  m_ctrl.Y1 = 1;
  DEBUG_LOG_FMT(WII_IPC, "GenerateReply: {:08x} | {:08x} [R:{} A:{} E:{}]", m_ppc_msg, address,
                m_ctrl.Y1, m_ctrl.Y2, m_ctrl.X1);
  // Based on a hardware test, the IPC interrupt takes approximately 100 TB ticks to fire
  // after Y1 is seen in the control register.
  m_system.GetCoreTiming().ScheduleEvent(100 * SystemTimers::TIMER_RATIO,
                                         m_event_type_update_interrupts);
}

bool WiiIPC::IsReady() const
{
  return ((m_ctrl.Y1 == 0) && (m_ctrl.Y2 == 0) &&
          ((m_ppc_irq_flags & INT_CAUSE_IPC_BROADWAY) == 0));
}
}  // namespace IOS
