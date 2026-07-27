#include "sf/game/dynamic_lighting.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error{message};
  }
}

sf::game::PersistentDynamicLightState lamp(std::uint32_t id, double x,
                                           bool destroyed = false) {
  return sf::game::PersistentDynamicLightState{
      sf::game::DynamicLightKind::street_lamp,
      {x, -700.0, 0.0},
      id,
      true,
      true,
      true,
      destroyed,
  };
}

void testGuestLampLifetimeIsAuthoritative() {
  const std::array sources{lamp(7U, 0.0), lamp(8U, 100.0, true)};
  const auto frame = sf::game::buildDynamicLightFrame(
      sources, {}, sf::game::DynamicLightPoint{});
  require(frame.count == 1U && frame.active().front().source_id == 7U,
          "Destroyed retail lamp still emitted dynamic light");

  auto unconfirmed = lamp(9U, 0.0);
  unconfirmed.identity_confirmed = false;
  auto dormant = lamp(10U, 0.0);
  dormant.resident = false;
  const std::array invalid{unconfirmed, dormant};
  require(sf::game::buildDynamicLightFrame(invalid, {},
                                           sf::game::DynamicLightPoint{})
                  .count == 0U,
          "Unconfirmed or non-resident lamp did not fail closed");

  auto streamed_destroyed = lamp(11U, 120000.0, true);
  streamed_destroyed.resident = false;
  const std::array distant{streamed_destroyed};
  require(sf::game::buildDynamicLightFrame(
              distant, {}, sf::game::DynamicLightPoint{-120000.0, 0.0, 0.0})
                  .count == 0U,
          "Destroyed lamp revived while streamed out");
  streamed_destroyed.resident = true;
  const std::array returned{streamed_destroyed};
  require(sf::game::buildDynamicLightFrame(
              returned, {}, sf::game::DynamicLightPoint{120000.0, 0.0, 0.0})
                  .count == 0U,
          "Destroyed lamp revived after returning into range");
}

void testTransientLightsAreExactAndFinite() {
  using sf::game::GameplayEffectType;
  const std::array effects{
      sf::game::TransientDynamicLightState{GameplayEffectType::muzzle_flash,
                                           {10.0, 20.0, 30.0},
                                           1U,
                                           1.0,
                                           1U,
                                           1U,
                                           true},
      sf::game::TransientDynamicLightState{GameplayEffectType::explosion,
                                           {40.0, 50.0, 60.0},
                                           2U,
                                           1.0,
                                           4U,
                                           8U,
                                           true},
      sf::game::TransientDynamicLightState{GameplayEffectType::blood_spray,
                                           {0.0, 0.0, 0.0},
                                           3U,
                                           1.0,
                                           1U,
                                           1U,
                                           true},
      sf::game::TransientDynamicLightState{GameplayEffectType::explosion,
                                           {0.0, 0.0, 0.0},
                                           4U,
                                           1.0,
                                           1U,
                                           1U,
                                           false},
  };
  const auto frame = sf::game::buildDynamicLightFrame(
      {}, effects, sf::game::DynamicLightPoint{});
  require(frame.count == 2U &&
              frame.active()[0].kind ==
                  sf::game::DynamicLightKind::muzzle_flash &&
              frame.active()[1].kind == sf::game::DynamicLightKind::explosion,
          "Transient effect classification accepted an inexact visual");
  require(std::abs(frame.active()[1].intensity - 0.17) < 0.000001,
          "Explosion lifetime did not fade its dynamic light");
}

void testFlashlightConeIsDirectionalAndAuthoritative() {
  auto source = sf::game::DirectionalDynamicLightState{
      sf::game::DynamicLightKind::flashlight,
      {0.0, 0.0, 0.0},
      {0.0, 0.0, 4.0},
      0x21U,
      true,
      true,
  };
  const std::array enabled{source};
  const auto frame = sf::game::buildDynamicLightFrame(
      {}, {}, sf::game::DynamicLightPoint{}, enabled);
  const auto forward = sf::game::sampleDynamicLighting(
      frame, sf::game::DynamicLightPoint{0.0, 0.0, 600.0});
  const auto feathered_edge = sf::game::sampleDynamicLighting(
      frame, sf::game::DynamicLightPoint{250.0, 0.0, 600.0});
  const auto outside = sf::game::sampleDynamicLighting(
      frame, sf::game::DynamicLightPoint{600.0, 0.0, 600.0});
  const auto behind = sf::game::sampleDynamicLighting(
      frame, sf::game::DynamicLightPoint{0.0, 0.0, -600.0});
  require(frame.count == 1U && frame.active().front().directional &&
              forward.blue > 0.0 && outside.blue == 0.0 && behind.blue == 0.0,
          "Flashlight escaped its normalized forward cone");
  require(forward.red == forward.green && forward.green == forward.blue &&
              feathered_edge.red > 0.0 && feathered_edge.red < forward.red &&
              feathered_edge.red == feathered_edge.green &&
              feathered_edge.green == feathered_edge.blue,
          "Flashlight lost its neutral soft-edged illumination");

  source.enabled = false;
  const std::array disabled{source};
  require(sf::game::buildDynamicLightFrame(
              {}, {}, sf::game::DynamicLightPoint{}, disabled)
                  .count == 0U,
          "Disabled retail flashlight still emitted light");
}

void testRadialSamplingAndNeutralModulation() {
  const std::array sources{lamp(1U, 0.0)};
  const auto frame = sf::game::buildDynamicLightFrame(
      sources, {}, sf::game::DynamicLightPoint{});
  const auto centre = sf::game::sampleDynamicLighting(
      frame, sf::game::DynamicLightPoint{0.0, -700.0, 0.0});
  const auto boundary = sf::game::sampleDynamicLighting(
      frame, sf::game::DynamicLightPoint{1400.0, -700.0, 0.0});
  require(centre.red > centre.green && centre.green > centre.blue &&
              centre.blue > 0.0,
          "Street lamp lost its warm radial light profile");
  require(boundary.red == 0.0 && boundary.green == 0.0 && boundary.blue == 0.0,
          "Dynamic light escaped its bounded radius");
  const auto lit = sf::game::applyDynamicLighting({128U, 128U, 128U}, centre);
  require(lit.red > lit.green && lit.green > lit.blue && lit.blue > 128U,
          "Dynamic illumination did not brighten neutral texture modulation");
  require(sf::game::applyDynamicLighting(
              {30U, 40U, 50U},
              {std::numeric_limits<double>::quiet_NaN(), -1.0, 0.0}) ==
              sf::game::DynamicLightVertexColor{30U, 40U, 50U},
          "Invalid lighting input changed authored vertex color");
}

void testSurfaceLightingRejectsBackFaces() {
  const std::array sources{lamp(1U, 0.0)};
  const auto frame = sf::game::buildDynamicLightFrame(
      sources, {}, sf::game::DynamicLightPoint{});
  const auto front = sf::game::sampleDynamicLighting(
      frame, sf::game::DynamicLightPoint{0.0, 0.0, 0.0},
      sf::game::DynamicLightPoint{0.0, -1.0, 0.0});
  const auto back = sf::game::sampleDynamicLighting(
      frame, sf::game::DynamicLightPoint{0.0, 0.0, 0.0},
      sf::game::DynamicLightPoint{0.0, 1.0, 0.0});
  require(front.red > front.green && front.green > front.blue &&
              back.red == 0.0 && back.green == 0.0 && back.blue == 0.0,
          "Dynamic light leaked through the back of a surface");
}

void testActorShadowTracksEligibleDynamicLight() {
  const std::array sources{lamp(1U, 0.0)};
  const auto frame = sf::game::buildDynamicLightFrame(
      sources, {}, sf::game::DynamicLightPoint{});
  const auto projection = sf::game::selectDynamicShadowProjection(
      frame, {300.0, 0.0, 0.0}, {0.0, 1.0, 0.0});
  require(projection.source_driven && projection.ray_direction.x > 0.0 &&
              projection.ray_direction.y > 0.0 && projection.darkness > 0.18,
          "Actor shadow ignored the strongest overhead dynamic light");

  const auto point = sf::game::projectDynamicShadowPoint(
      {300.0, -400.0, 0.0}, {300.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, projection);
  require(point && point->x > 300.0 && std::abs(point->y + 3.0) < 0.000001,
          "Actor silhouette did not project onto its biased support plane");
}

void testActorShadowRejectsLowLightAndMalformedPlane() {
  auto low = lamp(2U, 0.0);
  low.position = {-300.0, 0.0, 0.0};
  const std::array sources{low};
  const auto frame = sf::game::buildDynamicLightFrame(
      sources, {}, sf::game::DynamicLightPoint{});
  const auto projection = sf::game::selectDynamicShadowProjection(
      frame, {0.0, 0.0, 0.0}, {0.0, 1.0, 0.0});
  require(!projection.source_driven,
          "Floor-height light stretched an actor shadow at grazing angle");
  require(!sf::game::projectDynamicShadowPoint({0.0, -100.0, 0.0}, {}, {},
                                               projection),
          "Actor shadow accepted a malformed support plane");
}

void testActorShadowBlendsSourcesAndBoundsStretch() {
  const std::array balanced{lamp(3U, -350.0), lamp(4U, 350.0)};
  const auto frame = sf::game::buildDynamicLightFrame(
      balanced, {}, sf::game::DynamicLightPoint{});
  const auto projection = sf::game::selectDynamicShadowProjection(
      frame, {0.0, 0.0, 0.0}, {0.0, 1.0, 0.0});
  require(projection.source_driven &&
              std::abs(projection.ray_direction.x) < 0.2,
          "Balanced shadow sources snapped to one light instead of blending");

  const auto grazing = sf::game::projectDynamicShadowPoint(
      {0.0, -400.0, 0.0}, {}, {0.0, 1.0, 0.0},
      sf::game::DynamicShadowProjection{{1.0, 0.1, 0.0}, 0.3, true});
  require(grazing && grazing->x <= 340.000001 && grazing->x >= 339.999999 &&
              std::abs(grazing->y + 3.0) < 0.000001,
          "Grazing actor shadow exceeded its bounded silhouette stretch");
}

void testPersistentAnimationUsesGuestTime() {
  auto lightbar = lamp(0U, 0.0);
  lightbar.kind = sf::game::DynamicLightKind::police_lightbar;
  const std::array police{lightbar};
  const auto blue = sf::game::buildDynamicLightFrame(
      police, {}, sf::game::DynamicLightPoint{}, {}, 0U);
  const auto red = sf::game::buildDynamicLightFrame(
      police, {}, sf::game::DynamicLightPoint{}, {}, 2U);
  require(blue.active().front().color.blue > blue.active().front().color.red &&
              red.active().front().color.red > red.active().front().color.blue,
          "Police lightbar did not alternate its dynamic red/blue pulse");

  auto flame = lamp(7U, 0.0);
  flame.kind = sf::game::DynamicLightKind::steady_fire;
  const std::array fire{flame};
  const auto first = sf::game::buildDynamicLightFrame(
      fire, {}, sf::game::DynamicLightPoint{}, {}, 8U);
  const auto repeated = sf::game::buildDynamicLightFrame(
      fire, {}, sf::game::DynamicLightPoint{}, {}, 8U);
  const auto advanced = sf::game::buildDynamicLightFrame(
      fire, {}, sf::game::DynamicLightPoint{}, {}, 10U);
  require(first.active().front().intensity ==
                  repeated.active().front().intensity &&
              first.active().front().intensity !=
                  advanced.active().front().intensity,
          "Fire flicker is host-frame dependent or remains static");
}

void testBoundedSelectionKeepsTransientAndNearestLamps() {
  std::array<sf::game::PersistentDynamicLightState,
             sf::game::maximum_dynamic_lights + 4U>
      sources{};
  for (std::size_t index = 0U; index < sources.size(); ++index) {
    sources[index] = lamp(static_cast<std::uint32_t>(index),
                          static_cast<double>(index) * 100.0);
  }
  const std::array effects{sf::game::TransientDynamicLightState{
      sf::game::GameplayEffectType::muzzle_flash,
      {9000.0, 0.0, 0.0},
      0xf00dU,
      1.0,
      1U,
      1U,
      true,
  }};
  const auto frame = sf::game::buildDynamicLightFrame(
      sources, effects, sf::game::DynamicLightPoint{});
  require(frame.count == sf::game::maximum_dynamic_lights,
          "Dynamic light frame exceeded or underfilled its fixed capacity");
  require(frame.active().front().source_id == 0xf00dU,
          "Transient light lost priority over persistent sources");
  require(frame.active().back().source_id == 30U,
          "Bounded selection did not retain the nearest persistent lamps");
}

void testBoundedSelectionKeepsDirectionalLight() {
  std::array<sf::game::PersistentDynamicLightState,
             sf::game::maximum_dynamic_lights>
      sources{};
  for (std::size_t index = 0U; index < sources.size(); ++index) {
    sources[index] = lamp(static_cast<std::uint32_t>(index),
                          static_cast<double>(index) * 10.0);
  }
  const std::array directional{sf::game::DirectionalDynamicLightState{
      sf::game::DynamicLightKind::flashlight,
      {50000.0, 0.0, 0.0},
      {0.0, 0.0, 1.0},
      0xf1a5U,
      true,
      true,
  }};
  const auto frame = sf::game::buildDynamicLightFrame(
      sources, {}, sf::game::DynamicLightPoint{}, directional);
  require(frame.count == sf::game::maximum_dynamic_lights &&
              std::ranges::any_of(frame.active(),
                                  [](const auto &light) {
                                    return light.source_id == 0xf1a5U &&
                                           light.directional;
                                  }),
          "Bounded selection discarded the authoritative flashlight");
}

sf::game::RetailVertexLightState retailLight() {
  auto light = sf::game::RetailVertexLightState{};
  light.matrix.rotation = {4096, 0, 0, 0, 4096, 0, 0, 0, 4096};
  light.matrix.translation = {10, 20, 30};
  light.extent = 80;
  light.screen_shift = 14U;
  light.depth_shift = 3U;
  light.threshold = 0;
  light.channel_mask = 0x00ffffffU;
  return light;
}

void testRetailVertexLightSzBranchesAreExact() {
  const std::array lights{retailLight()};
  const auto sample = [&](double guest_depth) {
    return sf::game::applyRetailVertexLightingPacked(
        0x00c82010U, lights,
        sf::game::DynamicLightPoint{10.0, 20.0, 30.0 + guest_depth}, 320);
  };
  require(sample(256.0) == 0x00ff8070U,
          "Retail vertex light lost its SZ >= 256 depth falloff");
  require(sample(255.0) == 0x00ffbeaeU && sample(128.0) == 0x00ffbeaeU,
          "Retail vertex light lost its 128..255 adjusted-shift branch");
  require(sample(127.0) == 0x00ffbcacU,
          "Retail vertex light lost its SZ < 128 adjusted-shift branch");
  require(sample(0.0) == 0x00c82010U && sample(-256.0) == 0x00c82010U,
          "Retail vertex light illuminated a zero/behind-light vertex");
}

void testRetailGmdBackColorUsesNeutralTextureScale() {
  using sf::game::retailGmdBackColorModulation;
  require(retailGmdBackColorModulation({4096, 4096, 4096}) ==
                  sf::game::DynamicLightVertexColor{128U, 128U, 128U} &&
              retailGmdBackColorModulation({2048, 2048, 2048}) ==
                  sf::game::DynamicLightVertexColor{64U, 64U, 64U} &&
              retailGmdBackColorModulation({8192, 8192, 8192}) ==
                  sf::game::DynamicLightVertexColor{255U, 255U, 255U} &&
              retailGmdBackColorModulation({-1, -1, -1}) ==
                  sf::game::DynamicLightVertexColor{0U, 0U, 0U},
          "Retail GMD back-light did not darken/brighten around GPU neutral");
}

void testSceneTriangleLightingInterpolatesFloorColor() {
  using sf::game::DynamicLightPoint;
  using sf::game::DynamicLightVertexColor;
  const std::array vertices{
      DynamicLightPoint{0.0, 100.0, 0.0},
      DynamicLightPoint{100.0, 200.0, 0.0},
      DynamicLightPoint{0.0, 300.0, 100.0},
  };
  const std::array colors{
      DynamicLightVertexColor{16U, 32U, 48U},
      DynamicLightVertexColor{80U, 96U, 112U},
      DynamicLightVertexColor{144U, 160U, 176U},
  };
  const auto sample = sf::game::sampleSceneTriangleLighting(vertices, colors,
                                                            {25.0, 0.0, 25.0});
  require(sample && sample->color == DynamicLightVertexColor{64U, 80U, 96U} &&
              std::abs(sample->surface_y - 175.0) < 0.0001,
          "Weapon-crate floor lighting did not barycentrically interpolate");
  require(!sf::game::sampleSceneTriangleLighting(vertices, colors,
                                                 {120.0, 0.0, 120.0}) &&
              !sf::game::sampleSceneTriangleLighting(
                  {DynamicLightPoint{0.0, 0.0, 0.0},
                   DynamicLightPoint{0.0, 100.0, 0.0},
                   DynamicLightPoint{0.0, 0.0, 100.0}},
                  colors, {}),
          "Scene lighting accepted an outside point or vertical wall");
}

void testRetailVertexLightUsesExactGteProjection() {
  auto source = retailLight();
  std::array lights{source};

  // H >= 2*SZ takes the GTE divide-overflow quotient (0x1ffff), rather than
  // an ideal coordinate*H/SZ division.
  require(sf::game::applyRetailVertexLightingPacked(
              0x00010203U, lights, {41.0, 20.0, 33.0}, 17) == 0x00616263U,
          "Retail light lost the GTE divide-overflow projection");

  // The non-overflow path uses the PS1 UNR reciprocal approximation. At
  // H=1/SZ=7 it projects guest Y=7 to zero, while ideal division yields one.
  require(sf::game::applyRetailVertexLightingPacked(
              0x00010203U, lights, {10.0, 27.0, 37.0}, 1) == 0x009d9e9fU,
          "Retail light replaced the GTE UNR quotient with ideal division");

  // RTPT clamps IR before multiplying by the quotient.
  source = retailLight();
  source.matrix.rotation[4] = 8192;
  source.extent = 2508;
  source.matrix.translation = {0, 0, 0};
  lights[0] = source;
  require(sf::game::applyRetailVertexLightingPacked(
              0x00010203U, lights, {0.0, 20000.0, 20000.0}, 1) == 0x00090a0bU,
          "Retail light did not apply RTPT IR saturation");
}

void testRetailVertexLightPreservesGsSetViewRounding() {
  auto source = retailLight();
  source.matrix.rotation[4] = 2048;
  source.matrix.translation = {0, 1, 0};
  std::array lights{source};

  // GsSetView2 first rounds -ApplyMatrixLV(R^T,t), then RTPT rounds the
  // transformed vertex. Combining both operations into R^T*(v-t) changes
  // this boundary vertex from projected Y=1 to Y=0.
  require(sf::game::applyRetailVertexLightingPacked(
              0x00010203U, lights, {0.0, 2.0, 7.0}, 17) == 0x009b9c9dU,
          "Retail light collapsed the two GsSetView2/RTPT rounding stages");

  // ApplyMatrixLV itself is a high/remainder pair of GTE passes. Both passes
  // store MAC, so the valid 42421 remainder must not clamp to IR=32767.
  source.matrix.rotation = {2896, -2896, 0, 2896, 2896, 0, 0, 0, 4096};
  source.matrix.translation = {30000, 30000, 0};
  lights[0] = source;
  require(sf::game::applyRetailVertexLightingPacked(0x00010203U, lights,
                                                    {30000.0, 30000.0, 32.0},
                                                    320) == 0x009d9e9fU,
          "Retail light incorrectly clamped ApplyMatrixLV remainder MAC");
}

void testRetailVertexLightShiftSemanticsAreExact() {
  auto source = retailLight();
  source.screen_shift = 31U;
  std::array lights{source};
  require(sf::game::applyRetailVertexLightingPacked(
              0x00010203U, lights, {10.0, 20.0, 157.0}, 320) == 0x00a1a2a3U,
          "Retail near-depth shift did not wrap through the MIPS low bits");

  lights[0].screen_shift = 0U;
  require(sf::game::applyRetailVertexLightingPacked(
              0x00010203U, lights, {10.0, 20.0, 286.0}, 320) == 0x00616263U,
          "Retail zero screen shift was incorrectly rejected");
}

void testRetailVertexLightThresholdMaskAndSaturation() {
  auto source = retailLight();
  source.threshold = 49;
  std::array lights{source};
  const auto point = sf::game::DynamicLightPoint{10.0, 20.0, 286.0};
  require(sf::game::applyRetailVertexLightingPacked(0x00c82010U, lights, point,
                                                    320) == 0x00c82010U,
          "Retail vertex-light threshold accepted a weaker sample");
  lights[0].threshold = 48;
  lights[0].channel_mask = 0x0000ff00U;
  require(sf::game::applyRetailVertexLightingPacked(0x00c82010U, lights, point,
                                                    320) == 0x00c88010U,
          "Retail vertex-light channel mask leaked into disabled channels");
  lights[0].channel_mask = 0x00ffffffU;
  require(sf::game::applyRetailVertexLightingPacked(0x00f0e0d0U, lights, point,
                                                    320) == 0x00ffffffU,
          "Retail FUN_800d3cb4 color addition did not saturate per channel");
  require(sf::game::applyRetailVertexLightingPacked(0x80c82010U, lights, point,
                                                    320) == 0x80c82010U,
          "Retail signed primitive-color guard was not preserved");
}

void testRetailVertexLightDirectionAndMalformedRecordsFailClosed() {
  auto source = retailLight();
  source.flags = 1U;
  std::array lights{source};
  const auto reversed = sf::game::DynamicLightPoint{10.0, 20.0, -226.0};
  require(sf::game::applyRetailVertexLightingPacked(
              0x00010203U, lights, reversed, 320) == 0x00616263U,
          "Attached retail flashlight did not reverse its X/Z basis");

  lights[0].screen_shift = 32U;
  require(sf::game::applyRetailVertexLightingPacked(
              0x00010203U, lights, reversed, 320) == 0x00010203U,
          "Malformed retail vertex-light shift did not fail closed");
  lights[0] = retailLight();
  const auto nan = std::numeric_limits<double>::quiet_NaN();
  require(sf::game::applyRetailVertexLightingPacked(
              0x00010203U, lights, {nan, 0.0, 0.0}, 320) == 0x00010203U &&
              sf::game::applyRetailVertexLightingPacked(
                  0x00010203U, lights, {10.0, 20.0, 286.0}, 0) == 0x00010203U,
          "Invalid retail light projection input did not fail closed");

  std::array<sf::game::RetailVertexLightState,
             sf::game::maximum_retail_vertex_lights + 1U>
      overflow{};
  require(sf::game::applyRetailVertexLightingPacked(
              0x00010203U, overflow, {10.0, 20.0, 286.0}, 320) == 0x00010203U,
          "Oversized retail light list escaped its fixed guest capacity");
}

void testRetailVertexLightRayUsesGuestMatrixAndAttachedAxis() {
  auto source = retailLight();
  source.flags = 1U;
  const auto attached = sf::game::retailVertexLightRay(source);
  require(
      attached &&
          attached->origin == sf::game::DynamicLightPoint{10.0, 20.0, 30.0} &&
          attached->direction == sf::game::DynamicLightPoint{0.0, 0.0, -1.0},
      "Attached flashlight ray diverged from its retail light matrix");

  source.flags = 0U;
  const auto detached = sf::game::retailVertexLightRay(source);
  require(detached &&
              detached->direction == sf::game::DynamicLightPoint{0.0, 0.0, 1.0},
          "Detached retail light ray used the attached X/Z reversal");
  source.matrix.rotation[8] = 0;
  require(!sf::game::retailVertexLightRay(source),
          "Degenerate retail light matrix produced a presentation ray");
}

void testMissionFlashlightRecordLightsItsRetailForwardAxis() {
  auto source = sf::game::RetailVertexLightState{};
  source.matrix.rotation = {
      -4048, -376, -511, -449, 4026, 578, 449, 627, -4022,
  };
  source.matrix.translation = {-544, -6775, -4584};
  source.flags = 1U;
  source.extent = 80;
  source.screen_shift = 14U;
  source.depth_shift = 6U;
  source.threshold = 0;
  source.channel_mask = 0x00ffffffU;
  const auto ray = sf::game::retailVertexLightRay(source);
  require(ray.has_value(), "Mission flashlight record lost its retail ray");

  const auto point = [&](double distance) {
    return sf::game::DynamicLightPoint{
        ray->origin.x + ray->direction.x * distance,
        ray->origin.y + ray->direction.y * distance,
        ray->origin.z + ray->direction.z * distance,
    };
  };
  const std::array lights{source};
  constexpr auto base = std::uint32_t{0x00202020U};
  require(sf::game::applyRetailVertexLightingPacked(base, lights, point(512.0),
                                                    320) != base,
          "Mission flashlight did not illuminate its retail forward axis");
  require(sf::game::applyRetailVertexLightingPacked(base, lights, point(-512.0),
                                                    320) == base,
          "Mission flashlight illuminated geometry behind its retail axis");
}

void testCaveFlashlightUsesRetailWorldYAxis() {
  auto source = sf::game::RetailVertexLightState{};
  source.matrix.rotation = {
      1646, 711, -3684, -449, 4026, 578, 3725, 167, 1696,
  };
  source.matrix.translation = {-1235, -2201, -276};
  source.flags = 1U;
  source.extent = 90;
  source.screen_shift = 15U;
  source.depth_shift = 6U;
  source.threshold = 0;
  source.channel_mask = 0x00ffffffU;
  const std::array lights{source};
  constexpr auto base = std::uint32_t{0x00282060U};

  // CAVE2 EMD coordinates and the retail MATRIX use the same Y-down render
  // space. Re-negating the vertex here sends the entire cone off the level.
  require(sf::game::applyRetailVertexLightingPacked(
              base, lights, {196.0, -2385.0, -1087.0}, 320) == 0x005c5494U &&
              sf::game::applyRetailVertexLightingPacked(
                  base, lights, {196.0, 2385.0, -1087.0}, 320) == base,
          "CAVE2 flashlight diverged from retail world Y coordinates");
}

void testFlashlightSurfaceRayIsTwoSidedAndBounded() {
  const auto triangle = sf::game::DynamicLightSurfaceTriangle{
      {-100.0, -100.0, 500.0},
      {100.0, -100.0, 500.0},
      {0.0, 100.0, 500.0},
  };
  const auto hit =
      sf::game::dynamicLightSurfaceHit({}, {0.0, 0.0, 2.0}, triangle, 1000.0);
  require(hit && std::abs(hit->distance - 500.0) < 0.000001 &&
              hit->normal.z < 0.0,
          "Flashlight ray did not hit the nearest front-facing surface");
  const auto reverse = sf::game::dynamicLightSurfaceHit(
      {0.0, 0.0, 1000.0}, {0.0, 0.0, -1.0}, triangle, 1000.0);
  require(reverse && reverse->normal.z > 0.0,
          "Flashlight surface hit incorrectly culled a back face");
  require(
      !sf::game::dynamicLightSurfaceHit({}, {0.0, 0.0, 1.0}, triangle, 400.0) &&
          !sf::game::dynamicLightSurfaceHit({}, {1.0, 0.0, 0.0}, triangle,
                                            1000.0),
      "Flashlight ray escaped its range or accepted a parallel surface");
}

void testFlashlightSegmentBoundsBroadphaseIsBounded() {
  const auto bounds = sf::game::DynamicLightBounds{
      {-20.0, -10.0, 400.0},
      {20.0, 10.0, 600.0},
  };
  require(sf::game::dynamicLightSegmentIntersectsBounds({}, {0.0, 0.0, 2.0},
                                                        700.0, bounds) &&
              !sf::game::dynamicLightSegmentIntersectsBounds(
                  {}, {0.0, 0.0, 1.0}, 300.0, bounds) &&
              !sf::game::dynamicLightSegmentIntersectsBounds(
                  {}, {1.0, 0.0, 0.0}, 700.0, bounds),
          "Flashlight segment broadphase accepted an unreachable model");
  require(sf::game::dynamicLightSegmentIntersectsBounds(
              {0.0, 0.0, 500.0}, {1.0, 0.0, 0.0}, 1.0, bounds),
          "Flashlight broadphase rejected an origin inside its bounds");
  auto malformed = bounds;
  malformed.minimum.x = 30.0;
  require(!sf::game::dynamicLightSegmentIntersectsBounds({}, {0.0, 0.0, 1.0},
                                                         700.0, malformed),
          "Flashlight broadphase accepted malformed bounds");
}

} // namespace

int main() {
  try {
    testGuestLampLifetimeIsAuthoritative();
    testTransientLightsAreExactAndFinite();
    testFlashlightConeIsDirectionalAndAuthoritative();
    testRadialSamplingAndNeutralModulation();
    testSurfaceLightingRejectsBackFaces();
    testActorShadowTracksEligibleDynamicLight();
    testActorShadowRejectsLowLightAndMalformedPlane();
    testActorShadowBlendsSourcesAndBoundsStretch();
    testPersistentAnimationUsesGuestTime();
    testBoundedSelectionKeepsTransientAndNearestLamps();
    testBoundedSelectionKeepsDirectionalLight();
    testRetailGmdBackColorUsesNeutralTextureScale();
    testSceneTriangleLightingInterpolatesFloorColor();
    testRetailVertexLightSzBranchesAreExact();
    testRetailVertexLightUsesExactGteProjection();
    testRetailVertexLightPreservesGsSetViewRounding();
    testRetailVertexLightShiftSemanticsAreExact();
    testRetailVertexLightThresholdMaskAndSaturation();
    testRetailVertexLightDirectionAndMalformedRecordsFailClosed();
    testRetailVertexLightRayUsesGuestMatrixAndAttachedAxis();
    testMissionFlashlightRecordLightsItsRetailForwardAxis();
    testCaveFlashlightUsesRetailWorldYAxis();
    testFlashlightSurfaceRayIsTwoSidedAndBounded();
    testFlashlightSegmentBoundsBroadphaseIsBounded();
    std::cout << "Dynamic lighting tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "Dynamic lighting tests failed: " << error.what() << '\n';
    return 1;
  }
}
