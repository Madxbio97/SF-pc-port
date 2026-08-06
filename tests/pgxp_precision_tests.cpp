#include "psx/libgte.h"
#include "PsyX/PsyX_render.h"

#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

void require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error{message.data()};
  }
}

bool near(double first, double second, double tolerance = 0.00001) {
  return std::abs(first - second) <= tolerance;
}

MATRIX packedMatrix() {
  MATRIX matrix{};
  matrix.m[0][0] = 4096;
  matrix.m[0][2] = 41;
  matrix.m[1][1] = 4096;
  matrix.m[2][0] = -41;
  matrix.m[2][2] = 4096;
  matrix.t[0] = 10;
  matrix.t[1] = 20;
  matrix.t[2] = 1000;
  return matrix;
}

int project(MATRIX &matrix, SVECTOR &vertex) {
  SetRotMatrix(&matrix);
  SetTransMatrix(&matrix);
  int screen{};
  long depth_cue{};
  long flags{};
  return RotTransPers(&vertex, &screen, &depth_cue, &flags);
}

void testExactTransformSideChannel() {
  InitGeom();
  SetGeomOffset(192, 120);
  SetGeomScreen(320);

  auto matrix = packedMatrix();
  auto vertex = SVECTOR{100, 50, 200, 0};
  PGXP_ClearCache();
  const auto legacy_depth = project(matrix, vertex);

  PGXP_ClearCache();
  constexpr std::array exact_rotation{
      0.99995, 0.0, 0.01001, 0.0, 1.0, 0.0, -0.01001, 0.0, 0.99995,
  };
  constexpr std::array exact_translation{10.25, 20.5, 1000.75};
  constexpr std::array exact_vertex{100.25, 49.75, 200.125};
  PGXP_MatrixRegister(&matrix, exact_rotation.data());
  PGXP_MatrixRegisterTranslation(&matrix, exact_translation.data());

  // Register one producer address and project its packed copy. This exercises
  // the value recovery used by return-by-value model/clip vertices.
  auto registered_vertex = vertex;
  PGXP_VectorRegister(&registered_vertex, exact_vertex.data());
  auto projected_vertex = registered_vertex;
  const auto precise_depth = project(matrix, projected_vertex);

  const auto expected_x =
      exact_translation[0] + exact_rotation[0] * exact_vertex[0] +
      exact_rotation[1] * exact_vertex[1] + exact_rotation[2] * exact_vertex[2];
  const auto expected_y =
      exact_translation[1] + exact_rotation[3] * exact_vertex[0] +
      exact_rotation[4] * exact_vertex[1] + exact_rotation[5] * exact_vertex[2];
  const auto expected_z =
      exact_translation[2] + exact_rotation[6] * exact_vertex[0] +
      exact_rotation[7] * exact_vertex[1] + exact_rotation[8] * exact_vertex[2];

  require(precise_depth == legacy_depth, "PGXP changed the integer GTE depth");
  require(near(g_FP_SXYZ2.px, expected_x / 128.0), "Exact PGXP X was lost");
  require(near(g_FP_SXYZ2.py, expected_y / 128.0), "Exact PGXP Y was lost");
  require(near(g_FP_SXYZ2.pz, expected_z / 128.0), "Exact PGXP Z was lost");

  const auto cache_end = PGXP_GetIndex(0);
  require(cache_end > 0U, "PGXP did not cache the projected vertex");
  const auto lookup = PGXP_LOOKUP_VALUE(g_FP_SXYZ2.x, g_FP_SXYZ2.y);
  PGXPVData cached{};
  require(PGXP_GetCacheData(&cached, lookup,
                            static_cast<ushort>(cache_end - 1U)) != 0,
          "PGXP exact cache hint was not found");
  const auto expected_screen_x = 192.0 + expected_x * 320.0 / expected_z;
  const auto expected_screen_y = 120.0 + expected_y * 320.0 / expected_z;
  require(near(cached.sx, expected_screen_x, 0.0001),
          "Precise screen X was quantized");
  require(near(cached.sy, expected_screen_y, 0.0001),
          "Precise screen Y was quantized");
}

void testExactProjectionIsMarkedAcrossGteDivideSaturation() {
  InitGeom();
  SetGeomOffset(192, 120);
  SetGeomScreen(320);

  MATRIX matrix{};
  matrix.m[0][0] = 4096;
  matrix.m[1][1] = 4096;
  matrix.m[2][2] = 4096;
  constexpr std::array exact_rotation{
      1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0,
  };
  constexpr std::array exact_translation{0.0, 0.0, 0.0};
  auto vertex = SVECTOR{32, 0, 64, 0};

  PGXP_ClearCache();
  PGXP_MatrixRegister(&matrix, exact_rotation.data());
  PGXP_MatrixRegisterTranslation(&matrix, exact_translation.data());
  static_cast<void>(project(matrix, vertex));

  PGXPVData cached{};
  require(PGXP_GetCacheDataExact(&cached, 0U) != 0,
          "Close exact projection was not cached");
  require(cached.exact_projection != 0U,
          "Close native projection lost its exact-index marker");
  require(near(cached.sx, 352.0, 0.0001),
          "Close native projection retained the saturated GTE quotient");
  const auto packed_lookup =
      static_cast<uint>(PGXP_LOOKUP_VALUE(g_FP_SXYZ2.x, g_FP_SXYZ2.y));
  require(cached.lookup == packed_lookup,
          "Close exact projection cache key was detached from its vertex");
}

void testExactCacheHintWinsDuplicateLookup() {
  PGXP_ClearCache();
  constexpr uint duplicate_lookup = 0x12345678U;
  PGXPVData older{};
  older.lookup = duplicate_lookup;
  older.px = 1.0F;
  PGXPVData newer = older;
  newer.px = 2.0F;
  static_cast<void>(PGXP_EmitCacheData(&older));
  const auto newer_index = PGXP_EmitCacheData(&newer);

  PGXPVData result{};
  require(PGXP_GetCacheData(&result, duplicate_lookup, newer_index) != 0,
          "PGXP duplicate lookup was not found");
  require(near(result.px, newer.px), "PGXP ignored the exact cache hint");
}

void testExactCacheIndexDoesNotDependOnRoundedScreenLookup() {
  PGXP_ClearCache();
  PGXPVData vertex{};
  vertex.lookup = 0x11223344U;
  vertex.px = 7.5F;
  const auto index = PGXP_EmitCacheData(&vertex);

  PGXPVData result{};
  require(PGXP_GetCacheDataExact(&result, index) != 0,
          "PGXP exact cache index was not found");
  require(near(result.px, vertex.px),
          "PGXP exact cache index selected another vertex");
}

void testRoundedScreenCollisionKeepsPrimitiveIdentity() {
  InitGeom();
  SetGeomOffset(192, 120);
  SetGeomScreen(320);

  MATRIX matrix{};
  matrix.m[0][0] = 4096;
  matrix.m[1][1] = 4096;
  matrix.m[2][2] = 4096;
  constexpr std::array exact_rotation{
      1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0,
  };
  constexpr std::array exact_translation{0.0, 0.0, 0.0};

  // Both vertices produce the same packed PS1 XY lookup key, but their
  // sub-pixel positions are intentionally different. A rounded-key cache
  // search can therefore attach the second wall tile to the first tile's
  // projection and make their shared edge move as the camera approaches.
  auto first_vertex = SVECTOR{100, 0, 1000, 0};
  auto second_vertex = first_vertex;
  constexpr std::array first_exact{100.10, 0.0, 1000.0};
  constexpr std::array second_exact{100.20, 0.0, 1000.0};

  PGXP_ClearCache();
  PGXP_MatrixRegister(&matrix, exact_rotation.data());
  PGXP_MatrixRegisterTranslation(&matrix, exact_translation.data());
  PGXP_VectorRegister(&first_vertex, first_exact.data());
  PGXP_VectorRegister(&second_vertex, second_exact.data());

  const auto first_depth = project(matrix, first_vertex);
  const auto second_depth = project(matrix, second_vertex);
  require(first_depth == second_depth,
          "Rounded-screen collision changed integer GTE depth");

  PGXPVData first{};
  PGXPVData second{};
  require(PGXP_GetCacheDataExact(&first, 0U) != 0 &&
              PGXP_GetCacheDataExact(&second, 1U) != 0,
          "Rounded-screen collision lost an exact cache entry");
  require(first.lookup == second.lookup,
          "Test vertices did not collide in the packed PS1 lookup key");
  require(!near(first.sx, second.sx, 0.0001),
          "Exact cache collapsed distinct sub-pixel wall vertices");

  PGXPVData hinted_first{};
  PGXPVData hinted_second{};
  require(PGXP_GetCacheData(&hinted_first, first.lookup, 0U) != 0 &&
              PGXP_GetCacheData(&hinted_second, second.lookup, 1U) != 0,
          "Rounded-screen collision could not resolve packet hints");
  require(near(hinted_first.sx, first.sx) && near(hinted_second.sx, second.sx),
          "Rounded-screen collision crossed primitive identities");
}

void testPreciseTextureCoordsStayAttachedToTransformBatch() {
  PGXP_ClearCache();
  PGXPVData first{};
  PGXPVData second{};
  PGXPVData third{};
  static_cast<void>(PGXP_EmitCacheData(&first));
  static_cast<void>(PGXP_EmitCacheData(&second));
  const auto third_index = PGXP_EmitCacheData(&third);
  constexpr std::array precise_uv{
      12.25F, 31.75F, 63.5F, 32.125F, 14.875F, 95.625F,
  };
  constexpr std::array texture_bounds{10.4F, 20.5F, 70.1F, 100.9F};
  constexpr uint packed_bounds =
      10U | (20U << 8U) | (71U << 16U) | (101U << 24U);
  PGXP_SetLastTextureCoords(precise_uv.data(), 3, texture_bounds.data());

  for (ushort vertex = 0; vertex < 3; ++vertex) {
    PGXPVData cached{};
    require(PGXP_GetCacheDataExact(
                &cached, static_cast<ushort>(third_index - 2U + vertex)) != 0,
            "PGXP precise UV cache entry was not found");
    require(cached.precise_texcoord != 0U, "PGXP precise UV flag was lost");
    require(
        cached.texture_bounds == packed_bounds,
        "PGXP shared texture bounds were detached from the clipped polygon");
    require(near(cached.precise_u, precise_uv[vertex * 2U]) &&
                near(cached.precise_v, precise_uv[vertex * 2U + 1U]),
            "PGXP precise UV was detached from its projected vertex");
  }
}

void testCacheSaturatesWithoutWrapping() {
  PGXP_ClearCache();
  PGXPVData vertex{};
  vertex.lookup = 0x55667788U;
  constexpr auto sentinel = static_cast<ushort>(0xffffU);
  for (unsigned int index = 0; index < 0xffffU; ++index) {
    vertex.px = static_cast<float>(index);
    require(PGXP_EmitCacheData(&vertex) == static_cast<ushort>(index),
            "PGXP cache saturated before its packet sentinel");
  }
  require(PGXP_EmitCacheData(&vertex) == sentinel,
          "PGXP cache did not report saturation");
  require(PGXP_EmitCacheData(&vertex) == sentinel,
          "PGXP cache cursor wrapped after saturation");
  require(PGXP_GetIndex(0) == sentinel,
          "PGXP saturated cache exposed a wrapped packet index");

  PGXPVData first{};
  require(PGXP_GetCacheDataExact(&first, 0U) != 0,
          "PGXP saturation destroyed the first cache entry");
  require(near(first.px, 0.0),
          "PGXP saturation overwrote an early cache entry");
}

void testTextureFilterSelection() {
  require(GR_ResolveTextureFilterMode(TEXTURE_FILTER_NEAREST, 1, 1, 1) ==
              TEXTURE_FILTER_NEAREST,
          "Explicit nearest primitives were filtered");
  require(GR_ResolveTextureFilterMode(TEXTURE_FILTER_BILINEAR, 1, 1, 1) ==
              TEXTURE_FILTER_BILINEAR,
          "Bilinear UI filtering was not selected");
  require(GR_ResolveTextureFilterMode(TEXTURE_FILTER_BILINEAR, 0, 1, 1) ==
              TEXTURE_FILTER_NEAREST,
          "World-only filtering leaked into UI rendering");
  require(GR_ResolveTextureFilterMode(TEXTURE_FILTER_WORLD_ANISOTROPIC, 1, 0,
                                      0) ==
              TEXTURE_FILTER_BILINEAR,
          "World rendering ignored the bilinear selection");
  require(GR_ResolveTextureFilterMode(TEXTURE_FILTER_WORLD_ANISOTROPIC, 0, 0,
                                      0) ==
              TEXTURE_FILTER_NEAREST,
          "World filtering remained forced on");
  require(GR_ResolveTextureFilterMode(TEXTURE_FILTER_WORLD_ANISOTROPIC, 0, 1,
                                      0) ==
              TEXTURE_FILTER_WORLD_TRILINEAR,
          "Independent trilinear filtering was not selected");
  require(GR_ResolveTextureFilterMode(TEXTURE_FILTER_WORLD_ANISOTROPIC, 0, 1,
                                      1) ==
              TEXTURE_FILTER_WORLD_ANISOTROPIC,
          "Independent anisotropic filtering was not selected");
}

void testTrueColorExpansion() {
  require(GR_Expand5BitColor(0U) == 0U,
          "True-color expansion raised black");
  require(GR_Expand5BitColor(31U) == 255U,
          "True-color expansion did not reach white");
  require(GR_Expand5BitColor(16U) == 132U,
          "True-color expansion did not replicate low bits");
  for (unsigned int value = 1U; value < 32U; ++value) {
    require(GR_Expand5BitColor(static_cast<u_char>(value)) >
                GR_Expand5BitColor(static_cast<u_char>(value - 1U)),
            "True-color expansion is not monotonic");
  }
}

void testReversedDepthProjection() {
  constexpr float near_plane = 0.25F;
  constexpr float far_plane = 1000.0F;
  float scale{};
  float bias{};
  GR_CalculateReversedDepthProjection(
      near_plane, far_plane, &scale, &bias);
  const auto ndc_depth = [scale, bias](float distance) {
    return scale + bias / distance;
  };
  require(near(ndc_depth(near_plane), 1.0, 0.00001),
          "Reversed depth did not map the near plane to +1");
  require(near(ndc_depth(far_plane), -1.0, 0.00001),
          "Reversed depth did not map the far plane to -1");
  require(ndc_depth(1.0F) > ndc_depth(100.0F),
          "Reversed depth is not monotonic towards the camera");
}

void testWorldDepthClassificationIsCameraStable() {
  std::array<GrVertex, 3> triangle{};
  for (auto &vertex : triangle) {
    vertex.scr_h = 1.0F;
    vertex.z = 1000.0F;
  }

  require(GR_UsesWorldDepth(triangle.data(), 1) != 0,
          "Camera-facing world triangle lost depth testing");

  triangle[0].z = 999.0F;
  triangle[2].z = 1001.0F;
  require(GR_UsesWorldDepth(triangle.data(), 1) != 0,
          "Turning a world triangle changed depth classification");

  triangle[1].scr_h = 0.0F;
  require(GR_UsesWorldDepth(triangle.data(), 1) == 0,
          "Screen-space triangle entered the world depth buffer");
  triangle[1].scr_h = 1.0F;
  require(GR_UsesWorldDepth(triangle.data(), 0) == 0,
          "Disabled world depth was ignored");
}

} // namespace

int main() {
  try {
    testExactTransformSideChannel();
    testExactProjectionIsMarkedAcrossGteDivideSaturation();
    testExactCacheHintWinsDuplicateLookup();
    testExactCacheIndexDoesNotDependOnRoundedScreenLookup();
    testRoundedScreenCollisionKeepsPrimitiveIdentity();
    testPreciseTextureCoordsStayAttachedToTransformBatch();
    testCacheSaturatesWithoutWrapping();
    testTextureFilterSelection();
    testTrueColorExpansion();
    testReversedDepthProjection();
    testWorldDepthClassificationIsCameraStable();
    std::cout << "PGXP precision tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "PGXP precision test failed: " << error.what() << '\n';
    return 1;
  }
}
