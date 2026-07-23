#include "sf/core/error.hpp"
#include "sf/game/disc_movie.hpp"
#include "sf/game/game_disc.hpp"
#include "sf/game/mission.hpp"
#include "sf/game/title.hpp"
#include "sf/media/str_decoder.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace {

constexpr double timestamp_tolerance_seconds = 0.000'001;

struct MovieStats {
    std::size_t video_frames{};
    std::size_t audio_chunks{};
    std::uint64_t stereo_sample_frames{};
    std::uint64_t visible_pixels{};
};

struct ProbeTotals {
    std::size_t movies{};
    std::size_t video_frames{};
    std::size_t movies_with_audio{};
    std::size_t audio_chunks{};
    std::uint64_t stereo_sample_frames{};
};

[[noreturn]] void invalidMovie(
    std::string_view path,
    std::string_view reason) {
    throw sf::core::Error{
        sf::core::ErrorCode::invalid_format,
        std::string{path} + ": " + std::string{reason}};
}

void requireStableTimestamp(
    std::string_view path,
    std::string_view stream,
    double timestamp,
    double previous_timestamp,
    bool has_previous_timestamp) {
    if (!std::isfinite(timestamp)) {
        invalidMovie(path, std::string{stream} + " timestamp is not finite");
    }
    if (has_previous_timestamp &&
        timestamp + timestamp_tolerance_seconds < previous_timestamp) {
        invalidMovie(path, std::string{stream} + " timestamps moved backwards");
    }
}

MovieStats probeMovie(sf::game::DiscMovie movie) {
    if (movie.path.empty()) {
        invalidMovie("<unnamed>", "movie path is empty");
    }
    if (movie.sectors.sector_size != 2352U) {
        throw sf::core::Error{
            sf::core::ErrorCode::unsupported,
            movie.path + " does not use raw 2352-byte sectors"};
    }

    auto decoder = sf::media::StrDecoder::open(std::move(movie.sectors.bytes));
    const auto frames_per_second = decoder.framesPerSecond();
    if (!std::isfinite(frames_per_second) || frames_per_second <= 0.0 ||
        frames_per_second > 120.0) {
        invalidMovie(movie.path, "invalid video frame rate");
    }

    MovieStats stats;
    int width{};
    int height{};
    int audio_sample_rate{};
    double previous_video_timestamp{};
    double previous_audio_timestamp{};
    while (const auto event = decoder.next()) {
        if (const auto* frame = std::get_if<sf::media::MovieVideoFrame>(&*event)) {
            if (frame->width <= 0 || frame->height <= 0) {
                invalidMovie(movie.path, "invalid video dimensions");
            }
            if (stats.video_frames == 0U) {
                width = frame->width;
                height = frame->height;
            } else if (frame->width != width || frame->height != height) {
                invalidMovie(movie.path, "video dimensions changed during playback");
            }

            const auto expected_pixels = static_cast<std::size_t>(frame->width) *
                                         static_cast<std::size_t>(frame->height);
            if (frame->rgba8888.size() != expected_pixels * 4U) {
                invalidMovie(movie.path, "video frame has an invalid pixel count");
            }
            requireStableTimestamp(
                movie.path,
                "video",
                frame->timestamp_seconds,
                previous_video_timestamp,
                stats.video_frames != 0U);
            previous_video_timestamp = frame->timestamp_seconds;
            for (std::size_t pixel = 0; pixel < expected_pixels; ++pixel) {
                const auto offset = pixel * 4U;
                if (frame->rgba8888[offset] != 0U ||
                    frame->rgba8888[offset + 1U] != 0U ||
                    frame->rgba8888[offset + 2U] != 0U) {
                    ++stats.visible_pixels;
                }
                if (frame->rgba8888[offset + 3U] != 0xffU) {
                    invalidMovie(movie.path, "video frame contains non-opaque pixels");
                }
            }
            ++stats.video_frames;
            continue;
        }

        const auto& chunk = std::get<sf::media::MovieAudioChunk>(*event);
        if (chunk.sample_rate <= 0) {
            invalidMovie(movie.path, "invalid audio sample rate");
        }
        if (stats.audio_chunks == 0U) {
            audio_sample_rate = chunk.sample_rate;
        } else if (chunk.sample_rate != audio_sample_rate) {
            invalidMovie(movie.path, "audio sample rate changed during playback");
        }
        if (chunk.stereo_samples.empty() ||
            (chunk.stereo_samples.size() % 2U) != 0U) {
            invalidMovie(movie.path, "audio chunk is not non-empty stereo PCM");
        }
        requireStableTimestamp(
            movie.path,
            "audio",
            chunk.timestamp_seconds,
            previous_audio_timestamp,
            stats.audio_chunks != 0U);
        previous_audio_timestamp = chunk.timestamp_seconds;
        stats.stereo_sample_frames += chunk.stereo_samples.size() / 2U;
        ++stats.audio_chunks;
    }

    if (stats.video_frames == 0U) {
        invalidMovie(movie.path, "produced no video frame");
    }
    if (stats.visible_pixels == 0U) {
        invalidMovie(movie.path, "all decoded video frames are empty");
    }
    if (decoder.hasAudio() && stats.audio_chunks == 0U) {
        invalidMovie(movie.path, "declares audio but produced no audio");
    }
    if (!decoder.hasAudio() && stats.audio_chunks != 0U) {
        invalidMovie(movie.path, "produced audio without an audio stream");
    }

    std::cout << movie.path << ": " << width << 'x' << height << " @ "
              << frames_per_second << " fps, video=" << stats.video_frames
              << ", audio=" << stats.audio_chunks
              << ", samples=" << stats.stereo_sample_frames << '\n';
    return stats;
}

void addStats(ProbeTotals& totals, const MovieStats& stats) {
    ++totals.movies;
    totals.video_frames += stats.video_frames;
    totals.audio_chunks += stats.audio_chunks;
    totals.stereo_sample_frames += stats.stereo_sample_frames;
    if (stats.audio_chunks != 0U) {
        ++totals.movies_with_audio;
    }
}

void probeTitleMovies(sf::game::GameDisc& disc, ProbeTotals& totals) {
    auto movies = sf::game::TitleMovies::load(disc);
    for (auto& movie : movies.sequence()) {
        addStats(totals, probeMovie(std::move(movie)));
    }
}

void probeCampaignMovies(sf::game::GameDisc& disc, ProbeTotals& totals) {
    constexpr std::size_t retail_mission_count = 20U;
    constexpr std::size_t retail_opening_movie_count = 10U;
    constexpr std::size_t retail_ending_movie_count = 14U;
    constexpr std::size_t retail_unique_movie_count = 24U;
    const auto catalog = sf::game::missionCatalog();
    if (catalog.size() != retail_mission_count) {
        throw sf::core::Error{
            sf::core::ErrorCode::invalid_format,
            "Retail campaign catalog does not contain exactly 20 missions"};
    }

    std::set<std::string, std::less<>> unique_paths;
    std::size_t opening_movies{};
    std::size_t ending_movies{};
    const auto probe_path = [&](std::string_view path, bool opening) {
        if (path.empty()) {
            return;
        }
        if (opening) {
            ++opening_movies;
        } else {
            ++ending_movies;
        }
        const auto [iterator, inserted] = unique_paths.emplace(path);
        if (!inserted) {
            return;
        }

        // Read and decode a single STR at a time. In particular, do not keep
        // twenty MissionPackage instances (and their large FOG assets) alive.
        auto movie = sf::game::DiscMovie{
            *iterator,
            disc.image().readRawSectorFile(*iterator),
        };
        addStats(totals, probeMovie(std::move(movie)));
    };

    for (const auto& mission : catalog) {
        probe_path(mission.opening_movie_path, true);
        probe_path(mission.ending_movie_path, false);
    }

    if (opening_movies != retail_opening_movie_count ||
        ending_movies != retail_ending_movie_count ||
        unique_paths.size() != retail_unique_movie_count) {
        throw sf::core::Error{
            sf::core::ErrorCode::invalid_format,
            "Retail campaign SOL/EOL movie catalog is incomplete or duplicated"};
    }

    std::cout << "Campaign movies: missions=" << catalog.size()
              << ", SOL=" << opening_movies << ", EOL=" << ending_movies
              << ", unique=" << unique_paths.size() << '\n';
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) {
            std::cerr << "Usage: sf_movie_probe <game.cue>\n";
            return 64;
        }

        auto disc = sf::game::GameDisc::open(std::filesystem::path{argv[1]});
        ProbeTotals totals;
        probeTitleMovies(disc, totals);
        probeCampaignMovies(disc, totals);
        std::cout << "sf_movie_probe: PASS movies=" << totals.movies
                  << ", video=" << totals.video_frames
                  << ", audio-movies=" << totals.movies_with_audio
                  << ", audio-chunks=" << totals.audio_chunks
                  << ", samples=" << totals.stereo_sample_frames << '\n';
        return 0;
    } catch (const sf::core::Error& error) {
        std::cerr << "sf_movie_probe: " << error.what() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "sf_movie_probe: unexpected error: " << error.what() << '\n';
        return 1;
    }
}
