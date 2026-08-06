#if defined(RENDERER_OGL)

#include "smaa/smaa_lookup_data.hpp"
#include "smaa/smaa_shader_source.hpp"

#include <cstdint>
#include <string>

namespace {

struct PsyXSMAAProgram {
  GLuint id = 0;
  GLint metrics = -1;
  GLint sourceRect = -1;
};

struct PsyXSMAAResources {
  PsyXSMAAProgram edges;
  PsyXSMAAProgram depthEdges;
  PsyXSMAAProgram weights;
  PsyXSMAAProgram neighborhood;
  GLuint framebuffer = 0;
  GLuint vertexArray = 0;
  GLuint areaTexture = 0;
  GLuint searchTexture = 0;
  GLuint edgesTexture = 0;
  GLuint weightsTexture = 0;
  GLuint outputTexture = 0;
  int width = 0;
  int height = 0;
  bool available = false;
};

PsyXSMAAResources g_smaa;

constexpr const char *kSMAAShaderPrefix = R"PSYX(
#define SMAA_GLSL_3 1
#define SMAA_PRESET_ULTRA 1
#define SMAA_PREDICATION 1
#define SMAA_PREDICATION_THRESHOLD 0.002
#define SMAA_PREDICATION_SCALE 1.0
#define SMAA_PREDICATION_STRENGTH 0.75
#define SMAA_AREATEX_SELECT(sample) sample.rg
#define SMAA_SEARCHTEX_SELECT(sample) sample.r
#ifdef VERTEX
#define SMAA_INCLUDE_VS 1
#define SMAA_INCLUDE_PS 0
#else
#define SMAA_INCLUDE_VS 0
#define SMAA_INCLUDE_PS 1
#endif
uniform vec4 u_metrics;
#define SMAA_RT_METRICS u_metrics
)PSYX";

constexpr const char *kSMAAEdgesWrapper = R"PSYX(
#ifdef VERTEX
varying vec2 v_smaaTexcoord;
varying vec4 v_smaaOffset[3];
uniform vec4 u_source_rect;
void main() {
  vec2 position = gl_VertexID == 0 ? vec2(0.0, 0.0)
                : gl_VertexID == 1 ? vec2(2.0, 0.0)
                                   : vec2(0.0, 2.0);
  v_smaaTexcoord = u_source_rect.xy + position * u_source_rect.zw;
  SMAAEdgeDetectionVS(v_smaaTexcoord, v_smaaOffset);
  gl_Position = vec4(position * 2.0 - 1.0, 0.0, 1.0);
}
#else
varying vec2 v_smaaTexcoord;
varying vec4 v_smaaOffset[3];
uniform sampler2D u_color;
uniform sampler2D u_predication;
void main() {
  vec2 edges = SMAAColorEdgeDetectionPS(
      v_smaaTexcoord, v_smaaOffset, u_color, u_predication);
  fragColor = vec4(edges, 0.0, 0.0);
}
#endif
)PSYX";

constexpr const char *kSMAADepthEdgesWrapper = R"PSYX(
#ifdef VERTEX
varying vec2 v_smaaTexcoord;
varying vec4 v_smaaOffset[3];
uniform vec4 u_source_rect;
void main() {
  vec2 position = gl_VertexID == 0 ? vec2(0.0, 0.0)
                : gl_VertexID == 1 ? vec2(2.0, 0.0)
                                   : vec2(0.0, 2.0);
  v_smaaTexcoord = u_source_rect.xy + position * u_source_rect.zw;
  SMAAEdgeDetectionVS(v_smaaTexcoord, v_smaaOffset);
  gl_Position = vec4(position * 2.0 - 1.0, 0.0, 1.0);
}
#else
varying vec2 v_smaaTexcoord;
varying vec4 v_smaaOffset[3];
uniform sampler2D u_depth;
void main() {
  vec2 edges =
      SMAADepthEdgeDetectionPS(v_smaaTexcoord, v_smaaOffset, u_depth);
  fragColor = vec4(edges, 0.0, 0.0);
}
#endif
)PSYX";

constexpr const char *kSMAAWeightsWrapper = R"PSYX(
#ifdef VERTEX
varying vec2 v_smaaTexcoord;
varying vec2 v_smaaPixcoord;
varying vec4 v_smaaOffset[3];
uniform vec4 u_source_rect;
void main() {
  vec2 position = gl_VertexID == 0 ? vec2(0.0, 0.0)
                : gl_VertexID == 1 ? vec2(2.0, 0.0)
                                   : vec2(0.0, 2.0);
  v_smaaTexcoord = u_source_rect.xy + position * u_source_rect.zw;
  SMAABlendingWeightCalculationVS(v_smaaTexcoord, v_smaaPixcoord,
                                  v_smaaOffset);
  gl_Position = vec4(position * 2.0 - 1.0, 0.0, 1.0);
}
#else
varying vec2 v_smaaTexcoord;
varying vec2 v_smaaPixcoord;
varying vec4 v_smaaOffset[3];
uniform sampler2D u_edges;
uniform sampler2D u_area;
uniform sampler2D u_search;
void main() {
  fragColor = SMAABlendingWeightCalculationPS(
      v_smaaTexcoord, v_smaaPixcoord, v_smaaOffset, u_edges, u_area,
      u_search, vec4(0.0));
}
#endif
)PSYX";

constexpr const char *kSMAANeighborhoodWrapper = R"PSYX(
#ifdef VERTEX
varying vec2 v_smaaTexcoord;
varying vec4 v_smaaOffset;
uniform vec4 u_source_rect;
void main() {
  vec2 position = gl_VertexID == 0 ? vec2(0.0, 0.0)
                : gl_VertexID == 1 ? vec2(2.0, 0.0)
                                   : vec2(0.0, 2.0);
  v_smaaTexcoord = u_source_rect.xy + position * u_source_rect.zw;
  SMAANeighborhoodBlendingVS(v_smaaTexcoord, v_smaaOffset);
  gl_Position = vec4(position * 2.0 - 1.0, 0.0, 1.0);
}
#else
varying vec2 v_smaaTexcoord;
varying vec4 v_smaaOffset;
uniform sampler2D u_color;
uniform sampler2D u_blend;
void main() {
  fragColor = SMAANeighborhoodBlendingPS(v_smaaTexcoord, v_smaaOffset,
                                         u_color, u_blend);
}
#endif
)PSYX";

int PsyX_DecodeBase64Value(unsigned char value) {
  if (value >= 'A' && value <= 'Z')
    return value - 'A';
  if (value >= 'a' && value <= 'z')
    return value - 'a' + 26;
  if (value >= '0' && value <= '9')
    return value - '0' + 52;
  if (value == '+')
    return 62;
  if (value == '/')
    return 63;
  return -1;
}

std::vector<std::uint8_t> PsyX_DecodeBase64(const char *encoded) {
  std::vector<std::uint8_t> decoded;
  unsigned int accumulator = 0;
  int bits = 0;
  while (*encoded != '\0') {
    const unsigned char value = static_cast<unsigned char>(*encoded++);
    if (value == '=')
      break;
    const int digit = PsyX_DecodeBase64Value(value);
    if (digit < 0)
      continue;
    accumulator = (accumulator << 6) | static_cast<unsigned int>(digit);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      decoded.push_back(
          static_cast<std::uint8_t>((accumulator >> bits) & 0xffU));
    }
  }
  return decoded;
}

std::vector<std::uint8_t> PsyX_DecodePackBits(const char *encoded,
                                              std::size_t expectedSize) {
  const auto packed = PsyX_DecodeBase64(encoded);
  std::vector<std::uint8_t> decoded;
  decoded.reserve(expectedSize);
  std::size_t cursor = 0;
  while (cursor < packed.size() && decoded.size() < expectedSize) {
    const std::uint8_t control = packed[cursor++];
    const std::size_t length = (control & 0x7fU) + 1U;
    if (decoded.size() + length > expectedSize)
      return {};
    if ((control & 0x80U) != 0U) {
      if (cursor >= packed.size())
        return {};
      decoded.insert(decoded.end(), length, packed[cursor++]);
    } else {
      if (cursor + length > packed.size())
        return {};
      decoded.insert(decoded.end(), packed.begin() + cursor,
                     packed.begin() + cursor + length);
      cursor += length;
    }
  }
  if (decoded.size() != expectedSize)
    return {};
  return decoded;
}

GLuint PsyX_CompileSMAAProgram(const char *wrapper) {
  std::string source(kSMAAShaderPrefix);
  for (std::size_t index = 0; index < psyx::smaa::kShaderSourceChunkCount;
       ++index) {
    source.append(psyx::smaa::kShaderSourceChunks[index]);
  }
  source.append(wrapper);
  const GLuint program = GR_Shader_Compile(source.c_str(), 0);
  GLint linked = GL_FALSE;
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (linked != GL_TRUE) {
    glDeleteProgram(program);
    return 0;
  }
  return program;
}

PsyXSMAAProgram PsyX_CreateSMAAProgram(const char *wrapper) {
  PsyXSMAAProgram result;
  result.id = PsyX_CompileSMAAProgram(wrapper);
  if (result.id != 0) {
    result.metrics = glGetUniformLocation(result.id, "u_metrics");
    result.sourceRect = glGetUniformLocation(result.id, "u_source_rect");
  }
  return result;
}

void PsyX_SetSMAATexture(GLuint texture, GLint internalFormat, int width,
                         int height, GLenum format, const void *pixels,
                         GLint filtering) {
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filtering);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filtering);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, format,
               GL_UNSIGNED_BYTE, pixels);
}

bool PsyX_CreateSMAALookupTextures() {
  auto area = PsyX_DecodePackBits(psyx::smaa::kAreaDeltaPackBitsBase64,
                                  psyx::smaa::kAreaDecodedSize);
  auto search = PsyX_DecodeBase64(psyx::smaa::kSearchBase64);
  if (area.size() != psyx::smaa::kAreaDecodedSize ||
      search.size() != psyx::smaa::kSearchDecodedSize) {
    eprinterr("Failed to decode embedded SMAA lookup textures\n");
    return false;
  }

  constexpr std::size_t areaRowBytes =
      psyx::smaa::kAreaWidth * psyx::smaa::kAreaChannels;
  for (std::size_t index = areaRowBytes; index < area.size(); ++index) {
    area[index] =
        static_cast<std::uint8_t>(area[index] + area[index - areaRowBytes]);
  }

  GLint unpackAlignment = 4;
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpackAlignment);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glGenTextures(1, &g_smaa.areaTexture);
  PsyX_SetSMAATexture(g_smaa.areaTexture, GL_RG8, psyx::smaa::kAreaWidth,
                      psyx::smaa::kAreaHeight, GL_RG, area.data(), GL_LINEAR);
  glGenTextures(1, &g_smaa.searchTexture);
  PsyX_SetSMAATexture(g_smaa.searchTexture, GL_R8, psyx::smaa::kSearchWidth,
                      psyx::smaa::kSearchHeight, GL_RED, search.data(),
                      GL_NEAREST);
  glBindTexture(GL_TEXTURE_2D, 0);
  glPixelStorei(GL_UNPACK_ALIGNMENT, unpackAlignment);
  return true;
}

void PsyX_SetSMAASamplers() {
  glUseProgram(g_smaa.edges.id);
  glUniform1i(glGetUniformLocation(g_smaa.edges.id, "u_color"), 0);
  glUniform1i(glGetUniformLocation(g_smaa.edges.id, "u_predication"), 1);
  glUseProgram(g_smaa.depthEdges.id);
  glUniform1i(glGetUniformLocation(g_smaa.depthEdges.id, "u_depth"), 0);
  glUseProgram(g_smaa.weights.id);
  glUniform1i(glGetUniformLocation(g_smaa.weights.id, "u_edges"), 0);
  glUniform1i(glGetUniformLocation(g_smaa.weights.id, "u_area"), 1);
  glUniform1i(glGetUniformLocation(g_smaa.weights.id, "u_search"), 2);
  glUseProgram(g_smaa.neighborhood.id);
  glUniform1i(glGetUniformLocation(g_smaa.neighborhood.id, "u_color"), 0);
  glUniform1i(glGetUniformLocation(g_smaa.neighborhood.id, "u_blend"), 1);
  glUseProgram(0);
}

void PsyX_DestroySMAA() {
  if (g_smaa.edges.id != 0)
    glDeleteProgram(g_smaa.edges.id);
  if (g_smaa.depthEdges.id != 0)
    glDeleteProgram(g_smaa.depthEdges.id);
  if (g_smaa.weights.id != 0)
    glDeleteProgram(g_smaa.weights.id);
  if (g_smaa.neighborhood.id != 0)
    glDeleteProgram(g_smaa.neighborhood.id);
  glDeleteFramebuffers(1, &g_smaa.framebuffer);
  glDeleteVertexArrays(1, &g_smaa.vertexArray);
  glDeleteTextures(1, &g_smaa.areaTexture);
  glDeleteTextures(1, &g_smaa.searchTexture);
  glDeleteTextures(1, &g_smaa.edgesTexture);
  glDeleteTextures(1, &g_smaa.weightsTexture);
  glDeleteTextures(1, &g_smaa.outputTexture);
  g_smaa = {};
}

void PsyX_InitialiseSMAA() {
  if (!g_cfg_smaa)
    return;

  g_smaa.edges = PsyX_CreateSMAAProgram(kSMAAEdgesWrapper);
  g_smaa.depthEdges = PsyX_CreateSMAAProgram(kSMAADepthEdgesWrapper);
  g_smaa.weights = PsyX_CreateSMAAProgram(kSMAAWeightsWrapper);
  g_smaa.neighborhood = PsyX_CreateSMAAProgram(kSMAANeighborhoodWrapper);
  if (g_smaa.edges.id == 0 || g_smaa.depthEdges.id == 0 ||
      g_smaa.weights.id == 0 || g_smaa.neighborhood.id == 0 ||
      !PsyX_CreateSMAALookupTextures()) {
    eprintwarn("SMAA is unavailable; continuing without it\n");
    PsyX_DestroySMAA();
    g_cfg_smaa = 0;
    return;
  }

  PsyX_SetSMAASamplers();
  glGenFramebuffers(1, &g_smaa.framebuffer);
  glGenVertexArrays(1, &g_smaa.vertexArray);
  glGenTextures(1, &g_smaa.edgesTexture);
  glGenTextures(1, &g_smaa.weightsTexture);
  glGenTextures(1, &g_smaa.outputTexture);
  g_smaa.available = true;
  eprintf("*SMAA 1x: enabled (Ultra preset, colour + depth edges)\n");
}

bool PsyX_EnsureSMAATargets(int width, int height) {
  if (g_smaa.width == width && g_smaa.height == height)
    return true;
  if (width <= 0 || height <= 0)
    return false;

  PsyX_SetSMAATexture(g_smaa.edgesTexture, GL_RG8, width, height, GL_RG, NULL,
                      GL_LINEAR);
  PsyX_SetSMAATexture(g_smaa.weightsTexture, GL_RGBA8, width, height, GL_RGBA,
                      NULL, GL_LINEAR);
  PsyX_SetSMAATexture(g_smaa.outputTexture, GL_RGBA8, width, height, GL_RGBA,
                      NULL, GL_NEAREST);
  glBindTexture(GL_TEXTURE_2D, 0);

  glBindFramebuffer(GL_FRAMEBUFFER, g_smaa.framebuffer);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         g_smaa.outputTexture, 0);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    eprintwarn("SMAA framebuffer is incomplete; disabling SMAA\n");
    g_smaa.available = false;
    g_cfg_smaa = 0;
    glBindFramebuffer(GL_FRAMEBUFFER, PsyX_GetNativeDrawFramebuffer());
    return false;
  }
  g_smaa.width = width;
  g_smaa.height = height;
  return true;
}

void PsyX_BindSMAATexture(int unit, GLuint texture) {
  glActiveTexture(GL_TEXTURE0 + unit);
  glBindTexture(GL_TEXTURE_2D, texture);
}

void PsyX_DrawSMAAPass(const PsyXSMAAProgram &program, const float metrics[4],
                       const float sourceRect[4]) {
  glUseProgram(program.id);
  glUniform4fv(program.metrics, 1, metrics);
  glUniform4fv(program.sourceRect, 1, sourceRect);
  glDrawArrays(GL_TRIANGLES, 0, 3);
}

} // namespace

void GR_ApplySceneSMAA(void) {
  if (!g_cfg_smaa || !g_smaa.available || g_nativeFramebufferWidth <= 0 ||
      g_nativeFramebufferHeight <= 0)
    return;
  if (!PsyX_EnsureSMAATargets(g_nativeFramebufferWidth,
                              g_nativeFramebufferHeight))
    return;

  const int savedBlendMode = g_PreviousBlendMode;
  const int savedDepthMode = g_PreviousDepthMode;
  const int savedDepthWrite = g_PreviousDepthWrite;
  const int savedStencilMode = g_PreviousStencilMode;
  const GLboolean savedStencilTest = glIsEnabled(GL_STENCIL_TEST);
  const GLboolean savedScissorTest = glIsEnabled(GL_SCISSOR_TEST);
  const GLboolean savedFramebufferSrgb = glIsEnabled(GL_FRAMEBUFFER_SRGB);
  GLint savedViewport[4] = {};
  glGetIntegerv(GL_VIEWPORT, savedViewport);

  PsyX_ResolveNativeFramebuffer(1);

  glDisable(GL_BLEND);
  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_FRAMEBUFFER_SRGB);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glBindVertexArray(g_smaa.vertexArray);

  const PsyXPresentationViewport renderViewport = PsyX_GetRenderViewport();
  const float width = static_cast<float>(g_nativeFramebufferWidth);
  const float height = static_cast<float>(g_nativeFramebufferHeight);
  const float metrics[4] = {1.0F / width, 1.0F / height, width, height};
  const float sourceRect[4] = {static_cast<float>(renderViewport.x) / width,
                               static_cast<float>(renderViewport.y) / height,
                               static_cast<float>(renderViewport.w) / width,
                               static_cast<float>(renderViewport.h) / height};

  glBindFramebuffer(GL_FRAMEBUFFER, g_smaa.framebuffer);
  glViewport(0, 0, g_nativeFramebufferWidth, g_nativeFramebufferHeight);
  glClearColor(0.0F, 0.0F, 0.0F, 0.0F);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         g_smaa.edgesTexture, 0);
  glClear(GL_COLOR_BUFFER_BIT);
  glViewport(renderViewport.x, renderViewport.y, renderViewport.w,
             renderViewport.h);
  PsyX_BindSMAATexture(0, g_glNativeDepthTexture);
  PsyX_DrawSMAAPass(g_smaa.depthEdges, metrics, sourceRect);

  // Retain either detector's result. Depth catches geometric silhouettes;
  // colour catches alpha-tested sprites, texture edges and depth-free effects.
  glEnable(GL_BLEND);
  glBlendEquation(GL_MAX);
  glBlendFunc(GL_ONE, GL_ONE);
  PsyX_BindSMAATexture(0, g_glNativeColorTexture);
  PsyX_BindSMAATexture(1, g_glNativeDepthTexture);
  PsyX_DrawSMAAPass(g_smaa.edges, metrics, sourceRect);
  glDisable(GL_BLEND);
  glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);

  glViewport(0, 0, g_nativeFramebufferWidth, g_nativeFramebufferHeight);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         g_smaa.weightsTexture, 0);
  glClear(GL_COLOR_BUFFER_BIT);
  glViewport(renderViewport.x, renderViewport.y, renderViewport.w,
             renderViewport.h);
  PsyX_BindSMAATexture(0, g_smaa.edgesTexture);
  PsyX_BindSMAATexture(1, g_smaa.areaTexture);
  PsyX_BindSMAATexture(2, g_smaa.searchTexture);
  PsyX_DrawSMAAPass(g_smaa.weights, metrics, sourceRect);

  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         g_smaa.outputTexture, 0);
  PsyX_BindSMAATexture(0, g_glNativeColorTexture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  PsyX_BindSMAATexture(1, g_smaa.weightsTexture);
  PsyX_DrawSMAAPass(g_smaa.neighborhood, metrics, sourceRect);
  PsyX_BindSMAATexture(0, g_glNativeColorTexture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

  glBindFramebuffer(GL_READ_FRAMEBUFFER, g_smaa.framebuffer);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_glNativeFramebuffer);
  glBlitFramebuffer(
      renderViewport.x, renderViewport.y, renderViewport.x + renderViewport.w,
      renderViewport.y + renderViewport.h, renderViewport.x, renderViewport.y,
      renderViewport.x + renderViewport.w, renderViewport.y + renderViewport.h,
      GL_COLOR_BUFFER_BIT, GL_NEAREST);
  g_nativeFramePostprocessed = 1;
  glBindFramebuffer(GL_FRAMEBUFFER, g_glNativeFramebuffer);

  for (int unit = 2; unit >= 0; --unit)
    PsyX_BindSMAATexture(unit, 0);
  glActiveTexture(GL_TEXTURE0);
  glUseProgram(0);
  glBindVertexArray(0);
  glViewport(savedViewport[0], savedViewport[1], savedViewport[2],
             savedViewport[3]);
  if (savedScissorTest)
    glEnable(GL_SCISSOR_TEST);
  else
    glDisable(GL_SCISSOR_TEST);
  if (savedFramebufferSrgb)
    glEnable(GL_FRAMEBUFFER_SRGB);
  else
    glDisable(GL_FRAMEBUFFER_SRGB);
  if (savedStencilTest)
    glEnable(GL_STENCIL_TEST);
  else
    glDisable(GL_STENCIL_TEST);

  g_PreviousBlendMode = BM_NONE;
  if (savedBlendMode != BM_NONE)
    GR_SetBlendMode(static_cast<BlendMode>(savedBlendMode));
  g_PreviousDepthMode = -1;
  g_PreviousDepthWrite = -1;
  g_PreviousDepthFunc = -1;
  GR_SetDepthState(savedDepthMode, savedDepthWrite);
  g_PreviousStencilMode = savedStencilMode == 0 ? 1 : 0;
  GR_SetStencilMode(savedStencilMode);
  g_PreviousScissorState = savedScissorTest ? 1 : 0;
  g_PreviousShader = -1;
  g_PreviousTextureBlendShader = -1;
  g_PreviousTextureBlendMode = -1;
  g_lastBoundTexture = -1;
}

#else

namespace {
void PsyX_InitialiseSMAA() {}
void PsyX_DestroySMAA() {}
} // namespace
void GR_ApplySceneSMAA(void) {}

#endif
