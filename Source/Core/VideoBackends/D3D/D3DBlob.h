// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include "Common/CommonTypes.h"
#include "VideoBackends/D3D/D3DBase.h"

struct ID3D10Blob;

namespace DX11
{
// use this class instead ID3D10Blob or ID3D11Blob whenever possible.
// D3DBlob is not a COM object, but it is compatible with ComPtr.
class D3DBlob
{
public:
  // memory will be copied into an own buffer
  D3DBlob(unsigned int blob_size, const u8* init_data = nullptr);

  // Ownership of an existing blob is taken
  D3DBlob(ComPtr<ID3D10Blob>&& d3dblob);

  void AddRef();
  unsigned int Release();

  unsigned int Size() const;
  u8* Data();

private:
  ~D3DBlob();

  unsigned int ref;
  unsigned int size;

  u8* data;
  ComPtr<ID3D10Blob> blob;
};

}  // namespace
