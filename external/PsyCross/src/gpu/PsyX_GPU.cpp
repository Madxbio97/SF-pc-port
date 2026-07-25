#include "PsyX_GPU.h"

#include "PsyX/PsyX_globals.h"
#include "PsyX/PsyX_public.h"
#include "PsyX/PsyX_render.h"

#include "../PsyX_main.h"

#include <assert.h>
#include <math.h>
#include <string.h>

#define GET_TPAGE_FORMAT(tpage) ((TexFormat)((tpage >> 7) & 0x3))
#define GET_TPAGE_BLEND(tpage)	((BlendMode)(((tpage >> 5) & 3) + 1))

#define GET_TPAGE_DITHER(tpage) ((tpage >> 9) & 0x1)

#define GET_CLUT_X(clut) ((clut & 0x3F) << 4)
#define GET_CLUT_Y(clut) (clut >> 6)

#if USE_EXTENDED_PRIM_POINTERS
OT_TAG prim_terminator = {UINTPTR_MAX, 0}; // P_TAG with zero primLength
#else
OT_TAG prim_terminator = {0x00FFFFFFu, 0}; // P_TAG with zero primLength
#endif

DISPENV currentDispEnv;
DISPENV activeDispEnv;
DRAWENV activeDrawEnv;

static const char* currentSplitDebugText = nullptr;
TextureID		   overrideTexture		 = 0;
int				   overrideTextureWidth	 = 0;
int				   overrideTextureHeight = 0;
extern int		   g_RequestedDepthMode;

int g_GPUDisabledState = 0;
int g_DrawPrimMode	   = 0;

struct GPUDrawSplit
{
	DRAWENV drawenv;
	DISPENV dispenv;

	BlendMode blendMode;

	TexFormat		  texFormat;
	TextureID		  textureId;
	TextureFilterMode textureFilterMode;

	int drawPrimMode;

	u_short startVertex;
	u_short numVerts;

	const char* debugText;
};

#define MAX_DRAW_SPLITS 4096
// The largest supported PS1 packet expands to three independent line quads.
// Flush before parsing it so packet expansion can never write past the fixed
// CPU stream buffer. The flush preserves OT order and starts a fresh batch.
#define MAX_VERTICES_PER_PRIMITIVE 18

GrVertex	 g_vertexBuffer[MAX_VERTEX_BUFFER_SIZE];
GPUDrawSplit g_splits[MAX_DRAW_SPLITS];

int g_vertexIndex = 0;
int g_splitIndex  = 0;

// DuckStation's PGXP depth path clears depth when painter order jumps back to
// substantially farther geometry. PS1 ordering tables allow that jump; a
// persistent PC depth buffer otherwise cuts camera-dependent triangles out of
// later transparent surfaces. PGXP Z is in native GTE camera-depth units.
static const float PGXP_DEPTH_CLEAR_THRESHOLD = 4096.0f;
static const float PGXP_MAX_DEPTH = 65536.0f;
static float g_lastPolygonDepth = PGXP_MAX_DEPTH;

void ClearSplits()
{
	currentSplitDebugText = nullptr;
	g_vertexIndex		  = 0;
	g_splitIndex		  = 0;
	g_splits[0].texFormat = (TexFormat)0xFFFF;
}

template <class T>
void DrawEnvDimensions(T& width, T& height)
{
	if(activeDrawEnv.dfe)
	{
		width  = activeDispEnv.disp.w;
		height = activeDispEnv.disp.h;
	}
	else
	{
		width  = activeDrawEnv.clip.w;
		height = activeDrawEnv.clip.h;
	}
}

void DrawEnvOffset(float& ofsX, float& ofsY)
{
	if(activeDrawEnv.dfe)
	{
		// also make offset in draw dimensions range to prevent flicker
		const int x = activeDispEnv.disp.x;
		const int y = activeDispEnv.disp.y;
		ofsX		= activeDrawEnv.ofs[0] - activeDispEnv.disp.x;
		ofsY		= activeDrawEnv.ofs[1] - activeDispEnv.disp.y;
	}
	else
	{
		ofsX = 0.0f;
		ofsY = 0.0f;
	}
}

// remaps screen coordinates to [0..1]
// without clamping
inline void ScreenCoordsToEmulator(GrVertex* vertex, int count)
{
#if USE_PGXP
	float w, h;
	DrawEnvDimensions(w, h);

	while(count--)
	{
		// Precise PGXP vertices are normalized when the exact GTE projection is
		// applied. Legacy screen-space vertices still need conversion here.
		if(vertex[count].scr_h == 0.0f)
		{
			vertex[count].x = vertex[count].x / w - 0.5f;
			vertex[count].y = vertex[count].y / h - 0.5f;
		}
	}
#endif
}

void LineSwapSourceVerts(VERTTYPE*& p0, VERTTYPE*& p1, unsigned char*& c0,
						 unsigned char*& c1)
{
	// swap line coordinates for left-to-right and up-to-bottom direction
	if((p0[0] > p1[0]) || (p0[1] > p1[1] && p0[0] == p1[0]))
	{
		VERTTYPE* tmp = p0;
		p0			  = p1;
		p1			  = tmp;

		unsigned char* tmpCol = c0;
		c0					  = c1;
		c1					  = tmpCol;
	}
}

void MakeLineArray(GrVertex* vertex, VERTTYPE* p0, VERTTYPE* p1,
				   ushort gteidx)
{
	const VERTTYPE dx = p1[0] - p0[0];
	const VERTTYPE dy = p1[1] - p0[1];

	float ofsX, ofsY;
	DrawEnvOffset(ofsX, ofsY);

	memset(vertex, 0, sizeof(GrVertex) * 4);

	if(dx > abs((short)dy))
	{ // horizontal
		vertex[0].x = p0[0] + ofsX;
		vertex[0].y = p0[1] + ofsY;

		vertex[1].x = p1[0] + ofsX + 1;
		vertex[1].y = p1[1] + ofsY;

		vertex[2].x = vertex[1].x;
		vertex[2].y = vertex[1].y + 1;

		vertex[3].x = vertex[0].x;
		vertex[3].y = vertex[0].y + 1;
	}
	else
	{ // vertical
		vertex[0].x = p0[0] + ofsX;
		vertex[0].y = p0[1] + ofsY;

		vertex[1].x = p1[0] + ofsX;
		vertex[1].y = p1[1] + ofsY + 1;

		vertex[2].x = vertex[1].x + 1;
		vertex[2].y = vertex[1].y;

		vertex[3].x = vertex[0].x + 1;
		vertex[3].y = vertex[0].y;
	} // TODO diagonal line alignment

#if USE_PGXP
	vertex[0].scr_h = vertex[1].scr_h = vertex[2].scr_h = vertex[3].scr_h = 0.0f;
#endif

	ScreenCoordsToEmulator(vertex, 4);
}

inline bool LookupVertexPGXP(PGXPVData* result, const VERTTYPE* p,
							 ushort gteidx, int lookupOfs)
{
#if USE_PGXP
	const uint lookup	  = PGXP_LOOKUP_VALUE(p[0], p[1]);
	const int  cacheIndex = static_cast<int>(gteidx) + lookupOfs;
	return result != nullptr && gteidx != 0xffff && cacheIndex >= 0 &&
		   cacheIndex < 0xffff && g_cfg_pgxpTextureCorrection &&
		   PGXP_GetCacheDataExact(result, static_cast<ushort>(cacheIndex)) &&
		   (result->exact_projection || result->lookup == lookup) &&
		   result->pz > 0.0f;
#else
	(void)result;
	(void)p;
	(void)gteidx;
	(void)lookupOfs;
	return false;
#endif
}

inline void ApplyVertexPGXP(GrVertex* v, const PGXPVData& vd, float ofsX,
							float ofsY)
{
#if USE_PGXP
	float dispW, dispH;
	DrawEnvDimensions(dispW, dispH);

	// Preserve the exact unsaturated projection which generated the packet.
	// Only the framebuffer-page part of OFX/OFY is wrapped to the display.
	const float centerX = fmodf(vd.ofx, dispW);
	const float centerY = fmodf(vd.ofy, dispH);
	const float screenX = vd.sx - vd.ofx + centerX + ofsX;
	const float screenY = vd.sy - vd.ofy + centerY + ofsY;
	v->x				= screenX / dispW - 0.5f;
	v->y				= screenY / dispH - 0.5f;
	v->z				= vd.pz;
	v->scr_h			= vd.scr_h;
	v->ofsX				= 0.0f;
	v->ofsY				= 0.0f;
	if(vd.precise_texcoord)
	{
		v->precise_u = vd.precise_u;
		v->precise_v = vd.precise_v;
		v->umin		 = static_cast<unsigned char>(vd.texture_bounds);
		v->vmin		 = static_cast<unsigned char>(vd.texture_bounds >> 8);
		v->umax		 = static_cast<unsigned char>(vd.texture_bounds >> 16);
		v->vmax		 = static_cast<unsigned char>(vd.texture_bounds >> 24);
		v->_p1		 = 1;
	}
#else
	(void)v;
	(void)vd;
	(void)ofsX;
	(void)ofsY;
#endif
}

void MakeVertexTriangle(GrVertex* vertex, VERTTYPE* p0, VERTTYPE* p1,
						VERTTYPE* p2, ushort gteidx)
{
	assert(p0);
	assert(p1);
	assert(p2);

	float ofsX, ofsY;
	DrawEnvOffset(ofsX, ofsY);

	memset(vertex, 0, sizeof(GrVertex) * 3);

	vertex[0].x = p0[0] + ofsX;
	vertex[0].y = p0[1] + ofsY;

	vertex[1].x = p1[0] + ofsX;
	vertex[1].y = p1[1] + ofsY;

	vertex[2].x = p2[0] + ofsX;
	vertex[2].y = p2[1] + ofsY;

	// Perspective correction is primitive-atomic. Mixing one legacy W=1
	// vertex with precise depth vertices produces catastrophic texture warping.
	PGXPVData precise[3];
	if(LookupVertexPGXP(&precise[0], p0, gteidx, -3) &&
	   LookupVertexPGXP(&precise[1], p1, gteidx, -2) &&
	   LookupVertexPGXP(&precise[2], p2, gteidx, -1))
	{
		ApplyVertexPGXP(&vertex[0], precise[0], ofsX, ofsY);
		ApplyVertexPGXP(&vertex[1], precise[1], ofsX, ofsY);
		ApplyVertexPGXP(&vertex[2], precise[2], ofsX, ofsY);
	}

	ScreenCoordsToEmulator(vertex, 3);
}

void MakeVertexQuad(GrVertex* vertex, VERTTYPE* p0, VERTTYPE* p1, VERTTYPE* p2,
					VERTTYPE* p3, ushort gteidx)
{
	assert(p0);
	assert(p1);
	assert(p2);
	assert(p3);

	float ofsX, ofsY;
	DrawEnvOffset(ofsX, ofsY);

	memset(vertex, 0, sizeof(GrVertex) * 4);

	vertex[0].x = p0[0] + ofsX;
	vertex[0].y = p0[1] + ofsY;

	vertex[1].x = p1[0] + ofsX;
	vertex[1].y = p1[1] + ofsY;

	vertex[2].x = p2[0] + ofsX;
	vertex[2].y = p2[1] + ofsY;

	vertex[3].x = p3[0] + ofsX;
	vertex[3].y = p3[1] + ofsY;

	// Primitive parsing swaps packet x2/x3 into TL,TR,BR,BL order. Match the
	// exact GTE cache entries to those packet vertices, not to the local slots.
	PGXPVData precise[4];
	if(LookupVertexPGXP(&precise[0], p0, gteidx, -4) &&
	   LookupVertexPGXP(&precise[1], p1, gteidx, -3) &&
	   LookupVertexPGXP(&precise[2], p2, gteidx, -1) &&
	   LookupVertexPGXP(&precise[3], p3, gteidx, -2))
	{
		ApplyVertexPGXP(&vertex[0], precise[0], ofsX, ofsY);
		ApplyVertexPGXP(&vertex[1], precise[1], ofsX, ofsY);
		ApplyVertexPGXP(&vertex[2], precise[2], ofsX, ofsY);
		ApplyVertexPGXP(&vertex[3], precise[3], ofsX, ofsY);
	}

	ScreenCoordsToEmulator(vertex, 4);
}

void MakeVertexRect(GrVertex* vertex, VERTTYPE* p0, short w, short h,
					ushort gteidx)
{
	assert(p0);

	float ofsX, ofsY;
	DrawEnvOffset(ofsX, ofsY);

	memset(vertex, 0, sizeof(GrVertex) * 4);

	vertex[0].x = p0[0] + ofsX;
	vertex[0].y = p0[1] + ofsY;

	vertex[1].x = vertex[0].x;
	vertex[1].y = vertex[0].y + h;

	vertex[2].x = vertex[0].x + w;
	vertex[2].y = vertex[0].y + h;

	vertex[3].x = vertex[0].x + w;
	vertex[3].y = vertex[0].y;

#if USE_PGXP
	vertex[0].scr_h = vertex[1].scr_h = vertex[2].scr_h = vertex[3].scr_h = 0.0f;
#endif

	ScreenCoordsToEmulator(vertex, 4);
}

static void SetTexcoordFilteringState(GrVertex* vertex, int count,
									  int exclusiveFarEdge)
{
	// World UVs address inclusive texel centres and need no shift. Native
	// sprites/HUD use u+width/v+height as an exclusive far edge, so only that
	// convention receives the half-texel correction. Bounds keep every tap in
	// the primitive's authored atlas tile.
	const char	  offset			  = g_cfg_bilinearFiltering && exclusiveFarEdge ? -1 : 0;
	unsigned char minU				  = 255;
	unsigned char minV				  = 255;
	unsigned char maxU				  = 0;
	unsigned char maxV				  = 0;
	bool		  hasPreciseTexcoords = false;
	for(int i = 0; i < count; i++)
	{
		if(vertex[i]._p1)
			hasPreciseTexcoords = true;
		else
		{
			vertex[i].precise_u = static_cast<float>(vertex[i].u);
			vertex[i].precise_v = static_cast<float>(vertex[i].v);
		}
		if(vertex[i].u < minU)
			minU = vertex[i].u;
		if(vertex[i].v < minV)
			minV = vertex[i].v;
		if(vertex[i].u > maxU)
			maxU = vertex[i].u;
		if(vertex[i].v > maxV)
			maxV = vertex[i].v;
	}
	if(hasPreciseTexcoords)
	{
		minU = 255;
		minV = 255;
		maxU = 0;
		maxV = 0;
		for(int i = 0; i < count; ++i)
		{
			if(vertex[i].umin < minU)
				minU = vertex[i].umin;
			if(vertex[i].vmin < minV)
				minV = vertex[i].vmin;
			if(vertex[i].umax > maxU)
				maxU = vertex[i].umax;
			if(vertex[i].vmax > maxV)
				maxV = vertex[i].vmax;
		}
	}
	if(exclusiveFarEdge)
	{
		if(maxU > minU)
			--maxU;
		if(maxV > minV)
			--maxV;
	}
	for(int i = 0; i < count; i++)
	{
		vertex[i].tcx  = offset;
		vertex[i].tcy  = offset;
		vertex[i].umin = minU;
		vertex[i].vmin = minV;
		vertex[i].umax = maxU;
		vertex[i].vmax = maxV;
	}
}

void MakeTexcoordQuad(GrVertex* vertex, unsigned char* uv0, unsigned char* uv1,
					  unsigned char* uv2, unsigned char* uv3, short page,
					  short clut, unsigned char dither)
{
	assert(uv0);
	assert(uv1);
	assert(uv2);
	assert(uv3);

	const unsigned char bright = 2;

	vertex[0].u		 = uv0[0];
	vertex[0].v		 = uv0[1];
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page	 = page;
	vertex[0].clut	 = clut;

	vertex[1].u		 = uv1[0];
	vertex[1].v		 = uv1[1];
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page	 = page;
	vertex[1].clut	 = clut;

	vertex[2].u		 = uv2[0];
	vertex[2].v		 = uv2[1];
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page	 = page;
	vertex[2].clut	 = clut;

	vertex[3].u		 = uv3[0];
	vertex[3].v		 = uv3[1];
	vertex[3].bright = bright;
	vertex[3].dither = dither;
	vertex[3].page	 = page;
	vertex[3].clut	 = clut;
	// Native screen sprites/HUD in this backend use u+width/v+height as an
	// exclusive far edge. GTE-projected world quads retain authored inclusive
	// corner UVs. Preserve both conventions while deriving atlas bounds.
	int exclusiveFarEdge = 0;
#if USE_PGXP
	exclusiveFarEdge = vertex[0].scr_h == 0.0f && vertex[1].scr_h == 0.0f &&
					   vertex[2].scr_h == 0.0f && vertex[3].scr_h == 0.0f &&
					   vertex[0].y == vertex[1].y && vertex[1].x == vertex[2].x &&
					   vertex[2].y == vertex[3].y && vertex[3].x == vertex[0].x;
#endif
	SetTexcoordFilteringState(vertex, 4, exclusiveFarEdge);
}

void MakeTexcoordTriangle(GrVertex* vertex, unsigned char* uv0,
						  unsigned char* uv1, unsigned char* uv2, short page,
						  short clut, unsigned char dither)
{
	assert(uv0);
	assert(uv1);
	assert(uv2);

	const unsigned char bright = 2;

	vertex[0].u		 = uv0[0];
	vertex[0].v		 = uv0[1];
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page	 = page;
	vertex[0].clut	 = clut;

	vertex[1].u		 = uv1[0];
	vertex[1].v		 = uv1[1];
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page	 = page;
	vertex[1].clut	 = clut;

	vertex[2].u		 = uv2[0];
	vertex[2].v		 = uv2[1];
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page	 = page;
	vertex[2].clut	 = clut;
	SetTexcoordFilteringState(vertex, 3, 0);
}

void MakeTexcoordRect(GrVertex* vertex, unsigned char* uv, short page,
					  short clut, short w, short h)
{
	assert(uv);

	// sim overflow
	if(int(uv[0]) + w > 255)
		w = 255 - uv[0];
	if(int(uv[1]) + h > 255)
		h = 255 - uv[1];

	const unsigned char bright = 2;
	const unsigned char dither = 0;

	vertex[0].u		 = uv[0];
	vertex[0].v		 = uv[1];
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page	 = page;
	vertex[0].clut	 = clut;

	vertex[1].u		 = uv[0];
	vertex[1].v		 = uv[1] + h;
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page	 = page;
	vertex[1].clut	 = clut;

	vertex[2].u		 = uv[0] + w;
	vertex[2].v		 = uv[1] + h;
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page	 = page;
	vertex[2].clut	 = clut;

	vertex[3].u		 = uv[0] + w;
	vertex[3].v		 = uv[1];
	vertex[3].bright = bright;
	vertex[3].dither = dither;
	vertex[3].page	 = page;
	vertex[3].clut	 = clut;

	SetTexcoordFilteringState(vertex, 4, 1);
}

void MakeTexcoordLineZero(GrVertex* vertex, unsigned char dither)
{
	const unsigned char bright = 1;

	vertex[0].u		 = 0;
	vertex[0].v		 = 0;
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page	 = 0;
	vertex[0].clut	 = 0;

	vertex[1].u		 = 0;
	vertex[1].v		 = 0;
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page	 = 0;
	vertex[1].clut	 = 0;

	vertex[2].u		 = 0;
	vertex[2].v		 = 0;
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page	 = 0;
	vertex[2].clut	 = 0;

	vertex[3].u		 = 0;
	vertex[3].v		 = 0;
	vertex[3].bright = bright;
	vertex[3].dither = dither;
	vertex[3].page	 = 0;
	vertex[3].clut	 = 0;
}

void MakeTexcoordTriangleZero(GrVertex* vertex, unsigned char dither)
{
	const unsigned char bright = 1;

	vertex[0].u		 = 0;
	vertex[0].v		 = 0;
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page	 = 0;
	vertex[0].clut	 = 0;

	vertex[1].u		 = 0;
	vertex[1].v		 = 0;
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page	 = 0;
	vertex[1].clut	 = 0;

	vertex[2].u		 = 0;
	vertex[2].v		 = 0;
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page	 = 0;
	vertex[2].clut	 = 0;
}

void MakeTexcoordQuadZero(GrVertex* vertex, unsigned char dither)
{
	const unsigned char bright = 1;

	vertex[0].u		 = 0;
	vertex[0].v		 = 0;
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page	 = 0;
	vertex[0].clut	 = 0;

	vertex[1].u		 = 0;
	vertex[1].v		 = 0;
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page	 = 0;
	vertex[1].clut	 = 0;

	vertex[2].u		 = 0;
	vertex[2].v		 = 0;
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page	 = 0;
	vertex[2].clut	 = 0;

	vertex[3].u		 = 0;
	vertex[3].v		 = 0;
	vertex[3].bright = bright;
	vertex[3].dither = dither;
	vertex[3].page	 = 0;
	vertex[3].clut	 = 0;

#if USE_PGXP
	// Solid full-display tiles are fades/clears, not 4:3 content. Mark them so
	// the widescreen vertex transform covers the entire output while HUD,
	// sprites and movies remain centred at their authored aspect ratio.
	const bool fullDisplayTile =
		vertex[0].scr_h == 0.0f && vertex[1].scr_h == 0.0f &&
		vertex[2].scr_h == 0.0f && vertex[3].scr_h == 0.0f &&
		vertex[0].x <= -0.499f && vertex[0].y <= -0.499f &&
		vertex[2].x >= 0.499f && vertex[2].y >= 0.499f;
	if(fullDisplayTile)
		vertex[0]._p0 = vertex[1]._p0 = vertex[2]._p0 = vertex[3]._p0 = 1;
#endif
}

void MakeColourNoShade(GrVertex* vertex, int n)
{
	--n;
	while(n >= 0)
	{
		vertex[n].r = 128;
		vertex[n].g = 128;
		vertex[n].b = 128;
		vertex[n].a = 255;
		--n;
	}
}

void MakeColourLine(GrVertex* vertex, bool shadeTexOn, unsigned char* col0,
					unsigned char* col1)
{
	if(!shadeTexOn)
	{
		MakeColourNoShade(vertex, 4);
		return;
	}
	assert(col0);
	assert(col1);

	vertex[0].r = col0[0];
	vertex[0].g = col0[1];
	vertex[0].b = col0[2];
	vertex[0].a = 255;

	vertex[1].r = col1[0];
	vertex[1].g = col1[1];
	vertex[1].b = col1[2];
	vertex[1].a = 255;

	vertex[2].r = col1[0];
	vertex[2].g = col1[1];
	vertex[2].b = col1[2];
	vertex[2].a = 255;

	vertex[3].r = col0[0];
	vertex[3].g = col0[1];
	vertex[3].b = col0[2];
	vertex[3].a = 255;
}

void MakeColourTriangle(GrVertex* vertex, bool shadeTexOn, unsigned char* col0,
						unsigned char* col1, unsigned char* col2)
{
	if(!shadeTexOn)
	{
		MakeColourNoShade(vertex, 3);
		return;
	}

	assert(col0);
	assert(col1);
	assert(col2);

	vertex[0].r = col0[0];
	vertex[0].g = col0[1];
	vertex[0].b = col0[2];
	vertex[0].a = 255;

	vertex[1].r = col1[0];
	vertex[1].g = col1[1];
	vertex[1].b = col1[2];
	vertex[1].a = 255;

	vertex[2].r = col2[0];
	vertex[2].g = col2[1];
	vertex[2].b = col2[2];
	vertex[2].a = 255;
}

void MakeColourQuad(GrVertex* vertex, bool shadeTexOn, unsigned char* col0,
					unsigned char* col1, unsigned char* col2,
					unsigned char* col3)
{
	if(!shadeTexOn)
	{
		MakeColourNoShade(vertex, 4);
		return;
	}

	assert(col0);
	assert(col1);
	assert(col2);
	assert(col3);

	vertex[0].r = col0[0];
	vertex[0].g = col0[1];
	vertex[0].b = col0[2];
	vertex[0].a = 255;

	vertex[1].r = col1[0];
	vertex[1].g = col1[1];
	vertex[1].b = col1[2];
	vertex[1].a = 255;

	vertex[2].r = col2[0];
	vertex[2].g = col2[1];
	vertex[2].b = col2[2];
	vertex[2].a = 255;

	vertex[3].r = col3[0];
	vertex[3].g = col3[1];
	vertex[3].b = col3[2];
	vertex[3].a = 255;
}

void TriangulateQuad()
{
	/*
	Triangulate like this:

	v0--v1
	|  / |
	| /  |
	v2--v3

	NOTE: v2 swapped with v3 during primitive parsing but it not shown here
	*/

	g_vertexBuffer[g_vertexIndex + 4] = g_vertexBuffer[g_vertexIndex + 3];

	g_vertexBuffer[g_vertexIndex + 5] = g_vertexBuffer[g_vertexIndex + 2];
	g_vertexBuffer[g_vertexIndex + 2] = g_vertexBuffer[g_vertexIndex + 3];
	g_vertexBuffer[g_vertexIndex + 3] = g_vertexBuffer[g_vertexIndex + 1];
}

//------------------------------------------------------------------------------------------------------------------------

static void
AddSplit(bool semiTrans, bool textured,
		 TextureFilterMode textureFilterMode = TEXTURE_FILTER_NEAREST)
{
	int			  tpage	   = activeDrawEnv.tpage;
	GPUDrawSplit& curSplit = g_splits[g_splitIndex];

	BlendMode blendMode = semiTrans ? GET_TPAGE_BLEND(tpage) : BM_NONE;
	TexFormat texFormat = GET_TPAGE_FORMAT(tpage);
	TextureID textureId = textured ? g_vramTexture : g_whiteTexture;

	if(textured && overrideTexture != 0)
	{
		// override texture format, zero tpage
		texFormat = TF_32_BIT_RGBA;
		textureId = overrideTexture;
	}

	// FIXME: compare drawing environment too?
	if(curSplit.blendMode == blendMode && curSplit.texFormat == texFormat &&
	   curSplit.textureId == textureId &&
	   curSplit.textureFilterMode == textureFilterMode &&
	   curSplit.drawPrimMode == g_DrawPrimMode &&
	   curSplit.drawenv.clip.x == activeDrawEnv.clip.x &&
	   curSplit.drawenv.clip.y == activeDrawEnv.clip.y &&
	   curSplit.drawenv.clip.w == activeDrawEnv.clip.w &&
	   curSplit.drawenv.clip.h == activeDrawEnv.clip.h &&
	   curSplit.drawenv.dfe == activeDrawEnv.dfe &&
	   curSplit.debugText == currentSplitDebugText)
	{
		return;
	}

	curSplit.numVerts = g_vertexIndex - curSplit.startVertex;

	if(g_splitIndex + 1 >= MAX_DRAW_SPLITS)
	{
		eprinterr("MAX_DRAW_SPLITS reached (too many blend modes, texture formats, "
				  "drawEnv clip rects, dfe switches), expect rendering errors\n");
		return;
	}

	GPUDrawSplit& split		= g_splits[++g_splitIndex];
	split.blendMode			= blendMode;
	split.texFormat			= texFormat;
	split.textureId			= textureId;
	split.textureFilterMode = textureFilterMode;
	split.drawPrimMode		= g_DrawPrimMode;
	split.drawenv			= activeDrawEnv;
	split.dispenv			= activeDispEnv;
	split.debugText			= currentSplitDebugText;

	split.drawenv.tw.w = overrideTextureWidth;
	split.drawenv.tw.h = overrideTextureHeight;

	split.startVertex = g_vertexIndex;
	split.numVerts	  = 0;
}

void DrawSplit(const GPUDrawSplit& split)
{
	if(split.debugText)
		GR_PushDebugLabel(split.debugText);

	GR_SetStencilMode(split.drawPrimMode); // draw with mask 0x16

	GR_SetTexture(split.textureId, split.texFormat, split.textureFilterMode);
	GR_SetTextureBlendMode(split.blendMode);

	if(split.texFormat == TF_32_BIT_RGBA)
		GR_SetOverrideTextureSize(split.drawenv.tw.w, split.drawenv.tw.h);

	const bool drawOnScreen = split.drawenv.dfe;
	GR_SetupClipMode(&split.drawenv.clip, drawOnScreen);
	GR_SetOffscreenState(&split.drawenv.clip, !drawOnScreen);

	GR_SetBlendMode(split.blendMode);
	GR_SetPolygonOffset(0.0f, 0.0f);

	const bool depthRequested = g_RequestedDepthMode != 0;
	const bool depthWrite = split.blendMode == BM_NONE;
	int runStart = split.startVertex;
	int runVertices = 0;
	bool runUsesDepth = false;

	const auto flushRun = [&]()
	{
		if(runVertices == 0)
			return;
		GR_SetDepthState(runUsesDepth ? 1 : 0,
						 runUsesDepth && depthWrite ? 1 : 0);
		GR_DrawTriangles(runStart, runVertices / 3);
		runVertices = 0;
	};

	for(int vertexIndex = split.startVertex;
		vertexIndex < split.startVertex + split.numVerts; vertexIndex += 3)
	{
		const GrVertex* triangle = &g_vertexBuffer[vertexIndex];
		const bool precise = triangle[0].scr_h > 0.0f &&
			triangle[1].scr_h > 0.0f && triangle[2].scr_h > 0.0f;
		// Like the reference PGXP implementation, screen-aligned/2D polygons do
		// not participate in world depth. Their authored OT order is definitive.
		const bool is3D = precise &&
			(triangle[0].z != triangle[1].z || triangle[0].z != triangle[2].z);
		const bool usesDepth = depthRequested && is3D;
		const float averageDepth =
			(triangle[0].z + triangle[1].z + triangle[2].z) / 3.0f;
		const bool clearDepth = usesDepth &&
			averageDepth - g_lastPolygonDepth >= PGXP_DEPTH_CLEAR_THRESHOLD;

		if(runVertices != 0 && (runUsesDepth != usesDepth || clearDepth))
			flushRun();
		if(clearDepth)
			GR_ClearDepthBuffer();
		if(runVertices == 0)
		{
			runStart = vertexIndex;
			runUsesDepth = usesDepth;
		}
		runVertices += 3;
		if(usesDepth)
			g_lastPolygonDepth = averageDepth;
	}
	flushRun();

	if(split.debugText)
		GR_PopDebugLabel();
}

extern int g_dbg_polygonSelected;

//
// Draws all polygons after AggregatePTAG
//
void DrawAllSplits()
{
#ifdef _DEBUG
	if(g_dbg_emulatorPaused)
	{
		for(int i = 0; i < 3; i++)
		{
			GrVertex* vert = &g_vertexBuffer[g_dbg_polygonSelected + i];
			vert->r		   = 255;
			vert->g		   = 0;
			vert->b		   = 0;

			eprintf("==========================================\n");
			eprintf("POLYGON: %d\n", g_dbg_polygonSelected);
#if USE_PGXP
			eprintf("X: %.2f Y: %.2f\n", (float)vert->x, (float)vert->y);
			eprintf("U: %.2f V: %.2f\n", (float)vert->u, (float)vert->v);
			eprintf("TP: %d CLT: %d\n", (int)vert->page, (int)vert->clut);
#else
			eprintf("X: %d Y: %d\n", vert->x, vert->y);
			eprintf("U: %d V: %d\n", vert->u, vert->v);
			eprintf("TP: %d CLT: %d\n", vert->page, vert->clut);
#endif

			eprintf("==========================================\n");
		}

		PsyX_UpdateInput();
	}
#endif // _DEBUG

	// next code ideally should be called before EndScene
	GR_UpdateVertexBuffer(g_vertexBuffer, g_vertexIndex);

	for(int i = 1; i <= g_splitIndex; i++)
		DrawSplit(g_splits[i]);

	ClearSplits();
}

// forward declarations
int ParsePrimitive(P_TAG* polyTag);

void ParsePrimitivesLinkedList(u_long* p, int singlePrimitive)
{
	if(!p)
		return;
	g_lastPolygonDepth = PGXP_MAX_DEPTH;

	// setup single primitive flag (needed for AddSplits)
	g_DrawPrimMode = singlePrimitive;

	if(singlePrimitive)
	{
		P_TAG* polyTag = reinterpret_cast<P_TAG*>(p);
#if USE_PGXP && USE_EXTENDED_PRIM_POINTERS
		// force PGXP off
		polyTag->pgxp_index = 0xFFFF;
#endif
		if(g_vertexIndex >= MAX_VERTEX_BUFFER_SIZE - MAX_VERTICES_PER_PRIMITIVE)
		{
			GPUDrawSplit& lastSplit = g_splits[g_splitIndex];
			lastSplit.numVerts		= g_vertexIndex - lastSplit.startVertex;
			DrawAllSplits();
		}
		ParsePrimitive(polyTag);

		GPUDrawSplit& lastSplit = g_splits[g_splitIndex];
		lastSplit.numVerts		= g_vertexIndex - lastSplit.startVertex;
	}
	else
	{
		// walk OT_TAG linked list
		for(uintptr_t basePacket = reinterpret_cast<uintptr_t>(p);;
			basePacket			 = reinterpret_cast<uintptr_t>(nextPrim(basePacket)))
		{
			const int tagLength = getlen(basePacket);
			if(tagLength > 0)
			{
				if(tagLength > 32)
				{
					eprinterr("got invalid tag length %d, code %d\n", tagLength,
							  reinterpret_cast<P_TAG*>(basePacket)->code);
				}

				uintptr_t		currentPacket = basePacket;
				const uintptr_t endPacket =
					basePacket + (tagLength + P_LEN) * sizeof(u_int);
				int primLength = 0;
				while(currentPacket < endPacket)
				{
					if(g_vertexIndex >=
					   MAX_VERTEX_BUFFER_SIZE - MAX_VERTICES_PER_PRIMITIVE)
					{
						GPUDrawSplit& lastSplit = g_splits[g_splitIndex];
						lastSplit.numVerts		= g_vertexIndex - lastSplit.startVertex;
						DrawAllSplits();
					}
					primLength = ParsePrimitive(reinterpret_cast<P_TAG*>(currentPacket));
					currentPacket += (primLength + P_LEN) * sizeof(u_int);
				}

				if(currentPacket != endPacket)
				{
					eprinterr("did not output valid primitive or ptag length is not "
							  "valid (diff=%d)\n",
							  endPacket - currentPacket);
				}
			}

			GPUDrawSplit& lastSplit = g_splits[g_splitIndex];
			lastSplit.numVerts		= g_vertexIndex - lastSplit.startVertex;

			if(isendprim(basePacket))
				break;
		}
	}
}

inline int IsNull(POLY_FT3* poly)
{
	return poly->x0 == -1 && poly->y0 == -1 && poly->x1 == -1 && poly->y1 == -1 &&
		   poly->x2 == -1 && poly->y2 == -1;
}

static bool IsExactProjectedVertex(VERTTYPE* vertex, ushort gteIndex,
								   int cacheOffset)
{
#if USE_PGXP && USE_EXTENDED_PRIM_POINTERS
	PGXPVData cached;
	return LookupVertexPGXP(&cached, vertex, gteIndex, cacheOffset);
#else
	(void)vertex;
	(void)gteIndex;
	(void)cacheOffset;
	return true;
#endif
}

static TextureFilterMode TriangleTextureFilter(ushort gteIndex, VERTTYPE* p0,
											   VERTTYPE* p1, VERTTYPE* p2)
{
	return IsExactProjectedVertex(p0, gteIndex, -3) &&
			   IsExactProjectedVertex(p1, gteIndex, -2) &&
			   IsExactProjectedVertex(p2, gteIndex, -1)
			   ? TEXTURE_FILTER_WORLD_ANISOTROPIC
			   : TEXTURE_FILTER_BILINEAR;
}

static TextureFilterMode QuadTextureFilter(ushort gteIndex, VERTTYPE* p0,
										   VERTTYPE* p1, VERTTYPE* p2,
										   VERTTYPE* p3)
{
	return IsExactProjectedVertex(p0, gteIndex, -4) &&
				   IsExactProjectedVertex(p1, gteIndex, -3) &&
			   IsExactProjectedVertex(p2, gteIndex, -1) &&
			   IsExactProjectedVertex(p3, gteIndex, -2)
			   ? TEXTURE_FILTER_WORLD_ANISOTROPIC
			   : TEXTURE_FILTER_BILINEAR;
}

static int ProcessFlatLines(P_TAG* polyTag)
{
#if USE_PGXP && USE_EXTENDED_PRIM_POINTERS
	const u_short gteIndex = polyTag->pgxp_index;
#else
	const u_short gteIndex = 0xFFFF;
#endif

	const bool shadeTexOn  = true;
	const bool semiTrans   = (polyTag->code & 2);
	const int  primSubType = polyTag->code & 0x0C;

	switch(primSubType)
	{
	case 0x0:
	{
		LINE_F2* poly = (LINE_F2*)polyTag;

		AddSplit(semiTrans, false);

		VERTTYPE*	   p0 = &poly->x0;
		VERTTYPE*	   p1 = &poly->x1;
		unsigned char* c0 = &poly->r0;
		unsigned char* c1 = c0;

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		LineSwapSourceVerts(p0, p1, c0, c1);
		MakeLineArray(firstVertex, p0, p1, gteIndex);
		MakeTexcoordLineZero(firstVertex, 0);
		MakeColourLine(firstVertex, shadeTexOn, c0, c1);

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 3;
	}
	case 0x8: // TODO (unused)
	{
		LINE_F3* poly = (LINE_F3*)polyTag;

		AddSplit(semiTrans, false);

		{
			VERTTYPE*	   p0 = &poly->x0;
			VERTTYPE*	   p1 = &poly->x1;
			unsigned char* c0 = &poly->r0;
			unsigned char* c1 = c0;

			GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
			LineSwapSourceVerts(p0, p1, c0, c1);
			MakeLineArray(firstVertex, p0, p1, gteIndex);
			MakeTexcoordLineZero(firstVertex, 0);
			MakeColourLine(firstVertex, shadeTexOn, c0, c1);

			TriangulateQuad();

			g_vertexIndex += 6;
#if defined(DEBUG_POLY_COUNT)
			polygon_count++;
#endif
		}

		{
			VERTTYPE*	   p0 = &poly->x1;
			VERTTYPE*	   p1 = &poly->x2;
			unsigned char* c0 = &poly->r0;
			unsigned char* c1 = c0;

			GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
			LineSwapSourceVerts(p0, p1, c0, c1);
			MakeLineArray(firstVertex, p0, p1, gteIndex);
			MakeTexcoordLineZero(firstVertex, 0);
			MakeColourLine(firstVertex, shadeTexOn, c0, c1);

			TriangulateQuad();

			g_vertexIndex += 6;
#if defined(DEBUG_POLY_COUNT)
			polygon_count++;
#endif
		}

		return 5;
	}
	case 0xc:
	{
		int		 i;
		LINE_F4* poly = (LINE_F4*)polyTag;

		AddSplit(semiTrans, false);

		{
			VERTTYPE*	   p0 = &poly->x0;
			VERTTYPE*	   p1 = &poly->x1;
			unsigned char* c0 = &poly->r0;
			unsigned char* c1 = c0;

			GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
			LineSwapSourceVerts(p0, p1, c0, c1);
			MakeLineArray(firstVertex, p0, p1, gteIndex);
			MakeTexcoordLineZero(firstVertex, 0);
			MakeColourLine(firstVertex, shadeTexOn, c0, c1);

			TriangulateQuad();

			g_vertexIndex += 6;
#if defined(DEBUG_POLY_COUNT)
			polygon_count++;
#endif
		}

		{
			VERTTYPE*	   p0 = &poly->x1;
			VERTTYPE*	   p1 = &poly->x2;
			unsigned char* c0 = &poly->r0;
			unsigned char* c1 = c0;

			GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
			LineSwapSourceVerts(p0, p1, c0, c1);
			MakeLineArray(firstVertex, p0, p1, gteIndex);
			MakeTexcoordLineZero(firstVertex, 0);
			MakeColourLine(firstVertex, shadeTexOn, c0, c1);

			TriangulateQuad();

			g_vertexIndex += 6;
#if defined(DEBUG_POLY_COUNT)
			polygon_count++;
#endif
		}

		{
			VERTTYPE*	   p0 = &poly->x2;
			VERTTYPE*	   p1 = &poly->x3;
			unsigned char* c0 = &poly->r0;
			unsigned char* c1 = c0;

			GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
			LineSwapSourceVerts(p0, p1, c0, c1);
			MakeLineArray(firstVertex, p0, p1, gteIndex);
			MakeTexcoordLineZero(firstVertex, 0);
			MakeColourLine(firstVertex, shadeTexOn, c0, c1);

			TriangulateQuad();

			g_vertexIndex += 6;
#if defined(DEBUG_POLY_COUNT)
			polygon_count++;
#endif
		}

		return 6;
	}
	}
	return 0;
}

static int ProcessGouraudLines(P_TAG* polyTag)
{
#if USE_PGXP && USE_EXTENDED_PRIM_POINTERS
	const u_short gteIndex = polyTag->pgxp_index;
#else
	const u_short gteIndex = 0xFFFF;
#endif

	const bool shadeTexOn  = true;
	const bool semiTrans   = (polyTag->code & 2);
	const int  primSubType = polyTag->code & 0x0C;

	switch(primSubType)
	{
	case 0x0:
	{
		LINE_G2* poly = (LINE_G2*)polyTag;

		AddSplit(semiTrans, false);

		VERTTYPE*	   p0 = &poly->x0;
		VERTTYPE*	   p1 = &poly->x1;
		unsigned char* c0 = &poly->r0;
		unsigned char* c1 = &poly->r1;

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		LineSwapSourceVerts(p0, p1, c0, c1);
		MakeLineArray(firstVertex, p0, p1, gteIndex);
		MakeTexcoordLineZero(firstVertex, 0);
		MakeColourLine(firstVertex, shadeTexOn, c0, c1);

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 4;
	}
	case 0x8:
	{
		// TODO: LINE_G3
		return 7;
	}
	case 0xC:
	{
		// TODO: LINE_G4
		return 9;
	}
	}
	return 0;
}

static int ProcessFlatPoly(P_TAG* polyTag)
{
#if USE_PGXP && USE_EXTENDED_PRIM_POINTERS
	const u_short gteIndex = polyTag->pgxp_index;
#else
	const u_short gteIndex = 0xFFFF;
#endif

	const bool shadeTexOn  = (polyTag->code & 1) == 0;
	const bool semiTrans   = (polyTag->code & 2);
	const int  primSubType = polyTag->code & 0x0C;

	switch(primSubType)
	{
	case 0x0:
	{
		POLY_F3* poly = (POLY_F3*)polyTag;

		AddSplit(semiTrans, false);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexTriangle(firstVertex, &poly->x0, &poly->x1, &poly->x2, gteIndex);
		MakeTexcoordTriangleZero(firstVertex, 0);
		MakeColourTriangle(firstVertex, shadeTexOn, &poly->r0, &poly->r0,
						   &poly->r0);

		g_vertexIndex += 3;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 4;
	}
	case 0x4:
	{
		POLY_FT3* poly		= (POLY_FT3*)polyTag;
		activeDrawEnv.tpage = poly->tpage;

		// It is an official hack from SCE devs to not use DR_TPAGE and instead use
		// null polygon
		if(!IsNull(poly))
		{
			AddSplit(
				semiTrans, true,
				TriangleTextureFilter(gteIndex, &poly->x0, &poly->x1, &poly->x2));

			GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
			MakeVertexTriangle(firstVertex, &poly->x0, &poly->x1, &poly->x2,
							   gteIndex);
			MakeTexcoordTriangle(
				firstVertex, &poly->u0, &poly->u1, &poly->u2, poly->tpage, poly->clut,
				GET_TPAGE_DITHER(activeDrawEnv.tpage) || activeDrawEnv.dtd);
			MakeColourTriangle(firstVertex, shadeTexOn, &poly->r0, &poly->r0,
							   &poly->r0);

			g_vertexIndex += 3;

#if defined(DEBUG_POLY_COUNT)
			polygon_count++;
#endif
		}
		return 7;
	}
	case 0x8:
	{
		POLY_F4* poly = (POLY_F4*)polyTag;

		AddSplit(semiTrans, false);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexQuad(firstVertex, &poly->x0, &poly->x1, &poly->x3, &poly->x2,
					   gteIndex);
		MakeTexcoordQuadZero(firstVertex, 0);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0,
					   &poly->r0);

		TriangulateQuad();

		g_vertexIndex += 6;
#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 5;
	}
	case 0xC:
	{
		POLY_FT4* poly		= (POLY_FT4*)polyTag;
		activeDrawEnv.tpage = poly->tpage;

		AddSplit(semiTrans, true,
				 QuadTextureFilter(gteIndex, &poly->x0, &poly->x1, &poly->x3,
								   &poly->x2));

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexQuad(firstVertex, &poly->x0, &poly->x1, &poly->x3, &poly->x2,
					   gteIndex);
		MakeTexcoordQuad(
			firstVertex, &poly->u0, &poly->u1, &poly->u3, &poly->u2, poly->tpage,
			poly->clut, GET_TPAGE_DITHER(activeDrawEnv.tpage) || activeDrawEnv.dtd);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0,
					   &poly->r0);

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 9;
	}
	}
	return 0;
}

static int ProcessGouraudPoly(P_TAG* polyTag)
{
#if USE_PGXP && USE_EXTENDED_PRIM_POINTERS
	const u_short gteIndex = polyTag->pgxp_index;
#else
	const u_short gteIndex = 0xFFFF;
#endif

	const bool shadeTexOn  = true;
	const bool semiTrans   = (polyTag->code & 2);
	const int  primSubType = polyTag->code & 0x0C;

	switch(primSubType)
	{
	case 0x0:
	{
		POLY_G3* poly = (POLY_G3*)polyTag;

		AddSplit(semiTrans, false);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexTriangle(firstVertex, &poly->x0, &poly->x1, &poly->x2, gteIndex);
		MakeTexcoordTriangleZero(firstVertex, 1);
		MakeColourTriangle(firstVertex, shadeTexOn, &poly->r0, &poly->r1,
						   &poly->r2);

		g_vertexIndex += 3;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 6;
	}
	case 0x4:
	{
		POLY_GT3* poly		= (POLY_GT3*)polyTag;
		activeDrawEnv.tpage = poly->tpage;

		AddSplit(semiTrans, true,
				 TriangleTextureFilter(gteIndex, &poly->x0, &poly->x1, &poly->x2));

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexTriangle(firstVertex, &poly->x0, &poly->x1, &poly->x2, gteIndex);
		MakeTexcoordTriangle(
			firstVertex, &poly->u0, &poly->u1, &poly->u2, poly->tpage, poly->clut,
			GET_TPAGE_DITHER(activeDrawEnv.tpage) || activeDrawEnv.dtd);
		MakeColourTriangle(firstVertex, shadeTexOn, &poly->r0, &poly->r1,
						   &poly->r2);

		g_vertexIndex += 3;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 9;
	}
	case 0x8:
	{
		POLY_G4* poly = (POLY_G4*)polyTag;

		AddSplit(semiTrans, false);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexQuad(firstVertex, &poly->x0, &poly->x1, &poly->x3, &poly->x2,
					   gteIndex);
		MakeTexcoordQuadZero(firstVertex, 1);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r1, &poly->r3,
					   &poly->r2);

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 8;
	}
	case 0xC:
	{
		POLY_GT4* poly		= (POLY_GT4*)polyTag;
		activeDrawEnv.tpage = poly->tpage;

		AddSplit(semiTrans, true,
				 QuadTextureFilter(gteIndex, &poly->x0, &poly->x1, &poly->x3,
								   &poly->x2));

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexQuad(firstVertex, &poly->x0, &poly->x1, &poly->x3, &poly->x2,
					   gteIndex);
		MakeTexcoordQuad(
			firstVertex, &poly->u0, &poly->u1, &poly->u3, &poly->u2, poly->tpage,
			poly->clut, GET_TPAGE_DITHER(activeDrawEnv.tpage) || activeDrawEnv.dtd);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r1, &poly->r3,
					   &poly->r2);

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 12;
	}
	}
	return 0;
}

static int ProcessTileAndSprt(P_TAG* polyTag)
{
#if USE_PGXP && USE_EXTENDED_PRIM_POINTERS
	const u_short gteIndex = polyTag->pgxp_index;
#else
	const u_short gteIndex = 0xFFFF;
#endif

	// NOTE: TILE does not support switching shadeTex on real PSX
	const bool shadeTexOn = (polyTag->code & 1) == 0;
	const bool semiTrans  = (polyTag->code & 2);

	switch(polyTag->code & 0xFD)
	{
	case 0x60:
	{
		TILE* poly = (TILE*)polyTag;

		AddSplit(semiTrans, false);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, poly->w, poly->h, gteIndex);
		MakeTexcoordQuadZero(firstVertex, 0);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0,
					   &poly->r0);

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 3;
	}
	case 0x64:
	{
		SPRT* poly = (SPRT*)polyTag;

		AddSplit(semiTrans, true, TEXTURE_FILTER_BILINEAR);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, poly->w, poly->h, gteIndex);
		MakeTexcoordRect(firstVertex, &poly->u0, activeDrawEnv.tpage, poly->clut,
						 poly->w, poly->h);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0,
					   &poly->r0);

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 4;
	}
	case 0x68:
	{
		TILE_1* poly = (TILE_1*)polyTag;

		AddSplit(semiTrans, false);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, 1, 1, gteIndex);
		MakeTexcoordQuadZero(firstVertex, 0);
		MakeColourQuad(firstVertex, true, &poly->r0, &poly->r0, &poly->r0,
					   &poly->r0);

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 2;
	}
	case 0x70:
	{
		TILE_8* poly = (TILE_8*)polyTag;

		AddSplit(semiTrans, false);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, 8, 8, gteIndex);
		MakeTexcoordQuadZero(firstVertex, 0);
		MakeColourQuad(firstVertex, true, &poly->r0, &poly->r0, &poly->r0,
					   &poly->r0);

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 2;
	}
	case 0x74:
	{
		SPRT_8* poly = (SPRT_8*)polyTag;

		AddSplit(semiTrans, true, TEXTURE_FILTER_BILINEAR);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, 8, 8, gteIndex);
		MakeTexcoordRect(firstVertex, &poly->u0, activeDrawEnv.tpage, poly->clut, 8,
						 8);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0,
					   &poly->r0);

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 3;
	}
	case 0x78:
	{
		TILE_16* poly = (TILE_16*)polyTag;

		AddSplit(semiTrans, false);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, 16, 16, gteIndex);
		MakeTexcoordQuadZero(firstVertex, 0);
		MakeColourQuad(firstVertex, true, &poly->r0, &poly->r0, &poly->r0,
					   &poly->r0);

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 2;
	}
	case 0x7C:
	{
		SPRT_16* poly = (SPRT_16*)polyTag;

		AddSplit(semiTrans, true, TEXTURE_FILTER_BILINEAR);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, 16, 16, gteIndex);
		MakeTexcoordRect(firstVertex, &poly->u0, activeDrawEnv.tpage, poly->clut,
						 16, 16);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0,
					   &poly->r0);

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 3;
	}
	}
	return 0;
}

static int ProcessDrawEnv(P_TAG* polyTag)
{
	const u_int* codePtr		= (u_int*)&polyTag->pad0;
	int			 processedLongs = 0;
	for(int i = 0; i < polyTag->len; ++i)
	{
		const u_int code		= codePtr[i];
		const int	primSubType = code >> 24 & 0x0F;

		switch(primSubType)
		{
		case 0x1:
		{
			// DR_TPAGE
			activeDrawEnv.tpage = (code & 0x1FF);
			activeDrawEnv.dtd	= (code >> 9) & 1;
			activeDrawEnv.dfe	= (code >> 10) & 1;
			break;
		}
		case 0x2:
		{
			// DR_TWIN
			activeDrawEnv.tw.w = (code & 0x1F);
			activeDrawEnv.tw.h = ((code >> 5) & 0x1F);
			activeDrawEnv.tw.x = ((code >> 10) & 0x1F);
			activeDrawEnv.tw.y = ((code >> 15) & 0x1F);
			break;
		}
		case 0x3:
		{
			// DR_AREA
			activeDrawEnv.clip.x = code & 1023;
			activeDrawEnv.clip.y = (code >> 10) & 1023;
			break;
		}
		case 0x4:
		{
			// DR_AREA (second part)
			activeDrawEnv.clip.w = code & 1023;
			activeDrawEnv.clip.h = (code >> 10) & 1023;

			activeDrawEnv.clip.w -= activeDrawEnv.clip.x;
			activeDrawEnv.clip.h -= activeDrawEnv.clip.y;
			break;
		}
		case 0x5:
		{
			// DR_OFFSET
			// TODO
			activeDrawEnv.ofs[0] = code & 2047;
			activeDrawEnv.ofs[1] = (code >> 11) & 2047;
			break;
		}
		case 0x6:
		{
			eprintf("Mask setting: %08x\n", code);
			// MaskSetOR = (*cb & 1) ? 0x8000 : 0x0000;
			// MaskEvalAND = (*cb & 2) ? 0x8000 : 0x0000;
			break;
		}
		case 0:
			// proceed to next primitive tag
			return processedLongs;
		}
		++processedLongs;
	}

	return processedLongs;
}

static int ProcessPsyXPrims(P_TAG* polyTag)
{
	const int primType	  = polyTag->code & 0xF0;
	const int primSubType = polyTag->code & 0x0F;

	switch(primSubType)
	{
	case 0x01:
	{
		DR_PSYX_TEX* psytex	  = (DR_PSYX_TEX*)polyTag;
		overrideTexture		  = psytex->code[0] & 0xFFFFFF;
		overrideTextureWidth  = psytex->code[1] & 0xFFF;
		overrideTextureHeight = psytex->code[1] >> 16 & 0xFFF;
		return 2;
	}
	case 0x02:
	{
		// [A] Psy-X custom texture packet
		DR_PSYX_DBGMARKER* psydbg = (DR_PSYX_DBGMARKER*)polyTag;
		currentSplitDebugText	  = psydbg->text;
		return 2;
	}
	}

	return 0;
}

// Processes primitive
// returns processed primitive primLength in longs
int ParsePrimitive(P_TAG* polyTag)
{
	const int primType = polyTag->code & 0xF0;

	int primLength = 0;

	switch(primType)
	{
	case 0x00:
	{
		const int primSubType = polyTag->code & 0x0F;
		if(primSubType == 0x0)
		{
			primLength = 3;
		}
		else if(primSubType == 0x1)
		{
			DR_MOVE* drmove = (DR_MOVE*)polyTag;

			const int y = drmove->code[3] >> 0x10 & 0xFFFF;
			const int x = drmove->code[3] & 0xFFFF;

			RECT16 rect;
			*(uint*)&rect.x = *(uint*)&drmove->code[2];
			*(uint*)&rect.w = *(uint*)&drmove->code[4];

			MoveImage(&rect, x, y);
			primLength = 5;
		}
		break;
	}
	case 0x20:
		// Flat polygons
		primLength = ProcessFlatPoly(polyTag);
		break;
	case 0x30:
		// Gouraud shaded polygons
		primLength = ProcessGouraudPoly(polyTag);
		break;
	case 0x40:
		// Flat (single colour) Lines
		primLength = ProcessFlatLines(polyTag);
		break;
	case 0x50:
		// Gouraud lines
		primLength = ProcessGouraudLines(polyTag);
		break;
	case 0x60:
	case 0x70:
		// TILE and SPRT
		primLength = ProcessTileAndSprt(polyTag);
		break;
	case 0xA0:
		// DR_LOAD
		{
			DR_LOAD* drload = (DR_LOAD*)polyTag;

			RECT16 rect;
			*(uint*)&rect.x = *(uint*)&drload->code[1];
			*(uint*)&rect.w = *(uint*)&drload->code[2];

			LoadImage(&rect, (u_long*)drload->p);
			// Emulator_UpdateVRAM();			// FIXME: should it be updated
			// immediately?

			// FIXME: is there othercommands?
		}
		primLength = getlen(polyTag);
		break;
	case 0xB0:
		// [A] Psy-X custom primitives
		primLength = ProcessPsyXPrims(polyTag);
		break;
	case 0xE0:
		// Draw Env setup
		primLength = ProcessDrawEnv(polyTag);
		break;
		// default:
		//	eprinterr("got %0x primitive\n", primType);
	}

	if(primLength == 0)
	{
		eprinterr("Unhandled zero length %0x primitive\n", primType);
	}

	return primLength;
}
