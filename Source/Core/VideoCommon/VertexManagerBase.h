#pragma once

#include <vector>
#include "Common/CommonFuncs.h"
#include "Common/CommonTypes.h"
#include "VideoCommon/DataReader.h"
#include "VideoCommon/NativeVertexFormat.h"

class NativeVertexFormat;
class PointerWrap;

enum PrimitiveType {
	PRIMITIVE_POINTS,
	PRIMITIVE_LINES,
	PRIMITIVE_TRIANGLES,
};

struct Slope
{
	float dfdx;
	float dfdy;
	float f0;
	bool dirty;
};

class VertexManager
{
private:
	static const u32 SMALLEST_POSSIBLE_VERTEX = sizeof(float)*3;                 // 3 pos
	static const u32 LARGEST_POSSIBLE_VERTEX = sizeof(float)*45 + sizeof(u32)*2; // 3 pos, 3*3 normal, 2*u32 color, 8*4 tex, 1 posMat

	static const u32 MAX_PRIMITIVES_PER_COMMAND = (u16)-1;

public:
	static const u32 MAXVBUFFERSIZE = ROUND_UP_POW2(MAX_PRIMITIVES_PER_COMMAND * LARGEST_POSSIBLE_VERTEX);

	// We may convert triangle-fans to triangle-lists, almost 3x as many indices.
	static const u32 MAXIBUFFERSIZE = ROUND_UP_POW2(MAX_PRIMITIVES_PER_COMMAND * 3);

	VertexManager();
	// needs to be virtual for DX11's dtor
	virtual ~VertexManager();

	static DataReader PrepareForAdditionalData(int primitive, u32 count, u32 stride, bool cullall);
	static void FlushData(u32 count, u32 stride);

	static void Flush();

	virtual ::NativeVertexFormat* CreateNativeVertexFormat() = 0;

	static void DoState(PointerWrap& p);

protected:
	virtual void vDoState(PointerWrap& p) {  }

	static PrimitiveType current_primitive_type;

	virtual void ResetBuffer(u32 stride) = 0;

	static u8* s_pCurBufferPointer;
	static u8* s_pBaseBufferPointer;
	static u8* s_pEndBufferPointer;

	static u8* s_pCurReplayBufferPointer;
	static u8* s_pBaseReplayBufferPointer;

	static u32 GetRemainingSize();
	static u32 GetRemainingIndices(int primitive);

	static Slope s_zslope;
	static void CalculateZSlope(NativeVertexFormat* format);

	static bool s_cull_all;

private:
	static bool s_is_flushed;

	virtual void vFlush(bool useDstAlpha) = 0;

	virtual void CreateDeviceObjects() {}
	virtual void DestroyDeviceObjects() {}
};

extern VertexManager *g_vertex_manager;
