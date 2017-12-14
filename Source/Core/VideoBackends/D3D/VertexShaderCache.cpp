// Copyright 2010 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <string>

#include "Common/Align.h"
#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/LinearDiskCache.h"
#include "Common/StringUtil.h"

#include "Core/ConfigManager.h"

#include "VideoBackends/D3D/D3DShader.h"
#include "VideoBackends/D3D/VertexShaderCache.h"

#include "VideoCommon/Debugger.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/VR.h"
#include "VideoCommon/VertexShaderGen.h"
#include "VideoCommon/VertexShaderManager.h"

namespace DX11
{
VertexShaderCache::VSCache VertexShaderCache::vshaders;
const VertexShaderCache::VSCacheEntry* VertexShaderCache::last_entry;
VertexShaderUid VertexShaderCache::last_uid;

static ID3D11VertexShader* SimpleVertexShader = nullptr;
static ID3D11VertexShader* ClearVertexShader = nullptr;
static ID3D11InputLayout* SimpleLayout = nullptr;
static ID3D11InputLayout* ClearLayout = nullptr;

LinearDiskCache<VertexShaderUid, u8> g_vs_disk_cache;

ID3D11VertexShader* VertexShaderCache::GetSimpleVertexShader()
{
  return SimpleVertexShader;
}
ID3D11VertexShader* VertexShaderCache::GetClearVertexShader()
{
  return ClearVertexShader;
}
ID3D11InputLayout* VertexShaderCache::GetSimpleInputLayout()
{
  return SimpleLayout;
}
ID3D11InputLayout* VertexShaderCache::GetClearInputLayout()
{
  return ClearLayout;
}

ID3D11Buffer* vscbuf = nullptr;

ID3D11Buffer*& VertexShaderCache::GetConstantBuffer()
{
  // TODO: divide the global variables of the generated shaders into about 5 constant buffers to
  // speed this up
  if (VertexShaderManager::dirty)
  {
    D3D11_MAPPED_SUBRESOURCE map;
    D3D::context->Map(vscbuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &map);

    if (g_has_hmd)
    {
      static int i = 0;

      if (g_first_pass_vs_constants)
      {
        i = 0;
        if (!g_ActiveConfig.bReplayVertexData || !g_opcode_replay_frame)
        {
          VertexShaderManager::constants_replay.clear();
          VertexShaderManager::constants_replay.resize(0);
        }
      }

      if (!g_ActiveConfig.bReplayVertexData)
      {
        memcpy(map.pData, &VertexShaderManager::constants, sizeof(VertexShaderConstants));
      }
      else if (!g_opcode_replay_frame)
      {
        memcpy(map.pData, &VertexShaderManager::constants, sizeof(VertexShaderConstants));
        if (g_opcode_replay_log_frame)
          VertexShaderManager::constants_replay.push_back(VertexShaderManager::constants);
      }
      else
      {
        memcpy(&VertexShaderManager::constants_replay[i].projection,
               &VertexShaderManager::constants.projection,
               sizeof(VertexShaderManager::constants.projection));
        memcpy(map.pData, &VertexShaderManager::constants_replay[i++],
               sizeof(VertexShaderConstants));
      }
    }
    else
    {
      memcpy(map.pData, &VertexShaderManager::constants, sizeof(VertexShaderConstants));
    }

    D3D::context->Unmap(vscbuf, 0);
    VertexShaderManager::dirty = false;

    ADDSTAT(stats.thisFrame.bytesUniformStreamed, sizeof(VertexShaderConstants));
  }
  return vscbuf;
}

// this class will load the precompiled shaders into our cache
class VertexShaderCacheInserter : public LinearDiskCacheReader<VertexShaderUid, u8>
{
public:
  void Read(const VertexShaderUid& key, const u8* value, u32 value_size)
  {
    D3DBlob* blob = new D3DBlob(value_size, value);
    VertexShaderCache::InsertByteCode(key, blob);
    blob->Release();
  }
};

const char simple_shader_code[] = {
    "struct VSOUTPUT\n"
    "{\n"
    "float4 vPosition : POSITION;\n"
    "float3 vTexCoord : TEXCOORD0;\n"
    "float  vTexCoord1 : TEXCOORD1;\n"
    "};\n"
    "VSOUTPUT main(float4 inPosition : POSITION,float4 inTEX0 : TEXCOORD0)\n"
    "{\n"
    "VSOUTPUT OUT;\n"
    "OUT.vPosition = inPosition;\n"
    "OUT.vTexCoord = inTEX0.xyz;\n"
    "OUT.vTexCoord1 = inTEX0.w;\n"
    "return OUT;\n"
    "}\n"};

const char clear_shader_code[] = {
    "struct VSOUTPUT\n"
    "{\n"
    "float4 vPosition   : POSITION;\n"
    "float4 vColor0   : COLOR0;\n"
    "};\n"
    "VSOUTPUT main(float4 inPosition : POSITION,float4 inColor0: COLOR0)\n"
    "{\n"
    "VSOUTPUT OUT;\n"
    "OUT.vPosition = inPosition;\n"
    "OUT.vColor0 = inColor0;\n"
    "return OUT;\n"
    "}\n"};

void VertexShaderCache::Init()
{
  const D3D11_INPUT_ELEMENT_DESC simpleelems[2] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},

  };
  const D3D11_INPUT_ELEMENT_DESC clearelems[2] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
  };

  unsigned int cbsize = Common::AlignUp(static_cast<unsigned int>(sizeof(VertexShaderConstants)),
                                        16);  // must be a multiple of 16
  D3D11_BUFFER_DESC cbdesc = CD3D11_BUFFER_DESC(cbsize, D3D11_BIND_CONSTANT_BUFFER,
                                                D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE);
  HRESULT hr = D3D::device->CreateBuffer(&cbdesc, nullptr, &vscbuf);
  CHECK(hr == S_OK, "Create vertex shader constant buffer (size=%u)", cbsize);
  D3D::SetDebugObjectName((ID3D11DeviceChild*)vscbuf,
                          "vertex shader constant buffer used to emulate the GX pipeline");

  D3DBlob* blob;
  D3D::CompileVertexShader(simple_shader_code, &blob);
  D3D::device->CreateInputLayout(simpleelems, 2, blob->Data(), blob->Size(), &SimpleLayout);
  SimpleVertexShader = D3D::CreateVertexShaderFromByteCode(blob);
  if (SimpleLayout == nullptr || SimpleVertexShader == nullptr)
    PanicAlert("Failed to create simple vertex shader or input layout at %s %d\n", __FILE__,
               __LINE__);
  blob->Release();
  D3D::SetDebugObjectName((ID3D11DeviceChild*)SimpleVertexShader, "simple vertex shader");
  D3D::SetDebugObjectName((ID3D11DeviceChild*)SimpleLayout, "simple input layout");

  D3D::CompileVertexShader(clear_shader_code, &blob);
  D3D::device->CreateInputLayout(clearelems, 2, blob->Data(), blob->Size(), &ClearLayout);
  ClearVertexShader = D3D::CreateVertexShaderFromByteCode(blob);
  if (ClearLayout == nullptr || ClearVertexShader == nullptr)
    PanicAlert("Failed to create clear vertex shader or input layout at %s %d\n", __FILE__,
               __LINE__);
  blob->Release();
  D3D::SetDebugObjectName((ID3D11DeviceChild*)ClearVertexShader, "clear vertex shader");
  D3D::SetDebugObjectName((ID3D11DeviceChild*)ClearLayout, "clear input layout");

  Clear();

  SETSTAT(stats.numVertexShadersCreated, 0);
  SETSTAT(stats.numVertexShadersAlive, 0);

  if (g_ActiveConfig.bShaderCache)
  {
    if (!File::Exists(File::GetUserPath(D_SHADERCACHE_IDX)))
      File::CreateDir(File::GetUserPath(D_SHADERCACHE_IDX));

    std::string cache_filename =
        StringFromFormat("%sdx11-%s-vs.cache", File::GetUserPath(D_SHADERCACHE_IDX).c_str(),
                         SConfig::GetInstance().m_strGameID.c_str());
    VertexShaderCacheInserter inserter;
    g_vs_disk_cache.OpenAndRead(cache_filename, inserter);
  }

  last_entry = nullptr;
}

void VertexShaderCache::Clear()
{
  for (auto& iter : vshaders)
    iter.second.Destroy();
  vshaders.clear();

  last_entry = nullptr;
}

void VertexShaderCache::Shutdown()
{
  SAFE_RELEASE(vscbuf);

  SAFE_RELEASE(SimpleVertexShader);
  SAFE_RELEASE(ClearVertexShader);

  SAFE_RELEASE(SimpleLayout);
  SAFE_RELEASE(ClearLayout);

  Clear();
  VertexShaderManager::constants_replay.clear();
  VertexShaderManager::constants_replay.resize(0);
  g_vs_disk_cache.Sync();
  g_vs_disk_cache.Close();
}

bool VertexShaderCache::SetShader()
{
  VertexShaderUid uid = GetVertexShaderUid();

  if (last_entry)
  {
    if (uid == last_uid)
    {
      GFX_DEBUGGER_PAUSE_AT(NEXT_VERTEX_SHADER_CHANGE, true);
      return (last_entry->shader != nullptr);
    }
  }

  last_uid = uid;

  VSCache::iterator iter = vshaders.find(uid);
  if (iter != vshaders.end())
  {
    const VSCacheEntry& entry = iter->second;
    last_entry = &entry;

    GFX_DEBUGGER_PAUSE_AT(NEXT_VERTEX_SHADER_CHANGE, true);
    return (entry.shader != nullptr);
  }

  ShaderCode code = GenerateVertexShaderCode(APIType::D3D, uid.GetUidData());

  D3DBlob* pbytecode = nullptr;
  D3D::CompileVertexShader(code.GetBuffer(), &pbytecode);

  if (pbytecode == nullptr)
  {
    GFX_DEBUGGER_PAUSE_AT(NEXT_ERROR, true);
    return false;
  }
  g_vs_disk_cache.Append(uid, pbytecode->Data(), pbytecode->Size());

  bool success = InsertByteCode(uid, pbytecode);
  pbytecode->Release();

  GFX_DEBUGGER_PAUSE_AT(NEXT_VERTEX_SHADER_CHANGE, true);
  return success;
}

bool VertexShaderCache::InsertByteCode(const VertexShaderUid& uid, D3DBlob* bcodeblob)
{
  ID3D11VertexShader* shader = D3D::CreateVertexShaderFromByteCode(bcodeblob);
  if (shader == nullptr)
    return false;

  // TODO: Somehow make the debug name a bit more specific
  D3D::SetDebugObjectName((ID3D11DeviceChild*)shader, "a vertex shader of VertexShaderCache");

  // Make an entry in the table
  VSCacheEntry entry;
  entry.shader = shader;
  entry.SetByteCode(bcodeblob);

  vshaders[uid] = entry;
  last_entry = &vshaders[uid];

  INCSTAT(stats.numVertexShadersCreated);
  SETSTAT(stats.numVertexShadersAlive, (int)vshaders.size());

  return true;
}

}  // namespace DX11
