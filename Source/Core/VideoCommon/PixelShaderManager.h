// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <array>

#include "VideoCommon/BPMemory.h"
#include "VideoCommon/ConstantManager.h"
#include "VideoCommon/PixelShaderGen.h"
#include "VideoCommon/XFMemory.h"

class PointerWrap;



// The non-API dependent parts.
class PixelShaderManager
{
public:
	static void Init();
	static void Dirty();
	static void Shutdown();
	static void DoState(PointerWrap &p);

	static void SetConstants(); // sets pixel shader constants

	// constant management
	// Some of these functions grab the constant values from global state,
	// so make sure to call them after memory is committed
	static void SetTevColor(int index, int component, s32 value);
	static void SetTevKonstColor(int index, int component, s32 value);
	static void SetAlpha();
	static void SetDestAlpha();
	static void SetTexDims(int texmapid, u32 width, u32 height);
	static void SetZTextureBias();
	static void SetViewportChanged();
	static void SetEfbScaleChanged();
	static void SetZSlope(float dfdx, float dfdy, float f0);
	static void SetIndMatrixChanged(int matrixidx);
	static void SetZTextureTypeChanged();
	static void SetIndTexScaleChanged(bool high);
	static void SetTexCoordChanged(u8 texmapid);
	static void SetFogColorChanged();
	static void SetFogParamChanged();
	static void SetFogRangeAdjustChanged();
	static void UpdateBP(u32 addr, u32 newValue);

	static PixelShaderConstants constants;
	static UberShaderConstants more_constants;
	static bool dirty;

	static bool s_bFogRangeAdjustChanged;
	static bool s_bViewPortChanged;
};
