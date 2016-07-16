// Copyright 2016 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <memory>

#include "VideoCommon/FramebufferManagerBase.h"

#include "VideoBackends/Vulkan/Constants.h"
#include "VideoBackends/Vulkan/Texture2D.h"
#include "VideoBackends/Vulkan/VulkanImports.h"

namespace Vulkan
{
class StateTracker;

class XFBSource : public XFBSourceBase
{
  void DecodeToTexture(u32 xfb_addr, u32 fb_width, u32 fb_height) override {}
  void CopyEFB(float gamma) override {}
};

class FramebufferManager : public FramebufferManagerBase
{
public:
  FramebufferManager();
  ~FramebufferManager();

  VkRenderPass GetEFBRenderPass() const { return m_efb_render_pass; }
  u32 GetEFBWidth() const { return m_efb_width; }
  u32 GetEFBHeight() const { return m_efb_height; }
  u32 GetEFBLayers() const { return m_efb_layers; }
  VkSampleCountFlagBits GetEFBSamples() const { return m_efb_samples; }
  Texture2D* GetEFBColorTexture() const { return m_efb_color_texture.get(); }
  Texture2D* GetEFBDepthTexture() const { return m_efb_depth_texture.get(); }
  VkFramebuffer GetEFBFramebuffer() const { return m_efb_framebuffer; }
  void GetTargetSize(unsigned int* width, unsigned int* height) override;

  std::unique_ptr<XFBSourceBase> CreateXFBSource(unsigned int target_width,
                                                 unsigned int target_height,
                                                 unsigned int layers) override
  {
    return std::make_unique<XFBSource>();
  }

  void CopyToRealXFB(u32 xfb_addr, u32 fb_stride, u32 fb_height, const EFBRectangle& source_rc,
                     float gamma = 1.0f) override
  {
  }

  void ResizeEFBTextures();

  // Recompile shaders, use when MSAA mode changes.
  void RecreateRenderPass();
  void RecompileShaders();

  // Reinterpret pixel format of EFB color texture.
  // Assumes no render pass is currently in progress.
  // Swaps EFB framebuffers, so re-bind afterwards.
  void ReinterpretPixelData(int convtype);

  // Resolve color/depth textures to a non-msaa texture, and return it.
  Texture2D* ResolveEFBColorTexture(StateTracker* state_tracker, const VkRect2D& region);
  Texture2D* ResolveEFBDepthTexture(StateTracker* state_tracker, const VkRect2D& region);

private:
  bool CreateEFBRenderPass();
  void DestroyEFBRenderPass();
  bool CreateEFBFramebuffer();
  void DestroyEFBFramebuffer();
  bool CompileShaders();
  void DestroyShaders();

  VkRenderPass m_efb_render_pass = VK_NULL_HANDLE;
  VkRenderPass m_depth_resolve_render_pass = VK_NULL_HANDLE;

  u32 m_efb_width = 0;
  u32 m_efb_height = 0;
  u32 m_efb_layers = 1;
  VkSampleCountFlagBits m_efb_samples = VK_SAMPLE_COUNT_1_BIT;

  std::unique_ptr<Texture2D> m_efb_color_texture;
  std::unique_ptr<Texture2D> m_efb_convert_color_texture;
  std::unique_ptr<Texture2D> m_efb_depth_texture;
  std::unique_ptr<Texture2D> m_efb_resolve_color_texture;
  std::unique_ptr<Texture2D> m_efb_resolve_depth_texture;
  VkFramebuffer m_efb_framebuffer = VK_NULL_HANDLE;
  VkFramebuffer m_efb_convert_framebuffer = VK_NULL_HANDLE;
  VkFramebuffer m_depth_resolve_framebuffer = VK_NULL_HANDLE;

  // Format conversion shaders
  VkShaderModule m_ps_rgb8_to_rgba6 = VK_NULL_HANDLE;
  VkShaderModule m_ps_rgba6_to_rgb8 = VK_NULL_HANDLE;
  VkShaderModule m_ps_depth_resolve = VK_NULL_HANDLE;
};

}  // namespace Vulkan
