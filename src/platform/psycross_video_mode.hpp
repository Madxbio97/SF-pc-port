#pragma once

#include <PsyX/PsyX_public.h>
#include <PsyX/PsyX_render.h>
#include <psx/libetc.h>
#include <psx/libgte.h>
#include <psx/libgpu.h>

namespace sf::platform::detail {

struct PsyCrossVideoMode {
    int width{};
    int height{};
};

inline constexpr PsyCrossVideoMode gameplay_video_mode{384, 240};
inline constexpr PsyCrossVideoMode movie_video_mode{320, 240};

inline void configurePsyCrossVideoMode(PsyCrossVideoMode mode, bool reset_graph) noexcept {
    SetVideoMode(MODE_NTSC);
    if (reset_graph) {
        ResetGraph(0);
    }

    DISPENV display{};
    DRAWENV draw{};
    SetDefDispEnv(&display, 0, 0, mode.width, mode.height);
    SetDefDrawEnv(&draw, 0, 0, mode.width, mode.height);
    draw.isbg = 1;
    // Native PC output never uses the PS1's screen-space 4x4 dither matrix.
    // Besides visible color noise it shimmers as geometry moves underneath it.
    draw.dtd = 0;
    setRGB0(&draw, 0, 0, 0);
    PutDispEnv(&display);
    PutDrawEnv(&draw);
    SetDispMask(1);
}

class ScopedPsyCrossVideoMode final {
public:
    ScopedPsyCrossVideoMode(PsyCrossVideoMode active, PsyCrossVideoMode restore) noexcept
        : restore_(restore) {
        // A guest movie-loader edge can leave PsyCross between DrawPrim's
        // implicit BeginScene and the host's normal frame terminator. Close
        // that presentation transaction before changing DISPENV; otherwise
        // every movie/menu BeginScene returns zero while input and audio keep
        // advancing on an invisible frame.
        PsyX_EndScene();
        configurePsyCrossVideoMode(active, false);
        // Gameplay leaves hardware depth testing enabled. PsyCross immediate
        // SPRT/TILE primitives used by movies and frontend overlays do not
        // carry gameplay depth, so retaining that state makes their audio and
        // input live while every visible primitive is rejected.
        GR_SetBlendMode(BM_NONE);
        GR_SetPolygonOffset(0.0F, 0.0F);
        GR_SetDepthState(0, 0);
        GR_EnableDepth(0);
    }

    ~ScopedPsyCrossVideoMode() {
        configurePsyCrossVideoMode(restore_, false);
        GR_SetBlendMode(BM_NONE);
        GR_SetPolygonOffset(0.0F, 0.0F);
        GR_EnableDepth(1);
        GR_SetDepthState(1, 1);
    }

    ScopedPsyCrossVideoMode(const ScopedPsyCrossVideoMode&) = delete;
    ScopedPsyCrossVideoMode& operator=(const ScopedPsyCrossVideoMode&) = delete;

private:
    PsyCrossVideoMode restore_;
};

} // namespace sf::platform::detail
