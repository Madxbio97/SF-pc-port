#include "sf/media/str_decoder.hpp"

#include "sf/core/error.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <string>
#include <utility>

namespace sf::media {
namespace {

std::string ffmpegError(int error) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> text{};
    if (av_strerror(error, text.data(), text.size()) < 0) {
        return "FFmpeg error " + std::to_string(error);
    }
    return text.data();
}

void requireFfmpeg(int result, const char* operation) {
    if (result < 0) {
        throw core::Error{
            core::ErrorCode::invalid_format,
            std::string{operation} + ": " + ffmpegError(result)};
    }
}

double timestampSeconds(const AVFrame& frame, const AVStream& stream) {
    if (frame.best_effort_timestamp == AV_NOPTS_VALUE) {
        return 0.0;
    }
    return static_cast<double>(frame.best_effort_timestamp) * av_q2d(stream.time_base);
}

} // namespace

struct StrDecoder::Impl {
    explicit Impl(std::vector<std::byte> source) : source(std::move(source)) {}

    ~Impl() {
        sws_freeContext(sws);
        swr_free(&swr);
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&video_codec);
        avcodec_free_context(&audio_codec);
        avformat_close_input(&format);
        if (avio != nullptr) {
            av_freep(&avio->buffer);
            avio_context_free(&avio);
        }
    }

    static int read(void* opaque, std::uint8_t* output, int output_size) {
        auto& self = *static_cast<Impl*>(opaque);
        if (self.cursor >= self.source.size()) {
            return AVERROR_EOF;
        }
        const auto remaining = self.source.size() - self.cursor;
        const auto count = std::min<std::size_t>(remaining, static_cast<std::size_t>(output_size));
        std::memcpy(output, self.source.data() + self.cursor, count);
        self.cursor += count;
        return static_cast<int>(count);
    }

    static std::int64_t seek(void* opaque, std::int64_t offset, int origin) {
        auto& self = *static_cast<Impl*>(opaque);
        if ((origin & AVSEEK_SIZE) != 0) {
            return static_cast<std::int64_t>(self.source.size());
        }
        origin &= ~AVSEEK_FORCE;
        std::int64_t base = 0;
        if (origin == SEEK_CUR) {
            base = static_cast<std::int64_t>(self.cursor);
        } else if (origin == SEEK_END) {
            base = static_cast<std::int64_t>(self.source.size());
        } else if (origin != SEEK_SET) {
            return AVERROR(EINVAL);
        }
        if ((offset < 0 && offset < -base) ||
            (offset > 0 && base > std::numeric_limits<std::int64_t>::max() - offset)) {
            return AVERROR(EINVAL);
        }
        const auto target = base + offset;
        if (target < 0 || static_cast<std::uint64_t>(target) > self.source.size()) {
            return AVERROR(EINVAL);
        }
        self.cursor = static_cast<std::size_t>(target);
        return target;
    }

    void initialize() {
        if (source.empty()) {
            throw core::Error{core::ErrorCode::invalid_argument, "STR source is empty"};
        }
        av_log_set_level(AV_LOG_ERROR);

        constexpr int io_buffer_size = 64 * 1024;
        auto* io_buffer = static_cast<std::uint8_t*>(av_malloc(io_buffer_size));
        if (io_buffer == nullptr) {
            throw core::Error{core::ErrorCode::io, "Cannot allocate FFmpeg I/O buffer"};
        }
        avio = avio_alloc_context(
            io_buffer, io_buffer_size, 0, this, &Impl::read, nullptr, &Impl::seek);
        if (avio == nullptr) {
            av_free(io_buffer);
            throw core::Error{core::ErrorCode::io, "Cannot create FFmpeg I/O context"};
        }

        format = avformat_alloc_context();
        if (format == nullptr) {
            throw core::Error{core::ErrorCode::io, "Cannot create FFmpeg format context"};
        }
        format->pb = avio;
        format->flags |= AVFMT_FLAG_CUSTOM_IO;
        requireFfmpeg(avformat_open_input(&format, nullptr, nullptr, nullptr), "Cannot open STR stream");
        requireFfmpeg(avformat_find_stream_info(format, nullptr), "Cannot inspect STR stream");

        openCodec(AVMEDIA_TYPE_VIDEO, video_stream, video_codec, true);
        openCodec(AVMEDIA_TYPE_AUDIO, audio_stream, audio_codec, false);
        if (video_codec == nullptr) {
            throw core::Error{core::ErrorCode::unsupported, "STR stream has no supported video"};
        }

        const auto rate = av_guess_frame_rate(format, format->streams[video_stream], nullptr);
        if (rate.num > 0 && rate.den > 0) {
            frames_per_second = av_q2d(rate);
        }
        packet = av_packet_alloc();
        frame = av_frame_alloc();
        if (packet == nullptr || frame == nullptr) {
            throw core::Error{core::ErrorCode::io, "Cannot allocate FFmpeg decode buffers"};
        }
    }

    void openCodec(
        AVMediaType type,
        int& stream_index,
        AVCodecContext*& context,
        bool required) {
        const AVCodec* codec = nullptr;
        const auto result = av_find_best_stream(format, type, -1, -1, &codec, 0);
        if (result < 0) {
            if (required) {
                requireFfmpeg(result, "Cannot find STR codec");
            }
            return;
        }
        stream_index = result;
        context = avcodec_alloc_context3(codec);
        if (context == nullptr) {
            throw core::Error{core::ErrorCode::io, "Cannot allocate STR codec context"};
        }
        requireFfmpeg(
            avcodec_parameters_to_context(context, format->streams[stream_index]->codecpar),
            "Cannot configure STR codec");
        requireFfmpeg(avcodec_open2(context, codec, nullptr), "Cannot open STR codec");
    }

    void receiveVideo() {
        for (;;) {
            const auto result = avcodec_receive_frame(video_codec, frame);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
                return;
            }
            requireFfmpeg(result, "Cannot decode STR video frame");
            if (frame->width <= 0 || frame->height <= 0 ||
                static_cast<std::uint64_t>(frame->width) * frame->height >
                    std::numeric_limits<std::size_t>::max()) {
                throw core::Error{core::ErrorCode::invalid_format, "Invalid STR video dimensions"};
            }

            MovieVideoFrame output;
            output.width = frame->width;
            output.height = frame->height;
            output.timestamp_seconds = timestampSeconds(*frame, *format->streams[video_stream]);
            output.rgba8888.resize(
                static_cast<std::size_t>(output.width) *
                static_cast<std::size_t>(output.height) * 4U);
            sws = sws_getCachedContext(
                sws,
                frame->width,
                frame->height,
                static_cast<AVPixelFormat>(frame->format),
                frame->width,
                frame->height,
                AV_PIX_FMT_RGBA,
                SWS_BILINEAR | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT |
                    SWS_FULL_CHR_H_INP,
                nullptr,
                nullptr,
                nullptr);
            if (sws == nullptr) {
                throw core::Error{core::ErrorCode::unsupported, "Cannot convert STR video pixels"};
            }
            auto* destination = output.rgba8888.data();
            const int destination_stride = output.width * 4;
            requireFfmpeg(
                sws_scale(
                    sws,
                    frame->data,
                    frame->linesize,
                    0,
                    frame->height,
                    &destination,
                    &destination_stride),
                "Cannot convert STR video frame");
            events.emplace_back(std::move(output));
            av_frame_unref(frame);
        }
    }

    void initializeResampler(const AVFrame& input) {
        if (swr != nullptr) {
            return;
        }
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        const auto input_rate = input.sample_rate > 0 ? input.sample_rate : audio_codec->sample_rate;
        requireFfmpeg(
            swr_alloc_set_opts2(
                &swr,
                &stereo,
                AV_SAMPLE_FMT_S16,
                input_rate,
                &input.ch_layout,
                static_cast<AVSampleFormat>(input.format),
                input_rate,
                0,
                nullptr),
            "Cannot configure STR audio conversion");
        requireFfmpeg(swr_init(swr), "Cannot initialize STR audio conversion");
    }

    void receiveAudio() {
        for (;;) {
            const auto result = avcodec_receive_frame(audio_codec, frame);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
                return;
            }
            requireFfmpeg(result, "Cannot decode STR audio frame");
            initializeResampler(*frame);
            const auto sample_rate = frame->sample_rate > 0 ? frame->sample_rate : audio_codec->sample_rate;
            const auto delayed = swr_get_delay(swr, sample_rate);
            const auto capacity = static_cast<int>(av_rescale_rnd(
                delayed + frame->nb_samples,
                sample_rate,
                sample_rate,
                AV_ROUND_UP));

            MovieAudioChunk output;
            output.sample_rate = sample_rate;
            output.timestamp_seconds = timestampSeconds(*frame, *format->streams[audio_stream]);
            output.stereo_samples.resize(static_cast<std::size_t>(capacity) * 2U);
            std::uint8_t* destination =
                reinterpret_cast<std::uint8_t*>(output.stereo_samples.data());
            std::vector<const std::uint8_t*> input_planes(
                frame->extended_data,
                frame->extended_data + std::max(1, frame->ch_layout.nb_channels));
            const auto converted = swr_convert(
                swr,
                &destination,
                capacity,
                input_planes.data(),
                frame->nb_samples);
            requireFfmpeg(converted, "Cannot convert STR audio frame");
            output.stereo_samples.resize(static_cast<std::size_t>(converted) * 2U);
            if (!output.stereo_samples.empty()) {
                events.emplace_back(std::move(output));
            }
            av_frame_unref(frame);
        }
    }

    void sendPacket(AVCodecContext* codec, bool video) {
        const auto result = avcodec_send_packet(codec, packet);
        requireFfmpeg(result, "Cannot submit STR packet");
        if (video) {
            receiveVideo();
        } else {
            receiveAudio();
        }
    }

    void flush() {
        if (flushed) {
            return;
        }
        flushed = true;
        requireFfmpeg(avcodec_send_packet(video_codec, nullptr), "Cannot flush STR video");
        receiveVideo();
        if (audio_codec != nullptr) {
            requireFfmpeg(avcodec_send_packet(audio_codec, nullptr), "Cannot flush STR audio");
            receiveAudio();
        }
    }

    std::optional<MovieEvent> next() {
        while (events.empty() && !finished) {
            const auto result = av_read_frame(format, packet);
            if (result == AVERROR_EOF) {
                flush();
                finished = true;
                break;
            }
            requireFfmpeg(result, "Cannot read STR packet");
            if (packet->stream_index == video_stream) {
                sendPacket(video_codec, true);
            } else if (audio_codec != nullptr && packet->stream_index == audio_stream) {
                sendPacket(audio_codec, false);
            }
            av_packet_unref(packet);
        }
        if (events.empty()) {
            return std::nullopt;
        }
        auto event = std::move(events.front());
        events.pop_front();
        return event;
    }

    std::vector<std::byte> source;
    std::size_t cursor{};
    AVIOContext* avio{};
    AVFormatContext* format{};
    AVCodecContext* video_codec{};
    AVCodecContext* audio_codec{};
    AVPacket* packet{};
    AVFrame* frame{};
    SwsContext* sws{};
    SwrContext* swr{};
    int video_stream{-1};
    int audio_stream{-1};
    double frames_per_second{15.0};
    bool flushed{};
    bool finished{};
    std::deque<MovieEvent> events;
};

StrDecoder::StrDecoder(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

StrDecoder StrDecoder::open(std::vector<std::byte> raw_sectors) {
    auto impl = std::make_unique<Impl>(std::move(raw_sectors));
    impl->initialize();
    return StrDecoder{std::move(impl)};
}

StrDecoder::~StrDecoder() = default;
StrDecoder::StrDecoder(StrDecoder&&) noexcept = default;
StrDecoder& StrDecoder::operator=(StrDecoder&&) noexcept = default;

std::optional<MovieEvent> StrDecoder::next() {
    return impl_->next();
}

double StrDecoder::framesPerSecond() const noexcept {
    return impl_->frames_per_second;
}

bool StrDecoder::hasAudio() const noexcept {
    return impl_->audio_codec != nullptr;
}

} // namespace sf::media
