#include "sf/game/dynamic_lighting.hpp"
#include "sf/platform/retail_vertex_light_presentation.hpp"
#include "sf/platform/world_object_shadow_policy.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>

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
              frame.active().front().radius == 2800.0 &&
              frame.active().front().intensity > 0.60 &&
              forward.functional_blue > 0.0 && outside.functional_blue == 0.0 &&
              behind.functional_blue == 0.0,
          "Flashlight escaped its normalized forward cone");
  require(forward.functional_red == forward.functional_green &&
              forward.functional_green == forward.functional_blue &&
              feathered_edge.functional_red > 0.0 &&
              feathered_edge.functional_red < forward.functional_red &&
              feathered_edge.functional_red ==
                  feathered_edge.functional_green &&
              feathered_edge.functional_green == feathered_edge.functional_blue,
          "Flashlight lost its neutral soft-edged illumination");

  source.enabled = false;
  const std::array disabled{source};
  require(sf::game::buildDynamicLightFrame(
              {}, {}, sf::game::DynamicLightPoint{}, disabled)
                  .count == 0U,
          "Disabled retail flashlight still emitted light");
}

void testFunctionalDirectionalLightRevealsBlackTwoSidedReceivers() {
  for (const auto kind : {sf::game::DynamicLightKind::flashlight,
                          sf::game::DynamicLightKind::spotlight}) {
    const std::array directional{sf::game::DirectionalDynamicLightState{
        kind,
        {0.0, 0.0, 0.0},
        {0.0, 0.0, 1.0},
        static_cast<std::uint32_t>(kind),
        true,
        true,
    }};
    const auto frame = sf::game::buildDynamicLightFrame(
        {}, {}, sf::game::DynamicLightPoint{}, directional);
    const auto front = sf::game::sampleDynamicLighting(frame, {0.0, 0.0, 600.0},
                                                       {0.0, 0.0, -1.0});
    const auto reversed = sf::game::sampleDynamicLighting(
        frame, {0.0, 0.0, 600.0}, {0.0, 0.0, 1.0});
    require(front.red == 0.0 && front.green == 0.0 && front.blue == 0.0 &&
                front.functional_red > 0.0 &&
                front.functional_red == reversed.functional_red &&
                front.functional_green == reversed.functional_green &&
                front.functional_blue == reversed.functional_blue,
            "Functional directional light used normal-masked modulation");

    const auto lit = sf::game::applyDynamicLighting({0U, 0U, 0U}, reversed);
    require(lit.red > 0U && lit.green > 0U && lit.blue > 0U,
            "Functional directional light could not reveal a black receiver");
  }
}

void testAmbientLightStillPreservesAuthoredBlackness() {
  const std::array sources{lamp(1U, 0.0)};
  const auto frame = sf::game::buildDynamicLightFrame(
      sources, {}, sf::game::DynamicLightPoint{});
  const auto modulation = sf::game::sampleDynamicLighting(
      frame, sf::game::DynamicLightPoint{0.0, -700.0, 0.0});
  require(modulation.red > 0.0 && modulation.functional_red == 0.0 &&
              sf::game::applyDynamicLighting({0U, 0U, 0U}, modulation) ==
                  sf::game::DynamicLightVertexColor{0U, 0U, 0U},
          "Ambient lamp bypassed the authored-darkness exposure mask");
}

void testRadialSamplingAndNeutralModulation() {
  const std::array sources{lamp(1U, 0.0)};
  const auto frame = sf::game::buildDynamicLightFrame(
      sources, {}, sf::game::DynamicLightPoint{});
  const auto centre = sf::game::sampleDynamicLighting(
      frame, sf::game::DynamicLightPoint{0.0, -700.0, 0.0});
  const auto boundary = sf::game::sampleDynamicLighting(
      frame, sf::game::DynamicLightPoint{1200.0, -700.0, 0.0});
  require(frame.active().front().radius > 1100.0 &&
              frame.active().front().radius < 1200.0,
          "Native dynamic-light radius scale regressed");
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

void testGameplayGammaDarkensOnlyValidSceneLighting() {
  const auto base = sf::game::DynamicLightVertexColor{32U, 128U, 224U};
  const auto graded = sf::game::applyDynamicLighting(base, {});
  require(graded.red < base.red && graded.green < base.green &&
              graded.blue < base.blue,
          "Native gameplay gamma no longer darkens scene midtones");
  require(sf::game::applyDynamicLighting(
              base, {std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0}) ==
              base,
          "Invalid light input unexpectedly applied the gameplay gamma");
}

void testDynamicLightSoftKneePreservesRetailHeadroom() {
  const auto base = sf::game::DynamicLightVertexColor{248U, 240U, 232U};
  const auto lit = sf::game::applyDynamicLighting(base, {64.0, 64.0, 64.0});
  require(lit.red > base.red && lit.green > base.green &&
              lit.blue > base.blue && lit.red < 255U && lit.green < 255U &&
              lit.blue < 255U && lit.red > lit.green && lit.green > lit.blue,
          "Dynamic soft knee clipped or flattened bright retail colours");

  require(sf::game::applyDynamicLighting(
              base, {std::numeric_limits<double>::quiet_NaN(), 0.5, 0.5}) ==
              base,
          "NaN dynamic modulation partially changed authored colour");
  require(sf::game::applyDynamicLighting(
              base, {0.5, std::numeric_limits<double>::infinity(), 0.5}) ==
              base,
          "Infinite dynamic modulation changed authored colour");
  require(sf::game::applyDynamicLighting(base, {0.5, -0.01, 0.5}) == base,
          "Negative dynamic modulation changed authored colour");
}

void testDynamicLightPreservesAuthoredDarkness() {
  const auto dark = sf::game::DynamicLightVertexColor{24U, 20U, 16U};
  const auto neutral = sf::game::DynamicLightVertexColor{128U, 128U, 128U};
  const auto modulation = sf::game::DynamicLightModulation{64.0, 64.0, 64.0};
  const auto dark_lit = sf::game::applyDynamicLighting(dark, modulation);
  const auto neutral_lit = sf::game::applyDynamicLighting(neutral, modulation);

  require(dark_lit.red > dark.red && dark_lit.green > dark.green &&
              dark_lit.blue > dark.blue && dark_lit.red < 64U &&
              dark_lit.green < 64U && dark_lit.blue < 64U,
          "Dynamic light washed out an authored dark retail section");
  require(neutral_lit.red > dark_lit.red &&
              neutral_lit.green > dark_lit.green &&
              neutral_lit.blue > dark_lit.blue,
          "Retail darkness no longer limits native light exposure");
}

void testOverlappingDynamicLightsGrowWithoutWhitening() {
  const std::array one{lamp(100U, 0.0)};
  const std::array four{
      lamp(100U, 0.0),
      lamp(101U, 0.0),
      lamp(102U, 0.0),
      lamp(103U, 0.0),
  };
  const std::array sixteen{
      lamp(100U, 0.0), lamp(101U, 0.0), lamp(102U, 0.0), lamp(103U, 0.0),
      lamp(104U, 0.0), lamp(105U, 0.0), lamp(106U, 0.0), lamp(107U, 0.0),
      lamp(108U, 0.0), lamp(109U, 0.0), lamp(110U, 0.0), lamp(111U, 0.0),
      lamp(112U, 0.0), lamp(113U, 0.0), lamp(114U, 0.0), lamp(115U, 0.0),
  };
  const auto sample = [](const auto &sources) {
    const auto frame = sf::game::buildDynamicLightFrame(
        sources, {}, sf::game::DynamicLightPoint{});
    const auto energy = sf::game::sampleDynamicLighting(
        frame, sf::game::DynamicLightPoint{0.0, -700.0, 0.0});
    const auto colour = sf::game::applyDynamicLighting(
        sf::game::DynamicLightVertexColor{128U, 128U, 128U}, energy);
    return std::pair{energy, colour};
  };
  const auto [one_energy, one_colour] = sample(one);
  const auto [four_energy, four_colour] = sample(four);
  const auto [many_energy, many_colour] = sample(sixteen);

  require(one_energy.red < four_energy.red &&
              four_energy.red < many_energy.red && four_energy.red > 0.55,
          "Overlapping light energy stopped growing monotonically");
  require(one_colour.red < four_colour.red &&
              four_colour.red < many_colour.red && many_colour.red < 225U,
          "Overlapping lights clipped neutral retail colour to white");
}

void testDynamicLightCompositionIgnoresSourceOrder() {
  const std::array sources{
      lamp(201U, -360.0),
      lamp(202U, 75.0),
      lamp(203U, 420.0),
      lamp(204U, 900.0),
  };
  auto reversed = sources;
  std::reverse(reversed.begin(), reversed.end());
  const auto sample = [](const auto &ordered) {
    const auto frame = sf::game::buildDynamicLightFrame(
        ordered, {}, sf::game::DynamicLightPoint{});
    const auto energy = sf::game::sampleDynamicLighting(
        frame, sf::game::DynamicLightPoint{100.0, -700.0, 0.0});
    return sf::game::applyDynamicLighting(
        sf::game::DynamicLightVertexColor{128U, 96U, 64U}, energy);
  };
  require(sample(sources) == sample(reversed),
          "Dynamic lighting changed when source order was reversed");
}
void testBakedWorldRetainsConfirmedEmissiveLighting() {
  auto frame = sf::game::DynamicLightFrame{};
  frame.count = 5U;
  frame.lights[0].kind = sf::game::DynamicLightKind::street_lamp;
  frame.lights[1].kind = sf::game::DynamicLightKind::muzzle_flash;
  frame.lights[1].transient = true;
  frame.lights[2].kind = sf::game::DynamicLightKind::flashlight;
  frame.lights[2].directional = true;
  frame.lights[3].kind = sf::game::DynamicLightKind::steady_fire;
  frame.lights[4].kind = sf::game::DynamicLightKind::police_lightbar;

  const auto baked = sf::game::dynamicLightFrameForBakedWorld(
      frame, sf::game::DynamicLightPoint{});
  require(baked.count == frame.count &&
              baked.lights[0].kind ==
                  sf::game::DynamicLightKind::street_lamp &&
              baked.lights[1].kind == sf::game::DynamicLightKind::muzzle_flash &&
              baked.lights[2].kind == sf::game::DynamicLightKind::flashlight &&
              baked.lights[3].kind == sf::game::DynamicLightKind::steady_fire &&
              baked.lights[4].kind ==
                  sf::game::DynamicLightKind::police_lightbar,
          "Baked world discarded a confirmed local emissive source");
}

void testBakedWorldLightingIsBoundedAndPrioritized() {
  auto frame = sf::game::DynamicLightFrame{};
  constexpr auto persistent_count =
      sf::game::maximum_baked_world_dynamic_lights + 4U;
  for (std::size_t index = 0U; index < persistent_count; ++index) {
    auto &light = frame.lights[frame.count++];
    light.kind = sf::game::DynamicLightKind::street_lamp;
    light.position = {100.0 + static_cast<double>(index) * 100.0, 0.0, 0.0};
    light.source_id = static_cast<std::uint32_t>(index);
  }
  auto &transient = frame.lights[frame.count++];
  transient.kind = sf::game::DynamicLightKind::explosion;
  transient.position = {50000.0, 0.0, 0.0};
  transient.source_id = 0xf00dU;
  transient.transient = true;

  const auto baked = sf::game::dynamicLightFrameForBakedWorld(
      frame, sf::game::DynamicLightPoint{});
  require(baked.count == sf::game::maximum_baked_world_dynamic_lights,
          "Baked world light frame ignored its dedicated capacity");
  require(std::ranges::any_of(baked.active(), [](const auto &light) {
            return light.source_id == 0xf00dU && light.transient;
          }),
          "Baked world light cap discarded a transient explosion");
  require(std::ranges::none_of(baked.active(), [](const auto &light) {
            return light.source_id == persistent_count - 1U;
          }),
          "Baked world light cap retained a farther lamp over a nearer one");
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

void testFlamethrowerRibbonLightingIsBoundedAndSurfaceAware() {
  std::array<sf::game::FlamethrowerLightSegment, 9U> segments{};
  for (std::size_t index = 0U; index < segments.size(); ++index) {
    const auto x = static_cast<double>(index) * 300.0;
    segments[index] = {{x, -200.0, 0.0},
                       {x + 100.0, -200.0, 0.0},
                       static_cast<std::uint32_t>(0x60000000U | index)};
  }
  segments[2].second = segments[2].first;
  segments[3].first.x = std::numeric_limits<double>::infinity();

  const auto samples = sf::game::flamethrowerDynamicLightSamples(segments);
  require(samples.count ==
                  sf::game::maximum_flamethrower_dynamic_light_samples &&
              samples.active().front().position ==
                  sf::game::DynamicLightPoint{50.0, -200.0, 0.0} &&
              samples.active().back().position ==
                  sf::game::DynamicLightPoint{2450.0, -200.0, 0.0} &&
              std::ranges::all_of(samples.active(), [](const auto &light) {
                return light.effect_type ==
                           sf::game::GameplayEffectType::burning_fire &&
                       light.position_confirmed &&
                       light.remaining_updates == 1U &&
                       light.total_updates == 1U;
              }),
          "Flamethrower light samples left the retail ribbon or exceeded cap");

  const auto frame = sf::game::buildDynamicLightFrame(
      {}, samples.active().first(1U), sf::game::DynamicLightPoint{});
  require(frame.count == 1U && frame.active().front().transient &&
              frame.active().front().kind ==
                  sf::game::DynamicLightKind::steady_fire,
          "Flamethrower light lost transient steady-fire priority");
  const auto front = sf::game::sampleDynamicLighting(
      frame, {50.0, 0.0, 0.0}, {0.0, -1.0, 0.0});
  const auto back = sf::game::sampleDynamicLighting(
      frame, {50.0, 0.0, 0.0}, {0.0, 1.0, 0.0});
  const auto outside =
      sf::game::sampleDynamicLighting(frame, {50.0, 2000.0, 0.0});
  require(front.red > front.green && front.green > front.blue &&
              back.red == 0.0 && back.green == 0.0 && back.blue == 0.0 &&
              outside.red == 0.0 && outside.green == 0.0 &&
              outside.blue == 0.0,
          "Flamethrower light ignored surface orientation or bounded range");
}

void testWorldObjectShadowCasterEligibilityFailsClosed() {
  using sf::platform::selectWorldObjectShadowProjection;
  using sf::platform::worldObjectShadowCasterEligible;
  using sf::platform::WorldObjectShadowCasterFacts;

  const auto valid = WorldObjectShadowCasterFacts{
      {120.0, -40.0, 980.0}, 240.0, 36U, true, true, true, false, false};
  require(!worldObjectShadowCasterEligible(WorldObjectShadowCasterFacts{}),
          "Default map prop facts did not fail closed");
  require(worldObjectShadowCasterEligible(valid),
          "Opaque resident map prop was rejected as a dynamic shadow caster");

  auto rejected = valid;
  rejected.resident = false;
  require(!worldObjectShadowCasterEligible(rejected),
          "Non-resident map prop remained a dynamic shadow caster");
  rejected = valid;
  rejected.opaque_geometry = false;
  require(!worldObjectShadowCasterEligible(rejected),
          "Translucent/effect geometry cast a solid dynamic shadow");
  rejected = valid;
  rejected.authoritative_transform = false;
  require(!worldObjectShadowCasterEligible(rejected),
          "Map prop with a stale transform cast a dynamic shadow");
  rejected = valid;
  rejected.actor_shadow_owned = true;
  require(!worldObjectShadowCasterEligible(rejected),
          "Actor geometry was submitted to both dynamic shadow passes");
  rejected = valid;
  rejected.embedded_world_geometry = true;
  require(!worldObjectShadowCasterEligible(rejected),
          "Receiver chunk was also submitted as a separate prop caster");
  rejected = valid;
  rejected.triangle_count = 0U;
  require(!worldObjectShadowCasterEligible(rejected),
          "Empty map prop was accepted as a dynamic shadow caster");
  rejected = valid;
  rejected.bounding_radius = 0.0;
  require(!worldObjectShadowCasterEligible(rejected),
          "Degenerate map prop bounds were accepted for shadow casting");
  rejected = valid;
  rejected.bounding_radius = -1.0;
  require(!worldObjectShadowCasterEligible(rejected),
          "Negative map prop bounds were accepted for shadow casting");
  rejected = valid;
  rejected.bounding_radius = std::numeric_limits<double>::infinity();
  require(!worldObjectShadowCasterEligible(rejected),
          "Infinite map prop bounds were accepted for shadow casting");
  rejected = valid;
  rejected.bounding_radius = std::numeric_limits<double>::quiet_NaN();
  require(!worldObjectShadowCasterEligible(rejected),
          "NaN map prop bounds were accepted for shadow casting");
  rejected = valid;
  rejected.anchor.x = std::numeric_limits<double>::infinity();
  require(!worldObjectShadowCasterEligible(rejected),
          "Non-finite map prop anchor entered the shadow pass");
  rejected = valid;
  rejected.anchor.y = std::numeric_limits<double>::quiet_NaN();
  require(!worldObjectShadowCasterEligible(rejected),
          "NaN map prop anchor entered the shadow pass");
  rejected = valid;
  rejected.anchor.z = -std::numeric_limits<double>::infinity();
  require(!worldObjectShadowCasterEligible(rejected),
          "Infinite map prop anchor entered the shadow pass");

  const std::array sources{lamp(0x40U, valid.anchor.x)};
  rejected = valid;
  rejected.opaque_geometry = false;
  const auto rejected_projection =
      selectWorldObjectShadowProjection(sources, rejected, {0.0, 1.0, 0.0});
  require(!rejected_projection.source_driven,
          "Rejected translucent prop still selected a shadow light");
}

void testWorldObjectShadowSelectionUsesCasterAboveFrameCapacity() {
  using sf::platform::selectWorldObjectShadowProjection;
  using sf::platform::WorldObjectShadowCasterFacts;

  constexpr auto source_count = sf::game::maximum_dynamic_lights + 8U;
  std::array<sf::game::PersistentDynamicLightState, source_count> sources{};
  for (std::size_t index = 0U; index < sf::game::maximum_dynamic_lights;
       ++index) {
    sources[index] = lamp(static_cast<std::uint32_t>(index),
                          -20000.0 + static_cast<double>(index) * 10.0);
  }
  constexpr auto caster_anchor = sf::game::DynamicLightPoint{8000.0, 0.0, 0.0};
  for (std::size_t index = sf::game::maximum_dynamic_lights;
       index < sources.size(); ++index) {
    sources[index] =
        lamp(static_cast<std::uint32_t>(index),
             caster_anchor.x - 280.0 +
                 static_cast<double>(index - sf::game::maximum_dynamic_lights) *
                     70.0);
  }
  const auto facts = WorldObjectShadowCasterFacts{
      caster_anchor, 180.0, 24U, true, true, true, false, false};

  // This control demonstrates why the generic camera-observed frame is not
  // safe for object shadows once the resident source count exceeds capacity.
  const auto far_camera_frame =
      sf::game::buildDynamicLightFrame(sources, {}, {-20000.0, 0.0, 0.0});
  const auto near_camera_frame =
      sf::game::buildDynamicLightFrame(sources, {}, caster_anchor);
  const auto far_camera_projection = sf::game::selectDynamicShadowProjection(
      far_camera_frame, caster_anchor, {0.0, 1.0, 0.0});
  const auto near_camera_projection = sf::game::selectDynamicShadowProjection(
      near_camera_frame, caster_anchor, {0.0, 1.0, 0.0});
  require(!far_camera_projection.source_driven &&
              near_camera_projection.source_driven,
          "Object-shadow capacity fixture did not exercise camera selection");

  const auto per_caster =
      selectWorldObjectShadowProjection(sources, facts, {0.0, 1.0, 0.0}, 37U);
  std::ranges::reverse(sources);
  const auto reordered =
      selectWorldObjectShadowProjection(sources, facts, {0.0, 1.0, 0.0}, 37U);
  constexpr auto epsilon = 0.0000001;
  const auto same_projection = [&](const auto &left, const auto &right) {
    return left.source_driven == right.source_driven &&
           std::abs(left.ray_direction.x - right.ray_direction.x) < epsilon &&
           std::abs(left.ray_direction.y - right.ray_direction.y) < epsilon &&
           std::abs(left.ray_direction.z - right.ray_direction.z) < epsilon &&
           std::abs(left.darkness - right.darkness) < epsilon;
  };
  require(per_caster.source_driven &&
              same_projection(per_caster, near_camera_projection) &&
              same_projection(per_caster, reordered),
          "Map-object shadow selection followed the camera or source order");
}

void testWorldObjectShadowSelectionIsTranslationCovariant() {
  using sf::platform::selectWorldObjectShadowProjection;
  using sf::platform::WorldObjectShadowCasterFacts;

  const std::array original_sources{lamp(0x41U, -260.0), lamp(0x42U, 430.0)};
  constexpr auto translation =
      sf::game::DynamicLightPoint{15000.0, -2300.0, 7000.0};
  auto translated_sources = original_sources;
  for (auto &source : translated_sources) {
    source.position.x += translation.x;
    source.position.y += translation.y;
    source.position.z += translation.z;
  }
  constexpr auto anchor = sf::game::DynamicLightPoint{80.0, 0.0, 35.0};
  const auto translated_anchor = sf::game::DynamicLightPoint{
      anchor.x + translation.x, anchor.y + translation.y,
      anchor.z + translation.z};
  const auto facts = WorldObjectShadowCasterFacts{anchor, 160.0, 18U,   true,
                                                  true,   true,  false, false};
  const auto translated_facts = WorldObjectShadowCasterFacts{
      translated_anchor, 160.0, 18U, true, true, true, false, false};
  const auto original = selectWorldObjectShadowProjection(
      original_sources, facts, {0.0, 1.0, 0.0});
  const auto translated = selectWorldObjectShadowProjection(
      translated_sources, translated_facts, {0.0, 1.0, 0.0});
  constexpr auto epsilon = 0.0000001;
  require(original.source_driven && translated.source_driven &&
              std::abs(original.ray_direction.x - translated.ray_direction.x) <
                  epsilon &&
              std::abs(original.ray_direction.y - translated.ray_direction.y) <
                  epsilon &&
              std::abs(original.ray_direction.z - translated.ray_direction.z) <
                  epsilon &&
              std::abs(original.darkness - translated.darkness) < epsilon,
          "Map-object shadow direction depended on world origin or camera");
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

void testActorShadowIgnoresTransientAndAnimatedLightPulses() {
  auto lightbar = lamp(20U, -500.0);
  lightbar.kind = sf::game::DynamicLightKind::police_lightbar;
  const std::array persistent{lamp(19U, 500.0), lightbar};
  const auto blue = sf::game::buildDynamicLightFrame(
      persistent, {}, sf::game::DynamicLightPoint{}, {}, 0U);
  const auto red = sf::game::buildDynamicLightFrame(
      persistent, {}, sf::game::DynamicLightPoint{}, {}, 2U);
  const auto blue_projection =
      sf::game::selectDynamicShadowProjection(blue, {}, {0.0, 1.0, 0.0});
  const auto red_projection =
      sf::game::selectDynamicShadowProjection(red, {}, {0.0, 1.0, 0.0});
  require(std::abs(blue_projection.ray_direction.x -
                   red_projection.ray_direction.x) < 0.000001 &&
              std::abs(blue_projection.darkness - red_projection.darkness) <
                  0.000001,
          "Animated lightbar pulse rotated or darkened the actor shadow");

  const std::array explosion{sf::game::TransientDynamicLightState{
      sf::game::GameplayEffectType::explosion,
      {-400.0, -250.0, 0.0},
      0xe11U,
      4.0,
      1U,
      1U,
      true,
  }};
  const std::array key{lamp(21U, 450.0)};
  const auto without_effect =
      sf::game::buildDynamicLightFrame(key, {}, sf::game::DynamicLightPoint{});
  const auto with_effect = sf::game::buildDynamicLightFrame(
      key, explosion, sf::game::DynamicLightPoint{});
  const auto stable = sf::game::selectDynamicShadowProjection(
      without_effect, {}, {0.0, 1.0, 0.0});
  const auto during_explosion =
      sf::game::selectDynamicShadowProjection(with_effect, {}, {0.0, 1.0, 0.0});
  require(std::abs(stable.ray_direction.x - during_explosion.ray_direction.x) <
                  0.000001 &&
              std::abs(stable.darkness - during_explosion.darkness) < 0.000001,
          "One-update combat light yanked the common actor-shadow ray");
}

void testActorShadowDarknessDoesNotScaleWithSourceCount() {
  const std::array single{lamp(30U, 0.0)};
  const std::array crowded{lamp(30U, 0.0), lamp(31U, 0.0), lamp(32U, 0.0),
                           lamp(33U, 0.0)};
  const auto one = sf::game::selectDynamicShadowProjection(
      sf::game::buildDynamicLightFrame(single, {}, {}), {}, {0.0, 1.0, 0.0});
  const auto many = sf::game::selectDynamicShadowProjection(
      sf::game::buildDynamicLightFrame(crowded, {}, {}), {}, {0.0, 1.0, 0.0});
  require(std::abs(one.darkness - many.darkness) < 0.000001,
          "Resident source count amplified actor-shadow darkness");
}

void testActorShadowTemporalPolicyUsesGuestTime() {
  using sf::game::DynamicShadowProjection;
  using sf::game::DynamicShadowProjectionState;
  const auto first = DynamicShadowProjection{{0.0, 1.0, 0.0}, 0.20, false};
  const auto target = DynamicShadowProjection{{0.8, 0.6, 0.0}, 0.40, true};
  const auto initialized = sf::game::advanceDynamicShadowProjection(
      DynamicShadowProjectionState{}, first, 100U);
  const auto advanced =
      sf::game::advanceDynamicShadowProjection(initialized, target, 101U);
  require(advanced.current.ray_direction.x > 0.0 &&
              advanced.current.ray_direction.x < target.ray_direction.x &&
              advanced.current.darkness > first.darkness &&
              advanced.current.darkness < target.darkness,
          "Actor-shadow history snapped instead of damping a key-light swap");

  const auto repeated =
      sf::game::advanceDynamicShadowProjection(advanced, first, 101U);
  require(repeated.current.ray_direction == advanced.current.ray_direction &&
              repeated.current.darkness == advanced.current.darkness,
          "Actor-shadow history advanced more than once in one guest tick");
  const auto start = sf::game::sampleDynamicShadowProjection(advanced, 0.0);
  const auto finish = sf::game::sampleDynamicShadowProjection(advanced, 1.0);
  require(start.ray_direction == advanced.previous.ray_direction &&
              finish.ray_direction == advanced.current.ray_direction &&
              start.darkness == advanced.previous.darkness &&
              finish.darkness == advanced.current.darkness,
          "Actor-shadow host-frame interpolation lost its endpoints");

  const auto reset =
      sf::game::advanceDynamicShadowProjection(advanced, target, 3U);
  require(reset.previous.ray_direction == target.ray_direction &&
              reset.current.ray_direction == target.ray_direction &&
              reset.guest_tick == 3U,
          "Actor-shadow history survived a mission clock rollback");
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

void testAuthoredAppearanceSynchronizesPersistentAndTransientLight() {
  auto authored_lamp = lamp(0xa11U, 0.0);
  authored_lamp.appearance = {
      sf::game::DynamicLightRgb{0.18, 0.72, 1.35}, 0.50, 1.50};
  const std::array persistent{authored_lamp};
  const std::array fallback_persistent{lamp(0xa11U, 0.0)};
  const auto persistent_frame = sf::game::buildDynamicLightFrame(
      persistent, {}, sf::game::DynamicLightPoint{});
  const auto fallback_frame = sf::game::buildDynamicLightFrame(
      fallback_persistent, {}, sf::game::DynamicLightPoint{});
  require(persistent_frame.count == 1U &&
              persistent_frame.active().front().color ==
                  sf::game::DynamicLightRgb{0.18, 0.72, 1.35} &&
              std::abs(persistent_frame.active().front().radius -
                       fallback_frame.active().front().radius * 0.50) <
                  0.000001 &&
              std::abs(persistent_frame.active().front().intensity -
                       fallback_frame.active().front().intensity * 1.50) <
                  0.000001,
          "Persistent light ignored its authored visual appearance");

  auto authored_explosion = sf::game::TransientDynamicLightState{
      sf::game::GameplayEffectType::explosion,
      {10.0, 20.0, 30.0},
      0xe11U,
      1.0,
      1U,
      1U,
      true,
  };
  authored_explosion.appearance = {
      sf::game::DynamicLightRgb{0.35, 0.80, 0.22}, 1.75, 0.40};
  auto fallback_explosion = authored_explosion;
  fallback_explosion.appearance = {};
  const std::array transient{authored_explosion};
  const std::array fallback_transient{fallback_explosion};
  const auto transient_frame = sf::game::buildDynamicLightFrame(
      {}, transient, sf::game::DynamicLightPoint{});
  const auto fallback_transient_frame = sf::game::buildDynamicLightFrame(
      {}, fallback_transient, sf::game::DynamicLightPoint{});
  require(transient_frame.count == 1U &&
              transient_frame.active().front().color ==
                  sf::game::DynamicLightRgb{0.35, 0.80, 0.22} &&
              std::abs(transient_frame.active().front().radius -
                       fallback_transient_frame.active().front().radius *
                           1.75) < 0.000001 &&
              std::abs(transient_frame.active().front().intensity -
                       fallback_transient_frame.active().front().intensity *
                           0.40) < 0.000001,
          "Transient light diverged from its authored visual appearance");
}

void testMalformedAppearanceFallsBackToSafeProfile() {
  require(sf::game::validDynamicLightAppearance({}),
          "Neutral dynamic-light appearance was rejected");
  const auto nan = std::numeric_limits<double>::quiet_NaN();
  const std::array malformed{
      sf::game::DynamicLightAppearance{
          sf::game::DynamicLightRgb{nan, 0.5, 0.5}, 0.5, 1.5},
      sf::game::DynamicLightAppearance{
          sf::game::DynamicLightRgb{2.01, 0.5, 0.5}, 1.0, 1.0},
      sf::game::DynamicLightAppearance{std::nullopt, 0.0, 1.0},
      sf::game::DynamicLightAppearance{std::nullopt, 1.0,
                                       std::numeric_limits<double>::infinity()},
  };
  require(std::ranges::none_of(malformed,
                               sf::game::validDynamicLightAppearance),
          "Malformed dynamic-light appearance passed validation");

  const auto fallback = lamp(0xbadU, 0.0);
  auto invalid = fallback;
  invalid.appearance = malformed.front();
  const std::array fallback_sources{fallback};
  const std::array invalid_sources{invalid};
  const auto fallback_frame = sf::game::buildDynamicLightFrame(
      fallback_sources, {}, sf::game::DynamicLightPoint{});
  const auto invalid_frame = sf::game::buildDynamicLightFrame(
      invalid_sources, {}, sf::game::DynamicLightPoint{});
  require(invalid_frame.count == 1U && fallback_frame.count == 1U &&
              invalid_frame.active().front().color ==
                  fallback_frame.active().front().color &&
              invalid_frame.active().front().radius ==
                  fallback_frame.active().front().radius &&
              invalid_frame.active().front().intensity ==
                  fallback_frame.active().front().intensity,
          "Malformed appearance poisoned or removed a valid light source");
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

void testRetailSirenUsesGuestViewAfterFourOtherLights() {
  std::array<sf::game::RetailVertexLightState, 5U> lights{};
  auto &siren = lights.back();
  siren = retailLight();
  siren.channel_mask = 0xffU;
  siren.guest_view = siren.matrix;
  siren.guest_view->translation = {0, 0, 256};
  require(sf::game::applyRetailVertexLightingPacked(0x00081018U, lights, {},
                                                    320) == 0x00081078U,
          "A fifth red light lost its guest view or brightened other channels");
  siren.guest_view->translation[2] = -256;
  require(sf::game::applyRetailVertexLightingPacked(0x00081018U, lights, {},
                                                    320) == 0x00081018U,
          "A wall behind the siren received red light");
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
          "Scene-object floor lighting did not barycentrically interpolate");
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
              0x00010203U, lights, {41.0, 20.0, 33.0}, 17) == 0x00606262U,
          "Retail light lost the GTE divide-overflow projection");

  // The non-overflow path uses the PS1 UNR reciprocal approximation. At
  // H=1/SZ=7 it projects guest Y=7 to zero, while ideal division yields one.
  require(sf::game::applyRetailVertexLightingPacked(
              0x00010203U, lights, {10.0, 27.0, 37.0}, 1) == 0x009c9e9eU,
          "Retail light replaced the GTE UNR quotient with ideal division");

  // RTPT clamps IR before multiplying by the quotient.
  source = retailLight();
  source.matrix.rotation[4] = 8192;
  source.extent = 2508;
  source.matrix.translation = {0, 0, 0};
  lights[0] = source;
  require(sf::game::applyRetailVertexLightingPacked(
              0x00010203U, lights, {0.0, 20000.0, 20000.0}, 1) == 0x00080a0aU,
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
              0x00010203U, lights, {0.0, 2.0, 7.0}, 17) == 0x009a9c9cU,
          "Retail light collapsed the two GsSetView2/RTPT rounding stages");

  // ApplyMatrixLV itself is a high/remainder pair of GTE passes. Both passes
  // store MAC, so the valid 42421 remainder must not clamp to IR=32767.
  source.matrix.rotation = {2896, -2896, 0, 2896, 2896, 0, 0, 0, 4096};
  source.matrix.translation = {30000, 30000, 0};
  lights[0] = source;
  require(sf::game::applyRetailVertexLightingPacked(0x00010203U, lights,
                                                    {30000.0, 30000.0, 32.0},
                                                    320) == 0x009c9e9eU,
          "Retail light incorrectly clamped ApplyMatrixLV remainder MAC");
}

void testRetailVertexLightShiftSemanticsAreExact() {
  auto source = retailLight();
  source.screen_shift = 31U;
  std::array lights{source};
  require(sf::game::applyRetailVertexLightingPacked(
              0x00010203U, lights, {10.0, 20.0, 157.0}, 320) == 0x00a0a2a2U,
          "Retail near-depth shift did not wrap through the MIPS low bits");

  lights[0].screen_shift = 0U;
  require(sf::game::applyRetailVertexLightingPacked(
              0x00010203U, lights, {10.0, 20.0, 286.0}, 320) == 0x00606262U,
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
              0x00010203U, lights, reversed, 320) == 0x00606262U,
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

void testPreparedRetailVertexLightMatchesExactPath() {
  auto source = retailLight();
  source.flags = 1U;
  const auto prepared = sf::game::prepareRetailVertexLight(source);
  require(prepared.has_value(), "Valid retail light was not prepared");
  const std::array raw_lights{source};
  const std::array prepared_lights{*prepared};
  constexpr std::array points{
      sf::game::DynamicLightPoint{10.0, 20.0, -226.0},
      sf::game::DynamicLightPoint{42.0, 12.0, -98.0},
      sf::game::DynamicLightPoint{-12.0, 48.0, 286.0},
  };
  for (const auto point : points) {
    require(sf::game::applyRetailVertexLightingPacked(0x00204060U, raw_lights,
                                                      point, 320) ==
                sf::game::applyRetailVertexLightingPacked(
                    0x00204060U, prepared_lights, point, 320),
            "Prepared retail light changed exact projection output");
  }

  source.extent = 0;
  require(!sf::game::prepareRetailVertexLight(source),
          "Malformed retail light entered the prepared path");
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

void testRetailVertexLightingCacheKeepsSectionIdentity() {
  using Colour = sf::game::DynamicLightVertexColor;
  sf::platform::RetailSectionVertexLightCache<Colour> cache;
  auto evaluations = 0;
  const auto sample = [&](Colour colour) {
    ++evaluations;
    return colour;
  };
  const auto same = [](const Colour &a, const Colour &b) {
    return a.red == b.red && a.green == b.green && a.blue == b.blue;
  };
  const Colour first{10, 20, 30}, second{40, 50, 60}, changed{70, 80, 90};
  cache.beginSection(2);
  require(same(cache.resolve(0, [&] { return sample(first); }), first),
          "First retail vertex light result was lost");
  require(same(cache.resolve(0, [&] { return sample(changed); }), first) &&
              evaluations == 1,
          "A shared vertex was relit during the same section pass");
  require(same(cache.resolve(1, [&] { return sample(second); }), second),
          "Distinct vertex colours were merged by the shared-light cache");

  // A different section, light matrix or guest colour pass may reuse index
  // zero. Neither its previous colour nor a high-water allocation is valid.
  cache.beginSection(1);
  require(same(cache.resolve(0, [&] { return sample(changed); }), changed),
          "A new section inherited the preceding section's lighting");
  cache.beginSection(4096);
  require(
      same(cache.resolve(4095, [&] { return sample(second); }), second) &&
          same(cache.resolve(0, [&] { return sample(first); }), first),
      "Growing the section cache lost vertex identity or retained old light");
  cache.beginSection(0);
  cache.beginSection(2);
  require(same(cache.resolve(1, [&] { return sample(changed); }), changed) &&
              evaluations == 6,
          "A retired section retained a previous light or guest colour");
}

void testMissionLightsMatchExecutedPsxColours() {
  struct Fixture {
    std::array<std::int16_t, 9> rotation;
    std::array<std::int32_t, 3> translation;
    sf::game::DynamicLightPoint point;
    int extent;
    unsigned screen_shift, depth_shift, mask, base, expected;
  };
  // Captured FUN_800d3b8c inputs and completed FUN_800d3cb4 results from
  // SCUS-94240: BASEEXT flashlight + white searchlight, BASEEXT2 flashlight
  // + red projector, and CAVE2 flashlight. H=38 belongs to light-space RTPT,
  // independently of the reconstructed native camera H=261.
  const std::array fixtures{
      Fixture{{-3627, 465, -1852, 639, 3320, -425, 1753, -795, -3618},
              {339, 454, 2071},
              {-224, 0, 678},
              120,
              13,
              6,
              0xffffffU,
              0x09282828U,
              0x092e2e2eU},
      Fixture{{2564, 0, -3208, -1457, 2868, -1165, 2682, 2232, 2145},
              {-798, 235, 940},
              {225, -585, -1007},
              150,
              12,
              6,
              0xffffffU,
              0x1d8e8e8eU,
              0x1ddadadaU},
      Fixture{{561, 465, 4027, -715, 3320, -283, -3959, -795, 639},
              {5012, 933, 7653},
              {5656, -576, -6333},
              120,
              13,
              6,
              0xffffffU,
              0x15303030U,
              0x155e5e5eU},
      Fixture{{4096, 0, 0, 0, 3413, 0, 0, 0, 4096},
              {-5562, 345, 6113},
              {5656, -516, -5655},
              120,
              14,
              4,
              0xffU,
              0x02b6b6b6U,
              0x02b6b6ffU},
      Fixture{{-1875, 466, -3614, 766, 3320, 24, 3529, -795, -1929},
              {-558, 2018, 512},
              {-853, -2441, -510},
              90,
              15,
              6,
              0xffffffU,
              0x00282030U,
              0x00a69eaeU}};
  for (const auto &fixture : fixtures) {
    auto light = sf::game::RetailVertexLightState{};
    light.guest_view = sf::game::RetailVertexLightMatrix{fixture.rotation,
                                                         fixture.translation};
    light.extent = fixture.extent;
    light.screen_shift = fixture.screen_shift;
    light.depth_shift = fixture.depth_shift;
    light.channel_mask = fixture.mask;
    const std::array lights{light};
    require(sf::game::applyRetailVertexLightingPacked(
                fixture.base, lights, fixture.point, 38) == fixture.expected,
            "Mission light colour differs from executed PSX instructions");
    require(sf::game::applyRetailVertexLightingPacked(
                fixture.base, lights, fixture.point, 261) != fixture.expected,
            "Projection fixture no longer detects the camera/light H mix-up");
  }
  auto light = retailLight();
  // Select a point whose authored falloff reaches exactly zero.
  const std::array lights{light};
  require(sf::game::applyRetailVertexLightingPacked(
              0x00010203U, lights, {10, 20, 670}, 320) == 0x00000202U,
          "Retail zero-intensity channel rounding was lost");
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

} // namespace

int main() {
  try {
    testGuestLampLifetimeIsAuthoritative();
    testTransientLightsAreExactAndFinite();
    testFlashlightConeIsDirectionalAndAuthoritative();
    testFunctionalDirectionalLightRevealsBlackTwoSidedReceivers();
    testAmbientLightStillPreservesAuthoredBlackness();
    testRadialSamplingAndNeutralModulation();
    testGameplayGammaDarkensOnlyValidSceneLighting();
    testDynamicLightSoftKneePreservesRetailHeadroom();
    testDynamicLightPreservesAuthoredDarkness();
    testOverlappingDynamicLightsGrowWithoutWhitening();
    testDynamicLightCompositionIgnoresSourceOrder();
    testSurfaceLightingRejectsBackFaces();
    testBakedWorldRetainsConfirmedEmissiveLighting();
    testBakedWorldLightingIsBoundedAndPrioritized();
    testWorldObjectShadowCasterEligibilityFailsClosed();
    testWorldObjectShadowSelectionUsesCasterAboveFrameCapacity();
    testWorldObjectShadowSelectionIsTranslationCovariant();
    testActorShadowTracksEligibleDynamicLight();
    testFlamethrowerRibbonLightingIsBoundedAndSurfaceAware();
    testActorShadowRejectsLowLightAndMalformedPlane();
    testActorShadowBlendsSourcesAndBoundsStretch();
    testActorShadowIgnoresTransientAndAnimatedLightPulses();
    testActorShadowDarknessDoesNotScaleWithSourceCount();
    testActorShadowTemporalPolicyUsesGuestTime();
    testPersistentAnimationUsesGuestTime();
    testAuthoredAppearanceSynchronizesPersistentAndTransientLight();
    testMalformedAppearanceFallsBackToSafeProfile();
    testBoundedSelectionKeepsTransientAndNearestLamps();
    testBoundedSelectionKeepsDirectionalLight();
    testRetailGmdBackColorUsesNeutralTextureScale();
    testSceneTriangleLightingInterpolatesFloorColor();
    testRetailSirenUsesGuestViewAfterFourOtherLights();
    testRetailVertexLightSzBranchesAreExact();
    testRetailVertexLightUsesExactGteProjection();
    testRetailVertexLightPreservesGsSetViewRounding();
    testRetailVertexLightShiftSemanticsAreExact();
    testRetailVertexLightThresholdMaskAndSaturation();
    testRetailVertexLightDirectionAndMalformedRecordsFailClosed();
    testPreparedRetailVertexLightMatchesExactPath();
    testRetailVertexLightRayUsesGuestMatrixAndAttachedAxis();
    testMissionFlashlightRecordLightsItsRetailForwardAxis();
    testRetailVertexLightingCacheKeepsSectionIdentity();
    testMissionLightsMatchExecutedPsxColours();
    testCaveFlashlightUsesRetailWorldYAxis();
    std::cout << "Dynamic lighting tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "Dynamic lighting tests failed: " << error.what() << '\n';
    return 1;
  }
}
