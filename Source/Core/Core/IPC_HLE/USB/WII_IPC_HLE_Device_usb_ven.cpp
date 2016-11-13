// Copyright 2016 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <cstring>
#include <map>
#include <memory>
#include <utility>

#include "Common/ChunkFile.h"
#include "Common/CommonFuncs.h"
#include "Common/Event.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/Memmap.h"
#include "Core/IPC_HLE/USB/Common.h"
#include "Core/IPC_HLE/USB/USBV5.h"
#include "Core/IPC_HLE/USB/WII_IPC_HLE_Device_usb_ven.h"
#include "Core/IPC_HLE/WII_IPC_HLE_Device.h"

CWII_IPC_HLE_Device_usb_ven::CWII_IPC_HLE_Device_usb_ven(const u32 device_id,
                                                         const std::string& device_name)
    : CWII_IPC_HLE_Device_usb_host(device_id, device_name)
{
  m_ready_to_trigger_hooks.Set();
  // Force a device scan to complete, because some games (including Your Shape) only care
  // about the initial device list (in the first GETDEVICECHANGE reply).
  UpdateDevices();
}

CWII_IPC_HLE_Device_usb_ven::~CWII_IPC_HLE_Device_usb_ven()
{
  StopThreads();
}

static const std::map<u32, u32> s_expected_num_parameters = {
    {USBV5_IOCTL_CTRLMSG, 2},
    {USBV5_IOCTL_INTRMSG, 2},
    {USBV5_IOCTL_BULKMSG, 2},
    {USBV5_IOCTL_ISOMSG, 4},
};

IPCCommandResult CWII_IPC_HLE_Device_usb_ven::IOCtlV(const u32 command_address)
{
  const SIOCtlVBuffer cmd_buffer(command_address);
  switch (cmd_buffer.Parameter)
  {
  case USBV5_IOCTL_CTRLMSG:
  case USBV5_IOCTL_INTRMSG:
  case USBV5_IOCTL_BULKMSG:
  case USBV5_IOCTL_ISOMSG:
  {
    if (cmd_buffer.NumberInBuffer + cmd_buffer.NumberPayloadBuffer !=
        s_expected_num_parameters.at(cmd_buffer.Parameter))
    {
      Memory::Write_U32(FS_EINVAL, command_address + 4);
      return GetDefaultReply();
    }

    auto device = GetDeviceByIOSID(std::make_unique<USBV5TransferCommand>(cmd_buffer)->device_id);
    if (Core::g_want_determinism || !device)
    {
      Memory::Write_U32(FS_ENOENT, command_address + 4);
      return GetDefaultReply();
    }
    int ret = -1;
    if (cmd_buffer.Parameter == USBV5_IOCTL_CTRLMSG)
      ret = device->SubmitTransfer(std::make_unique<USBV5CtrlMessage>(cmd_buffer));
    else if (cmd_buffer.Parameter == USBV5_IOCTL_INTRMSG)
      ret = device->SubmitTransfer(std::make_unique<USBV5IntrMessage>(cmd_buffer));
    else if (cmd_buffer.Parameter == USBV5_IOCTL_BULKMSG)
      ret = device->SubmitTransfer(std::make_unique<USBV5BulkMessage>(cmd_buffer));
    else if (cmd_buffer.Parameter == USBV5_IOCTL_ISOMSG)
      ret = device->SubmitTransfer(std::make_unique<USBV5IsoMessage>(cmd_buffer));

    if (ret < 0)
    {
      ERROR_LOG(WII_IPC_USB, "[%04x:%04x] Failed to submit transfer (IOCtlV %d): %s",
                device->GetVid(), device->GetPid(), cmd_buffer.Parameter,
                device->GetErrorName(ret).c_str());
      Memory::Write_U32(FS_ENOENT, command_address + 4);
      return GetDefaultReply();
    }
    Memory::Write_U32(FS_SUCCESS, command_address + 4);
    return GetNoReply();
  }
  default:
    ERROR_LOG(WII_IPC_USB, "Unknown IOCtlV: %x", cmd_buffer.Parameter);
    Memory::Write_U32(FS_EINVAL, command_address + 4);
    return GetDefaultReply();
  }
}

IPCCommandResult CWII_IPC_HLE_Device_usb_ven::IOCtl(const u32 command_address)
{
  const IOCtlBuffer cmd_buffer(command_address);
  Memory::Write_U32(FS_SUCCESS, command_address + 4);
  INFO_LOG(WII_IPC_USB, "/dev/usb/ven - IOCtl %d", cmd_buffer.m_request);
  switch (cmd_buffer.m_request)
  {
  case USBV5_IOCTL_GETVERSION:
    Memory::Write_U32(VERSION, cmd_buffer.m_out_buffer_addr);
    return GetDefaultReply();

  case USBV5_IOCTL_GETDEVICECHANGE:
  {
    std::lock_guard<std::mutex> lk{m_devicechange_hook_address_mutex};
    m_devicechange_hook_address = command_address;
    // On the first call, the reply is sent immediately (instead of on device insertion/removal)
    if (m_devicechange_first_call)
    {
      TriggerDeviceChangeReply();
      m_devicechange_first_call = false;
    }
    return GetNoReply();
  }

  case USBV5_IOCTL_SHUTDOWN:
  {
    std::lock_guard<std::mutex> lk{m_devicechange_hook_address_mutex};
    if (m_devicechange_hook_address != 0)
    {
      // Write the return value.
      Memory::Write_U32(-1, m_devicechange_hook_address);
      WII_IPC_HLE_Interface::EnqueueReply(m_devicechange_hook_address);
      m_devicechange_hook_address = 0;
    }
    return GetDefaultReply();
  }

  case USBV5_IOCTL_GETDEVPARAMS:
  {
    const s32 device_id = Memory::Read_U32(cmd_buffer.m_in_buffer_addr);
    auto device = GetDeviceByIOSID(device_id);
    if (Core::g_want_determinism || !device)
    {
      Memory::Write_U32(FS_ENOENT, command_address + 4);
      return GetDefaultReply();
    }
    INFO_LOG(WII_IPC_USB, "[%04x:%04x %d] GETDEVPARAMS in:\n%s", device->GetVid(), device->GetPid(),
             device->GetInterface(), ArrayToString(Memory::GetPointer(cmd_buffer.m_in_buffer_addr),
                                                   cmd_buffer.m_in_buffer_size, 4)
                                         .c_str());
    if (cmd_buffer.m_out_buffer_size != 0xc0)
    {
      WARN_LOG(WII_IPC_USB, "Invalid output buffer size (%d)", cmd_buffer.m_out_buffer_size);
      Memory::Write_U32(FS_EINVAL, command_address + 4);
      return GetDefaultReply();
    }
    const u8 alt_setting = Memory::Read_U8(cmd_buffer.m_in_buffer_addr + 8);
    std::vector<u8> descriptors = device->GetIOSDescriptors(IOSVersion::USBV5, alt_setting);
    if (descriptors.empty())
    {
      Memory::Write_U32(FS_EINVAL, command_address + 4);
      return GetDefaultReply();
    }
    descriptors.resize(cmd_buffer.m_out_buffer_size - 20);
    if (descriptors.size() > cmd_buffer.m_out_buffer_size - 20)
      WARN_LOG(WII_IPC_USB, "Buffer is too large. Only the first 172 bytes will be copied.");
    Memory::Memset(cmd_buffer.m_out_buffer_addr, 0, cmd_buffer.m_out_buffer_size);
    Memory::Write_U32(device_id, cmd_buffer.m_out_buffer_addr);
    Memory::Write_U32(1, cmd_buffer.m_out_buffer_addr + 4);
    Memory::CopyToEmu(cmd_buffer.m_out_buffer_addr + 20, descriptors.data(), descriptors.size());
    return GetDefaultReply();
  }

  case USBV5_IOCTL_ATTACHFINISH:
    return GetDefaultReply();

  case USBV5_IOCTL_SETALTERNATE:
  {
    const s32 device_id = Memory::Read_U32(cmd_buffer.m_in_buffer_addr);
    const u8 alt_setting =
        static_cast<u8>(Memory::Read_U32(cmd_buffer.m_in_buffer_addr + 2 * sizeof(s32)));

    auto device = GetDeviceByIOSID(device_id);
    if (Core::g_want_determinism || !device)
    {
      Memory::Write_U32(FS_ENOENT, command_address + 4);
      return GetDefaultReply();
    }
    device->SetAltSetting(alt_setting);
    return GetDefaultReply();
  }

  case USBV5_IOCTL_SUSPEND_RESUME:
  {
    const s32 device_id = Memory::Read_U32(cmd_buffer.m_in_buffer_addr);
    const s32 resumed = Memory::Read_U32(cmd_buffer.m_in_buffer_addr + 2 * sizeof(s32));
    auto device = GetDeviceByIOSID(device_id);
    if (Core::g_want_determinism || !device)
    {
      Memory::Write_U32(FS_ENOENT, command_address + 4);
      return GetDefaultReply();
    }
    // Note: this is unimplemented because there's no easy way to do this in a
    // platform-independant way (libusb does not support power management).
    INFO_LOG(WII_IPC_USB, "[%04x:%04x %d] Received %s command", device->GetVid(), device->GetPid(),
             device->GetInterface(), resumed == 0 ? "suspend" : "resume");
    if (resumed)
      device->AttachDevice();
    return GetDefaultReply();
  }

  case USBV5_IOCTL_CANCELENDPOINT:
  {
    const s32 device_id = Memory::Read_U32(cmd_buffer.m_in_buffer_addr);
    const u8 endpoint =
        static_cast<u8>(Memory::Read_U32(cmd_buffer.m_in_buffer_addr + 2 * sizeof(s32)));
    auto device = GetDeviceByIOSID(device_id);
    if (Core::g_want_determinism || !device)
    {
      Memory::Write_U32(FS_ENOENT, command_address + 4);
      return GetDefaultReply();
    }
    Memory::Write_U32(device->CancelTransfer(endpoint), command_address + 4);
    return GetDefaultReply();
  }

  default:
    ERROR_LOG(WII_IPC_USB, "Unknown IOCtl: %x", cmd_buffer.m_request);
    ERROR_LOG(
        WII_IPC_USB, "In (size %d)\n%s", cmd_buffer.m_in_buffer_size,
        ArrayToString(Memory::GetPointer(cmd_buffer.m_in_buffer_addr), cmd_buffer.m_in_buffer_size)
            .c_str());
    ERROR_LOG(WII_IPC_USB, "Out (size %d)\n%s", cmd_buffer.m_out_buffer_size,
              ArrayToString(Memory::GetPointer(cmd_buffer.m_out_buffer_addr),
                            cmd_buffer.m_out_buffer_size)
                  .c_str());
    return GetDefaultReply();
  }
}

void CWII_IPC_HLE_Device_usb_ven::DoState(PointerWrap& p)
{
  CWII_IPC_HLE_Device_usb_host::DoState(p);
  p.Do(m_devicechange_first_call);
  p.Do(m_devicechange_hook_address);
  p.Do(m_ios_ids_to_device_ids_map);
  p.Do(m_device_ids_to_ios_ids_map);
}

std::shared_ptr<USBDevice> CWII_IPC_HLE_Device_usb_ven::GetDeviceByIOSID(const s32 ios_id) const
{
  std::lock_guard<std::mutex> lk{m_id_map_mutex};
  if (m_ios_ids_to_device_ids_map.count(ios_id) == 0)
    return nullptr;
  return GetDeviceById(m_ios_ids_to_device_ids_map.at(ios_id));
}

void CWII_IPC_HLE_Device_usb_ven::OnDeviceChange(ChangeEvent event, std::shared_ptr<USBDevice> dev)
{
  std::lock_guard<std::mutex> id_map_lock{m_id_map_mutex};
  if (event == ChangeEvent::Inserted)
  {
    // IOS's device list contains entries of the form:
    //   e7 XX 00 YY  VV VV PP PP  00 YY DD AA
    //
    // The first part is the device ID.
    // - XX is 1e (for a device plugged in to the left port) + DD (interface number).
    // - YY appears to start at 21, then 24, then is incremented every time a device is
    //   plugged in or unplugged.
    // - e7 can sometimes and randomly be e6 instead, for no obvious reason.
    //
    // The second part is the device's vendor ID and product ID.
    // The third part is unknown, and is what libogc currently calls the "device token".
    // However, it can be split into 4 bytes:
    // - 00: unknown.
    // - YY: refer to the device ID.
    // - DD: interface number (since VEN exposes each interface as a separate device).
    // - AA: number of alternate settings on this interface.
    s32 id = static_cast<u32>(0xe7) << 24 | static_cast<u32>(0x1e + dev->GetInterface()) << 16 |
             static_cast<u32>(0x00) << 8 | static_cast<u32>(m_device_number);
    while (m_ios_ids_to_device_ids_map.count(id) != 0)
    {
      id = static_cast<u32>(0xe7) << 24 | static_cast<u32>(0x1e + dev->GetInterface()) << 16 |
           static_cast<u32>(0x00) << 8 | static_cast<u32>(++m_device_number);
    }
    m_ios_ids_to_device_ids_map.emplace(id, dev->GetId());
    m_device_ids_to_ios_ids_map.emplace(dev->GetId(), id);
  }
  else if (event == ChangeEvent::Removed)
  {
    m_ios_ids_to_device_ids_map.erase(m_device_ids_to_ios_ids_map.at(dev->GetId()));
    m_device_ids_to_ios_ids_map.erase(dev->GetId());
  }
}

void CWII_IPC_HLE_Device_usb_ven::OnDeviceChangeEnd()
{
  std::lock_guard<std::mutex> lk{m_devicechange_hook_address_mutex};
  TriggerDeviceChangeReply();
  ++m_device_number;
}

void CWII_IPC_HLE_Device_usb_ven::TriggerDeviceChangeReply()
{
  if (m_devicechange_hook_address == 0)
    return;
  if (Core::g_want_determinism)
  {
    if (m_devicechange_first_call)
    {
      Memory::Write_U32(0, m_devicechange_hook_address + 4);
      WII_IPC_HLE_Interface::EnqueueReply(m_devicechange_hook_address);
    }
    m_devicechange_hook_address = 0;
    return;
  }

  const IOCtlBuffer cmd_buffer(m_devicechange_hook_address);
  std::vector<u8> buffer(cmd_buffer.m_out_buffer_size);
  const u8 number_of_devices = GetIOSDeviceList(&buffer);
  Memory::CopyToEmu(cmd_buffer.m_out_buffer_addr, buffer.data(), buffer.size());

  Memory::Write_U32(number_of_devices, m_devicechange_hook_address + 4);
  WII_IPC_HLE_Interface::EnqueueReply(m_devicechange_hook_address, 0, CoreTiming::FromThread::ANY);
  m_devicechange_hook_address = 0;
  INFO_LOG(WII_IPC_USB, "%d device%s", number_of_devices, number_of_devices == 1 ? "" : "s");
}

u8 CWII_IPC_HLE_Device_usb_ven::GetIOSDeviceList(std::vector<u8>* buffer) const
{
  // Return an empty device list.
  if (Core::g_want_determinism)
    return 0;

  std::unique_lock<std::mutex> id_map_lock{m_id_map_mutex, std::defer_lock};
  std::unique_lock<std::mutex> devices_lock{m_devices_mutex, std::defer_lock};
  std::lock(id_map_lock, devices_lock);

  auto it = buffer->begin();
  u8 num_devices = 0;
  const size_t max_num = buffer->size() / sizeof(IOSDeviceEntry);
  for (const auto& device : m_devices)
  {
    if (num_devices >= max_num)
    {
      WARN_LOG(WII_IPC_USB, "Too many devices (%d ≥ %zu), skipping", num_devices, max_num);
      break;
    }

    const s32 ios_id = m_device_ids_to_ios_ids_map.at(device.second->GetId());
    IOSDeviceEntry entry;
    entry.device_id = Common::swap32(ios_id);
    entry.vid = Common::swap16(device.second->GetVid());
    entry.pid = Common::swap16(device.second->GetPid());
    entry.unknown = 0x00;
    entry.device_number = ios_id & 0xff;
    entry.interface_number = device.second->GetInterface();
    entry.num_altsettings = device.second->GetNumberOfAltSettings();

    std::memcpy(&*it, &entry, sizeof(entry));
    it += sizeof(entry);
    ++num_devices;
  }
  return num_devices;
}
