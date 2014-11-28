// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.

#pragma once

#include "Core/IPC_HLE/WII_IPC_HLE_Device.h"

namespace DiscIO
{
	class IVolume;
	class IFileSystem;
}

class CWII_IPC_HLE_Device_di : public IWII_IPC_HLE_Device
{
public:

	CWII_IPC_HLE_Device_di(u32 _DeviceID, const std::string& _rDeviceName);

	virtual ~CWII_IPC_HLE_Device_di();

	u64 Open(u32 _CommandAddress, u32 _Mode) override;
	u64 Close(u32 _CommandAddress, bool _bForce) override;

	u64 IOCtl(u32 _CommandAddress) override;
	u64 IOCtlV(u32 _CommandAddress) override;

private:

	u64 CWII_IPC_HLE_Device_di::GetCmdDelay(u32 _CommandAddress);

	u32 ExecuteCommand(u32 BufferIn, u32 BufferInSize, u32 _BufferOut, u32 BufferOutSize);

	DiscIO::IFileSystem* m_pFileSystem;
	u32 m_ErrorStatus;
	// This flag seems to only be reset with poweron/off, not sure
	u32 m_CoverStatus;
};
