// A faded actor needs its nearest surface once, while world depth must remain
// unchanged for smoke, glass and other transparent effects drawn afterwards.
namespace {
int g_actorDepthPass;
#if defined(RENDERER_OGL)
GLuint g_actorDepthFramebuffer;
GLuint g_actorDepthRenderbuffer;
int g_actorDepthWidth;
int g_actorDepthHeight;
int g_actorDepthSamples{-1};
GLenum g_actorDepthFormat;
GLint g_actorDepthSource;
GLint g_actorDepthRead;
GLboolean g_actorDepthColorMask[4];
GLboolean g_actorDepthSavedTest;
GLboolean g_actorDepthSavedWrite;

void PsyX_CopyActorDepth(GLuint source, GLuint destination) {
  const auto scissor = glIsEnabled(GL_SCISSOR_TEST);
  glDisable(GL_SCISSOR_TEST);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, source);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, destination);
  glBlitFramebuffer(0, 0, g_actorDepthWidth, g_actorDepthHeight, 0, 0,
                    g_actorDepthWidth, g_actorDepthHeight, GL_DEPTH_BUFFER_BIT,
                    GL_NEAREST);
  if (scissor)
    glEnable(GL_SCISSOR_TEST);
}

void PsyX_DestroyActorDepth() {
  glDeleteFramebuffers(1, &g_actorDepthFramebuffer);
  glDeleteRenderbuffers(1, &g_actorDepthRenderbuffer);
  g_actorDepthFramebuffer = g_actorDepthRenderbuffer = 0;
  g_actorDepthWidth = g_actorDepthHeight = 0;
  g_actorDepthSamples = -1;
  g_actorDepthPass = 0;
}
#else
void PsyX_DestroyActorDepth() {}
#endif
} // namespace

float GR_ActorSelfDepthNearPlane() { return 1.0f; }

int GR_BeginActorSelfDepth() {
#if defined(RENDERER_OGL)
  if (!g_cfg_pgxpZBuffer || g_actorDepthPass != 0 ||
      g_nativeFramebufferWidth <= 0 || g_nativeFramebufferHeight <= 0)
    return 0;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &g_actorDepthSource);
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &g_actorDepthRead);
  GLint samples;
  glGetIntegerv(GL_SAMPLES, &samples);
  if (!g_actorDepthFramebuffer)
    glGenFramebuffers(1, &g_actorDepthFramebuffer);
  if (!g_actorDepthRenderbuffer)
    glGenRenderbuffers(1, &g_actorDepthRenderbuffer);
  glBindFramebuffer(GL_FRAMEBUFFER, g_actorDepthFramebuffer);
  if (g_actorDepthWidth != g_nativeFramebufferWidth ||
      g_actorDepthHeight != g_nativeFramebufferHeight ||
      g_actorDepthSamples != samples ||
      g_actorDepthFormat != g_nativeDepthInternalFormat) {
    GLint renderbuffer;
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &renderbuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, g_actorDepthRenderbuffer);
    if (samples > 1)
      glRenderbufferStorageMultisample(
          GL_RENDERBUFFER, samples, g_nativeDepthInternalFormat,
          g_nativeFramebufferWidth, g_nativeFramebufferHeight);
    else
      glRenderbufferStorage(GL_RENDERBUFFER, g_nativeDepthInternalFormat,
                            g_nativeFramebufferWidth,
                            g_nativeFramebufferHeight);
    glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER, g_actorDepthRenderbuffer);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    g_actorDepthWidth = g_nativeFramebufferWidth;
    g_actorDepthHeight = g_nativeFramebufferHeight;
    g_actorDepthSamples = samples;
    g_actorDepthFormat = g_nativeDepthInternalFormat;
  }
  const auto complete =
      glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  if (complete)
    PsyX_CopyActorDepth(g_actorDepthSource, g_actorDepthFramebuffer);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, g_actorDepthRead);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_actorDepthSource);
  if (!complete)
    return 0;
  glGetBooleanv(GL_COLOR_WRITEMASK, g_actorDepthColorMask);
  glGetBooleanv(GL_DEPTH_WRITEMASK, &g_actorDepthSavedWrite);
  g_actorDepthSavedTest = glIsEnabled(GL_DEPTH_TEST);
  glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
  g_actorDepthPass = 1;
  GR_SetDepthState(1, 1);
  return 1;
#else
  return 0;
#endif
}

void GR_BlendActorSelfDepth() {
#if defined(RENDERER_OGL)
  if (g_actorDepthPass != 1)
    return;
  g_actorDepthPass = 2;
  glColorMask(g_actorDepthColorMask[0], g_actorDepthColorMask[1],
              g_actorDepthColorMask[2], g_actorDepthColorMask[3]);
  GR_SetDepthState(1, 0);
#endif
}

void GR_EndActorSelfDepth() {
#if defined(RENDERER_OGL)
  if (!g_actorDepthPass)
    return;
  PsyX_CopyActorDepth(g_actorDepthFramebuffer, g_actorDepthSource);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, g_actorDepthRead);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_actorDepthSource);
  glColorMask(g_actorDepthColorMask[0], g_actorDepthColorMask[1],
              g_actorDepthColorMask[2], g_actorDepthColorMask[3]);
  g_actorDepthPass = 0;
  GR_SetDepthState(g_actorDepthSavedTest, g_actorDepthSavedWrite);
#endif
}
