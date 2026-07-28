#include "PsyX/PsyX_public.h"

#include "../gpu/PsyX_GPU.h"
#include "../platform.h"

#include "PsyX/PsyX_globals.h"
#include "PsyX/PsyX_render.h"
#include "PsyX/util/timer.h"

#include <algorithm>
#include <array>
#include <assert.h>
#include <string.h>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#if defined(_LANGUAGE_C_PLUS_PLUS) || defined(__cplusplus) ||                  \
    defined(c_plusplus)
extern "C" {
#endif

__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;

#if defined(_LANGUAGE_C_PLUS_PLUS) || defined(__cplusplus) ||                  \
    defined(c_plusplus)
}
#endif

#endif // def WIN32

#if defined(RENDERER_OGL)

#define USE_PBO 1
#define USE_OFFSCREEN_BLIT 1
#define USE_FRAMEBUFFER_BLIT 1

#else

// OpenGL ES/Web GL has slowdowns and doesn't allow GL_LUMINANCE_ALPHA format as
// framebuffer, so it's disabled
#define USE_PBO (OGLES_VERSION == 3)
#define USE_OFFSCREEN_BLIT (OGLES_VERSION == 3)
#define USE_FRAMEBUFFER_BLIT (OGLES_VERSION == 3)

#endif

extern SDL_Window *g_window;

#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif
#ifndef GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#endif

static GLfloat g_maxTextureAnisotropy = 1.0f;

#define MAX_NUM_VERTEX_BUFFERS (2)

// The display width is selected by the game (320, 384, 512, ...). A fixed
// 320x240 pixel aspect rescales every wider mode a second time after GPU
// coordinates have already been normalized by activeDispEnv, producing a
// tall/narrow image. Keep projection, clipping and widescreen mapping on the
// same active display aspect instead.
static float PsyX_GetActiveScreenAspect() {
  if (activeDispEnv.disp.w > 0 && activeDispEnv.disp.h > 0)
    return (float)activeDispEnv.disp.h / (float)activeDispEnv.disp.w;
  return 240.0f / 320.0f;
}

int g_PreviousBlendMode = BM_NONE;
int g_PreviousDepthMode = 0;
int g_PreviousDepthWrite = 1;
int g_PreviousDepthFunc = -1;
int g_RequestedDepthMode = 0;
float g_PreviousPolygonOffsetSlope = 0.0f;
float g_PreviousPolygonOffsetUnits = 0.0f;
int g_PreviousStencilMode = 0;
int g_ShadowStencilPhase = 0;
int g_PreviousScissorState = 0;
int g_PreviousOffscreenState = 0;
RECT16 g_PreviousFramebuffer = {0, 0, 0, 0};
RECT16 g_PreviousOffscreen = {0, 0, 0, 0};

ShaderID g_PreviousShader = -1;

TextureID g_vramTexturesDouble[2];
TextureID g_vramTexture;
TextureID g_vramAliasTexture;
TextureID g_rgLutTexture;
int g_vramTextureIdx = 0;

TextureID g_fbTexture = -1;
TextureID g_offscreenRTTexture = -1;

TextureID g_whiteTexture = -1;
TextureID g_lastBoundTexture = -1;

int g_windowWidth = 0;
int g_windowHeight = 0;
static int g_drawableWidth = 0;
static int g_drawableHeight = 0;

static void PsyX_UpdateDrawableSize() {
  int width = g_windowWidth > 0 ? g_windowWidth : 1;
  int height = g_windowHeight > 0 ? g_windowHeight : 1;
#if defined(RENDERER_OGL) || defined(RENDERER_OGLES)
  if (g_window)
    SDL_GL_GetDrawableSize(g_window, &width, &height);
#endif
  g_drawableWidth = std::max(width, 1);
  g_drawableHeight = std::max(height, 1);
}

static PsyXPresentationViewport PsyX_GetLogicalViewport() {
  const int displayWidth =
      activeDispEnv.disp.w > 0 ? activeDispEnv.disp.w : 320;
  const int displayHeight =
      activeDispEnv.disp.h > 0 ? activeDispEnv.disp.h : 240;
  PsyXPresentationViewport viewport = {0, 0, displayWidth, displayHeight};
  return viewport;
}

// The PSX framebuffer uses mode-dependent non-square pixels. Original mode
// presents 384x240 gameplay and 320x240 movies at retail 4:3. Adaptive mode
// uses the complete drawable; its vertex transform adds horizontal or vertical
// world view while authored 2D content keeps its original proportions.
PsyXPresentationViewport PsyX_CalculatePresentationViewport(int drawableWidth,
                                                            int drawableHeight,
                                                            int aspectMode) {
  if (drawableWidth < 1)
    drawableWidth = 1;
  if (drawableHeight < 1)
    drawableHeight = 1;

  PsyXPresentationViewport viewport = {0, 0, drawableWidth, drawableHeight};
  if (aspectMode == PSYX_ASPECT_ADAPTIVE)
    return viewport;

  if ((long long)drawableWidth * 3 > (long long)drawableHeight * 4)
    viewport.w = drawableHeight * 4 / 3;
  else
    viewport.h = drawableWidth * 3 / 4;

  if (viewport.w < 1)
    viewport.w = 1;
  if (viewport.h < 1)
    viewport.h = 1;
  viewport.x = (drawableWidth - viewport.w) / 2;
  viewport.y = (drawableHeight - viewport.h) / 2;
  return viewport;
}

static PsyXPresentationViewport PsyX_GetPresentationViewport() {
  return PsyX_CalculatePresentationViewport(
      g_drawableWidth > 0 ? g_drawableWidth : std::max(g_windowWidth, 1),
      g_drawableHeight > 0 ? g_drawableHeight : std::max(g_windowHeight, 1),
      g_cfg_aspectMode);
}

// Rasterize directly at the selected presentation size. PSX coordinates remain
// logical inputs, while color/depth/MSAA use the real high-DPI drawable area.
static PsyXPresentationViewport PsyX_GetRenderViewport() {
  PsyXPresentationViewport viewport = PsyX_GetPresentationViewport();
  viewport.x = 0;
  viewport.y = 0;
  return viewport;
}

PsyXPresentationScale PsyX_CalculatePresentationScale(int drawableWidth,
                                                      int drawableHeight,
                                                      int aspectMode) {
  PsyXPresentationScale scale = {1.0f, 1.0f};
  if (aspectMode != PSYX_ASPECT_ADAPTIVE)
    return scale;

  if (drawableWidth < 1)
    drawableWidth = 1;
  if (drawableHeight < 1)
    drawableHeight = 1;
  const float targetAspect = (float)drawableWidth / (float)drawableHeight;
  const float originalAspect = 4.0f / 3.0f;
  if (targetAspect > originalAspect)
    scale.x = originalAspect / targetAspect;
  else if (targetAspect < originalAspect)
    scale.y = targetAspect / originalAspect;
  return scale;
}

static PsyXPresentationScale PsyX_GetPresentationScale() {
  const PsyXPresentationViewport viewport = PsyX_GetRenderViewport();
  return PsyX_CalculatePresentationScale(viewport.w, viewport.h,
                                         g_cfg_aspectMode);
}

int g_dbg_wireframeMode = 0;
int g_dbg_texturelessMode = 0;

int g_cfg_pgxpTextureCorrection = 1;
int g_cfg_pgxpZBuffer = 1;
int g_cfg_bilinearFiltering = 0;
int g_cfg_anisotropicFiltering = 0;
int g_cfg_ssao = 0;
int g_cfg_msaaSamples = 0;
int g_cfg_aspectMode = PSYX_ASPECT_ORIGINAL_4_3;

int vram_need_update = 1;
int framebuffer_need_update = 0;
static int g_appliedSwapInterval = -1000;

typedef struct {
  int x0[VRAM_HEIGHT];
  int x1[VRAM_HEIGHT];
} GrVRAMDirtyRows;

static GrVRAMDirtyRows g_vramDirtyRows[2];

static void GR_ResetVRAMDirtyRects() {
  for (int texture = 0; texture < 2; ++texture) {
    for (int row = 0; row < VRAM_HEIGHT; ++row) {
      g_vramDirtyRows[texture].x0[row] = 0;
      g_vramDirtyRows[texture].x1[row] = VRAM_WIDTH;
    }
  }
  vram_need_update = 1;
}

static void GR_MarkVRAMDirty(int x, int y, int w, int h) {
  const int x0 = std::max(0, x);
  const int y0 = std::max(0, y);
  const int x1 = std::min(VRAM_WIDTH, x + w);
  const int y1 = std::min(VRAM_HEIGHT, y + h);
  if (x0 >= x1 || y0 >= y1)
    return;

  for (int texture = 0; texture < 2; ++texture) {
    GrVRAMDirtyRows &dirty = g_vramDirtyRows[texture];
    for (int row = y0; row < y1; ++row) {
      if (dirty.x0[row] >= dirty.x1[row]) {
        dirty.x0[row] = x0;
        dirty.x1[row] = x1;
      } else {
        dirty.x0[row] = std::min(dirty.x0[row], x0);
        dirty.x1[row] = std::max(dirty.x1[row], x1);
      }
    }
  }
  vram_need_update = 1;
}

#if defined(__EMSCRIPTEN__) || defined(__RPI__) || defined(__ANDROID__)
#if defined(RENDERER_OGL)
#error It should not be enabled
#endif
#endif

#if USE_OPENGL
typedef struct {
  GLenum fmt;
  GLuint *pbos;
  uint64_t num_pbos;
  uint64_t dx;
  uint64_t num_downloads;

  int width;
  int height;
  int nbytes;            /* number of bytes in the pbo buffer. */
  unsigned char *pixels; /* the downloaded pixels. */
} GrPBO;

int PBO_Init(GrPBO *pbo, GLenum format, int w, int h, int num) {
  if (pbo->pbos) {
    eprinterr("Already initialized. Not necessary to initialize again; or "
              "shutdown first.");
    return -1;
  }

  if (0 >= num) {
    eprinterr("Invalid number of PBOs: %d", num);
    return -2;
  }

  pbo->fmt = format;
  pbo->width = w;
  pbo->height = h;
  pbo->num_pbos = num;

#ifndef GL_BGR
#define GL_BGR 0x80E0
#endif

#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif

#if USE_PBO
  if (GL_RED == pbo->fmt || GL_GREEN == pbo->fmt || GL_BLUE == pbo->fmt) {
    pbo->nbytes = pbo->width * pbo->height;
  } else if (GL_RGB == pbo->fmt || GL_BGR == pbo->fmt) {
    pbo->nbytes = pbo->width * pbo->height * 3;
  } else if (GL_RGBA == pbo->fmt || GL_BGRA == pbo->fmt) {
    pbo->nbytes = pbo->width * pbo->height * 4;
  } else {
    eprinterr("Unhandled pixel format, use GL_R, GL_RG, GL_RGB or GL_RGBA.");
    return -3;
  }

  if (pbo->nbytes == 0) {
    eprinterr("Invalid width or height given: %d x %d", pbo->width,
              pbo->height);
    return -4;
  }

  pbo->pbos = (GLuint *)malloc(sizeof(GLuint) * num);
  pbo->pixels = (u_char *)malloc(pbo->nbytes);

  glGenBuffers(num, pbo->pbos);
  for (int i = 0; i < num; ++i) {
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo->pbos[i]);
    glBufferData(GL_PIXEL_PACK_BUFFER, pbo->nbytes, NULL, GL_STREAM_READ);
  }

  glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
#endif
  return 0;
}

void PBO_Destroy(GrPBO *pbo) {
#if USE_PBO
  if (pbo->pbos) {
    glDeleteBuffers(pbo->num_pbos, pbo->pbos);

    free(pbo->pbos);
    pbo->num_pbos = 0;
    pbo->pbos = NULL;
  }

#endif
  if (pbo->pixels) {
    free(pbo->pixels);
    pbo->pixels = NULL;
  }

  pbo->num_downloads = 0;
  pbo->dx = 0;
  pbo->fmt = 0;
  pbo->nbytes = 0;
}

void PBO_Download(GrPBO *pbo) {
  unsigned char *ptr;

#if USE_PBO
  if (pbo->num_downloads < pbo->num_pbos) {
    /*
       First we need to make sure all our pbos are bound, so glMap/Unmap will
       read from the oldest bound buffer first.
    */
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo->pbos[pbo->dx]);

#if defined(RENDERER_OGL)
    glGetTexImage(GL_TEXTURE_2D, 0, pbo->fmt, GL_UNSIGNED_BYTE, 0);
#else
    glReadPixels(0, 0, pbo->width, pbo->height, pbo->fmt, GL_UNSIGNED_BYTE,
                 0); /* When a GL_PIXEL_PACK_BUFFER is bound, the last 0 is used
                        as offset into the buffer to read into. */
#endif
  } else {
    /* Read from the oldest bound pbo */
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo->pbos[pbo->dx]);

#if defined(RENDERER_OGL)
    ptr = (unsigned char *)glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
    if (NULL != ptr) {
      memcpy(pbo->pixels, ptr, pbo->nbytes);
      glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    } else
      eprintwarn("Failed to map the buffer\n");

    /* Trigger the next read. */
    glGetTexImage(GL_TEXTURE_2D, 0, pbo->fmt, GL_UNSIGNED_BYTE, 0);
#else
    glReadPixels(0, 0, pbo->width, pbo->height, GL_RGBA, GL_UNSIGNED_BYTE,
                 pbo->pixels);
#endif
  }

  ++pbo->dx;
  pbo->dx = pbo->dx % pbo->num_pbos;

  pbo->num_downloads++;

  if (pbo->num_downloads == UINT64_MAX)
    pbo->num_downloads = pbo->num_pbos;

  glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
#else
  // FIXME: THIS is very slow
  // Do not use at all

  // glBindBuffer(GL_PIXEL_PACK_BUFFER, 0); /* just make sure we're not
  // accidentilly using a PBO. */ glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA,
  // GL_UNSIGNED_BYTE, pbo->pixels);
#endif
}

GLuint g_glVertexArray[2];
GLuint g_glVertexBuffer[2];
int g_curVertexBuffer = 0;
GLuint g_boundVertexArray = 0;

GLuint g_glBlitFramebuffer;
GrPBO g_glFramebufferPBO;

GLuint g_glVRAMFramebuffer;

GLuint g_glOffscreenFramebuffer;
GrPBO g_glOffscreenPBO;

GLuint g_glNativeFramebuffer;
GLuint g_glNativeColorTexture;
GLuint g_glNativeDepthRenderbuffer;
GLuint g_glNativeStencilRenderbuffer;
GLuint g_glNativeDepthTexture;
GLuint g_glNativeMultisampleFramebuffer;
GLuint g_glNativeMultisampleColorRenderbuffer;
GLuint g_glNativeMultisampleDepthRenderbuffer;
int g_nativeFramebufferWidth;
int g_nativeFramebufferHeight;
int g_nativeFramebufferSamples;

GLuint g_glSSAOFramebuffer[2];
GLuint g_glSSAOTexture[2];
GLuint g_glSSAOVertexArray;
int g_ssaoFramebufferWidth;
int g_ssaoFramebufferHeight;
int g_ssaoFramebufferAvailable;
ShaderID g_ssaoShader;
ShaderID g_ssaoBlurShader;
ShaderID g_ssaoCompositeShader;
GLint g_ssaoProjectionScaleLoc = -1;
GLint g_ssaoNearFarLoc = -1;
GLint g_ssaoBlurInvResolutionLoc = -1;
GLint g_ssaoBlurNearFarLoc = -1;
float g_ssaoProjectionScaleX;
float g_ssaoProjectionScaleY;
float g_ssaoNearPlane;
float g_ssaoFarPlane;

#if defined(RENDERER_OGL)
static GLenum g_nativeDepthInternalFormat = GL_DEPTH32F_STENCIL8;

static void PsyX_AllocateNativeDepthTexture(GLenum internalFormat, int width,
                                            int height) {
  glBindTexture(GL_TEXTURE_2D, g_glNativeDepthTexture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  if (internalFormat == GL_DEPTH32F_STENCIL8) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH32F_STENCIL8, width, height, 0,
                 GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV, NULL);
  } else {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, width, height, 0,
                 GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, NULL);
  }
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                         GL_TEXTURE_2D, g_glNativeDepthTexture, 0);
  glBindTexture(GL_TEXTURE_2D, 0);
}

static int PsyX_EnsureSSAOFramebuffers(int width, int height) {
  if (!g_cfg_ssao)
    return 1;

  const int aoWidth = std::max(1, (width + 1) / 2);
  const int aoHeight = std::max(1, (height + 1) / 2);
  if (g_ssaoFramebufferAvailable && g_ssaoFramebufferWidth == aoWidth &&
      g_ssaoFramebufferHeight == aoHeight) {
    return 1;
  }

  g_ssaoFramebufferAvailable = 0;
  for (int index = 0; index < 2; ++index) {
    glBindTexture(GL_TEXTURE_2D, g_glSSAOTexture[index]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, aoWidth, aoHeight, 0, GL_RED,
                 GL_UNSIGNED_BYTE, NULL);
    glBindFramebuffer(GL_FRAMEBUFFER, g_glSSAOFramebuffer[index]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           g_glSSAOTexture[index], 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      eprintwarn("SSAO framebuffer is unavailable; disabling SSAO\n");
      glBindTexture(GL_TEXTURE_2D, 0);
      return 0;
    }
  }
  glBindTexture(GL_TEXTURE_2D, 0);
  g_ssaoFramebufferWidth = aoWidth;
  g_ssaoFramebufferHeight = aoHeight;
  g_ssaoFramebufferAvailable = 1;
  return 1;
}
#endif

static GLuint PsyX_GetNativeDrawFramebuffer() {
  return g_nativeFramebufferSamples > 1 ? g_glNativeMultisampleFramebuffer
                                        : g_glNativeFramebuffer;
}

static int PsyX_EnsureNativeFramebuffer() {
  const PsyXPresentationViewport nativeViewport = PsyX_GetRenderViewport();
  if (g_nativeFramebufferWidth == nativeViewport.w &&
      g_nativeFramebufferHeight == nativeViewport.h &&
      g_nativeFramebufferSamples == g_cfg_msaaSamples) {
    glBindFramebuffer(GL_FRAMEBUFFER, PsyX_GetNativeDrawFramebuffer());
    return 1;
  }

#if defined(RENDERER_OGLES) && OGLES_VERSION == 2
  const GLenum nativeColorFormat = GL_RGBA;
#else
  const GLenum nativeColorFormat = GL_RGBA8;
#endif
  glBindTexture(GL_TEXTURE_2D, g_glNativeColorTexture);
  glTexImage2D(GL_TEXTURE_2D, 0, nativeColorFormat, nativeViewport.w,
               nativeViewport.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glBindTexture(GL_TEXTURE_2D, 0);

  glBindFramebuffer(GL_FRAMEBUFFER, g_glNativeFramebuffer);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         g_glNativeColorTexture, 0);

#if defined(RENDERER_OGLES) && OGLES_VERSION == 2
  glBindRenderbuffer(GL_RENDERBUFFER, g_glNativeDepthRenderbuffer);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, nativeViewport.w,
                        nativeViewport.h);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                            GL_RENDERBUFFER, g_glNativeDepthRenderbuffer);

  glBindRenderbuffer(GL_RENDERBUFFER, g_glNativeStencilRenderbuffer);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, nativeViewport.w,
                        nativeViewport.h);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
                            GL_RENDERBUFFER, g_glNativeStencilRenderbuffer);
#else
  PsyX_AllocateNativeDepthTexture(g_nativeDepthInternalFormat, nativeViewport.w,
                                  nativeViewport.h);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    // Some older Windows OpenGL drivers expose 32F depth but reject it as a
    // framebuffer attachment. Preserve reversed-Z with their packed D24S8
    // path instead of aborting renderer initialization.
    g_nativeDepthInternalFormat = GL_DEPTH24_STENCIL8;
    PsyX_AllocateNativeDepthTexture(g_nativeDepthInternalFormat,
                                    nativeViewport.w, nativeViewport.h);
    eprintwarn("32F depth unavailable; using reversed D24S8\n");
  }
#endif

  glBindRenderbuffer(GL_RENDERBUFFER, 0);

  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    eprinterr("Failed to create native PSX framebuffer (%dx%d)\n",
              nativeViewport.w, nativeViewport.h);
    return 0;
  }

  g_nativeFramebufferSamples = 0;
#if USE_FRAMEBUFFER_BLIT
  if (g_cfg_msaaSamples > 1) {
    glBindRenderbuffer(GL_RENDERBUFFER, g_glNativeMultisampleColorRenderbuffer);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, g_cfg_msaaSamples,
                                     GL_RGBA8, nativeViewport.w,
                                     nativeViewport.h);

    glBindRenderbuffer(GL_RENDERBUFFER, g_glNativeMultisampleDepthRenderbuffer);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, g_cfg_msaaSamples,
                                     g_nativeDepthInternalFormat,
                                     nativeViewport.w, nativeViewport.h);

    glBindFramebuffer(GL_FRAMEBUFFER, g_glNativeMultisampleFramebuffer);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              GL_RENDERBUFFER,
                              g_glNativeMultisampleColorRenderbuffer);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER,
                              g_glNativeMultisampleDepthRenderbuffer);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE &&
        g_nativeDepthInternalFormat == GL_DEPTH32F_STENCIL8) {
      // Depth blits require compatible source and destination formats. If
      // multisampled D32FS8 is rejected, downgrade both attachments together.
      g_nativeDepthInternalFormat = GL_DEPTH24_STENCIL8;
      glBindFramebuffer(GL_FRAMEBUFFER, g_glNativeFramebuffer);
      PsyX_AllocateNativeDepthTexture(g_nativeDepthInternalFormat,
                                      nativeViewport.w, nativeViewport.h);
      glBindFramebuffer(GL_FRAMEBUFFER, g_glNativeMultisampleFramebuffer);
      glBindRenderbuffer(GL_RENDERBUFFER,
                         g_glNativeMultisampleDepthRenderbuffer);
      glRenderbufferStorageMultisample(GL_RENDERBUFFER, g_cfg_msaaSamples,
                                       GL_DEPTH24_STENCIL8, nativeViewport.w,
                                       nativeViewport.h);
      eprintwarn("32F MSAA depth unavailable; using reversed D24S8\n");
    }
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
      g_nativeFramebufferSamples = g_cfg_msaaSamples;
    } else {
      eprintwarn("MSAA framebuffer is unavailable; falling back to disabled\n");
      g_cfg_msaaSamples = 0;
    }
  }
#else
  g_cfg_msaaSamples = 0;
#endif

  g_nativeFramebufferWidth = nativeViewport.w;
  g_nativeFramebufferHeight = nativeViewport.h;
#if defined(RENDERER_OGL)
  PsyX_EnsureSSAOFramebuffers(nativeViewport.w, nativeViewport.h);
#endif
  eprintf("*Internal render target: %dx%d, MSAA: %dx\n",
          g_nativeFramebufferWidth, g_nativeFramebufferHeight,
          g_nativeFramebufferSamples);
  glBindRenderbuffer(GL_RENDERBUFFER, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, PsyX_GetNativeDrawFramebuffer());
  return 1;
}

static void PsyX_ResolveNativeFramebuffer() {
#if USE_FRAMEBUFFER_BLIT
  if (g_nativeFramebufferSamples <= 1)
    return;

  const int scissorEnabled = g_PreviousScissorState;
  glDisable(GL_SCISSOR_TEST);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, g_glNativeMultisampleFramebuffer);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_glNativeFramebuffer);
  glBlitFramebuffer(0, 0, g_nativeFramebufferWidth, g_nativeFramebufferHeight,
                    0, 0, g_nativeFramebufferWidth, g_nativeFramebufferHeight,
                    GL_COLOR_BUFFER_BIT, GL_NEAREST);
  if (scissorEnabled)
    glEnable(GL_SCISSOR_TEST);
#endif
}

static void PsyX_PresentNativeFramebuffer() {
  if (g_nativeFramebufferWidth <= 0 || g_nativeFramebufferHeight <= 0)
    return;

  const PsyXPresentationViewport viewport = PsyX_GetPresentationViewport();
  const int scissorEnabled = g_PreviousScissorState;
  glDisable(GL_SCISSOR_TEST);
  PsyX_ResolveNativeFramebuffer();

  glBindFramebuffer(GL_READ_FRAMEBUFFER, g_glNativeFramebuffer);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glBlitFramebuffer(0, 0, g_nativeFramebufferWidth, g_nativeFramebufferHeight,
                    viewport.x, viewport.y, viewport.x + viewport.w,
                    viewport.y + viewport.h, GL_COLOR_BUFFER_BIT, GL_NEAREST);

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  if (scissorEnabled)
    glEnable(GL_SCISSOR_TEST);
}

#endif

#if defined(RENDERER_OGL) || defined(RENDERER_OGLES)
int GR_InitialiseGLContext(char *windowName, int fullscreen) {
  int windowFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE;

#if defined(__ANDROID__)
  windowFlags |= SDL_WINDOW_FULLSCREEN;
#else
  if (fullscreen)
    windowFlags |= SDL_WINDOW_FULLSCREEN;
#endif

  if (g_windowWidth <= 0 || g_windowHeight <= 0)
    windowFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

  g_window = SDL_CreateWindow(windowName, SDL_WINDOWPOS_UNDEFINED,
                              SDL_WINDOWPOS_UNDEFINED, g_windowWidth,
                              g_windowHeight, windowFlags);

  if (g_window == NULL) {
    eprinterr("Failed to initialise SDL window!\n");
    return 0;
  }

#if defined(RENDERER_OGLES)

#if defined(__ANDROID__)
  // Override to full screen.
  SDL_DisplayMode displayMode;
  if (SDL_GetCurrentDisplayMode(0, &displayMode) == 0) {
    screenWidth = displayMode.w;
    windowWidth = displayMode.w;
    screenHeight = displayMode.h;
    windowHeight = displayMode.h;
  }
#endif

  // SDL_GL_SetAttribute(SDL_GL_CONTEXT_EGL, 1);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, OGLES_VERSION);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);

  if (!SDL_GL_CreateContext(g_window)) {
    eprinterr("Failed to initialise - OpenGL ES %d.x is not supported.\n",
              OGLES_VERSION);
    return 0;
  }

#elif defined(RENDERER_OGL)

  int major_version = 3;
  int minor_version = 3;
  int profile = SDL_GL_CONTEXT_PROFILE_CORE;

  // find best OpenGL version
  do {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major_version);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor_version);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, profile);

    if (SDL_GL_CreateContext(g_window))
      break;

    minor_version--;

  } while (minor_version >= 0);

  if (minor_version == -1) {
    eprinterr("Failed to initialise - OpenGL 3.x is not supported. Please "
              "update video drivers.\n");
    return 0;
  }
#endif

  return 1;
}
#endif

int GR_InitialiseGLExt() {
#ifdef USE_GLAD
  GLenum err = gladLoadGL();

  if (err == 0)
    return 0;
#endif

  const char *rend = (const char *)glGetString(GL_RENDERER);
  const char *vendor = (const char *)glGetString(GL_VENDOR);
  eprintf("*Video adapter: %s by %s\n", rend, vendor);

  const char *versionStr = (const char *)glGetString(GL_VERSION);
  eprintf("*OpenGL version: %s\n", versionStr);

  const char *glslVersionStr =
      (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
  eprintf("*GLSL version: %s\n", glslVersionStr);

  if (SDL_GL_ExtensionSupported("GL_EXT_texture_filter_anisotropic")) {
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &g_maxTextureAnisotropy);
    if (g_maxTextureAnisotropy < 1.0f)
      g_maxTextureAnisotropy = 1.0f;
  }
  eprintf("*Hardware anisotropy: %.0fx\n", g_maxTextureAnisotropy);

#if USE_FRAMEBUFFER_BLIT
  GLint maxSamples = 0;
  glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
  const int requestedSamples = g_cfg_msaaSamples;
  g_cfg_msaaSamples = 0;
  const int supportedSamples[] = {8, 4, 2};
  for (int samples : supportedSamples) {
    if (samples <= requestedSamples && samples <= maxSamples) {
      g_cfg_msaaSamples = samples;
      break;
    }
  }
  if (requestedSamples > 1 && g_cfg_msaaSamples != requestedSamples)
    eprintwarn("Requested %dx MSAA, using %dx (driver maximum: %d)\n",
               requestedSamples, g_cfg_msaaSamples, maxSamples);
#else
  g_cfg_msaaSamples = 0;
#endif
  eprintf("*MSAA samples: %d\n", g_cfg_msaaSamples);

  return 1;
}

int GR_InitialiseRender(char *windowName, int width, int height,
                        int fullscreen) {
  g_windowWidth = width;
  g_windowHeight = height;
  g_drawableWidth = width;
  g_drawableHeight = height;

  // Due to debugging in fullscreen
  SDL_SetHint(SDL_HINT_ALLOW_TOPMOST, "0");
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
#ifdef SDL_HINT_WINDOWS_DPI_AWARENESS
  SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitor");
#endif

#if USE_OPENGL
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 1);

#if defined(RENDERER_OGL) || defined(RENDERER_OGLES)
  if (!GR_InitialiseGLContext(windowName, fullscreen)) {
    eprinterr("Failed to Initialise GL Context!\n");
    return 0;
  }
#endif

  if (!GR_InitialiseGLExt()) {
    eprinterr("Failed to Intialise GL extensions\n");
    return 0;
  }
#endif

  return 1;
}

void GR_Shutdown() {
#if USE_OPENGL
  glDeleteVertexArrays(2, g_glVertexArray);
  glDeleteBuffers(2, g_glVertexBuffer);

  PBO_Destroy(&g_glFramebufferPBO);
  PBO_Destroy(&g_glOffscreenPBO);

  glDeleteFramebuffers(1, &g_glBlitFramebuffer);
  glDeleteFramebuffers(1, &g_glOffscreenFramebuffer);
  glDeleteFramebuffers(1, &g_glVRAMFramebuffer);
  glDeleteFramebuffers(1, &g_glNativeFramebuffer);
  glDeleteFramebuffers(1, &g_glNativeMultisampleFramebuffer);
  glDeleteRenderbuffers(1, &g_glNativeDepthRenderbuffer);
  glDeleteRenderbuffers(1, &g_glNativeStencilRenderbuffer);
  glDeleteRenderbuffers(1, &g_glNativeMultisampleColorRenderbuffer);
  glDeleteRenderbuffers(1, &g_glNativeMultisampleDepthRenderbuffer);
  glDeleteTextures(1, &g_glNativeColorTexture);
#if defined(RENDERER_OGL)
  glDeleteTextures(1, &g_glNativeDepthTexture);
  glDeleteFramebuffers(2, g_glSSAOFramebuffer);
  glDeleteTextures(2, g_glSSAOTexture);
  glDeleteVertexArrays(1, &g_glSSAOVertexArray);
  glDeleteProgram(g_ssaoShader);
  glDeleteProgram(g_ssaoBlurShader);
  glDeleteProgram(g_ssaoCompositeShader);
#endif

  GR_DestroyTexture(g_vramTexturesDouble[0]);
  GR_DestroyTexture(g_vramTexturesDouble[1]);
  GR_DestroyTexture(g_vramAliasTexture);

  GR_DestroyTexture(g_whiteTexture);
  GR_DestroyTexture(g_rgLutTexture);
  GR_DestroyTexture(g_fbTexture);
  GR_DestroyTexture(g_offscreenRTTexture);
#endif
}

void GR_UpdateSwapIntervalState(int swapInterval) {
#if defined(RENDERER_OGL)
  if (g_appliedSwapInterval == swapInterval)
    return;

  g_appliedSwapInterval = swapInterval;
  SDL_GL_SetSwapInterval(swapInterval);
#endif
}

void GR_BeginScene() {
  g_lastBoundTexture = 0;

#if USE_OPENGL
  // Query the high-DPI drawable exactly once per presented frame. Projection,
  // native framebuffer, offscreen and present paths all consume this cache;
  // repeatedly crossing SDL's window-system boundary was measurable at 240 Hz.
  PsyX_UpdateDrawableSize();
  PsyX_EnsureNativeFramebuffer();
  // glClear obeys the depth write mask. The previous frame normally ends in
  // the depth-free HUD pass, so restore writes before clearing world depth.
  glDisable(GL_SCISSOR_TEST);
  g_PreviousScissorState = 0;
  glDepthMask(GL_TRUE);
  g_PreviousDepthWrite = -1;
#ifdef RENDERER_OGLES
  glClearDepthf(0.0f);
#else
  glClearDepth(0.0f);
#endif
  glClear(GL_DEPTH_BUFFER_BIT);
  glClear(GL_STENCIL_BUFFER_BIT);
#endif

  GR_UpdateVRAM();
  const PsyXPresentationViewport viewport = PsyX_GetRenderViewport();
  GR_SetViewPort(0, 0, viewport.w, viewport.h);

  if (g_dbg_wireframeMode) {
    GR_SetWireframe(1);

#if USE_OPENGL
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
#endif
  }
}

void GR_EndScene() {
  // Finish a pending VRAM/offscreen pass before the native display image is
  // stored or presented. This also restores the native framebuffer binding.
  if (g_PreviousOffscreenState)
    GR_SetOffscreenState(&g_PreviousOffscreen, 0);

  framebuffer_need_update = 1;

  if (g_dbg_wireframeMode)
    GR_SetWireframe(0);

#if USE_OPENGL
  glBindVertexArray(0);
  g_boundVertexArray = 0;
#endif
}

//----------------------------------------------------------------------------------------

unsigned short vram[VRAM_WIDTH * VRAM_HEIGHT];
static unsigned short g_vramAliasPages[VRAM_ALIAS_WIDTH * VRAM_ALIAS_HEIGHT]{};

// A bounded CPU-side journal for diagnosing texture-page corruption. Every
// LoadImage/MoveImage/ClearImage/framebuffer transfer ultimately reaches one
// of the helpers below, so the scene streamer can report the exact writes
// which touched a page after it was made resident without logging every GPU
// command during normal play.
static constexpr size_t GR_VRAM_WRITE_JOURNAL_CAPACITY = 512;
static std::array<GrVRAMWriteEvent, GR_VRAM_WRITE_JOURNAL_CAPACITY>
    g_vramWriteJournal{};
static unsigned long long g_vramWriteSequence = 0;

static void GR_RecordVRAMWrite(int kind, int source_x, int source_y,
                               int destination_x, int destination_y, int width,
                               int height) {
  if (width <= 0 || height <= 0)
    return;
  const unsigned long long sequence = ++g_vramWriteSequence;
  g_vramWriteJournal[(sequence - 1) % GR_VRAM_WRITE_JOURNAL_CAPACITY] = {
      sequence,      kind,          source_x, source_y,
      destination_x, destination_y, width,    height};
}

unsigned long long GR_GetVRAMWriteSequence() { return g_vramWriteSequence; }

int GR_ReadVRAMWriteEvents(unsigned long long after_sequence,
                           GrVRAMWriteEvent *events, int capacity) {
  if (events == NULL || capacity <= 0)
    return 0;
  const unsigned long long current = g_vramWriteSequence;
  const unsigned long long oldest =
      current > GR_VRAM_WRITE_JOURNAL_CAPACITY
          ? current - GR_VRAM_WRITE_JOURNAL_CAPACITY
          : 0;
  unsigned long long sequence = std::max(after_sequence, oldest) + 1;
  int count = 0;
  while (sequence <= current && count < capacity) {
    events[count++] =
        g_vramWriteJournal[(sequence - 1) % GR_VRAM_WRITE_JOURNAL_CAPACITY];
    ++sequence;
  }
  return count;
}
static u_char rgLUT[LUT_WIDTH * LUT_HEIGHT * sizeof(u_int)];

void GR_ResetDevice() {
  g_appliedSwapInterval = -1000;
  GR_UpdateSwapIntervalState(0);
}

typedef struct {
  // shader itself
  ShaderID shader;

#if USE_OPENGL
  GLint projectionLoc;
  GLint projection3DLoc;
  GLint presentationScaleLoc;
  GLint textureFilterModeLoc;
  GLint textureBlendModeLoc;
  GLint texelSizeLoc;
  GLint texLoc;
  GLint lutLoc;
  GLint aliasLoc;
  GLint appliedTextureFilterMode;
#endif
} PSXGPU_Shader;

PSXGPU_Shader g_gpu_shader_4;
PSXGPU_Shader g_gpu_shader_8;
PSXGPU_Shader g_gpu_shader_16;
PSXGPU_Shader g_gpu_shader_32_rgba;

ShaderID g_PreviousTextureBlendShader = -1;
int g_PreviousTextureBlendMode = -1;

typedef struct {
  TextureID texture;
  GLint filterMode;
} GrTextureFilterCacheEntry;

static std::vector<GrTextureFilterCacheEntry> g_rgbaTextureFilterCache;

#if USE_OPENGL

GLint u_projectionLoc;
GLint u_projection3DLoc;
GLint u_presentationScaleLoc;
GLint u_texelSizeLoc;
GLint u_textureBlendModeLoc;

#define GPU_SAMPLE_TEXTURE_4BIT_FUNC                                           \
  "   // returns 16 bit colour\n"                                              \
  "   vec2 samplePSX(vec2 tc) {\n"                                             \
  "       vec2 texel = tc * vec2(0.25, 1.0) + v_page_clut.xy;\n"               \
  "       vec2 comp = PAGE(texel);\n"                                          \
  "       int index = int(fract(tc.x / 4.0 + 0.0001) * 4.0);\n"                \
  "       float v = _idx2(comp, index / 2) * (255.0 / 16.0);\n"                \
  "       float f = floor(v + 0.001);\n"                                       \
  "       vec2 c = vec2( (v - f) * 16.0, f );\n"                               \
  "       vec2 clut_pos = v_page_clut.zw;\n"                                   \
  "       clut_pos.x += mix(c[0], c[1], mod(float(index), 2.0)) * "            \
  "c_VRAMTexel.x;\n"                                                           \
  "       return VRAM(clut_pos);\n"                                            \
  "   }\n"

#define GPU_SAMPLE_TEXTURE_8BIT_FUNC                                           \
  "	// returns 16 bit colour\n"                                                \
  "	vec2 samplePSX(vec2 tc) {\n"                                               \
  "		vec2 texel = tc * vec2(0.5, 1.0) + v_page_clut.xy;\n"                     \
  "		vec2 comp = PAGE(texel);\n"                                               \
  "		vec2 clut_pos = v_page_clut.zw;\n"                                        \
  "		int index = int(mod(tc.x, 2.0));\n"                                       \
  "		clut_pos.x += _idx2(comp, index) * 255.0 * c_VRAMTexel.x;\n"              \
  "		vec2 color_rg = VRAM(clut_pos);\n"                                        \
  "		return VRAM(clut_pos);\n"                                                 \
  "	}\n"

#define GPU_SAMPLE_TEXTURE_16BIT_FUNC                                          \
  "	vec2 samplePSX(vec2 tc) {\n"                                               \
  "		vec2 texel = tc + v_page_clut.xy;\n"                                      \
  "		vec2 color_rg = PAGE(texel);\n"                                           \
  "		return color_rg;\n"                                                       \
  "	}\n"

#if (VRAM_FORMAT == GL_LUMINANCE_ALPHA)

#define GPU_FETCH_VRAM_FUNC                                                    \
  "	const vec2 c_VRAMTexel = vec2(1.0 / 1024.0, 1.0 / 512.0);\n"               \
  "	const vec2 c_AliasTexel = vec2(1.0 / 1024.0, 1.0 / 1024.0);\n"             \
  "	uniform sampler2D s_texture;\n"                                            \
  "	uniform sampler2D s_aliasTexture;\n"                                       \
  "	vec2 VRAM(vec2 uv) { return texture2D(s_texture, uv).ra; }\n"              \
  "	vec2 PAGE(vec2 texel) { return v_alias_page > 0.5 ? "                      \
  "texture2D(s_aliasTexture, texel * c_AliasTexel).ra : VRAM(texel * "         \
  "c_VRAMTexel); }\n"
#else

#define GPU_FETCH_VRAM_FUNC                                                    \
  "	const vec2 c_VRAMTexel = vec2(1.0 / 1024.0, 1.0 / 512.0);\n"               \
  "	const vec2 c_AliasTexel = vec2(1.0 / 1024.0, 1.0 / 1024.0);\n"             \
  "	uniform sampler2D s_texture;\n"                                            \
  "	uniform sampler2D s_aliasTexture;\n"                                       \
  "	vec2 VRAM(vec2 uv) { return texture2D(s_texture, uv).rg; }\n"              \
  "	vec2 PAGE(vec2 texel) { return v_alias_page > 0.5 ? "                      \
  "texture2D(s_aliasTexture, texel * c_AliasTexel).rg : VRAM(texel * "         \
  "c_VRAMTexel); }\n"
#endif

#if defined(RENDERER_OGL) || (OGLES_VERSION == 3)

#define GPU_DITHERING "	vec4 dither(vec4 color) { return color; }\n"

#define GPU_ARRAY_FUNC                                                         \
  "	float _idx2(vec2 array, int idx) { return array[idx]; }\n"

#else

#define GPU_DITHERING "	vec4 dither(vec4 color) { return color; }\n"

#define GPU_ARRAY_FUNC                                                         \
  "	float _idx2(vec2 array, int idx) { return idx == 0 ? array.x : "           \
  "array.y; }\n"

#endif

#if defined(RENDERER_OGL) || (OGLES_VERSION == 3)

#define GPU_MINIFICATION_FILTER                                                \
  "\tvoid accumulateTileTap(vec2 P, float weight, inout vec4 premultiplied, "  \
  "inout float coverageSum, inout float weightSum) {\n"                        \
  "\t\tfloat tapCoverage;\n"                                                   \
  "\t\tvec4 tapColor = bilinearTextureSample(boundedPSX(P), tapCoverage);\n"   \
  "\t\tpremultiplied += tapColor * tapCoverage * weight;\n"                    \
  "\t\tcoverageSum += tapCoverage * weight;\n"                                 \
  "\t\tweightSum += weight;\n"                                                 \
  "\t}\n"                                                                      \
  "\tvec4 edgeSafeBilinearTextureSample(vec2 P, out float coverage) {\n"       \
  "\t\treturn bilinearTextureSample(boundedPSX(P), coverage);\n"               \
  "\t}\n"                                                                      \
  "\tvec4 anisotropicTextureSample(vec2 P, out float coverage) {\n"            \
  "\t\tvec2 boundedP = boundedPSX(P);\n"                                       \
  "\t\tfloat centerCoverage;\n"                                                \
  "\t\tvec4 centerColor = bilinearTextureSample(boundedP, centerCoverage);\n"  \
  "\t\tvec2 axisX = dFdx(P);\n"                                                \
  "\t\tvec2 axisY = dFdy(P);\n"                                                \
  "\t\tfloat covarianceA = axisX.x * axisX.x + axisY.x * axisY.x;\n"           \
  "\t\tfloat covarianceB = axisX.x * axisX.y + axisY.x * axisY.y;\n"           \
  "\t\tfloat covarianceC = axisX.y * axisX.y + axisY.y * axisY.y;\n"           \
  "\t\tfloat discriminant = sqrt(max((covarianceA - covarianceC) * "           \
  "(covarianceA - covarianceC) + 4.0 * covarianceB * covarianceB, 0.0));\n"    \
  "\t\tfloat majorSquared = max(0.5 * (covarianceA + covarianceC + "           \
  "discriminant), 0.0);\n"                                                     \
  "\t\tfloat majorLength = sqrt(majorSquared);\n"                              \
  "\t\tfloat filterBlend = smoothstep(0.75, 1.25, majorLength);\n"             \
  "\t\tif (filterBlend <= 0.0) {\n"                                            \
  "\t\t\tcoverage = centerCoverage;\n"                                         \
  "\t\t\treturn centerColor;\n"                                                \
  "\t\t}\n"                                                                    \
  "\t\tfloat footprintScale = min(1.0, 16.0 / max(majorLength, 0.0001));\n"    \
  "\t\tvec2 footprintX = axisX * footprintScale;\n"                            \
  "\t\tvec2 footprintY = axisY * footprintScale;\n"                            \
  "\t\tvec4 areaPremultiplied = vec4(0.0);\n"                                  \
  "\t\tfloat areaCoverageSum = 0.0;\n"                                         \
  "\t\tfloat areaWeightSum = 0.0;\n"                                           \
  "\t\tfor (int tap = 0; tap < 16; ++tap) {\n"                                 \
  "\t\t\tfloat index = float(tap);\n"                                          \
  "\t\t\tfloat offsetX = (index + 0.5) * (1.0 / 16.0) - 0.5;\n"                \
  "\t\t\tfloat offsetY = mod(index, 2.0) * 0.5 + mod(floor(index * 0.5), "     \
  "2.0) * 0.25 + mod(floor(index * 0.25), 2.0) * 0.125 + mod(floor(index * "   \
  "0.125), 2.0) * 0.0625 + (1.0 / 32.0) - 0.5;\n"                              \
  "\t\t\tvec2 offset = vec2(offsetX, offsetY);\n"                              \
  "\t\t\tfloat weight = exp2(-4.0 * dot(offset, offset));\n"                   \
  "\t\t\taccumulateTileTap(P + footprintX * offset.x + footprintY * "          \
  "offset.y, weight, areaPremultiplied, areaCoverageSum, areaWeightSum);\n"    \
  "\t\t}\n"                                                                    \
  "\t\tfloat filteredCoverage = areaCoverageSum / areaWeightSum;\n"            \
  "\t\tvec4 filteredPremultiplied = areaPremultiplied / areaWeightSum;\n"      \
  "\t\tcoverage = mix(centerCoverage, filteredCoverage, filterBlend);\n"       \
  "\t\tvec4 premultiplied = mix(centerColor * centerCoverage, "                \
  "filteredPremultiplied, filterBlend);\n"                                     \
  "\t\treturn premultiplied / max(coverage, 0.0001);\n"                        \
  "\t}\n"

#else

#define GPU_MINIFICATION_FILTER                                                \
  "\tvec4 edgeSafeBilinearTextureSample(vec2 P, out float coverage) {\n"       \
  "\t\treturn bilinearTextureSample(boundedPSX(P), coverage);\n"               \
  "\t}\n"                                                                      \
  "\tvec4 anisotropicTextureSample(vec2 P, out float coverage) {\n"            \
  "\t\treturn edgeSafeBilinearTextureSample(P, coverage);\n"                   \
  "\t}\n"

#endif

#define GPU_FRAGMENT_SAMPLE_SHADER(bit)                                        \
  GPU_FETCH_VRAM_FUNC                                                          \
  GPU_ARRAY_FUNC                                                               \
  GPU_SAMPLE_TEXTURE_##bit##BIT_FUNC                                           \
      "	uniform sampler2D s_rgLut;\n"                                          \
      "	const vec2 c_LUTTexel = vec2(1.0 / 256.0, 1.0 / 256.0);\n"             \
      "	vec4 lut(vec2 rg) { return texture2D(s_rgLut, rg - c_LUTTexel * "      \
      "0.0001); }\n"                                                           \
      "	vec2 boundedPSX(vec2 P) { return clamp(P, v_texbounds.xy, "            \
      "v_texbounds.zw); }\n"                                                   \
      "	vec4 bilinearTextureSample(vec2 P, out float coverage) {\n"            \
      "		// Clamp the coordinate and every footprint tap to this "             \
      "primitive's\n"                                                          \
      "		// inclusive atlas tile. This is clamp-to-edge in texel "             \
      "space, so\n"                                                            \
      "		// filtering cannot pull colours from a neighbouring atlas "          \
      "tile.\n"                                                                \
      "		vec2 safeP = boundedPSX(P);\n"                                        \
      "		vec2 pixel = floor(safeP);\n"                                         \
      "		vec2 frac = safeP - pixel;\n"                                         \
      "		vec2 C11 = samplePSX(boundedPSX(pixel));\n"                           \
      "		vec2 C21 = samplePSX(boundedPSX(pixel + vec2(1.0, 0.0)));\n"          \
      "		vec2 C12 = samplePSX(boundedPSX(pixel + vec2(0.0, 1.0)));\n"          \
      "		vec2 C22 = samplePSX(boundedPSX(pixel + vec2(1.0, 1.0)));\n"          \
      "		vec4 weights = vec4((1.0 - frac.x) * (1.0 - frac.y), frac.x "         \
      "* (1.0 - frac.y), (1.0 - frac.x) * frac.y, frac.x * frac.y);\n"         \
      "		vec4 solid = vec4(float(C11.r + C11.g > 0.0), float(C21.r + "         \
      "C21.g > 0.0), float(C12.r + C12.g > 0.0), float(C22.r + C22.g > "       \
      "0.0));\n"                                                               \
      "		coverage = dot(weights, solid);\n"                                    \
      "		vec4 t = (lut(C11) * weights.x * solid.x + lut(C21) * "               \
      "weights.y * solid.y + lut(C12) * weights.z * solid.z + lut(C22) * "     \
      "weights.w * solid.w) / max(coverage, 0.0001);\n"                        \
      "		t.w = 1.0 - t.w;\n"                                                   \
      "		return t;\n"                                                          \
      "	}\n"                                                                   \
      "	vec4 pointTextureSample(vec2 P, out float coverage) {\n"               \
      "		vec2 rg = samplePSX(floor(boundedPSX(P) + vec2(0.5)));\n"             \
      "		coverage = float(rg.r + rg.g > 0.0);\n"                               \
      "		vec4 t = lut(rg);\n"                                                  \
      "		t.w = 1.0 - t.w;\n"                                                   \
      "		return t;\n"                                                          \
      "	}\n" GPU_MINIFICATION_FILTER "	vec4 nearestTextureSample(vec2 P) {\n"  \
      "		vec2 rg = samplePSX(boundedPSX(P));\n"                                \
      "		float rgm = rg.x + rg.y;\n"                                           \
      "		if (rgm == 0.0) { discard; }\n"                                       \
      "		vec4 t = lut(rg);\n"                                                  \
      "		t.w = 1.0 - t.w;\n"                                                   \
      "		return t;\n"                                                          \
      "	}\n"                                                                   \
      "	uniform int textureFilterMode;\n"                                      \
      "	uniform int textureBlendMode;\n"                                       \
      "	void main() {\n"                                                       \
      "		float coverage = 1.0;\n"                                              \
      "		vec4 color;\n"                                                        \
      "		if (textureFilterMode > 0) {\n"                                       \
      "			color = textureFilterMode > 1\n"                                     \
      "				? anisotropicTextureSample(v_texcoord.xy, "                         \
      "coverage)\n"                                                            \
      "				: "                                                                 \
      "edgeSafeBilinearTextureSample(v_texcoord.xy, coverage);\n"              \
      "			float sourceCoverage;\n"                                             \
      "			pointTextureSample(v_texcoord.xy, sourceCoverage);\n"                \
      "			if (textureBlendMode == 0) {\n"                                      \
      "				if (sourceCoverage < 0.5) { discard; }\n"                           \
      "				coverage = 1.0;\n"                                                  \
      "			} else {\n"                                                          \
      "				coverage = smoothstep(0.015, 0.98, "                                \
      "coverage);\n"                                                           \
      "				if (coverage <= 0.0) { discard; }\n"                                \
      "			}\n"                                                                 \
      "		} else {\n"                                                           \
      "			color = nearestTextureSample(v_texcoord.xy);\n"                      \
      "		}\n"                                                                  \
      "		vec4 shaded = dither(color * v_color);\n"                             \
      "		if (textureFilterMode > 0 && textureBlendMode > 0) {\n"               \
      "			shaded.a *= coverage;\n"                                             \
      "			if (textureBlendMode != 1) { shaded.rgb *= coverage; "               \
      "}\n"                                                                    \
      "		}\n"                                                                  \
      "		fragColor = shaded;\n"                                                \
      "	}\n"

static const char *gpu_shader_common = R"(
	varying vec4 v_texcoord;
	varying vec4 v_color;
	FLAT varying vec4 v_page_clut;
	FLAT varying float v_alias_page;
	FLAT varying vec4 v_texbounds;
	varying float v_z;
)";

const char *gpu_shader_4 = GPU_FRAGMENT_SAMPLE_SHADER(4);
const char *gpu_shader_8 = GPU_FRAGMENT_SAMPLE_SHADER(8);
const char *gpu_shader_16 = GPU_FRAGMENT_SAMPLE_SHADER(16);
const char *gpu_shader_32_rgba =
    "	uniform sampler2D s_texture;\n"
    "	uniform vec2 texelSize;\n"
    "	void main() {\n"
    "		vec2 tc = v_texcoord.xy * texelSize + texelSize * 0.5;\n"
    "		vec4 color = texture2D(s_texture, tc);\n"
    "		if (color.a <= 0.0) discard;\n"
    "		fragColor = dither(color * v_color);\n"
    "	}\n";

#if USE_PGXP
#define GTE_PERSPECTIVE_CORRECTION                                             \
  "	if (a_zw.y > 0.0) {\n"                                                     \
  "		vec4 depthPosition = Projection3D * vec4(0.0, 0.0, a_zw.x, "              \
  "1.0);\n"                                                                    \
  "		vec2 ndc = a_position.xy * vec2(2.0, -2.0);\n"                            \
  "		gl_Position = vec4(ndc * depthPosition.w, depthPosition.z, "              \
  "depthPosition.w);\n"                                                        \
  "	} else {\n"                                                                \
  "		gl_Position = Projection * vec4(a_position.xy, 0.5, 1.0);\n"              \
  "	}\n"
#else
#define GTE_PERSPECTIVE_CORRECTION                                             \
  "	gl_Position = Projection * vec4(a_position.xy, 0.0, 1.0);\n"
#endif

#define GTE_VERTEX_SHADER                                                      \
  "	attribute vec2 a_position;\n"                                              \
  "	attribute vec2 a_page_clut; // unsigned host TPAGE and CLUT\n"             \
  "	attribute vec4 a_texcoord; // uv, color multiplier, dither\n"              \
  "	attribute vec2 a_precise_uv;\n"                                            \
  "	attribute vec4 a_color;\n"                                                 \
  "	attribute vec4 a_extra; // texcoord.xy ofs, presentation flag, "           \
  "precise-UV flag\n"                                                          \
  "	attribute vec4 a_texbounds; // inclusive primitive UV bounds\n"            \
  "	attribute vec4 a_zw;\n"                                                    \
  "	uniform mat4 Projection;\n"                                                \
  "	uniform mat4 Projection3D;\n"                                              \
  "	uniform vec2 PresentationScale;\n"                                         \
  "	const vec2 c_UVFudge = vec2(0.00025, 0.00025);\n"                          \
  "	void main() {\n"                                                           \
  "		v_texcoord = a_texcoord;\n"                                               \
  "		v_texcoord.xy = a_precise_uv;\n"                                          \
  "		v_texcoord.xy += a_extra.xy * 0.5;\n"                                     \
  "		v_texbounds = a_texbounds;\n"                                             \
  "		v_color = a_color;\n"                                                     \
  "		v_color.xyz *= a_texcoord.z;\n"                                           \
  "		v_alias_page = floor(a_page_clut.x / 1024.0);\n"                          \
  "		float nativePage = mod(a_page_clut.x, 32.0);\n"                           \
  "		float aliasIndex = max(v_alias_page - 1.0, 0.0);\n"                       \
  "		v_page_clut.x = mix(mod(nativePage, 16.0) * 64.0, "                       \
  "mod(aliasIndex, 16.0) * 64.0, step(0.5, v_alias_page));\n"                  \
  "		v_page_clut.y = mix(floor(nativePage / 16.0) * 256.0, "                   \
  "floor(aliasIndex / 16.0) * 256.0, step(0.5, v_alias_page));\n"              \
  "		v_page_clut.z = fract(a_page_clut.y / 64.0);\n"                           \
  "		v_page_clut.w = floor(a_page_clut.y / 64.0) / 512.0;\n"                   \
  "		v_page_clut.xy += c_UVFudge;\n"                                           \
  "		v_page_clut.zw += c_UVFudge;\n" GTE_PERSPECTIVE_CORRECTION     \
  "		gl_Position.xy *= mix(PresentationScale, vec2(1.0), "                     \
  "clamp(a_extra.z, 0.0, 1.0));\n"                                             \
  "		v_z = (gl_Position.z - 40.0) * 0.005;\n"                                  \
  "	}\n"

int GR_Shader_CheckShaderStatus(GLuint shader) {
  char info[1024];
  GLint result;

  glGetShaderiv(shader, GL_COMPILE_STATUS, &result);

  if (result == GL_TRUE)
    return 1;

  glGetShaderInfoLog(shader, sizeof(info), NULL, info);
  if (info[0] && strlen(info) > 8) {
    eprinterr("%s\n", info);
    assert(0);
  }

  return 0;
}

int GR_Shader_CheckProgramStatus(GLuint program) {
  char info[1024];
  GLint result;

  glGetProgramiv(program, GL_LINK_STATUS, &result);

  if (result == GL_TRUE)
    return 1;

  glGetProgramInfoLog(program, sizeof(info), NULL, info);
  if (info[0] && strlen(info) > 8) {
    eprinterr("%s\n", info);
    assert(0);
  }

  return 0;
}

ShaderID GR_Shader_Compile(const char *source, int isPsxShader) {
#if defined(ES2_SHADERS)
  const char *GLSL_HEADER_VERT = R"(
		#version 100
		precision lowp  int;
		precision highp float;
		#define FLAT
	)";

  const char *GLSL_HEADER_FRAG = R"(
		#version 100
		precision lowp  int;
		precision highp float;
		#define FLAT
		#define fragColor gl_FragColor
	)";
#elif defined(ES3_SHADERS)
  const char *GLSL_HEADER_VERT = R"(
		#version 300 es
		precision lowp  int;
		precision highp float;
		#define FLAT flat
		#define varying   out
		#define attribute in
		#define texture2D texture
	)";

  const char *GLSL_HEADER_FRAG = R"(
		#version 300 es
		precision lowp  int;
		precision highp float;
		#define FLAT flat
		#define varying     in
		#define texture2D   texture
		out vec4 fragColor;
	)";
#else
  const char *GLSL_HEADER_VERT = R"(
		#version 140
		precision lowp  int;
		precision highp float;
		#define FLAT flat
		#define varying   out
		#define attribute in
		#define texture2D texture
	)";

  const char *GLSL_HEADER_FRAG = R"(
		#version 140
		precision lowp  int;
		precision highp float;
		#define FLAT flat
		#define varying     in
		#define texture2D   texture
		out vec4 fragColor;
	)";
#endif

  char extra_vs_defines[1024];
  char extra_fs_defines[1024];
  extra_vs_defines[0] = 0;
  extra_fs_defines[0] = 0;

  strcat(extra_vs_defines, "#define VERTEX\n");
  strcat(extra_fs_defines, "#define FRAGMENT\n");
  if (g_cfg_bilinearFiltering || g_cfg_anisotropicFiltering) {
    strcat(extra_fs_defines, "#define BILINEAR_FILTER\n");
  }

  const char *vs_list_psx[] = {GLSL_HEADER_VERT, extra_vs_defines,
                               gpu_shader_common, GTE_VERTEX_SHADER};
  const char *fs_list_psx[] = {GLSL_HEADER_FRAG, extra_fs_defines,
                               gpu_shader_common, GPU_DITHERING, source};

  const char *vs_list_src[] = {
      GLSL_HEADER_VERT,
      extra_vs_defines,
      source,
  };
  const char *fs_list_src[] = {GLSL_HEADER_FRAG, extra_fs_defines, source};

  const char **vs_list = isPsxShader ? vs_list_psx : vs_list_src;
  const char **fs_list = isPsxShader ? fs_list_psx : fs_list_src;
  const int vs_list_cnt = isPsxShader ? 4 : 3;
  const int fs_list_cnt = isPsxShader ? 5 : 3;

  GLuint program = glCreateProgram();

  {
    GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertexShader, vs_list_cnt, vs_list, NULL);
    glCompileShader(vertexShader);

    if (GR_Shader_CheckShaderStatus(vertexShader) == 0)
      eprinterr("Failed to compile Vertex Shader!\n");

    glAttachShader(program, vertexShader);
    glDeleteShader(vertexShader);
  }

  {
    GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragmentShader, fs_list_cnt, fs_list, NULL);
    glCompileShader(fragmentShader);

    if (GR_Shader_CheckShaderStatus(fragmentShader) == 0)
      eprinterr("Failed to compile Fragment Shader!\n");

    glAttachShader(program, fragmentShader);
    glDeleteShader(fragmentShader);
  }

  glBindAttribLocation(program, a_position, "a_position");
  glBindAttribLocation(program, a_page_clut, "a_page_clut");
  glBindAttribLocation(program, a_texcoord, "a_texcoord");
  glBindAttribLocation(program, a_color, "a_color");
  glBindAttribLocation(program, a_extra, "a_extra");
  glBindAttribLocation(program, a_texbounds, "a_texbounds");
  glBindAttribLocation(program, a_precise_uv, "a_precise_uv");

#if USE_PGXP
  glBindAttribLocation(program, a_zw, "a_zw");
#endif

  glLinkProgram(program);
  if (GR_Shader_CheckProgramStatus(program) == 0)
    eprinterr("Failed to link Shader!\n");

  GLint sampler = 0;
  glUseProgram(program);
  glUniform1iv(glGetUniformLocation(program, "s_rgLut"), 1, &sampler);
  glUniform1iv(glGetUniformLocation(program, "s_texture"), 1, &sampler);
  glUseProgram(0);

  return program;
}
#else
#error
#endif

//--------------------------------------------------------------------------------------------

void GR_GenerateCommonTextures() {
  unsigned int whitePixelData = 0xFFFFFFFF;

#if USE_OPENGL
  glGenTextures(1, &g_whiteTexture);
  {
    glBindTexture(GL_TEXTURE_2D, g_whiteTexture);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 &whitePixelData);

    glBindTexture(GL_TEXTURE_2D, 0);
  }

  glGenTextures(1, &g_rgLutTexture);
  {
    glBindTexture(GL_TEXTURE_2D, g_rgLutTexture);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, LUT_WIDTH, LUT_HEIGHT, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, &rgLUT);

    glBindTexture(GL_TEXTURE_2D, 0);
  }
#endif
}

TextureID GR_CreateRGBATexture(int width, int height,
                               u_char *data /*= nullptr*/) {
  TextureID newTexture;
  glGenTextures(1, &newTexture);

  glBindTexture(GL_TEXTURE_2D, newTexture);
  const int filtered = g_cfg_bilinearFiltering || g_cfg_anisotropicFiltering;
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                  filtered ? GL_LINEAR : GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                  filtered ? GL_LINEAR : GL_NEAREST);

  // another WebGL stuff. Texture will be black without clamp to edge
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  if (g_maxTextureAnisotropy > 1.0f)
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT,
                    g_cfg_anisotropicFiltering ? g_maxTextureAnisotropy : 1.0f);

  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, data);
  glBindTexture(GL_TEXTURE_2D, 0);

  return newTexture;
}

void GR_UpdateRGBATexture(TextureID texture, int width, int height,
                          const u_char *data) {
  if (texture == 0 || width <= 0 || height <= 0 || data == nullptr)
    return;

#if USE_OPENGL
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA,
                  GL_UNSIGNED_BYTE, data);
  glBindTexture(GL_TEXTURE_2D, 0);
  g_lastBoundTexture = 0;
#else
#error
#endif
}

void GR_CompilePSXShader(PSXGPU_Shader *sh, const char *source) {
  sh->shader = GR_Shader_Compile(source, true);

#if USE_OPENGL
  sh->textureFilterModeLoc =
      glGetUniformLocation(sh->shader, "textureFilterMode");
  sh->textureBlendModeLoc =
      glGetUniformLocation(sh->shader, "textureBlendMode");
  sh->projectionLoc = glGetUniformLocation(sh->shader, "Projection");
  sh->presentationScaleLoc =
      glGetUniformLocation(sh->shader, "PresentationScale");
  sh->texelSizeLoc = glGetUniformLocation(sh->shader, "texelSize");
  sh->texLoc = glGetUniformLocation(sh->shader, "s_texture");
  sh->lutLoc = glGetUniformLocation(sh->shader, "s_rgLut");
  sh->aliasLoc = glGetUniformLocation(sh->shader, "s_aliasTexture");
  sh->appliedTextureFilterMode = -1;
#if USE_PGXP
  sh->projection3DLoc = glGetUniformLocation(sh->shader, "Projection3D");
#endif
  glUseProgram(sh->shader);
  if (sh->texLoc != -1)
    glUniform1i(sh->texLoc, 0);
  if (sh->lutLoc != -1)
    glUniform1i(sh->lutLoc, 1);
  if (sh->aliasLoc != -1)
    glUniform1i(sh->aliasLoc, 2);
#endif
}

void GR_InitialisePSXShaders() {
  GR_CompilePSXShader(&g_gpu_shader_4, gpu_shader_4);
  GR_CompilePSXShader(&g_gpu_shader_8, gpu_shader_8);
  GR_CompilePSXShader(&g_gpu_shader_16, gpu_shader_16);
  GR_CompilePSXShader(&g_gpu_shader_32_rgba, gpu_shader_32_rgba);
#if USE_OPENGL
  glUseProgram(0);
  g_PreviousShader = -1;
#endif
}

#if defined(RENDERER_OGL)
static const char *g_ssaoShaderSource = R"(
#ifdef VERTEX
varying vec2 v_uv;
void main()
{
	vec2 position = vec2(gl_VertexID == 1 ? 3.0 : -1.0,
		gl_VertexID == 2 ? 3.0 : -1.0);
	v_uv = position * 0.5 + 0.5;
	gl_Position = vec4(position, 0.0, 1.0);
}
#endif
#ifdef FRAGMENT
varying vec2 v_uv;
uniform sampler2D s_depth;
uniform vec2 u_projectionScale;
uniform vec2 u_nearFar;

float linearDepth(float depth)
{
	return (u_nearFar.x * u_nearFar.y) /
		(u_nearFar.x + depth * (u_nearFar.y - u_nearFar.x));
}

vec3 viewPosition(vec2 uv, float depth)
{
	float z = linearDepth(depth);
	return vec3((uv * 2.0 - 1.0) * z / u_projectionScale, z);
}

float hash12(vec2 value)
{
	return fract(sin(dot(value, vec2(12.9898, 78.233))) * 43758.5453);
}

void main()
{
	float centerDepth = texture2D(s_depth, v_uv).r;
	if (centerDepth <= 0.000001)
	{
		fragColor = vec4(1.0);
		return;
	}

	vec3 center = viewPosition(v_uv, centerDepth);
	vec3 normal = normalize(cross(dFdx(center), dFdy(center)));
	if (dot(normal, -center) < 0.0)
		normal = -normal;
	vec3 helper = abs(normal.z) < 0.85 ? vec3(0.0, 0.0, 1.0)
		: vec3(0.0, 1.0, 0.0);
	vec3 tangent = normalize(cross(helper, normal));
	vec3 bitangent = cross(normal, tangent);
	float rotation = hash12(gl_FragCoord.xy) * 6.28318530718;
	float radius = clamp(center.z * 0.018, 0.45, 7.0);
	float bias = max(radius * 0.035, 0.025);
	float occlusion = 0.0;
	for (int sampleIndex = 0; sampleIndex < 8; ++sampleIndex)
	{
		float sequence = (float(sampleIndex) + 0.5) / 8.0;
		float angle = float(sampleIndex) * 2.39996323 + rotation;
		float diskRadius = sqrt(sequence);
		vec3 hemisphere = vec3(cos(angle) * diskRadius,
			sin(angle) * diskRadius, sqrt(max(1.0 - sequence, 0.0)));
		vec3 direction = tangent * hemisphere.x + bitangent * hemisphere.y +
			normal * hemisphere.z;
		vec3 probe = center + direction * radius;
		if (probe.z <= u_nearFar.x)
			continue;
		vec2 sampleUv = probe.xy * u_projectionScale / probe.z * 0.5 + 0.5;
		if (any(lessThan(sampleUv, vec2(0.0))) ||
			any(greaterThan(sampleUv, vec2(1.0))))
			continue;
		float sampleDepth = texture2D(s_depth, sampleUv).r;
		if (sampleDepth <= 0.000001)
			continue;
		float sampleZ = linearDepth(sampleDepth);
		float rangeWeight = 1.0 - smoothstep(radius, radius * 2.5,
			abs(sampleZ - center.z));
		occlusion += (sampleZ < probe.z - bias ? 1.0 : 0.0) * rangeWeight;
	}
	float ao = clamp(1.0 - occlusion * (0.68 / 8.0), 0.58, 1.0);
	fragColor = vec4(ao, ao, ao, 1.0);
}
#endif
)";

static const char *g_ssaoBlurShaderSource = R"(
#ifdef VERTEX
varying vec2 v_uv;
void main()
{
	vec2 position = vec2(gl_VertexID == 1 ? 3.0 : -1.0,
		gl_VertexID == 2 ? 3.0 : -1.0);
	v_uv = position * 0.5 + 0.5;
	gl_Position = vec4(position, 0.0, 1.0);
}
#endif
#ifdef FRAGMENT
varying vec2 v_uv;
uniform sampler2D s_ao;
uniform sampler2D s_depth;
uniform vec2 u_invResolution;
uniform vec2 u_nearFar;

float linearDepth(float depth)
{
	return (u_nearFar.x * u_nearFar.y) /
		(u_nearFar.x + depth * (u_nearFar.y - u_nearFar.x));
}

void main()
{
	float centerDepth = texture2D(s_depth, v_uv).r;
	float centerZ = linearDepth(centerDepth);
	float depthTolerance = max(0.12, centerZ * 0.012);
	float total = 0.0;
	float weightTotal = 0.0;
	for (int tap = 0; tap < 5; ++tap)
	{
		vec2 direction = tap == 0 ? vec2(0.0) :
			(tap == 1 ? vec2(1.0, 0.0) :
			(tap == 2 ? vec2(-1.0, 0.0) :
			(tap == 3 ? vec2(0.0, 1.0) : vec2(0.0, -1.0))));
		vec2 offset = direction * u_invResolution;
		float tapDepth = texture2D(s_depth, v_uv + offset).r;
		float tapZ = linearDepth(tapDepth);
		float spatialWeight = tap == 0 ? 1.0 : 0.72;
		float depthWeight = exp(-abs(tapZ - centerZ) / depthTolerance);
		float weight = spatialWeight * depthWeight;
		total += texture2D(s_ao, v_uv + offset).r * weight;
		weightTotal += weight;
	}
	float ao = total / max(weightTotal, 0.0001);
	fragColor = vec4(ao, ao, ao, 1.0);
}
#endif
)";

static const char *g_ssaoCompositeShaderSource = R"(
#ifdef VERTEX
varying vec2 v_uv;
void main()
{
	vec2 position = vec2(gl_VertexID == 1 ? 3.0 : -1.0,
		gl_VertexID == 2 ? 3.0 : -1.0);
	v_uv = position * 0.5 + 0.5;
	gl_Position = vec4(position, 0.0, 1.0);
}
#endif
#ifdef FRAGMENT
varying vec2 v_uv;
uniform sampler2D s_ao;
void main()
{
	float ao = texture2D(s_ao, v_uv).r;
	fragColor = vec4(ao, ao, ao, 1.0);
}
#endif
)";

static void GR_InitialiseSSAOShaders() {
  g_ssaoShader = GR_Shader_Compile(g_ssaoShaderSource, false);
  g_ssaoBlurShader = GR_Shader_Compile(g_ssaoBlurShaderSource, false);
  g_ssaoCompositeShader = GR_Shader_Compile(g_ssaoCompositeShaderSource, false);

  glUseProgram(g_ssaoShader);
  glUniform1i(glGetUniformLocation(g_ssaoShader, "s_depth"), 0);
  g_ssaoProjectionScaleLoc =
      glGetUniformLocation(g_ssaoShader, "u_projectionScale");
  g_ssaoNearFarLoc = glGetUniformLocation(g_ssaoShader, "u_nearFar");

  glUseProgram(g_ssaoBlurShader);
  glUniform1i(glGetUniformLocation(g_ssaoBlurShader, "s_ao"), 0);
  glUniform1i(glGetUniformLocation(g_ssaoBlurShader, "s_depth"), 1);
  g_ssaoBlurInvResolutionLoc =
      glGetUniformLocation(g_ssaoBlurShader, "u_invResolution");
  g_ssaoBlurNearFarLoc = glGetUniformLocation(g_ssaoBlurShader, "u_nearFar");
  glUseProgram(0);
}
#endif

u_char GR_Expand5BitColor(u_char value) {
  value &= 31;
  return (u_char)((value << 3) | (value >> 2));
}

void GR_InitRG8LUT() {
  for (u_short y = 0; y < LUT_HEIGHT; y++) {
    u_char *row = rgLUT + y * (LUT_HEIGHT * 4);
    for (u_short x = 0; x < LUT_WIDTH; x++) {
      const u_short c = (y << 8) | x;
      u_char *pixel = row + x * 4;
      pixel[0] = GR_Expand5BitColor((u_char)(c & 31));
      pixel[1] = GR_Expand5BitColor((u_char)((c >> 5) & 31));
      pixel[2] = GR_Expand5BitColor((u_char)((c >> 10) & 31));
      pixel[3] = (u_char)((c >> 15) & 1) << 7;
    }
  }
}

int GR_InitialisePSX() {
  SDL_memset(vram, 0, VRAM_WIDTH * VRAM_HEIGHT * sizeof(unsigned short));
  SDL_memset(g_vramAliasPages, 0,
             VRAM_ALIAS_WIDTH * VRAM_ALIAS_HEIGHT * sizeof(unsigned short));
  GR_ResetVRAMDirtyRects();
  GR_InitRG8LUT();
  GR_GenerateCommonTextures();
  GR_InitialisePSXShaders();
#if defined(RENDERER_OGL)
  GR_InitialiseSSAOShaders();
#endif

#if USE_OPENGL
  glDepthFunc(GL_GEQUAL);
  g_PreviousDepthFunc = GL_GEQUAL;
  glEnable(GL_STENCIL_TEST);
  glBlendColor(0.5f, 0.5f, 0.5f, 0.25f);

  // All display primitives are rasterized at the active PSX DISPENV size.
  // The default framebuffer is used only for the final 4:3 presentation blit.
  glGenTextures(1, &g_glNativeColorTexture);
  glBindTexture(GL_TEXTURE_2D, g_glNativeColorTexture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);
  glGenFramebuffers(1, &g_glNativeFramebuffer);
#if defined(RENDERER_OGL)
  glGenTextures(1, &g_glNativeDepthTexture);
#else
  glGenRenderbuffers(1, &g_glNativeDepthRenderbuffer);
  glGenRenderbuffers(1, &g_glNativeStencilRenderbuffer);
#endif
  glGenFramebuffers(1, &g_glNativeMultisampleFramebuffer);
  glGenRenderbuffers(1, &g_glNativeMultisampleColorRenderbuffer);
  glGenRenderbuffers(1, &g_glNativeMultisampleDepthRenderbuffer);
#if defined(RENDERER_OGL)
  glGenFramebuffers(2, g_glSSAOFramebuffer);
  glGenTextures(2, g_glSSAOTexture);
  glGenVertexArrays(1, &g_glSSAOVertexArray);
#endif

  // gen framebuffer
  {
    memset(&g_glFramebufferPBO, 0, sizeof(g_glFramebufferPBO));
    PBO_Init(&g_glFramebufferPBO, GL_RGBA, VRAM_WIDTH, VRAM_HEIGHT, 2);

    // make a special texture
    // it will be resized later
    glGenTextures(1, &g_fbTexture);
    {
      glBindTexture(GL_TEXTURE_2D, g_fbTexture);

      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

      // default to VRAM size
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, VRAM_WIDTH, VRAM_HEIGHT, 0,
                   GL_RGBA, GL_UNSIGNED_BYTE, NULL);

      glBindTexture(GL_TEXTURE_2D, 0);
    }

    glGenFramebuffers(1, &g_glBlitFramebuffer);
    {
      glBindFramebuffer(GL_FRAMEBUFFER, g_glBlitFramebuffer);

      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, g_fbTexture, 0);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                             0, 0);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
                             GL_TEXTURE_2D, 0, 0);

      glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
  }

  // gen offscreen RT
  {
    memset(&g_glOffscreenPBO, 0, sizeof(g_glOffscreenPBO));
    PBO_Init(&g_glOffscreenPBO, GL_RGBA, VRAM_WIDTH, VRAM_HEIGHT, 2);

    // offscreen texture render target
    glGenTextures(1, &g_offscreenRTTexture);
    {
      glBindTexture(GL_TEXTURE_2D, g_offscreenRTTexture);

      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

      // default to VRAM size
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, VRAM_WIDTH, VRAM_HEIGHT, 0,
                   GL_RGBA, GL_UNSIGNED_BYTE, NULL);

      glBindTexture(GL_TEXTURE_2D, 0);
    }

    glGenFramebuffers(1, &g_glOffscreenFramebuffer);
    {
      glBindFramebuffer(GL_FRAMEBUFFER, g_glOffscreenFramebuffer);

      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, g_offscreenRTTexture, 0);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                             0, 0);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
                             GL_TEXTURE_2D, 0, 0);

      glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
  }

  // gen VRAM textures.
  // double-buffered
  {
    int i;

    glGenTextures(2, g_vramTexturesDouble);

    for (i = 0; i < 2; i++) {
      glBindTexture(GL_TEXTURE_2D, g_vramTexturesDouble[i]);

      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

      // set storage size
      glTexImage2D(GL_TEXTURE_2D, 0, VRAM_INTERNAL_FORMAT, VRAM_WIDTH,
                   VRAM_HEIGHT, 0, VRAM_FORMAT, GL_UNSIGNED_BYTE, NULL);
    }

    g_vramTexture = g_vramTexturesDouble[0];

    glBindTexture(GL_TEXTURE_2D, 0);

    // VRAM framebuffer for offscreen blitting to VRAM
    glGenFramebuffers(1, &g_glVRAMFramebuffer);
    {
      glBindFramebuffer(GL_FRAMEBUFFER, g_glVRAMFramebuffer);

      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, g_vramTexture, 0);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                             0, 0);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
                             GL_TEXTURE_2D, 0, 0);

      glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
  }

  // Host-only page aliases are never attached as PS1 VRAM or a framebuffer.
  // Keeping them separate preserves framebuffer and MoveImage semantics.
  {
    glGenTextures(1, &g_vramAliasTexture);
    glBindTexture(GL_TEXTURE_2D, g_vramAliasTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, VRAM_INTERNAL_FORMAT, VRAM_ALIAS_WIDTH,
                 VRAM_ALIAS_HEIGHT, 0, VRAM_FORMAT, GL_UNSIGNED_BYTE,
                 g_vramAliasPages);
    glBindTexture(GL_TEXTURE_2D, 0);
  }

  // gen vertex buffer and index buffer
  {
    int i;

    glGenBuffers(MAX_NUM_VERTEX_BUFFERS, g_glVertexBuffer);
    glGenVertexArrays(MAX_NUM_VERTEX_BUFFERS, g_glVertexArray);

    for (i = 0; i < MAX_NUM_VERTEX_BUFFERS; i++) {
      glBindVertexArray(g_glVertexArray[i]);

      glBindBuffer(GL_ARRAY_BUFFER, g_glVertexBuffer[i]);
      glBufferData(GL_ARRAY_BUFFER, sizeof(GrVertex) * MAX_VERTEX_BUFFER_SIZE,
                   NULL, GL_DYNAMIC_DRAW);
    }

    glBindVertexArray(0);
  }
#else
#error
#endif

  GR_ResetDevice();

  return 1;
}

void GR_Ortho2D(float left, float right, float bottom, float top, float znear,
                float zfar) {
  float a = 2.0f / (right - left);
  float b = 2.0f / (top - bottom);
  float c = 2.0f / (znear - zfar);

  float x = (left + right) / (left - right);
  float y = (bottom + top) / (bottom - top);

#if USE_OPENGL
  // -1..1
  float z = (znear + zfar) / (znear - zfar);
#endif

  float ortho[16] = {a, 0, 0, 0, 0, b, 0, 0, 0, 0, c, 0, x, y, z, 1};

#if USE_OPENGL
  glUniformMatrix4fv(u_projectionLoc, 1, GL_FALSE, ortho);
#endif
}

void GR_Perspective3D(const float fov, const float width, const float height,
                      const float zNear, const float zFar) {
  float sinF, cosF;
  sinF = sinf(0.5f * fov);
  cosF = cosf(0.5f * fov);

  float h = cosF / sinF;
  float w = (h * height) / width;
  g_ssaoProjectionScaleX = w;
  g_ssaoProjectionScaleY = h;
  g_ssaoNearPlane = zNear;
  g_ssaoFarPlane = zFar;
  float depthScale;
  float depthBias;
  GR_CalculateReversedDepthProjection(zNear, zFar, &depthScale, &depthBias);

  float persp[16] = {w, 0, 0,          0, 0, h, 0,         0,
                     0, 0, depthScale, 1, 0, 0, depthBias, 0};

#if USE_OPENGL
  glUniformMatrix4fv(u_projection3DLoc, 1, GL_FALSE, persp);
#endif
}

void GR_SetupClipMode(const RECT16 *rect, int enable) {
  // [A] isinterlaced dirty hack for widescreen
  const bool scissorOn =
      enable && (activeDispEnv.isinter || (rect->x - activeDispEnv.disp.x > 0 ||
                                           rect->y - activeDispEnv.disp.y > 0 ||
                                           rect->w < activeDispEnv.disp.w - 1 ||
                                           rect->h < activeDispEnv.disp.h - 1));

  GR_SetScissorState(scissorOn);

  if (!scissorOn || activeDispEnv.disp.w <= 0 || activeDispEnv.disp.h <= 0)
    return;

  const float psxScreenWInv = 1.0f / (float)activeDispEnv.disp.w;
  const float psxScreenHInv = 1.0f / (float)activeDispEnv.disp.h;

  // first map to 0..1
  float clipRectX = (float)(rect->x - activeDispEnv.disp.x) * psxScreenWInv;
  float clipRectY = (float)(rect->y - activeDispEnv.disp.y) * psxScreenHInv;
  float clipRectW = (float)(rect->w) * psxScreenWInv;
  float clipRectH = (float)(rect->h) * psxScreenHInv;

#if USE_OPENGL
  // Map directly into the high-resolution framebuffer. OpenGL's scissor origin
  // is bottom-left, while PSX draw environments use a top-left origin.
  const PsyXPresentationViewport viewport = PsyX_GetRenderViewport();
  const int crx = viewport.x + (int)(clipRectX * (float)viewport.w + 0.5f);
  const int cry =
      viewport.y +
      (int)((1.0f - clipRectY - clipRectH) * (float)viewport.h + 0.5f);
  const int crw = (int)(clipRectW * (float)viewport.w + 0.5f);
  const int crh = (int)(clipRectH * (float)viewport.h + 0.5f);

  glScissor(crx, cry, crw, crh);
#endif
}

void GR_CalculateReversedDepthProjection(float zNear, float zFar, float *scale,
                                         float *bias) {
  if (scale == NULL || bias == NULL)
    return;
  if (zNear <= 0.0f || zFar <= zNear) {
    *scale = -1.0f;
    *bias = 0.0f;
    return;
  }
  const float range = zFar - zNear;
  *scale = -(zFar + zNear) / range;
  *bias = (2.0f * zFar * zNear) / range;
}

void PsyX_GetPSXWidescreenMappedViewport(struct _RECT16 *rect) {
#if USE_PGXP
  float psxScreenW, psxScreenH;
  float emuScreenAspect;

  const PsyXPresentationViewport viewport = PsyX_GetLogicalViewport();
  emuScreenAspect = (float)viewport.w / (float)viewport.h;

  psxScreenW = activeDispEnv.disp.w;
  psxScreenH = activeDispEnv.disp.h;

  rect->x = activeDispEnv.screen.x;
  rect->y = activeDispEnv.screen.y;

  rect->w = psxScreenW * emuScreenAspect *
            PsyX_GetActiveScreenAspect(); // windowWidth;
  rect->h = psxScreenH;                   // windowHeight;

  rect->x -= (rect->w - activeDispEnv.disp.w) / 2;

  rect->w += rect->x;
#else
  rect->x = activeDispEnv.screen.x;
  rect->y = activeDispEnv.screen.y;
  rect->w = activeDispEnv.disp.w;
  rect->h = activeDispEnv.disp.h;
#endif
}

void GR_SetShader(const ShaderID shader) {
  if (g_PreviousShader != shader) {
#if USE_OPENGL
    glUseProgram(shader);
#else
#error
#endif

    g_PreviousShader = shader;
  }
}

TextureFilterMode GR_ResolveTextureFilterMode(TextureFilterMode requestedMode,
                                              int bilinearFiltering,
                                              int anisotropicFiltering) {
  if (requestedMode == TEXTURE_FILTER_WORLD_ANISOTROPIC && anisotropicFiltering)
    return TEXTURE_FILTER_WORLD_ANISOTROPIC;
  if (requestedMode != TEXTURE_FILTER_NEAREST && bilinearFiltering)
    return TEXTURE_FILTER_BILINEAR;
  return TEXTURE_FILTER_NEAREST;
}

void GR_SetTexture(TextureID texture, TexFormat texFormat,
                   TextureFilterMode filterMode) {
  GLint textureFilterModeLoc = 0;
  PSXGPU_Shader *activeShader = NULL;
  switch (texFormat) {
  case TF_4_BIT:
    activeShader = &g_gpu_shader_4;
    GR_SetShader(g_gpu_shader_4.shader);
    textureFilterModeLoc = g_gpu_shader_4.textureFilterModeLoc;
    u_textureBlendModeLoc = g_gpu_shader_4.textureBlendModeLoc;
    u_projectionLoc = g_gpu_shader_4.projectionLoc;
    u_projection3DLoc = g_gpu_shader_4.projection3DLoc;
    u_presentationScaleLoc = g_gpu_shader_4.presentationScaleLoc;
    u_texelSizeLoc = -1;
    break;
  case TF_8_BIT:
    activeShader = &g_gpu_shader_8;
    GR_SetShader(g_gpu_shader_8.shader);
    textureFilterModeLoc = g_gpu_shader_8.textureFilterModeLoc;
    u_textureBlendModeLoc = g_gpu_shader_8.textureBlendModeLoc;
    u_projectionLoc = g_gpu_shader_8.projectionLoc;
    u_projection3DLoc = g_gpu_shader_8.projection3DLoc;
    u_presentationScaleLoc = g_gpu_shader_8.presentationScaleLoc;
    u_texelSizeLoc = -1;
    break;
  case TF_16_BIT:
    activeShader = &g_gpu_shader_16;
    GR_SetShader(g_gpu_shader_16.shader);
    textureFilterModeLoc = g_gpu_shader_16.textureFilterModeLoc;
    u_textureBlendModeLoc = g_gpu_shader_16.textureBlendModeLoc;
    u_projectionLoc = g_gpu_shader_16.projectionLoc;
    u_projection3DLoc = g_gpu_shader_16.projection3DLoc;
    u_presentationScaleLoc = g_gpu_shader_16.presentationScaleLoc;
    u_texelSizeLoc = -1;
    break;
  case TF_32_BIT_RGBA:
    activeShader = &g_gpu_shader_32_rgba;
    GR_SetShader(g_gpu_shader_32_rgba.shader);
    textureFilterModeLoc = -1;
    u_textureBlendModeLoc = -1;
    u_projectionLoc = g_gpu_shader_32_rgba.projectionLoc;
    u_projection3DLoc = g_gpu_shader_32_rgba.projection3DLoc;
    u_presentationScaleLoc = g_gpu_shader_32_rgba.presentationScaleLoc;
    u_texelSizeLoc = g_gpu_shader_32_rgba.texelSizeLoc;
    break;
  }

  if (g_dbg_texturelessMode) {
    texture = g_whiteTexture;
  }

#if USE_OPENGL
  if (g_lastBoundTexture != texture) {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, g_rgLutTexture);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, g_vramAliasTexture);

    glActiveTexture(GL_TEXTURE0);
    g_lastBoundTexture = texture;
  }

  // Launcher filtering applies to the complete render path. Anisotropy is a
  // separate world-only enhancement; when it is disabled, world textures
  // follow the same bilinear/nearest selection as all other primitives.
  const GLint effectiveFilterMode =
      static_cast<GLint>(GR_ResolveTextureFilterMode(
          filterMode, g_cfg_bilinearFiltering, g_cfg_anisotropicFiltering));
  if (textureFilterModeLoc != -1 && activeShader != NULL &&
      activeShader->appliedTextureFilterMode != effectiveFilterMode) {
    glUniform1i(textureFilterModeLoc, effectiveFilterMode);
    activeShader->appliedTextureFilterMode = effectiveFilterMode;
  }
  if (texFormat == TF_32_BIT_RGBA) {
    auto cached = std::find_if(
        g_rgbaTextureFilterCache.begin(), g_rgbaTextureFilterCache.end(),
        [texture](const auto &entry) { return entry.texture == texture; });
    if (cached == g_rgbaTextureFilterCache.end() ||
        cached->filterMode != effectiveFilterMode) {
      const GLint filter = effectiveFilterMode != TEXTURE_FILTER_NEAREST
                               ? GL_LINEAR
                               : GL_NEAREST;
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
      if (g_maxTextureAnisotropy > 1.0f) {
        const GLfloat anisotropy =
            effectiveFilterMode == TEXTURE_FILTER_WORLD_ANISOTROPIC
                ? g_maxTextureAnisotropy
                : 1.0f;
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT,
                        anisotropy);
      }
      if (cached == g_rgbaTextureFilterCache.end())
        g_rgbaTextureFilterCache.push_back({texture, effectiveFilterMode});
      else
        cached->filterMode = effectiveFilterMode;
    }
  }
#endif
}

void GR_SetTextureBlendMode(BlendMode blendMode) {
#if USE_OPENGL
  if (u_textureBlendModeLoc != -1 &&
      (g_PreviousTextureBlendShader != g_PreviousShader ||
       g_PreviousTextureBlendMode != blendMode)) {
    glUniform1i(u_textureBlendModeLoc, blendMode);
    g_PreviousTextureBlendShader = g_PreviousShader;
    g_PreviousTextureBlendMode = blendMode;
  }
#endif
}

void GR_SetOverrideTextureSize(int width, int height) {
  if (u_texelSizeLoc == -1)
    return;

  // WebGL is fucking around with glUniform2f, so use vector version
  float vec[] = {1.0f / (float)width, 1.0f / (float)height};
  glUniform2fv(u_texelSizeLoc, 1, vec);
}

void GR_DestroyTexture(TextureID texture) {
  if (texture == -1)
    return;

#if USE_OPENGL
  glDeleteTextures(1, &texture);
  g_rgbaTextureFilterCache.erase(
      std::remove_if(
          g_rgbaTextureFilterCache.begin(), g_rgbaTextureFilterCache.end(),
          [texture](const auto &entry) { return entry.texture == texture; }),
      g_rgbaTextureFilterCache.end());
  if (g_lastBoundTexture == texture)
    g_lastBoundTexture = 0;
#else
#error
#endif
}

void GR_ClearVRAM(int x, int y, int w, int h, unsigned char r, unsigned char g,
                  unsigned char b) {
  if (x + w > VRAM_WIDTH)
    w = VRAM_WIDTH - x;

  if (y + h > VRAM_HEIGHT)
    h = VRAM_HEIGHT - y;

  GR_RecordVRAMWrite(GR_VRAM_WRITE_CLEAR, x, y, x, y, w, h);

  if (w <= 0 || h <= 0)
    return;

  GR_MarkVRAMDirty(x, y, w, h);
  u_short *dst = vram + x + y * VRAM_WIDTH;

  // clear VRAM region with given color
  for (int i = 0; i < h; i++) {
    u_short *tmp = dst;

    for (int j = 0; j < w; j++)
      *tmp++ = r | (g << 5) | (b << 11);

    dst += VRAM_WIDTH;
  }
}

void GR_Clear(int x, int y, int w, int h, unsigned char r, unsigned char g,
              unsigned char b) {
  framebuffer_need_update = 1;

#if USE_OPENGL
  glClearColor(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
#endif
}

void GR_SaveVRAM(const char *outputFileName, int x, int y, int width,
                 int height, int bReadFromFrameBuffer) {
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__)

#if USE_OPENGL

#define FLIP_Y (VRAM_HEIGHT - i - 1)

#endif

  FILE *fp = fopen(outputFileName, "wb");
  if (fp == NULL)
    return;

  unsigned char TGAheader[12] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  unsigned char header[6];
  header[0] = (width % 256);
  header[1] = (width / 256);
  header[2] = (height % 256);
  header[3] = (height / 256);
  header[4] = 16;
  header[5] = 0;

  fwrite(TGAheader, sizeof(unsigned char), 12, fp);
  fwrite(header, sizeof(unsigned char), 6, fp);

  for (int i = 0; i < VRAM_HEIGHT; i++) {
    fwrite(vram + VRAM_WIDTH * FLIP_Y, sizeof(short), VRAM_WIDTH, fp);
  }

  fclose(fp);

#undef FLIP_Y
#endif
}

void GR_CopyRGBAFramebufferToVRAM(u_int *src, int x, int y, int w, int h,
                                  int update_vram, int flip_y) {
  assert(x >= 0);
  assert(y >= 0);
  assert(x + w <= VRAM_WIDTH);
  assert(y + h <= VRAM_HEIGHT);
  GR_RecordVRAMWrite(GR_VRAM_WRITE_FRAMEBUFFER, x, y, x, y, w, h);

  // Convert directly into the emulated destination. The former two-pass path
  // allocated a full temporary image for every framebuffer feedback read,
  // then immediately copied it again; menus and effects can execute this path
  // several times in one presentation frame.
  const uint *source = (const uint *)src;
  for (int destinationRow = 0; destinationRow < h; ++destinationRow) {
    const int sourceRow = flip_y ? h - destinationRow - 1 : destinationRow;
    const uint *sourcePixel = source + sourceRow * w;
    ushort *destinationPixel =
        (ushort *)vram + VRAM_WIDTH * (y + destinationRow) + x;
    for (int column = 0; column < w; ++column) {
      const uint color = sourcePixel[column];
      const u_char blue = (color >> 3) & 0x1F;
      const u_char green = (color >> 11) & 0x1F;
      const u_char red = (color >> 19) & 0x1F;
      const int alpha = red == 0 && green == 0 && blue == 0 ? 0 : 1;
      destinationPixel[column] =
          red | (green << 5) | (blue << 10) | (alpha << 15);
    }
  }

  if (update_vram)
    GR_MarkVRAMDirty(x, y, w, h);
}

void GR_ReadFramebufferDataToVRAM() {
  int x, y, w, h;
  if (!g_cfg_framebufferFeedback) {
    framebuffer_need_update = 0;
    return;
  }

  if (!framebuffer_need_update)
    return;

  framebuffer_need_update = 0;

  x = g_PreviousFramebuffer.x;
  y = g_PreviousFramebuffer.y;
  w = g_PreviousFramebuffer.w;
  h = g_PreviousFramebuffer.h;

  // now we can read it back to VRAM texture

#if USE_OPENGL && defined(USE_PBO)
  // read the texture
  if (g_glFramebufferPBO.pixels) {
    glBindTexture(GL_TEXTURE_2D, g_fbTexture);
    PBO_Download(&g_glFramebufferPBO);
    glBindTexture(GL_TEXTURE_2D, 0);
    GR_CopyRGBAFramebufferToVRAM((u_int *)g_glFramebufferPBO.pixels, x, y, w, h,
                                 0, 0);
  }
#endif
}

void GR_SetScissorState(int enable) {
  if (g_PreviousScissorState == enable)
    return;

#if USE_OPENGL
  if (g_PreviousScissorState)
    glDisable(GL_SCISSOR_TEST);
  else
    glEnable(GL_SCISSOR_TEST);
#endif
  g_PreviousScissorState = enable;
}

void GR_SetOffscreenState(const RECT16 *offscreenRect, int enable) {
  const PsyXPresentationViewport logicalViewport = PsyX_GetLogicalViewport();
  const PsyXPresentationViewport renderViewport = PsyX_GetRenderViewport();

  if (enable) {
    // setup render target viewport
#if USE_PGXP
    GR_Ortho2D(-0.5f, 0.5f, 0.5f, -0.5f, -1.0f, 1.0f);
#else
    GR_Ortho2D(0, offscreenRect->w, offscreenRect->h, 0, -1.0f, 1.0f);
#endif
  } else {
    // setup default viewport
#if USE_PGXP

    // these constants below are guessed
    const float perspectiveFOV = 0.9265f;
    const float perspectiveZNear = 0.25f;
    const float perspectiveZFar = 1000.0f;

    const float emuScreenAspect =
        (float)logicalViewport.w / (float)logicalViewport.h;

    const float screenAspect = PsyX_GetActiveScreenAspect();
    GR_Ortho2D(-0.5f * emuScreenAspect * screenAspect,
               0.5f * emuScreenAspect * screenAspect, 0.5f, -0.5f, -1.0f, 1.0f);
    GR_Perspective3D(perspectiveFOV, 1.0f,
                     1.0f / (emuScreenAspect * screenAspect), perspectiveZNear,
                     perspectiveZFar);
#else
    GR_Ortho2D(0, activeDispEnv.disp.w, activeDispEnv.disp.h, 0, -1.0f, 1.0f);
#endif
  }

#if USE_OPENGL
  const PsyXPresentationScale presentationScale =
      enable ? PsyXPresentationScale{1.0f, 1.0f} : PsyX_GetPresentationScale();
  glUniform2f(u_presentationScaleLoc, presentationScale.x, presentationScale.y);
#endif

  // Reapply the viewport even when the render-target state did not change;
  // DISPENV can switch resolution without switching draw target type.
  if (enable)
    GR_SetViewPort(0, 0, offscreenRect->w, offscreenRect->h);
  else
    GR_SetViewPort(renderViewport.x, renderViewport.y, renderViewport.w,
                   renderViewport.h);

  if (g_PreviousOffscreenState == enable)
    return;

  g_PreviousOffscreenState = enable;

#if USE_OPENGL
  if (enable) {
    // set storage size first
    if (g_PreviousOffscreen.w != offscreenRect->w &&
        g_PreviousOffscreen.h != offscreenRect->h) {
      glBindTexture(GL_TEXTURE_2D, g_offscreenRTTexture);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, offscreenRect->w,
                   offscreenRect->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
      glBindTexture(GL_TEXTURE_2D, 0);
    }

    g_PreviousOffscreen = *offscreenRect;

    glBindFramebuffer(GL_FRAMEBUFFER, g_glOffscreenFramebuffer);

    // clear it out
    glClearColor(0.5f, 0.5f, 0.5f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
  } else {
#if USE_OFFSCREEN_BLIT
    // before drawing set source and target
    {
      glBindFramebuffer(GL_FRAMEBUFFER, g_glVRAMFramebuffer);

      // rebind texture
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, g_vramTexture, 0);

      // setup draw and read framebuffers
      glBindFramebuffer(GL_READ_FRAMEBUFFER,
                        g_glOffscreenFramebuffer); // source is backbuffer
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_glVRAMFramebuffer);

      glBlitFramebuffer(0, 0, g_PreviousOffscreen.w, g_PreviousOffscreen.h,
                        g_PreviousOffscreen.x,
                        g_PreviousOffscreen.y + g_PreviousOffscreen.h,
                        g_PreviousOffscreen.x + g_PreviousOffscreen.w,
                        g_PreviousOffscreen.y, GL_COLOR_BUFFER_BIT, GL_NEAREST);

      // done, unbind
      glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    }
#endif

    glBindFramebuffer(GL_FRAMEBUFFER, PsyX_GetNativeDrawFramebuffer());
    // copy rendering results to VRAM texture
    {
      // reat the texture
      glBindTexture(GL_TEXTURE_2D, g_offscreenRTTexture);
      // glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
      PBO_Download(&g_glOffscreenPBO);
      glBindTexture(GL_TEXTURE_2D, g_lastBoundTexture);

      // Don't forcely update VRAM
      GR_CopyRGBAFramebufferToVRAM((u_int *)g_glOffscreenPBO.pixels,
                                   g_PreviousOffscreen.x, g_PreviousOffscreen.y,
                                   g_PreviousOffscreen.w, g_PreviousOffscreen.h,
                                   USE_OFFSCREEN_BLIT == 0, 1);
    }
  }
#endif
}

void GR_StoreFrameBuffer(int x, int y, int w, int h) {
#if USE_OPENGL
  // set storage size first
  if (g_PreviousFramebuffer.w != w || g_PreviousFramebuffer.h != h) {
    glBindTexture(GL_TEXTURE_2D, g_fbTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 NULL);
    glBindTexture(GL_TEXTURE_2D, 0);
  }

  g_PreviousFramebuffer.x = x;
  g_PreviousFramebuffer.y = y;
  g_PreviousFramebuffer.w = w;
  g_PreviousFramebuffer.h = h;

#if USE_FRAMEBUFFER_BLIT
  PsyX_ResolveNativeFramebuffer();
  glBindFramebuffer(GL_FRAMEBUFFER, g_glBlitFramebuffer);

  // before drawing set source and target
  {
    // setup draw and read framebuffers
    glBindFramebuffer(
        GL_READ_FRAMEBUFFER,
        g_glNativeFramebuffer); // source is native PSX framebuffer
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_glBlitFramebuffer);

    const PsyXPresentationViewport viewport = PsyX_GetRenderViewport();
    glBlitFramebuffer(0, 0, viewport.w, viewport.h, x, y + h, x + w, y,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);

    // Blit framebuffer to VRAM screen area

    // before drawing set source and target
    glBindFramebuffer(GL_FRAMEBUFFER, g_glVRAMFramebuffer);

    // rebind vram texture
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           g_vramTexture, 0);

    // setup draw and read framebuffers
    glBindFramebuffer(GL_READ_FRAMEBUFFER,
                      g_glBlitFramebuffer); // source is backbuffer
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_glVRAMFramebuffer);

    glBlitFramebuffer(0, 0, w, h, x, y + h, x + w, y, GL_COLOR_BUFFER_BIT,
                      GL_NEAREST);

    // done, unbind
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  }

  // after drawing
  glBindFramebuffer(GL_FRAMEBUFFER, PsyX_GetNativeDrawFramebuffer());
  // The following PBO transfer is ordered after the blit by OpenGL itself.
  // An explicit flush here forced every framebuffer-feedback effect to submit
  // the whole command queue early and caused visible frame-time spikes.
#endif

  GR_ReadFramebufferDataToVRAM();
#endif
}

void GR_CopyVRAM(unsigned short *src, int x, int y, int w, int h, int dst_x,
                 int dst_y) {
  if (w <= 0 || h <= 0)
    return;
  assert(x >= 0 && y >= 0 && dst_x >= 0 && dst_y >= 0);
  assert(dst_x + w <= VRAM_WIDTH && dst_y + h <= VRAM_HEIGHT);

  GR_MarkVRAMDirty(dst_x, dst_y, w, h);

  int stride = w;
  const int sourceY = y;
  const bool internalCopy = src == NULL;
  GR_RecordVRAMWrite(internalCopy ? GR_VRAM_WRITE_MOVE : GR_VRAM_WRITE_UPLOAD,
                     x, y, dst_x, dst_y, w, h);

  if (internalCopy) {
    assert(x + w <= VRAM_WIDTH && y + h <= VRAM_HEIGHT);
    framebuffer_need_update = 1;
    src = vram;
    stride = VRAM_WIDTH;
  }

  if (internalCopy && dst_y > sourceY && dst_y < sourceY + h) {
    for (int row = h - 1; row >= 0; --row) {
      SDL_memmove(vram + dst_x + (dst_y + row) * VRAM_WIDTH,
                  vram + x + (sourceY + row) * VRAM_WIDTH,
                  w * sizeof(unsigned short));
    }
  } else {
    src += x + y * stride;
    unsigned short *dst = vram + dst_x + dst_y * VRAM_WIDTH;
    for (int row = 0; row < h; ++row) {
      SDL_memmove(dst, src, w * sizeof(unsigned short));
      dst += VRAM_WIDTH;
      src += stride;
    }
  }
}

void GR_ReadVRAM(unsigned short *dst, int x, int y, int dst_w, int dst_h) {
  unsigned short *src = vram + x + VRAM_WIDTH * y;

  for (int i = 0; i < dst_h; i++) {
    SDL_memcpy(dst, src, dst_w * sizeof(short));
    dst += dst_w;
    src += VRAM_WIDTH;
  }
}

void GR_UploadVRAMAliasPage(int page, const unsigned short *src) {
  if (page < 0 || page >= VRAM_ALIAS_PAGE_COUNT || src == NULL)
    return;
  const unsigned short *upload = src;
  const int pageX = (page & 15) * 64;
  const int pageY = (page >> 4) * 256;
  unsigned short *destination =
      g_vramAliasPages + pageX + pageY * VRAM_ALIAS_WIDTH;
  for (int row = 0; row < 256; ++row) {
    SDL_memcpy(destination, src, 64 * sizeof(unsigned short));
    destination += VRAM_ALIAS_WIDTH;
    src += 64;
  }
#if USE_OPENGL
  glActiveTexture(GL_TEXTURE2);
  glBindTexture(GL_TEXTURE_2D, g_vramAliasTexture);
  glTexSubImage2D(GL_TEXTURE_2D, 0, pageX, pageY, 64, 256, VRAM_FORMAT,
                  GL_UNSIGNED_BYTE, upload);
  glActiveTexture(GL_TEXTURE0);
  g_lastBoundTexture = 0;
#endif
}

void GR_ReadVRAMAliasPage(int page, unsigned short *dst) {
  if (page < 0 || page >= VRAM_ALIAS_PAGE_COUNT || dst == NULL)
    return;
  const int pageX = (page & 15) * 64;
  const int pageY = (page >> 4) * 256;
  const unsigned short *source =
      g_vramAliasPages + pageX + pageY * VRAM_ALIAS_WIDTH;
  for (int row = 0; row < 256; ++row) {
    SDL_memcpy(dst, source, 64 * sizeof(unsigned short));
    dst += 64;
    source += VRAM_ALIAS_WIDTH;
  }
}

void GR_UpdateVRAM() {
  if (!vram_need_update)
    return;

  vram_need_update = 0;

#if USE_OPENGL
  const int textureIndex = g_vramTextureIdx;
  g_vramTexture = g_vramTexturesDouble[textureIndex];
  g_vramTextureIdx = (g_vramTextureIdx + 1) & 1;

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, g_vramTexture);
  GrVRAMDirtyRows &dirty = g_vramDirtyRows[textureIndex];

#if defined(RENDERER_OGL) || (defined(RENDERER_OGLES) && OGLES_VERSION >= 3)
  // Each alternating object owns an independent dirty journal. A page is
  // updated with every write accumulated since that exact object was last
  // selected, so partial uploads remain complete snapshots without paying a
  // fixed 1 MiB transfer for a one-pixel CLUT animation. Consecutive rows with
  // the same span are submitted as one rectangle to keep driver call count low.
  glPixelStorei(GL_UNPACK_ROW_LENGTH, VRAM_WIDTH);
  int row = 0;
  while (row < VRAM_HEIGHT) {
    if (dirty.x0[row] >= dirty.x1[row]) {
      ++row;
      continue;
    }
    const int x0 = dirty.x0[row];
    const int x1 = dirty.x1[row];
    const int firstRow = row;
    while (row + 1 < VRAM_HEIGHT && dirty.x0[row + 1] == x0 &&
           dirty.x1[row + 1] == x1) {
      ++row;
    }
    const int rowCount = row - firstRow + 1;
    glTexSubImage2D(GL_TEXTURE_2D, 0, x0, firstRow, x1 - x0, rowCount,
                    VRAM_FORMAT, GL_UNSIGNED_BYTE,
                    vram + firstRow * VRAM_WIDTH + x0);
    for (int clearRow = firstRow; clearRow <= row; ++clearRow) {
      dirty.x0[clearRow] = 0;
      dirty.x1[clearRow] = 0;
    }
    ++row;
  }
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
#else
  // OpenGL ES 2 has no row-length unpack state. Preserve the complete upload
  // there; desktop and ES3 use the batched dirty path above.
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, VRAM_WIDTH, VRAM_HEIGHT, VRAM_FORMAT,
                  GL_UNSIGNED_BYTE, vram);
  for (int row = 0; row < VRAM_HEIGHT; ++row) {
    dirty.x0[row] = 0;
    dirty.x1[row] = 0;
  }
#endif
  // GR_UpdateVRAM changes the GL binding behind GR_SetTexture's cache. Force
  // the next split to bind its exact texture and restore the LUT sampler.
  g_lastBoundTexture = 0;

#endif
}

void GR_SwapWindow() {
#if defined(RENDERER_OGL) || defined(RENDERER_OGLES)
  PsyX_PresentNativeFramebuffer();
  SDL_GL_SwapWindow(g_window);
#endif

  // glFinish();
}

void GR_ApplySSAO(void) {
#if defined(RENDERER_OGL)
  if (!g_cfg_ssao || !g_cfg_pgxpZBuffer || !g_ssaoFramebufferAvailable ||
      g_nativeFramebufferWidth <= 0 || g_nativeFramebufferHeight <= 0 ||
      g_ssaoProjectionScaleX <= 0.0f || g_ssaoProjectionScaleY <= 0.0f ||
      g_ssaoNearPlane <= 0.0f || g_ssaoFarPlane <= g_ssaoNearPlane) {
    return;
  }

  // This pass has a strict call-site contract: native world framebuffer,
  // opaque blend state and depth/stencil enabled. Preserve only the renderer's
  // tracked objects and restore that canonical state explicitly. Querying all
  // OpenGL state with glGet* here serialized the CPU with the GPU every frame,
  // turning an otherwise half-resolution effect into a major 120/240 Hz stall.
  const GLuint nativeFramebuffer = PsyX_GetNativeDrawFramebuffer();
  const ShaderID previousShader = g_PreviousShader;
  const TextureID previousTexture = g_lastBoundTexture;
  const GLuint previousVertexArray = g_boundVertexArray;
  const int previousScissorState = g_PreviousScissorState;

  glDisable(GL_BLEND);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_CULL_FACE);
  glDepthMask(GL_FALSE);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

#if USE_FRAMEBUFFER_BLIT
  if (g_nativeFramebufferSamples > 1) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_glNativeMultisampleFramebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_glNativeFramebuffer);
    glBlitFramebuffer(0, 0, g_nativeFramebufferWidth, g_nativeFramebufferHeight,
                      0, 0, g_nativeFramebufferWidth, g_nativeFramebufferHeight,
                      GL_DEPTH_BUFFER_BIT, GL_NEAREST);
  }
#endif

  glBindVertexArray(g_glSSAOVertexArray);
  glBindFramebuffer(GL_FRAMEBUFFER, g_glSSAOFramebuffer[0]);
  glViewport(0, 0, g_ssaoFramebufferWidth, g_ssaoFramebufferHeight);
  glUseProgram(g_ssaoShader);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, g_glNativeDepthTexture);
  glUniform2f(g_ssaoProjectionScaleLoc, g_ssaoProjectionScaleX,
              g_ssaoProjectionScaleY);
  glUniform2f(g_ssaoNearFarLoc, g_ssaoNearPlane, g_ssaoFarPlane);
  glDrawArrays(GL_TRIANGLES, 0, 3);

  glBindFramebuffer(GL_FRAMEBUFFER, g_glSSAOFramebuffer[1]);
  glUseProgram(g_ssaoBlurShader);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, g_glSSAOTexture[0]);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, g_glNativeDepthTexture);
  glUniform2f(g_ssaoBlurInvResolutionLoc, 1.0f / (float)g_ssaoFramebufferWidth,
              1.0f / (float)g_ssaoFramebufferHeight);
  glUniform2f(g_ssaoBlurNearFarLoc, g_ssaoNearPlane, g_ssaoFarPlane);
  glDrawArrays(GL_TRIANGLES, 0, 3);

  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, nativeFramebuffer);
  glViewport(0, 0, g_nativeFramebufferWidth, g_nativeFramebufferHeight);
  glUseProgram(g_ssaoCompositeShader);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, g_glSSAOTexture[1]);
  glEnable(GL_BLEND);
  glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
  glBlendFuncSeparate(GL_ZERO, GL_SRC_COLOR, GL_ZERO, GL_ONE);
  glDrawArrays(GL_TRIANGLES, 0, 3);

  // Re-establish the tracked world-render contract without querying the
  // driver. The fire and presentation passes which follow can now use the
  // ordinary PsyCross state cache without a forced cache invalidation.
  glDisable(GL_BLEND);
  glEnable(GL_DEPTH_TEST);
  glEnable(GL_STENCIL_TEST);
  if (previousScissorState)
    glEnable(GL_SCISSOR_TEST);
  else
    glDisable(GL_SCISSOR_TEST);
  glDisable(GL_CULL_FACE);
  glDepthMask(GL_TRUE);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glDepthFunc(GL_GREATER);
  glUseProgram(previousShader);
  glBindVertexArray(previousVertexArray);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, previousTexture);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, g_rgLutTexture);
  glActiveTexture(GL_TEXTURE2);
  glBindTexture(GL_TEXTURE_2D, g_vramAliasTexture);
  glActiveTexture(GL_TEXTURE0);
  glBindFramebuffer(GL_FRAMEBUFFER, nativeFramebuffer);
  glViewport(0, 0, g_nativeFramebufferWidth, g_nativeFramebufferHeight);
#endif
}

void GR_SetDepthState(int testEnable, int writeEnable) {
  const int appliedTest = testEnable && g_cfg_pgxpZBuffer ? 1 : 0;
  const int appliedWrite = appliedTest && writeEnable ? 1 : 0;

#if USE_OPENGL
  // Opaque geometry owns depth and therefore remains strict. Transparent
  // polygons only test the established depth; equal values retain PS1 OT
  // painter order without allowing them to occlude later geometry.
  const int depthFunc = appliedWrite ? GL_GREATER : GL_GEQUAL;
  if (appliedTest && g_PreviousDepthFunc != depthFunc) {
    g_PreviousDepthFunc = depthFunc;
    glDepthFunc(depthFunc);
  }
  if (g_PreviousDepthMode != appliedTest) {
    g_PreviousDepthMode = appliedTest;
    if (appliedTest)
      glEnable(GL_DEPTH_TEST);
    else
      glDisable(GL_DEPTH_TEST);
  }
  if (g_PreviousDepthWrite != appliedWrite) {
    g_PreviousDepthWrite = appliedWrite;
    glDepthMask(appliedWrite ? GL_TRUE : GL_FALSE);
  }
#endif
}

void GR_ClearDepthBuffer(void) {
#if USE_OPENGL
  // glClear obeys the depth write mask. Preserve the caller's write state so a
  // transparent run stays test-only after an OT depth discontinuity.
  const int restoreWrite = g_PreviousDepthWrite;
  glDepthMask(GL_TRUE);
#ifdef RENDERER_OGLES
  glClearDepthf(0.0f);
#else
  glClearDepth(0.0f);
#endif
  glClear(GL_DEPTH_BUFFER_BIT);
  if (restoreWrite == 0)
    glDepthMask(GL_FALSE);
#endif
}

void GR_EnableDepth(int enable) {
  g_RequestedDepthMode = enable ? 1 : 0;
  GR_SetDepthState(enable, enable);
}

void GR_SetStencilMode(int drawPrim) {
  if (g_ShadowStencilPhase != 0)
    return;

  if (g_PreviousStencilMode == drawPrim)
    return;

  g_PreviousStencilMode = drawPrim;

#if USE_OPENGL
  if (drawPrim) {
    glStencilFunc(GL_ALWAYS, 1, 0x10);
    glStencilOp(GL_REPLACE, GL_REPLACE, GL_REPLACE);
  } else {
    glStencilFunc(GL_NOTEQUAL, 1, 0xFF);
    glStencilOp(GL_REPLACE, GL_KEEP, GL_KEEP);
  }
#endif
}

void GR_BeginShadowMask(void) {
  g_ShadowStencilPhase = 1;
#if USE_OPENGL
  // Preserve the retail mask bit (0x01) and reserve bit 0x02 for the native
  // shadow union. glClear obeys the stencil write mask, so this never erases
  // guest-authored PSX mask state from the opaque world pass.
  glStencilMask(0x02);
  glClearStencil(0);
  glClear(GL_STENCIL_BUFFER_BIT);
  glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
  // Reject guest pixels carrying the retail mask bit while writing only the
  // native shadow bit. The replacement reference supplies bit 0x02 without
  // disturbing bit 0x01.
  glStencilFunc(GL_NOTEQUAL, 0x03, 0x01);
  glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
#endif
}

void GR_BeginShadowShade(void) {
  g_ShadowStencilPhase = 2;
#if USE_OPENGL
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glStencilMask(0x02);
  glStencilFunc(GL_EQUAL, 0x02, 0x03);
  // Clear the native bit as each visible pixel is shaded. Overlapping body
  // triangles consequently darken a receiver exactly once.
  glStencilOp(GL_KEEP, GL_KEEP, GL_ZERO);
#endif
}

void GR_EndShadowMask(void) {
#if USE_OPENGL
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  // Remove any mask fragment which failed the shade depth test while retaining
  // the retail PSX bit, then restore the normal mask-test contract.
  glStencilMask(0x02);
  glClearStencil(0);
  glClear(GL_STENCIL_BUFFER_BIT);
  glStencilMask(0xFF);
  glStencilFunc(GL_NOTEQUAL, 1, 0xFF);
  glStencilOp(GL_REPLACE, GL_KEEP, GL_KEEP);
#endif
  g_ShadowStencilPhase = 0;
  g_PreviousStencilMode = 0;
}

void GR_SetBlendMode(BlendMode blendMode) {
  if (g_PreviousBlendMode == blendMode)
    return;

#if USE_OPENGL
  if (blendMode == BM_NONE) {
    if (g_PreviousBlendMode != BM_NONE) {
      glBlendColor(1.f, 1.f, 1.f, 1.f);
      glDisable(GL_BLEND);
    }

    g_PreviousBlendMode = blendMode;
    return;
  } else {
    if (g_PreviousBlendMode == BM_NONE) {
      glBlendColor(0.25f, 0.25f, 0.25f, 0.5f);
      glEnable(GL_BLEND);
    }

    g_PreviousBlendMode = blendMode;
  }

  glBlendEquationSeparate(blendMode == BM_SUBTRACT ? GL_FUNC_REVERSE_SUBTRACT
                                                   : GL_FUNC_ADD,
                          GL_FUNC_ADD);
  switch (blendMode) {
  case BM_AVERAGE:
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    break;
  case BM_ADD:
  case BM_SUBTRACT:
    glBlendFunc(GL_ONE, GL_ONE);
    break;
  case BM_ADD_QUATER_SOURCE:
    glBlendFunc(GL_CONSTANT_ALPHA, GL_ONE);
    break;
  }
#endif

  g_PreviousBlendMode = blendMode;
}

void GR_SetPolygonOffset(float slope, float units) {
  // GPU split submission restores the generic polygon state for every split.
  // Keep the receiver bias selected by the two-pass shadow operation until
  // that operation ends; otherwise coplanar wall portions flicker per split.
  if (g_ShadowStencilPhase != 0)
    return;

#if USE_OPENGL
  if (g_PreviousPolygonOffsetSlope == slope &&
      g_PreviousPolygonOffsetUnits == units)
    return;
  g_PreviousPolygonOffsetSlope = slope;
  g_PreviousPolygonOffsetUnits = units;
  if (slope == 0.0f && units == 0.0f) {
    glDisable(GL_POLYGON_OFFSET_FILL);
  } else {
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(slope, units);
  }
#endif
}

void GR_SetViewPort(int x, int y, int width, int height) {
#if USE_OPENGL
  glViewport(x, y, width, height);
#endif
}

void GR_SetWireframe(int enable) {
#if defined(RENDERER_OGL)
  glPolygonMode(GL_FRONT_AND_BACK, enable ? GL_LINE : GL_FILL);
#endif
}

void GR_BindVertexBuffer() {
#if USE_OPENGL
  glBindVertexArray(g_glVertexArray[g_curVertexBuffer]);
  g_boundVertexArray = g_glVertexArray[g_curVertexBuffer];
  // GL_ARRAY_BUFFER is not VAO binding state. Explicitly select the matching
  // stream buffer before recording attribute pointers and uploading data;
  // otherwise both VAOs silently reuse the last buffer created at startup.
  glBindBuffer(GL_ARRAY_BUFFER, g_glVertexBuffer[g_curVertexBuffer]);

  glEnableVertexAttribArray(a_position);
  glEnableVertexAttribArray(a_page_clut);
  glEnableVertexAttribArray(a_texcoord);
  glEnableVertexAttribArray(a_color);
  glEnableVertexAttribArray(a_extra);
  glEnableVertexAttribArray(a_texbounds);
  glEnableVertexAttribArray(a_precise_uv);

#if USE_PGXP
  glVertexAttribPointer(a_position, 2, GL_FLOAT, GL_FALSE, sizeof(GrVertex),
                        &((GrVertex *)NULL)->x);
  glVertexAttribPointer(a_page_clut, 2, GL_FLOAT, GL_FALSE, sizeof(GrVertex),
                        &((GrVertex *)NULL)->page);
  glVertexAttribPointer(a_zw, 4, GL_FLOAT, GL_FALSE, sizeof(GrVertex),
                        &((GrVertex *)NULL)->z);

  glEnableVertexAttribArray(a_zw);
#else
  glVertexAttribPointer(a_position, 2, GL_SHORT, GL_FALSE, sizeof(GrVertex),
                        &((GrVertex *)NULL)->x);
  glVertexAttribPointer(a_page_clut, 2, GL_UNSIGNED_SHORT, GL_FALSE,
                        sizeof(GrVertex), &((GrVertex *)NULL)->page);
#endif
  glVertexAttribPointer(a_texcoord, 4, GL_UNSIGNED_BYTE, GL_FALSE,
                        sizeof(GrVertex), &((GrVertex *)NULL)->u);
  glVertexAttribPointer(a_color, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GrVertex),
                        &((GrVertex *)NULL)->r);
  glVertexAttribPointer(a_extra, 4, GL_BYTE, GL_FALSE, sizeof(GrVertex),
                        &((GrVertex *)NULL)->tcx);
  glVertexAttribPointer(a_texbounds, 4, GL_UNSIGNED_BYTE, GL_FALSE,
                        sizeof(GrVertex), &((GrVertex *)NULL)->umin);
  glVertexAttribPointer(a_precise_uv, 2, GL_FLOAT, GL_FALSE, sizeof(GrVertex),
                        &((GrVertex *)NULL)->precise_u);

  g_curVertexBuffer++;
  g_curVertexBuffer &= 1;
#else
#error
#endif
}

void GR_UpdateVertexBuffer(const GrVertex *vertices, int num_vertices) {
  if (num_vertices > MAX_VERTEX_BUFFER_SIZE) {
    eprinterr("MAX_VERTEX_BUFFER_SIZE reached, expect rendering errors\n");
    num_vertices = MAX_VERTEX_BUFFER_SIZE;
  }

  // assert(num_vertices <= MAX_VERTEX_BUFFER_SIZE);
  GR_BindVertexBuffer();

#if USE_OPENGL
  // One orphaning upload avoids both an in-flight wait and a second driver
  // copy. Allocate only the submitted range; HUD DrawSync calls are small and
  // must not repeatedly reserve the full world-sized stream buffer.
  glBufferData(GL_ARRAY_BUFFER, num_vertices * sizeof(GrVertex), vertices,
               GL_STREAM_DRAW);
#else
#error
#endif
}

void GR_DrawTriangles(int start_vertex, int triangles) {
#if USE_OPENGL
  glDrawArrays(GL_TRIANGLES, start_vertex, triangles * 3);
#else
#error
#endif
}

void GR_PushDebugLabel(const char *label) {
#if USE_OPENGL && !defined(__EMSCRIPTEN__) &&                                  \
    defined(GL_DEBUG_SOURCE_APPLICATION)
  if (!glPushDebugGroup)
    return;
  glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 0x8000, strlen(label), label);
#endif
}

void GR_PopDebugLabel() {
#if USE_OPENGL && !defined(__EMSCRIPTEN__) &&                                  \
    defined(GL_DEBUG_SOURCE_APPLICATION)
  if (!glPopDebugGroup)
    return;
  glPopDebugGroup();
#endif
}
