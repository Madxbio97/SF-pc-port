#define SDL_MAIN_HANDLED
#include <PsyX/PsyX_globals.h>
#include <PsyX/PsyX_public.h>
#include <PsyX/PsyX_render.h>
#include <PsyX/common/glad.h>
#include <SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
extern int g_nativeFramebufferWidth, g_nativeFramebufferHeight;
GLuint shader(GLenum kind, const char *src) {
  auto s = glCreateShader(kind);
  glShaderSource(s, 1, &src, nullptr);
  glCompileShader(s);
  GLint ok;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    std::array<char, 4096U> log{};
    glGetShaderInfoLog(s, static_cast<GLsizei>(log.size()), nullptr,
                       log.data());
    glDeleteShader(s);
    throw std::runtime_error(std::string{"GPU shader compile failed: "} +
                             log.data());
  }
  return s;
}
GLuint program() {
  auto p = glCreateProgram();
  glAttachShader(p, shader(GL_VERTEX_SHADER, R"(#version 330
 uniform float z;
 void main(){ vec2 p=gl_VertexID==0?vec2(-1,-1):gl_VertexID==1?vec2(3,-1):vec2(-1,3);gl_Position=vec4(p,z*2.0-1.0,1);})"));
  glAttachShader(p, shader(GL_FRAGMENT_SHADER, R"(#version 330
 uniform vec4 color;out vec4 fragColor;void main(){fragColor=color;})"));
  glLinkProgram(p);
  GLint ok;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok)
    throw std::runtime_error("link");
  return p;
}
void require(bool v, const char *m) {
  if (!v)
    throw std::runtime_error(m);
}
extern char begin_scene_flag;
void testNestedSceneRetainsInputBoundary() {
  const auto event_type = SDL_RegisterEvents(1);
  require(event_type != static_cast<Uint32>(-1),
          "could not reserve a renderer input-boundary test event");
  SDL_Event event{};
  event.type = event_type;
  event.user.code = 42;
  require(SDL_PushEvent(&event) == 1, "could not queue input-boundary event");
  const auto saved = begin_scene_flag;
  begin_scene_flag = 1;
  const auto opened = PsyX_BeginScene();
  begin_scene_flag = saved;
  require(opened == 0, "nested primitive reopened the scene");
  require(SDL_PeepEvents(&event, 1, SDL_GETEVENT, event_type, event_type) ==
                  1 &&
              event.user.code == 42,
          "nested primitive pumped events after the frame input was sampled");
}

void testActorDepth() {
  auto p = program();
  glUseProgram(p);
  GLuint vao;
  glGenVertexArrays(1, &vao);
  glBindVertexArray(vao);
  g_cfg_pgxpZBuffer = 1;
  for (int samples : {0, 4, 0}) {
    int size = samples == 4 ? 64 : 96;
    g_nativeFramebufferWidth = g_nativeFramebufferHeight = size;
    GLuint fbo, rb[2], resolve, texture;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenRenderbuffers(2, rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb[0]);
    if (samples > 0)
      glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, size,
                                       size);
    else
      glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, size, size);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              GL_RENDERBUFFER, rb[0]);
    glBindRenderbuffer(GL_RENDERBUFFER, rb[1]);
    if (samples > 0)
      glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples,
                                       GL_DEPTH32F_STENCIL8, size, size);
    else
      glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH32F_STENCIL8, size, size);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER, rb[1]);
    require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE,
            "main FBO");
    glGenFramebuffers(1, &resolve);
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size, size, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, nullptr);
    glBindFramebuffer(GL_FRAMEBUFFER, resolve);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           texture, 0);
    require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE,
            "resolve FBO");
    auto pixel = [&] {
      glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resolve);
      auto sc = glIsEnabled(GL_SCISSOR_TEST);
      glDisable(GL_SCISSOR_TEST);
      glBlitFramebuffer(0, 0, size, size, 0, 0, size, size, GL_COLOR_BUFFER_BIT,
                        GL_NEAREST);
      if (sc)
        glEnable(GL_SCISSOR_TEST);
      glBindFramebuffer(GL_READ_FRAMEBUFFER, resolve);
      std::array<unsigned char, 4> c{};
      glReadPixels(size / 2, size / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                   c.data());
      glBindFramebuffer(GL_FRAMEBUFFER, fbo);
      return c;
    };
    auto draw = [&](float z, float r, float g, float b, float alpha) {
      glUniform1f(glGetUniformLocation(p, "z"), z);
      glUniform4f(glGetUniformLocation(p, "color"), r, g, b, alpha);
      GR_SetDepthState(1, 0);
      glDrawArrays(GL_TRIANGLES, 0, 3);
    };
    for (bool wall : {false, true}) {
      glBindFramebuffer(GL_FRAMEBUFFER, fbo);
      glViewport(0, 0, size, size);
      glDisable(GL_SCISSOR_TEST);
      GR_SetDepthState(1, 1);
      glClearColor(0, 0, 1, 1);
      glClearDepth(wall ? 0.8 : 0.25);
      glClearStencil(90);
      glStencilMask(255);
      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT |
              GL_STENCIL_BUFFER_BIT);
      glEnable(GL_BLEND);
      glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      glEnable(GL_SCISSOR_TEST);
      glScissor(0, 0, size, size);
      require(GR_BeginActorSelfDepth() == 1, "begin self depth");
      draw(0.5f, 1, 0, 0, 0.5f);
      draw(0.75f, 0, 1, 0, 0.5f);
      GR_BlendActorSelfDepth();
      draw(0.5f, 1, 0, 0, 0.5f);
      draw(0.75f, 0, 1, 0, 0.5f);
      GR_EndActorSelfDepth();
      auto color = pixel();
      std::cout << "MSAA=" << samples << " wall=" << wall
                << " actor RGB=" << int(color[0]) << "," << int(color[1]) << ","
                << int(color[2]) << "\n";
      require(color[0] <= 1,
              "rear red actor surface leaked through green front");
      require(wall ? color[1] == 0 && color[2] == 255
                   : abs(int(color[1]) - 128) <= 1 &&
                         abs(int(color[2]) - 128) <= 1,
              "wrong actor opacity or wall occlusion");
      // A later particle behind the actor must see the original world depth.
      draw(0.6f, 1, 1, 1, 0.5f);
      color = pixel();
      require(wall ? color[0] == 0 : abs(int(color[0]) - 128) <= 1,
              "world depth was not restored for later effects");
      require(glIsEnabled(GL_SCISSOR_TEST), "scissor not restored");
      require(glGetError() == GL_NO_ERROR, "GL error");
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteFramebuffers(1, &resolve);
    glDeleteRenderbuffers(2, rb);
    glDeleteTextures(1, &texture);
  }
}

extern const char *gpu_shader_4;
extern const char *gpu_shader_8;
extern const char *gpu_shader_16;
// This compiles the production PSX vertex shader and reads the actual
// projection installed for a scene split, rather than mirroring its constants.
extern void GR_InitialisePSXShaders();
void testActorNearProjection() {
  GR_InitialisePSXShaders();
  g_windowWidth = 640;
  g_windowHeight = 480;
  GR_SetTexture(0, TF_16_BIT, TEXTURE_FILTER_NEAREST);
  const RECT16 bounds{0, 0, 384, 240};
  GR_SetOffscreenState(&bounds, 0);
  GLint program{};
  glGetIntegerv(GL_CURRENT_PROGRAM, &program);
  const auto location =
      glGetUniformLocation(static_cast<GLuint>(program), "Projection3D");
  require(location >= 0, "scene shader lost its depth projection");
  std::array<float, 16U> projection{};
  glGetUniformfv(static_cast<GLuint>(program), location, projection.data());
  for (const auto guest_z : {2.0F, 4.0F, 16.0F, 32.0F, 128.0F}) {
    const auto z = guest_z / 128.0F;
    const auto clip = projection[10] * z + projection[14];
    require(clip >= -z && clip <= z,
            "GPU near plane sliced a positive camera-close head surface");
  }
  require(glGetError() == GL_NO_ERROR, "actor near projection GL error");
}

void testSceneTone() {
  // Compile the shipping vertex and fragment sources as well as the controlled
  // fullscreen fixture below. No game window or gameplay input is involved.
  for (const char *src : {gpu_shader_4, gpu_shader_8, gpu_shader_16}) {
    auto linked = GR_Shader_Compile(src, 1);
    GLint ok{};
    glGetProgramiv(linked, GL_LINK_STATUS, &ok);
    require(ok == GL_TRUE, "shipping PSX shader did not link");
    glDeleteProgram(linked);
  }
  const char *vs = R"(#version 330
 out vec4 v_texcoord;out vec4 v_color;
 flat out vec4 v_page_clut;flat out float v_alias_page;flat out vec4 v_texbounds;
 uniform float world;uniform float shade;
 void main(){
  vec2 p=gl_VertexID==0?vec2(-1,-1):gl_VertexID==1?vec2(3,-1):vec2(-1,3);
  gl_Position=vec4(p,0,1);v_texcoord=vec4(0.0,0.0,1.0,world);
  v_color=vec4(vec3(shade/255.0),1);
  v_page_clut=vec4(0.00025,0.00025,0,0);v_alias_page=0;v_texbounds=vec4(0);
 })";
  auto fs = std::string(R"(#version 330
 #define texture2D texture
 #define FLAT flat
 in vec4 v_texcoord;in vec4 v_color;
 flat in vec4 v_page_clut;flat in float v_alias_page;flat in vec4 v_texbounds;
 out vec4 fragColor;
 )") + gpu_shader_16;
  auto p = glCreateProgram();
  glAttachShader(p, shader(GL_VERTEX_SHADER, vs));
  glAttachShader(p, shader(GL_FRAGMENT_SHADER, fs.c_str()));
  glLinkProgram(p);
  GLint ok{};
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  require(ok == GL_TRUE, "tone fixture did not link");
  glUseProgram(p);
  GLuint tex[3], fbo;
  glGenTextures(3, tex);
  std::vector<std::uint8_t> vram(1024 * 512 * 2);
  vram[0] = vram[1] = 255;
  std::vector<std::uint8_t> lut(256 * 256 * 4);
  for (unsigned v = 0; v < 65536; ++v) {
    for (unsigned ch = 0; ch < 3; ++ch) {
      unsigned c = (v >> (ch * 5)) & 31;
      lut[v * 4 + ch] = std::uint8_t((c << 3) | (c >> 2));
    }
    lut[v * 4 + 3] = (v & 0x8000) ? 128 : 0;
  }
  for (int i = 0; i < 3; ++i) {
    glActiveTexture(GL_TEXTURE0 + i);
    glBindTexture(GL_TEXTURE_2D, tex[i]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, i == 0 ? GL_RG8 : GL_RGBA8,
                 i == 0   ? 1024
                 : i == 1 ? 256
                          : 32,
                 i == 0   ? 512
                 : i == 1 ? 256
                          : 32,
                 0, i == 0 ? GL_RG : GL_RGBA, GL_UNSIGNED_BYTE,
                 i == 0   ? vram.data()
                 : i == 1 ? lut.data()
                          : nullptr);
  }
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         tex[2], 0);
  require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE,
          "tone FBO");
  glUniform1i(glGetUniformLocation(p, "s_texture"), 0);
  glUniform1i(glGetUniformLocation(p, "s_aliasTexture"), 0);
  glUniform1i(glGetUniformLocation(p, "s_rgLut"), 1);
  glUniform1i(glGetUniformLocation(p, "textureFilterMode"), 0);
  glViewport(0, 0, 32, 32);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_DITHER);
  auto render = [&](float scene, float shade, int blend) {
    glUniform1f(glGetUniformLocation(p, "world"), scene);
    glUniform1f(glGetUniformLocation(p, "shade"), shade);
    glUniform1i(glGetUniformLocation(p, "textureBlendMode"), blend);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    std::vector<std::uint8_t> result(32 * 32 * 4);
    glReadPixels(0, 0, 32, 32, GL_RGBA, GL_UNSIGNED_BYTE, result.data());
    return result;
  };
  const auto ui = render(0, 96, 0), world = render(1, 96, 0),
             repeat = render(1, 96, 0), emissive = render(1, 96, 2),
             black = render(1, 0, 0), white = render(1, 255, 0);
  require(world == repeat, "scene grain changed between identical frames");
  require(ui == emissive, "world tone altered additive light colours");
  unsigned minimum = 255, maximum = 0;
  double total = 0;
  for (size_t i = 0; i < world.size(); i += 4) {
    require(ui[i] == 99 && ui[i + 1] == 99 && ui[i + 2] == 99,
            "scene tone altered exact HUD RGB555");
    require(world[i] == world[i + 1] && world[i] == world[i + 2],
            "monochrome dither introduced colour noise");
    require(world[i + 3] == ui[i + 3], "scene tone changed material alpha");
    require(black[i] == 0 && white[i] == 255,
            "scene grain lifted black or clipped white");
    minimum = std::min(minimum, unsigned(world[i]));
    maximum = std::max(maximum, unsigned(world[i]));
    total += world[i];
  }
  require(maximum > minimum && maximum - minimum <= 6,
          "smooth scene grain missing or stronger than its subtle range");
  require(total / 1024.0 < 85 && total / 1024.0 > 72,
          "scene curve did not retain the darker contrast grade");
  std::array<bool, 256U> tones{};
  double previous = -1.0;
  for (int shade = 0; shade < 256; ++shade) {
    const auto ramp = render(1, static_cast<float>(shade), 0);
    double sum{};
    for (std::size_t pixel{}; pixel < ramp.size(); pixel += 4)
      sum += ramp[pixel];
    const auto mean = sum / 1024.0;
    require(mean >= previous && mean - previous <= 3.5,
            "scene gradient reversed or retained a hard palette jump");
    tones[static_cast<std::size_t>(std::lround(mean))] = true;
    previous = mean;
  }
  require(std::count(tones.begin(), tones.end(), true) > 160,
          "filtered scene gradients collapsed back to 32 colour steps");
  // BASEEXT/BASEEXT2's original FUN_800d6f50 supplies 8-bit Gouraud
  // atmosphere, including colours as low as (3,4,3). RGB555 truncation before
  // additive blending turns that shallow gradient into wide coloured bands.
  constexpr int gradient_width = 1024, gradient_height = 32;
  glActiveTexture(GL_TEXTURE2);
  glBindTexture(GL_TEXTURE_2D, tex[2]);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, gradient_width, gradient_height, 0,
               GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glViewport(0, 0, gradient_width, gradient_height);
  // Use both shipping stages and the actual Gouraud vertex attributes here:
  // testing only a fragment-only marker would miss lost dither metadata.
  auto atmosphere_program = GR_Shader_Compile(gpu_shader_16, 1);
  glUseProgram(atmosphere_program);
  const std::array<float, 16> identity{1, 0, 0, 0, 0, 1, 0, 0,
                                       0, 0, 1, 0, 0, 0, 0, 1};
  glUniformMatrix4fv(glGetUniformLocation(atmosphere_program, "Projection"), 1,
                     GL_FALSE, identity.data());
  glUniform2f(glGetUniformLocation(atmosphere_program, "PresentationScale"), 1,
              1);
  glUniform1i(glGetUniformLocation(atmosphere_program, "s_texture"), 0);
  glUniform1i(glGetUniformLocation(atmosphere_program, "s_aliasTexture"), 0);
  glUniform1i(glGetUniformLocation(atmosphere_program, "s_rgLut"), 1);
  glUniform1i(glGetUniformLocation(atmosphere_program, "textureBlendMode"), 2);
  glUniform1i(glGetUniformLocation(atmosphere_program, "textureFilterMode"), 0);
  GLuint atmosphere_vao{}, atmosphere_vbo{};
  glGenVertexArrays(1, &atmosphere_vao);
  glBindVertexArray(atmosphere_vao);
  glGenBuffers(1, &atmosphere_vbo);
  glBindBuffer(GL_ARRAY_BUFFER, atmosphere_vbo);
  const std::array<float, 18> vertices{
      -1, -1, 3.0F / 255,   4.0F / 255,   3.0F / 255,   1,
      3,  -1, 231.0F / 255, 232.0F / 255, 231.0F / 255, 1,
      -1, 3,  3.0F / 255,   4.0F / 255,   3.0F / 255,   1};
  glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices.data(),
               GL_STATIC_DRAW);
  const auto position = glGetAttribLocation(atmosphere_program, "a_position");
  const auto colour = glGetAttribLocation(atmosphere_program, "a_color");
  const auto texcoord = glGetAttribLocation(atmosphere_program, "a_texcoord");
  require(position >= 0 && colour >= 0 && texcoord >= 0,
          "shipping Gouraud vertex attributes are missing");
  glEnableVertexAttribArray(static_cast<GLuint>(position));
  glVertexAttribPointer(static_cast<GLuint>(position), 2, GL_FLOAT, GL_FALSE,
                        6 * sizeof(float), nullptr);
  glEnableVertexAttribArray(static_cast<GLuint>(colour));
  glVertexAttribPointer(static_cast<GLuint>(colour), 4, GL_FLOAT, GL_FALSE,
                        6 * sizeof(float),
                        reinterpret_cast<const void *>(2 * sizeof(float)));
  // MakeTexcoordTriangleZero(..., 1): solid colour with Gouraud dithering.
  glVertexAttrib4f(static_cast<GLuint>(texcoord), 0, 0, 1, 1);
  glClearColor(24.0F / 255.0F, 38.0F / 255.0F, 56.0F / 255.0F, 1.0F);
  glClear(GL_COLOR_BUFFER_BIT);
  glEnable(GL_BLEND);
  glBlendEquation(GL_FUNC_ADD);
  glBlendFunc(GL_ONE, GL_ONE);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  std::vector<std::uint8_t> gradient(gradient_width * gradient_height * 4);
  glReadPixels(0, 0, gradient_width, gradient_height, GL_RGBA, GL_UNSIGNED_BYTE,
               gradient.data());
  std::array<bool, 256U> gradient_tones{};
  for (int x = 0; x < gradient_width; ++x) {
    std::array<double, 3U> mean{};
    for (int y = 0; y < gradient_height; ++y)
      for (std::size_t c = 0; c < 3U; ++c)
        mean[c] += gradient[(y * gradient_width + x) * 4 + c];
    for (auto &channel : mean)
      channel /= gradient_height;
    const double expected = (x + 0.5) * 114.0 / gradient_width;
    require(std::abs(mean[0] - (27.0 + expected)) < 1.5 &&
                std::abs(mean[1] - (42.0 + expected)) < 1.5 &&
                std::abs(mean[2] - (59.0 + expected)) < 1.5,
            "Kazakhstan atmosphere retained RGB555 colour contour bands");
    gradient_tones[static_cast<std::size_t>(std::lround(mean[0]))] = true;
  }
  require(std::count(gradient_tones.begin(), gradient_tones.end(), true) > 100,
          "Gouraud atmosphere lost its continuous low-amplitude gradient");
  glDisable(GL_BLEND);
  glDeleteBuffers(1, &atmosphere_vbo);
  glDeleteVertexArrays(1, &atmosphere_vao);
  glDeleteProgram(atmosphere_program);
  require(glGetError() == GL_NO_ERROR, "tone GL error");
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDeleteFramebuffers(1, &fbo);
  glDeleteTextures(3, tex);
  glDeleteProgram(p);
}
int main() {
  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    std::cout << "No SDL video driver, skipping GPU tests\n";
    return 77;
  }
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  auto *w = SDL_CreateWindow("Renderer subsystem tests", 0, 0, 64, 64,
                             SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
  auto ctx = w ? SDL_GL_CreateContext(w) : nullptr;
  if (!ctx || !gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)) {
    if (ctx)
      SDL_GL_DeleteContext(ctx);
    if (w)
      SDL_DestroyWindow(w);
    SDL_Quit();
    std::cout << "No OpenGL 3.3 context, skipping GPU tests\n";
    return 77;
  }
  int result = 0;
  try {
    testNestedSceneRetainsInputBoundary();
    testActorNearProjection();
    testActorDepth();
    testSceneTone();
    std::cout << "Actor self-depth and scene tone GPU tests passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << "\n";
    result = 1;
  }
  SDL_GL_DeleteContext(ctx);
  SDL_DestroyWindow(w);
  SDL_Quit();
  return result;
}
