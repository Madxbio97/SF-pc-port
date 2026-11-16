#include "PsyX/PsyX_public.h"
#include "sf/platform/retail_ui_presentation.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error{message};
    }
}

bool near(float first, float second) {
    return std::abs(first - second) < 0.0001F;
}

void testOriginalFourThree() {
    const auto widescreen = PsyX_CalculatePresentationViewport(
        1920, 1080, PSYX_ASPECT_ORIGINAL_4_3);
    require(
        widescreen.x == 240 && widescreen.y == 0 &&
            widescreen.w == 1440 && widescreen.h == 1080,
        "Original mode did not pillarbox a 16:9 drawable to 4:3");

    const auto tall = PsyX_CalculatePresentationViewport(
        1280, 1024, PSYX_ASPECT_ORIGINAL_4_3);
    require(
        tall.x == 0 && tall.y == 32 && tall.w == 1280 && tall.h == 960,
        "Original mode did not letterbox a tall drawable to 4:3");
}

void testAdaptiveUsesEntireDrawable() {
    const auto widescreen = PsyX_CalculatePresentationViewport(
        1920, 1080, PSYX_ASPECT_ADAPTIVE);
    require(
        widescreen.x == 0 && widescreen.y == 0 &&
            widescreen.w == 1920 && widescreen.h == 1080,
        "Adaptive mode did not use the complete 16:9 drawable");

    const auto ultrawide = PsyX_CalculatePresentationViewport(
        3440, 1440, PSYX_ASPECT_ADAPTIVE);
    require(
        ultrawide.x == 0 && ultrawide.y == 0 &&
            ultrawide.w == 3440 && ultrawide.h == 1440,
        "Adaptive mode unexpectedly clamped an ultrawide drawable");
}

void testAdaptivePreservesPixelAspect() {
    const auto widescreen = PsyX_CalculatePresentationScale(
        1920, 1080, PSYX_ASPECT_ADAPTIVE);
    require(
        near(widescreen.x, 0.75F) && near(widescreen.y, 1.0F),
        "Adaptive widescreen did not use an undistorted Hor+ scale");

    const auto tall = PsyX_CalculatePresentationScale(
        1280, 1024, PSYX_ASPECT_ADAPTIVE);
    require(
        near(tall.x, 1.0F) && near(tall.y, 0.9375F),
        "Adaptive narrow output did not use an undistorted Vert+ scale");

    const auto original = PsyX_CalculatePresentationScale(
        3440, 1440, PSYX_ASPECT_ORIGINAL_4_3);
    require(
        near(original.x, 1.0F) && near(original.y, 1.0F),
        "Original mode unexpectedly changed its presentation scale");
}

void testWidescreenUiAnchors() {
  const auto scale =
      PsyX_CalculatePresentationScale(1920, 1080, PSYX_ASPECT_ADAPTIVE);
  const auto canvas = sf::platform::retailUiCanvas(scale.x, scale.y);
  require(canvas.left == -64 && canvas.right == 448 && canvas.top == 0 &&
              canvas.bottom == 240 && canvas.width() == 512 &&
              canvas.mapX(192) == 192,
          "16:9 UI canvas did not preserve the centre and expose both edges");
  require(canvas.left + 12 == -52 && canvas.right - 12 == 436 &&
              canvas.mapX(36) == -16 && canvas.mapX(226) == 237,
          "Radar/weapon margins or pause-panel bounds did not adapt to 16:9");
  const auto ultra_scale =
      PsyX_CalculatePresentationScale(3440, 1440, PSYX_ASPECT_ADAPTIVE);
  const auto ultra = sf::platform::retailUiCanvas(ultra_scale.x, ultra_scale.y);
  require(ultra.left == -152 && ultra.right == 536 &&
              ultra.mapX(0) == ultra.left && ultra.mapX(384) == ultra.right &&
              ultra.mapX(192) == 192,
          "Ultrawide menu and centred weapon selection diverged");
  const auto original_scale =
      PsyX_CalculatePresentationScale(1920, 1080, PSYX_ASPECT_ORIGINAL_4_3);
  const auto original =
      sf::platform::retailUiCanvas(original_scale.x, original_scale.y);
  require(original.left == 0 && original.right == 384 &&
              original.mapX(36) == 36 && original.mapX(226) == 226,
          "Original 4:3 UI layout changed");
}

void testInvalidSizeIsBounded() {
    const auto viewport = PsyX_CalculatePresentationViewport(
        0, -1, PSYX_ASPECT_ADAPTIVE);
    require(
        viewport.x == 0 && viewport.y == 0 &&
            viewport.w == 1 && viewport.h == 1,
        "Viewport dimensions were not bounded to one pixel");
}

} // namespace

int main() {
    try {
        testOriginalFourThree();
        testAdaptiveUsesEntireDrawable();
        testAdaptivePreservesPixelAspect();
        testWidescreenUiAnchors();
        testInvalidSizeIsBounded();
        std::cout << "Aspect ratio tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Aspect ratio tests failed: " << error.what() << '\n';
        return 1;
    }
}
