extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
}
#include <SDL2/SDL.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "AsyncNetworkWorker.h"
#include "ClientConfig.h"
#include "ClientNetworking.h"
#include "ConfigurationPanel.h"
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

// Uncomment the following line to enable verbose debug logging:
// #define DEBUG_LOGGING

// Global debug flags that can be set via command line
bool g_audio_debug = false;

// Audio debug logging helper
#define AUDIO_LOG(msg)                                   \
    do                                                   \
    {                                                    \
        if (g_audio_debug)                               \
        {                                                \
            std::cout << "[Audio] " << msg << std::endl; \
        }                                                \
    } while (0)

namespace
{
    using client_config::CameraConfig;
    using client_config::ClientConfig;
    using client_config::create_default_client_config;
    using client_config::kClientConfigFileName;
    using client_config::kUnknownCameraName;
    using client_config::load_client_config;
    using client_config::resolve_config_path;
    using client_config::save_client_config;
    using client_config::sync_json_from_client_config;
    using client_network::build_proxy_rtsp_url;
    using client_network::extract_host_from_endpoint;
    using client_network::send_add_camera_request;

    constexpr int kDefaultCellWidth = 640;
    constexpr int kDefaultCellHeight = 360;
    constexpr std::chrono::milliseconds kStreamRetryInitialDelay{1500};
    constexpr std::chrono::seconds kStreamStallThreshold{5};
    constexpr std::chrono::seconds kStreamReadTimeout{5};

} // namespace

struct AudioData
{
    std::deque<uint8_t> buffer;
    std::mutex mutex;
    std::atomic<int> volume_percent{100};
    std::atomic<bool> muted{false};
};

// SDL audio callback
void audio_callback(void *userdata, Uint8 *stream, int len)
{
    if (g_audio_debug)
    {
        static int call_count = 0;
        if (call_count++ % 100 == 0) // Log every 100th call
        {
            std::cout << "[Audio] Callback requested " << len << " bytes" << std::endl;
        }

        if (call_count == 1)
        {
            AudioData *audio = (AudioData *)userdata;
            std::lock_guard<std::mutex> lock(audio->mutex);
            std::cout << "[Audio] Callback buffer size: " << audio->buffer.size() << " bytes" << std::endl;
        }
    }

    AudioData *audio = (AudioData *)userdata;
    std::lock_guard<std::mutex> lock(audio->mutex);

    if (false) // Placeholder to keep structure
    {
        std::cout << "" << std::endl;
    }

    int copied = 0;
    while (!audio->buffer.empty() && copied < len)
    {
        stream[copied++] = audio->buffer.front();
        audio->buffer.pop_front();
    }
    while (copied < len)
        stream[copied++] = 0; // silence

    const bool muted = audio->muted.load(std::memory_order_relaxed);
    const int volume_percent = audio->volume_percent.load(std::memory_order_relaxed);

    if (muted || volume_percent <= 0)
    {
        std::memset(stream, 0, static_cast<size_t>(len));
        return;
    }

    if (volume_percent >= 100)
    {
        return;
    }

    // Wanted spec is AUDIO_S16SYS stereo; scale samples in-place.
    const int sample_count = len / static_cast<int>(sizeof(int16_t));
    auto *samples = reinterpret_cast<int16_t *>(stream);
    for (int i = 0; i < sample_count; ++i)
    {
        int32_t scaled = (static_cast<int32_t>(samples[i]) * volume_percent) / 100;
        scaled = std::clamp<int32_t>(scaled, -32768, 32767);
        samples[i] = static_cast<int16_t>(scaled);
    }
}

struct StreamInterruptContext
{
    bool abort = false;
    std::chrono::steady_clock::time_point deadline{};
};

static int ffmpeg_interrupt_callback(void *opaque)
{
    auto *ctx = static_cast<StreamInterruptContext *>(opaque);
    if (!ctx)
    {
        return 0;
    }
    if (ctx->abort)
    {
        return 1;
    }
    if (ctx->deadline != std::chrono::steady_clock::time_point{} &&
        std::chrono::steady_clock::now() >= ctx->deadline)
    {
        return 1;
    }
    return 0;
}

struct VideoStreamCtx
{
    AVFormatContext *fmt_ctx = nullptr;
    AVCodecContext *vctx = nullptr;
    AVCodecContext *actx = nullptr; // only used for stream 0 (audio)
    int video_stream_index = -1;
    int audio_stream_index = -1;
    SwsContext *sws = nullptr;

    AVFrame *vframe = nullptr;
    AVFrame *vframe_rgb = nullptr;
    uint8_t *rgb_buffer = nullptr;
    int rgb_linesize = 0;

    AVPacket *pkt = nullptr;
    AVFrame *aframe = nullptr; // only used if audio
    StreamInterruptContext interrupt_ctx;
    int frame_width = 0;
    int frame_height = 0;
    AVPixelFormat frame_pix_fmt = AV_PIX_FMT_NONE;
    std::thread worker;
    std::mutex frame_mutex;
    int64_t frame_generation = 0;
    int64_t last_consumed_generation = -1;
    bool frame_available = false;
    std::atomic<bool> worker_stop{false};
    std::atomic<bool> worker_failed{false};
    std::atomic<bool> pending_reference_update{false};
};

int main(int argc, char **argv)
{
    const int GRID_COLS = 2; // 4 for 4x4
    const int GRID_ROWS = 2; // 4 for 4x4
    const int TOTAL_SLOTS = GRID_COLS * GRID_ROWS;

    // Parse command-line arguments for debug flags and RTSP URLs
    std::vector<std::string> rtsp_urls;
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "--debug" && i + 1 < argc)
        {
            std::string debug_type = argv[++i];
            if (debug_type == "audio")
            {
                g_audio_debug = true;
                std::cout << "[Debug] Audio debugging enabled" << std::endl;
            }
            else
            {
                std::cerr << "Unknown debug type: " << debug_type << std::endl;
                std::cerr << "Available types: audio" << std::endl;
            }
        }
        else if (arg.find("rtsp://") == 0)
        {
            rtsp_urls.push_back(arg);
        }
    }

    std::vector<CameraConfig> stream_configs;
    std::filesystem::path config_path = resolve_config_path(argv[0]);
    nlohmann::json client_config_json = nlohmann::json::object();
    ClientConfig client_config;
    bool config_loaded = false;
    bool placeholder_dimensions = false;

    if (!rtsp_urls.empty())
    {
        int provided_streams = static_cast<int>(rtsp_urls.size());
        if (provided_streams > TOTAL_SLOTS)
        {
            std::cerr << "Warning: ignoring extra RTSP URLs beyond " << TOTAL_SLOTS << " slots." << "\n";
        }
        stream_configs.reserve(std::min(provided_streams, TOTAL_SLOTS));
        for (int i = 0; i < std::min(provided_streams, TOTAL_SLOTS); ++i)
        {
            CameraConfig camera;
            camera.ip = rtsp_urls[i];
            camera.name = camera.ip;
            stream_configs.push_back(std::move(camera));
        }
    }
    else
    {
        try
        {
            client_config = load_client_config(config_path, client_config_json);
            config_loaded = true;
            stream_configs = client_config.cameras;
        }
        catch (const std::exception &err)
        {
            std::cerr << err.what() << "\n";
            return 1;
        }
        if (stream_configs.size() > static_cast<size_t>(TOTAL_SLOTS))
        {
            std::cerr << "Warning: more cameras than available slots; truncating to "
                      << TOTAL_SLOTS << " entries from config." << "\n";
            stream_configs.resize(TOTAL_SLOTS);
            client_config.cameras = stream_configs;
        }
    }

    if (!config_loaded)
    {
        client_config.cameras = stream_configs;
    }

    if (client_config.server_endpoint.empty())
    {
        client_config.server_endpoint = "http://localhost:8080";
    }
    if (client_config.server_ip.empty())
    {
        client_config.server_ip = extract_host_from_endpoint(client_config.server_endpoint);
    }

    if (stream_configs.empty())
    {
        std::cout << "No camera streams configured; starting with an empty dashboard." << "\n";
        placeholder_dimensions = true;
    }

    av_log_set_level(AV_LOG_ERROR);
    avformat_network_init();

    // Initialize SDL
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0)
    {
        std::cerr << "SDL_Init error: " << SDL_GetError() << "\n";
        return 1;
    }

    // We assume all streams have the same resolution; we’ll use stream 0 as reference
    int single_w = 0;
    int single_h = 0;
    bool reference_dimensions_ready = false;

    std::size_t limited_streams = std::min(stream_configs.size(), static_cast<std::size_t>(TOTAL_SLOTS));
    int stream_count = static_cast<int>(limited_streams);
    stream_configs.resize(stream_count);
    std::vector<std::string> stream_urls(stream_count);
    std::vector<std::string> stream_names(stream_count);
    for (int i = 0; i < stream_count; ++i)
    {
        stream_urls[i] = stream_configs[i].ip;
        stream_names[i] = stream_configs[i].name.empty() ? kUnknownCameraName : stream_configs[i].name;
    }

    ConfigurationWindowSettings window_settings = client_config.window_settings;

    auto persist_config = [&]() -> bool
    {
        client_config.cameras = stream_configs;
        try
        {
            sync_json_from_client_config(client_config_json, client_config);
            save_client_config(client_config_json, config_path);
            return true;
        }
        catch (const std::exception &err)
        {
            std::cerr << "Failed to persist configuration: " << err.what() << "\n";
            return false;
        }
    };

    auto persist_window_settings = [&](const ConfigurationWindowSettings &settings)
    {
        client_config.window_settings = settings;
        persist_config();
    };

    std::deque<VideoStreamCtx> streams(stream_count);
    std::vector<bool> overlay_always_show_stream(stream_count, false);
    overlay_always_show_stream.reserve(TOTAL_SLOTS);
    std::vector<std::chrono::steady_clock::time_point> stream_retry_deadlines(stream_count);
    stream_retry_deadlines.reserve(TOTAL_SLOTS);
    std::vector<std::chrono::steady_clock::time_point> stream_last_frame_times(stream_count);
    stream_last_frame_times.reserve(TOTAL_SLOTS);
    std::vector<bool> stream_stall_reported(stream_count, false);
    stream_stall_reported.reserve(TOTAL_SLOTS);
    std::mutex stream_state_mutex;

    SwrContext *swr = nullptr;
    AudioData audio_data;
    SDL_AudioSpec wanted_spec{};
    bool audio_device_open = false;
    std::atomic<int> active_audio_stream{0}; // Track which stream provides audio

    // Audio source switch notification
    std::string audio_switch_notification;
    std::chrono::steady_clock::time_point audio_switch_notification_time;
    const std::chrono::milliseconds kNotificationDisplayDuration{2500};
    const std::chrono::milliseconds kNotificationFadeInDuration{150};
    const std::chrono::milliseconds kNotificationFadeOutDuration{300};

    const std::chrono::milliseconds kOverlayAutoHideDuration{3000};
    std::chrono::steady_clock::time_point last_pointer_activity_time = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point audio_controls_last_interaction_time = std::chrono::steady_clock::now();

    auto record_stream_open = [&](int idx)
    {
        if (idx < 0 || idx >= static_cast<int>(stream_last_frame_times.size()))
        {
            return;
        }
        std::lock_guard<std::mutex> lock(stream_state_mutex);
        stream_last_frame_times[idx] = std::chrono::steady_clock::now();
        stream_stall_reported[idx] = false;
        if (idx < static_cast<int>(stream_retry_deadlines.size()))
        {
            stream_retry_deadlines[idx] = std::chrono::steady_clock::time_point{};
        }
    };

    auto schedule_stream_retry = [&](int idx, std::chrono::milliseconds delay)
    {
        if (idx < 0 || idx >= static_cast<int>(stream_retry_deadlines.size()))
        {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(stream_state_mutex);
            if (idx < static_cast<int>(stream_stall_reported.size()))
            {
                stream_stall_reported[idx] = false;
            }
            if (idx < static_cast<int>(stream_last_frame_times.size()))
            {
                stream_last_frame_times[idx] = std::chrono::steady_clock::time_point{};
            }
            stream_retry_deadlines[idx] = std::chrono::steady_clock::now() + delay;
        }
        std::string stream_label = (idx < static_cast<int>(stream_names.size()) && !stream_names[idx].empty())
                                       ? stream_names[idx]
                                       : ("Stream " + std::to_string(idx));
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1) << std::chrono::duration<double>(delay).count();
#ifdef DEBUG_LOGGING
        std::cerr << "[diag] Stream \"" << stream_label << "\" retry scheduled in " << oss.str() << "s\n";
#endif
    };

    int canvas_w = 0;
    int canvas_h = 0;
    std::vector<uint8_t> canvas_buffer;
    uint8_t *canvas_data[4] = {nullptr, nullptr, nullptr, nullptr};
    int canvas_linesize[4] = {0, 0, 0, 0};
    SDL_Window *win = nullptr;
    SDL_Renderer *renderer = nullptr;
    SDL_Texture *texture = nullptr;
    std::function<bool(int, int, bool)> ensure_canvas_dimensions;

    auto stop_stream_worker = [&](VideoStreamCtx &s)
    {
        s.worker_stop.store(true);
        s.interrupt_ctx.abort = true;
        if (s.worker.joinable())
        {
            s.worker.join();
        }
        s.worker_stop.store(false);
    };

    auto release_stream = [&](VideoStreamCtx &s)
    {
        stop_stream_worker(s);
        s.interrupt_ctx.abort = true;
        s.interrupt_ctx.deadline = std::chrono::steady_clock::time_point{};
        if (s.pkt)
        {
            av_packet_free(&s.pkt);
            s.pkt = nullptr;
        }
        if (s.aframe)
        {
            av_frame_free(&s.aframe);
            s.aframe = nullptr;
        }
        if (s.vframe)
        {
            av_frame_free(&s.vframe);
            s.vframe = nullptr;
        }
        if (s.vframe_rgb)
        {
            av_frame_free(&s.vframe_rgb);
            s.vframe_rgb = nullptr;
        }
        if (s.rgb_buffer)
        {
            av_free(s.rgb_buffer);
            s.rgb_buffer = nullptr;
        }
        if (s.sws)
        {
            sws_freeContext(s.sws);
            s.sws = nullptr;
        }
        if (s.vctx)
        {
            avcodec_free_context(&s.vctx);
            s.vctx = nullptr;
        }
        if (s.actx)
        {
            avcodec_free_context(&s.actx);
            s.actx = nullptr;
        }
        if (s.fmt_ctx)
        {
            s.fmt_ctx->interrupt_callback.callback = nullptr;
            s.fmt_ctx->interrupt_callback.opaque = nullptr;
            avformat_close_input(&s.fmt_ctx);
            s.fmt_ctx = nullptr;
        }
        s.video_stream_index = -1;
        s.audio_stream_index = -1;
        s.rgb_linesize = 0;
        s.interrupt_ctx = {};
        s.frame_width = 0;
        s.frame_height = 0;
        s.frame_pix_fmt = AV_PIX_FMT_NONE;
        s.frame_generation = 0;
        s.last_consumed_generation = -1;
        s.frame_available = false;
        s.worker_failed.store(false);
        s.pending_reference_update.store(false);
    };

    auto configure_scaler = [&](VideoStreamCtx &stream, int width, int height, AVPixelFormat src_fmt, const std::string &label) -> bool
    {
        stream.frame_width = 0;
        stream.frame_height = 0;
        stream.frame_pix_fmt = AV_PIX_FMT_NONE;

        if (width <= 0 || height <= 0)
        {
            std::cerr << "Invalid frame dimensions for stream " << label << ": " << width << "x" << height << "\n";
            return false;
        }

        if (stream.sws)
        {
            sws_freeContext(stream.sws);
            stream.sws = nullptr;
        }
        if (stream.vframe_rgb)
        {
            av_frame_free(&stream.vframe_rgb);
            stream.vframe_rgb = nullptr;
        }
        if (stream.rgb_buffer)
        {
            av_free(stream.rgb_buffer);
            stream.rgb_buffer = nullptr;
        }
        stream.rgb_linesize = 0;

        stream.sws = sws_getContext(
            width, height, src_fmt,
            width, height, AV_PIX_FMT_RGB24,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!stream.sws)
        {
            std::cerr << "Failed to create scaler for stream " << label << " (" << width << "x" << height << ")\n";
            return false;
        }

        stream.vframe_rgb = av_frame_alloc();
        if (!stream.vframe_rgb)
        {
            std::cerr << "Failed to allocate RGB frame for stream " << label << "\n";
            sws_freeContext(stream.sws);
            stream.sws = nullptr;
            return false;
        }

        int num_bytes = av_image_get_buffer_size(AV_PIX_FMT_RGB24, width, height, 1);
        if (num_bytes <= 0)
        {
            std::cerr << "Invalid RGB buffer size for stream " << label << "\n";
            av_frame_free(&stream.vframe_rgb);
            stream.vframe_rgb = nullptr;
            sws_freeContext(stream.sws);
            stream.sws = nullptr;
            return false;
        }

        stream.rgb_buffer = static_cast<uint8_t *>(av_malloc(num_bytes));
        if (!stream.rgb_buffer)
        {
            std::cerr << "Failed to allocate RGB buffer for stream " << label << "\n";
            av_frame_free(&stream.vframe_rgb);
            stream.vframe_rgb = nullptr;
            sws_freeContext(stream.sws);
            stream.sws = nullptr;
            return false;
        }

        if (av_image_fill_arrays(
                stream.vframe_rgb->data, stream.vframe_rgb->linesize,
                stream.rgb_buffer, AV_PIX_FMT_RGB24,
                width, height, 1) < 0)
        {
            std::cerr << "Failed to setup RGB frame for stream " << label << "\n";
            av_free(stream.rgb_buffer);
            stream.rgb_buffer = nullptr;
            av_frame_free(&stream.vframe_rgb);
            stream.vframe_rgb = nullptr;
            sws_freeContext(stream.sws);
            stream.sws = nullptr;
            return false;
        }

        stream.rgb_linesize = stream.vframe_rgb->linesize[0];
        stream.frame_width = width;
        stream.frame_height = height;
        stream.frame_pix_fmt = src_fmt;
        return true;
    };

    auto open_stream = [&](int idx, const std::string &url, bool set_reference) -> bool
    {
        if (idx < 0 || idx >= stream_count)
        {
            return false;
        }
        VideoStreamCtx &s = streams[idx];
        release_stream(s);

        s.worker_stop.store(false);
        s.worker_failed.store(false);
        s.pending_reference_update.store(false);
        s.last_consumed_generation = -1;
        s.frame_generation = 0;
        s.frame_available = false;

        s.interrupt_ctx = {};
        s.fmt_ctx = avformat_alloc_context();
        if (!s.fmt_ctx)
        {
            std::cerr << "Failed to allocate format context for: " << url << "\n";
            return false;
        }
        s.fmt_ctx->interrupt_callback.callback = ffmpeg_interrupt_callback;
        s.fmt_ctx->interrupt_callback.opaque = &s.interrupt_ctx;

        AVDictionary *opts = nullptr;
        av_dict_set(&opts, "rtsp_transport", "tcp", 0);
        av_dict_set(&opts, "fflags", "nobuffer", 0);
        av_dict_set(&opts, "max_delay", "500000", 0);

        s.interrupt_ctx.deadline = std::chrono::steady_clock::now() + kStreamReadTimeout;
        if (avformat_open_input(&s.fmt_ctx, url.c_str(), nullptr, &opts) < 0)
        {
            std::cerr << "Could not open input: " << url << "\n";
            av_dict_free(&opts);
            release_stream(s);
            return false;
        }
        av_dict_free(&opts);

        s.interrupt_ctx.deadline = std::chrono::steady_clock::now() + kStreamReadTimeout;
        if (avformat_find_stream_info(s.fmt_ctx, nullptr) < 0)
        {
            std::cerr << "Could not find stream info: " << url << "\n";
            release_stream(s);
            return false;
        }
        s.interrupt_ctx.deadline = std::chrono::steady_clock::time_point{};

        s.video_stream_index = -1;
        s.audio_stream_index = -1;

        for (unsigned j = 0; j < s.fmt_ctx->nb_streams; ++j)
        {
            AVStream *st = s.fmt_ctx->streams[j];
            if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && s.video_stream_index == -1)
            {
                s.video_stream_index = j;
            }
            else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && s.audio_stream_index == -1)
            {
                s.audio_stream_index = j;
                AUDIO_LOG("Found audio stream at index " << j);
                AUDIO_LOG("Codec: " << avcodec_get_name(st->codecpar->codec_id));
                AUDIO_LOG("Sample rate: " << st->codecpar->sample_rate << " Hz");
#if LIBAVCODEC_VERSION_MAJOR >= 59
                AUDIO_LOG("Channels: " << st->codecpar->ch_layout.nb_channels);
#else
                AUDIO_LOG("Channels: " << st->codecpar->channels);
#endif
                AUDIO_LOG("Format: " << av_get_sample_fmt_name((AVSampleFormat)st->codecpar->format));
            }
        }
        if (s.video_stream_index == -1)
        {
            std::cerr << "No video stream in: " << url << "\n";
            release_stream(s);
            return false;
        }

        if (s.audio_stream_index == -1)
        {
            AUDIO_LOG("WARNING: No audio stream found in stream!");
        }

        const AVCodec *vcodec = avcodec_find_decoder(
            s.fmt_ctx->streams[s.video_stream_index]->codecpar->codec_id);
        if (!vcodec)
        {
            std::cerr << "Could not find video decoder for: " << url << "\n";
            release_stream(s);
            return false;
        }

        s.vctx = avcodec_alloc_context3(vcodec);
        if (!s.vctx)
        {
            std::cerr << "Failed to allocate video codec context for: " << url << "\n";
            release_stream(s);
            return false;
        }

        if (avcodec_parameters_to_context(
                s.vctx, s.fmt_ctx->streams[s.video_stream_index]->codecpar) < 0)
        {
            std::cerr << "Failed to copy video codec parameters for: " << url << "\n";
            release_stream(s);
            return false;
        }

        if (avcodec_open2(s.vctx, vcodec, nullptr) < 0)
        {
            std::cerr << "Could not open video codec for: " << url << "\n";
            release_stream(s);
            return false;
        }

        s.vframe = av_frame_alloc();
        if (!s.vframe)
        {
            std::cerr << "Failed to allocate video frame for: " << url << "\n";
            release_stream(s);
            return false;
        }

        s.pkt = av_packet_alloc();
        s.aframe = av_frame_alloc();
        if (!s.pkt || !s.aframe)
        {
            std::cerr << "Failed to allocate packet/frame for: " << url << "\n";
            release_stream(s);
            return false;
        }

        if (!configure_scaler(s, s.vctx->width, s.vctx->height, s.vctx->pix_fmt, url))
        {
            release_stream(s);
            return false;
        }

        if (set_reference && (!reference_dimensions_ready || single_w <= 0 || single_h <= 0))
        {
            single_w = s.frame_width;
            single_h = s.frame_height;
            reference_dimensions_ready = true;
        }

        return true;
    };

    auto start_stream_worker = [&](int idx)
    {
        if (idx < 0 || idx >= stream_count)
        {
            return;
        }

        VideoStreamCtx &stream = streams[idx];
        if (!stream.fmt_ctx || !stream.vctx || !stream.pkt || !stream.vframe)
        {
            return;
        }

        stop_stream_worker(stream);
        stream.worker_stop.store(false);
        stream.worker_failed.store(false);
        stream.pending_reference_update.store(false);
        stream.last_consumed_generation = -1;
        stream.frame_generation = 0;
        stream.frame_available = false;

        stream.worker = std::thread([&, idx]()
                                    {
            VideoStreamCtx &worker_stream = streams[idx];
            const std::string stream_label = (idx < static_cast<int>(stream_names.size()) && !stream_names[idx].empty())
                                                 ? stream_names[idx]
                                                 : ("Stream " + std::to_string(idx));

            while (!worker_stream.worker_stop.load())
            {
                worker_stream.interrupt_ctx.abort = false;
                worker_stream.interrupt_ctx.deadline = std::chrono::steady_clock::now() + kStreamReadTimeout;
                int read_result = av_read_frame(worker_stream.fmt_ctx, worker_stream.pkt);
                worker_stream.interrupt_ctx.deadline = std::chrono::steady_clock::time_point{};

                if (worker_stream.worker_stop.load())
                {
                    break;
                }

                if (read_result < 0)
                {
                    if (worker_stream.worker_stop.load() && read_result == AVERROR_EXIT)
                    {
                        break;
                    }
                    if (read_result == AVERROR(EAGAIN))
                    {
                        continue;
                    }
                    char errbuf[AV_ERROR_MAX_STRING_SIZE] = {};
                    av_strerror(read_result, errbuf, sizeof(errbuf));
#ifdef DEBUG_LOGGING
                    std::cerr << "[diag] worker read failure for \"" << stream_label << "\" (" << errbuf << ", code " << read_result << ")\n";
#endif
                    worker_stream.worker_failed.store(true);
                    break;
                }

                if (worker_stream.pkt->stream_index == worker_stream.video_stream_index)
                {
                    avcodec_send_packet(worker_stream.vctx, worker_stream.pkt);
                    while (!worker_stream.worker_stop.load() && avcodec_receive_frame(worker_stream.vctx, worker_stream.vframe) == 0)
                    {
                        int decoded_w = worker_stream.vframe->width > 0 ? worker_stream.vframe->width : worker_stream.vctx->width;
                        int decoded_h = worker_stream.vframe->height > 0 ? worker_stream.vframe->height : worker_stream.vctx->height;
                        AVPixelFormat decoded_fmt = worker_stream.vframe->format >= 0
                                                         ? static_cast<AVPixelFormat>(worker_stream.vframe->format)
                                                         : worker_stream.vctx->pix_fmt;
                        if (decoded_fmt == AV_PIX_FMT_NONE)
                        {
                            decoded_fmt = worker_stream.vctx->pix_fmt;
                        }

                        bool worker_failed = false;
                        {
                            std::lock_guard<std::mutex> lock(worker_stream.frame_mutex);
                            bool geometry_changed = decoded_w != worker_stream.frame_width ||
                                                     decoded_h != worker_stream.frame_height ||
                                                     decoded_fmt != worker_stream.frame_pix_fmt;
                            bool context_missing = !worker_stream.sws || !worker_stream.vframe_rgb || worker_stream.rgb_linesize <= 0;
                            if (geometry_changed || context_missing)
                            {
                                if (!configure_scaler(worker_stream, decoded_w, decoded_h, decoded_fmt, stream_label))
                                {
                                    worker_failed = true;
                                }
                                else
                                {
                                    worker_stream.pending_reference_update.store(true);
                                }
                            }

                            if (!worker_failed && worker_stream.sws && worker_stream.vframe_rgb)
                            {
                                sws_scale(worker_stream.sws,
                                          worker_stream.vframe->data, worker_stream.vframe->linesize,
                                          0, worker_stream.frame_height,
                                          worker_stream.vframe_rgb->data, worker_stream.vframe_rgb->linesize);
                                worker_stream.frame_generation++;
                                worker_stream.frame_available = true;
                            }
                            else if (worker_failed)
                            {
                                worker_stream.frame_available = false;
                            }
                        }

                        if (worker_failed)
                        {
                            worker_stream.worker_failed.store(true);
                            av_frame_unref(worker_stream.vframe);
                            break;
                        }

                        av_frame_unref(worker_stream.vframe);
                        if (worker_stream.worker_failed.load())
                        {
                            break;
                        }
                    }
                }
                else if (idx == active_audio_stream.load() && worker_stream.pkt->stream_index == worker_stream.audio_stream_index && worker_stream.actx)
                {
                    avcodec_send_packet(worker_stream.actx, worker_stream.pkt);
                    while (!worker_stream.worker_stop.load() && avcodec_receive_frame(worker_stream.actx, worker_stream.aframe) == 0)
                    {
                        if (!swr)
                        {
                            av_frame_unref(worker_stream.aframe);
                            continue;
                        }

                        uint8_t *out_buf[2] = {nullptr};
                        int out_samples = av_rescale_rnd(
                            swr_get_out_samples(swr, worker_stream.aframe->nb_samples),
                            44100, worker_stream.actx->sample_rate, AV_ROUND_UP);

                        if (av_samples_alloc(out_buf, nullptr, 2, out_samples, AV_SAMPLE_FMT_S16, 0) < 0)
                        {
                            av_frame_unref(worker_stream.aframe);
                            break;
                        }

                        int converted = swr_convert(
                            swr, out_buf, out_samples,
                            (const uint8_t **)worker_stream.aframe->data, worker_stream.aframe->nb_samples);

                        int out_size = av_samples_get_buffer_size(nullptr, 2, converted, AV_SAMPLE_FMT_S16, 1);
                        if (out_size > 0 && out_buf[0])
                        {
                            std::lock_guard<std::mutex> lock(audio_data.mutex);
                            audio_data.buffer.insert(audio_data.buffer.end(), out_buf[0], out_buf[0] + out_size);
                        }
                        av_freep(&out_buf[0]);
                        av_frame_unref(worker_stream.aframe);
                    }
                }

                if (worker_stream.pkt)
                {
                    av_packet_unref(worker_stream.pkt);
                }
            }

            if (worker_stream.pkt)
            {
                av_packet_unref(worker_stream.pkt);
            } });
    };

    auto configure_audio = [&](VideoStreamCtx &audio_stream) -> bool
    {
        AUDIO_LOG("configure_audio() called");

        // Find which stream index this is
        int stream_idx = -1;
        for (int i = 0; i < stream_count; ++i)
        {
            if (&streams[i] == &audio_stream)
            {
                stream_idx = i;
                break;
            }
        }
        AUDIO_LOG("Configuring audio for stream index: " << stream_idx);
        AUDIO_LOG("audio_stream.audio_stream_index = " << audio_stream.audio_stream_index);

        if (audio_device_open)
        {
            AUDIO_LOG("Audio device already open, pausing...");
            SDL_PauseAudio(1);
        }

        {
            std::lock_guard<std::mutex> lock(audio_data.mutex);
            audio_data.buffer.clear();
        }

        if (swr)
        {
            swr_free(&swr);
            swr = nullptr;
        }

        if (audio_stream.audio_stream_index == -1)
        {
            AUDIO_LOG("No audio stream index, skipping audio setup");
            return true;
        }

        AUDIO_LOG("Setting up audio decoder...");

        const AVCodec *acodec =
            avcodec_find_decoder(audio_stream.fmt_ctx
                                     ->streams[audio_stream.audio_stream_index]
                                     ->codecpar->codec_id);
        if (!acodec)
        {
            std::cerr << "Failed to find audio decoder\n";
            return false;
        }

        audio_stream.actx = avcodec_alloc_context3(acodec);
        if (!audio_stream.actx)
        {
            std::cerr << "Failed to allocate audio codec context\n";
            return false;
        }

        if (avcodec_parameters_to_context(
                audio_stream.actx,
                audio_stream.fmt_ctx->streams[audio_stream.audio_stream_index]->codecpar) < 0)
        {
            std::cerr << "Failed to copy audio codec parameters\n";
            avcodec_free_context(&audio_stream.actx);
            return false;
        }

        if (avcodec_open2(audio_stream.actx, acodec, nullptr) < 0)
        {
            std::cerr << "Failed to open audio codec\n";
            avcodec_free_context(&audio_stream.actx);
            return false;
        }

        bool need_init = true;
#if LIBAVCODEC_VERSION_MAJOR >= 59
        AVChannelLayout out_layout;
        av_channel_layout_default(&out_layout, 2);
        AVChannelLayout in_layout = audio_stream.actx->ch_layout;
        if (in_layout.nb_channels == 0)
        {
            av_channel_layout_default(&in_layout, 2);
        }
        if (swr_alloc_set_opts2(&swr,
                                &out_layout, AV_SAMPLE_FMT_S16, 44100,
                                &in_layout, audio_stream.actx->sample_fmt, audio_stream.actx->sample_rate,
                                0, nullptr) < 0)
        {
            std::cerr << "Failed to configure audio resampler\n";
            avcodec_free_context(&audio_stream.actx);
            return false;
        }
#else
        swr = swr_alloc();
        if (!swr)
        {
            std::cerr << "Failed to configure audio resampler\n";
            avcodec_free_context(&audio_stream.actx);
            return false;
        }
        uint64_t in_layout = audio_stream.actx->channel_layout ? audio_stream.actx->channel_layout
                                                               : av_get_default_channel_layout(audio_stream.actx->channels);
        av_opt_set_int(swr, "in_channel_layout", in_layout, 0);
        av_opt_set_int(swr, "out_channel_layout", AV_CH_LAYOUT_STEREO, 0);
        av_opt_set_int(swr, "in_sample_rate", audio_stream.actx->sample_rate, 0);
        av_opt_set_int(swr, "out_sample_rate", 44100, 0);
        av_opt_set_sample_fmt(swr, "in_sample_fmt", audio_stream.actx->sample_fmt, 0);
        av_opt_set_sample_fmt(swr, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
        if (swr_init(swr) < 0)
        {
            std::cerr << "Failed to initialise audio resampler\n";
            avcodec_free_context(&audio_stream.actx);
            swr_free(&swr);
            return false;
        }
        need_init = false;
#endif

        if (need_init && swr_init(swr) < 0)
        {
            std::cerr << "Failed to initialise audio resampler\n";
            avcodec_free_context(&audio_stream.actx);
            swr_free(&swr);
            return false;
        }

        wanted_spec.freq = 44100;
        wanted_spec.format = AUDIO_S16SYS;
        wanted_spec.channels = 2;
        wanted_spec.samples = 1024;
        wanted_spec.callback = audio_callback;
        wanted_spec.userdata = &audio_data;

        if (!audio_device_open)
        {
            if (SDL_OpenAudio(&wanted_spec, nullptr) < 0)
            {
                AUDIO_LOG("ERROR: SDL_OpenAudio error: " << SDL_GetError());
            }
            else
            {
                audio_device_open = true;
                AUDIO_LOG("Opened audio device successfully");
                AUDIO_LOG("Requested: " << wanted_spec.freq << "Hz, " << (int)wanted_spec.channels << " channels");
            }
        }

        if (audio_device_open)
        {
            SDL_PauseAudio(0);
            AUDIO_LOG("Audio playback started/resumed");
        }

        return true;
    };

    auto add_camera_handler = [&](const AddCameraRequest &request) -> AddCameraResult
    {
        AddCameraResult result;

        if (stream_configs.size() >= static_cast<size_t>(TOTAL_SLOTS))
        {
            result.success = false;
            result.message = "All grid slots are in use.";
            return result;
        }
        if (request.rtsp_url.empty())
        {
            result.success = false;
            result.message = "RTSP address is required.";
            return result;
        }

        CameraConfig camera;
        std::string display_name = request.name;
        std::string status_message;

        auto trim_in_place = [](std::string &msg)
        {
            while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
            {
                msg.pop_back();
            }
        };

        if (request.connect_via_server)
        {
            if (display_name.empty())
            {
                result.success = false;
                result.message = "Camera name is required when using RichServer.";
                return result;
            }
            if (request.server_endpoint.empty())
            {
                result.success = false;
                result.message = "Server endpoint is required.";
                return result;
            }

            std::string response_body;
            auto network_result = send_add_camera_request(request, response_body);
            if (!network_result.success)
            {
                status_message = network_result.message;
                trim_in_place(status_message);
                if (status_message.empty())
                {
                    status_message = "Failed to add camera via RichServer.";
                }
                result.success = false;
                result.message = status_message;
                return result;
            }

            status_message = network_result.message;
            trim_in_place(status_message);
            if (status_message.empty())
            {
                status_message = "Camera added via RichServer.";
            }

            camera.name = display_name;
            camera.ip = request.rtsp_url;
            camera.via_server = true;
            camera.original_uri = request.rtsp_url;
            camera.segment = request.segment;
            camera.recording = request.recording;
            camera.overlay = request.overlay;
            camera.motion_frame = request.motion_frame;
            camera.gstreamer_proxy = request.gstreamer_proxy;
            camera.live555_proxy = request.live555_proxy;
            camera.segment_bitrate = request.segment_bitrate;
            camera.segment_speed_preset = request.segment_speed_preset;
            camera.proxy_bitrate = request.proxy_bitrate;
            camera.proxy_speed_preset = request.proxy_speed_preset;
            camera.motion_frame_width = request.motion_frame_width;
            camera.motion_frame_height = request.motion_frame_height;
            camera.motion_frame_scale = request.motion_frame_scale;
            camera.noise_threshold = request.noise_threshold;
            camera.motion_threshold = request.motion_threshold;
            camera.motion_min_hits = request.motion_min_hits;
            camera.motion_decay = request.motion_decay;
            camera.motion_arrow_scale = request.motion_arrow_scale;
            camera.motion_arrow_thickness = request.motion_arrow_thickness;

            if (request.gstreamer_proxy || request.live555_proxy)
            {
                std::string proxied = build_proxy_rtsp_url(request.server_endpoint, request.name);
                if (!proxied.empty())
                {
                    camera.ip = proxied;
                    status_message += " Stream available at " + proxied + '.';
                }
            }

            client_config.server_endpoint = request.server_endpoint;
            auto host = extract_host_from_endpoint(request.server_endpoint);
            if (!host.empty())
            {
                client_config.server_ip = host;
            }
        }
        else
        {
            if (display_name.empty())
            {
                display_name = request.rtsp_url;
            }
            camera.name = display_name;
            camera.ip = request.rtsp_url;
            camera.via_server = false;
            status_message = "Camera added directly.";
            if (!camera.ip.empty())
            {
                status_message += " Stream available at " + camera.ip + '.';
            }
        }

        if (camera.name.empty())
        {
            camera.name = camera.ip.empty() ? kUnknownCameraName : camera.ip;
        }

        int new_index = stream_count;

        stream_configs.push_back(camera);
        stream_urls.push_back(camera.ip);
        stream_names.push_back(camera.name.empty() ? kUnknownCameraName : camera.name);
        streams.emplace_back();
        overlay_always_show_stream.push_back(false);
        stream_retry_deadlines.push_back(std::chrono::steady_clock::time_point{});
        stream_last_frame_times.push_back(std::chrono::steady_clock::time_point{});
        stream_stall_reported.push_back(false);
        stream_count = static_cast<int>(stream_configs.size());

        bool opened_immediately = open_stream(new_index, stream_urls[new_index], new_index == 0);
        if (!opened_immediately)
        {
            release_stream(streams[new_index]);
            if (request.connect_via_server)
            {
                schedule_stream_retry(new_index, kStreamRetryInitialDelay);
                status_message = "Camera registered. Waiting for stream to become available...";
                result.success = true;
                result.message = status_message;
            }
            else
            {
                streams.pop_back();
                overlay_always_show_stream.pop_back();
                stream_retry_deadlines.pop_back();
                stream_last_frame_times.pop_back();
                stream_stall_reported.pop_back();
                stream_configs.pop_back();
                stream_urls.pop_back();
                stream_names.pop_back();
                stream_count = static_cast<int>(stream_configs.size());

                result.success = false;
                result.message = "Failed to open new stream locally.";
                return result;
            }
        }
        else
        {
            record_stream_open(new_index);
            if (new_index == 0)
            {
                if (!configure_audio(streams[0]))
                {
                    std::cerr << "Failed to configure audio for new stream" << "\n";
                }
            }
            if (ensure_canvas_dimensions && streams[new_index].vctx)
            {
                if (!ensure_canvas_dimensions(streams[new_index].vctx->width, streams[new_index].vctx->height, placeholder_dimensions))
                {
                    std::cerr << "Failed to resize canvas for new stream." << "\n";
                }
            }
            start_stream_worker(new_index);
        }

        bool persisted = persist_config();
        if (!persisted)
        {
            status_message += " Configuration not saved.";
        }

        if (status_message.empty())
        {
            status_message = opened_immediately ? "Camera added." : "Camera registered. Waiting for stream to become available...";
        }
        result.message = status_message;
        result.success = opened_immediately || request.connect_via_server;
        return result;
    };

    bool show_metrics_window = window_settings.show_imgui_metrics;

    auto show_metrics_callback = [&](bool enabled)
    {
        show_metrics_window = enabled;
    };

    auto get_cameras_callback = [&]() -> std::vector<ConfigurationPanel::CameraInfo>
    {
        std::vector<ConfigurationPanel::CameraInfo> cameras;

        // Get cameras from server
        auto server_cameras = client_network::get_cameras_from_server(client_config.server_endpoint);

        // Merge with local camera info
        for (int i = 0; i < stream_count; ++i)
        {
            ConfigurationPanel::CameraInfo info;
            std::string cam_name = (i < static_cast<int>(stream_names.size()) && !stream_names[i].empty())
                                       ? stream_names[i]
                                       : ("Stream " + std::to_string(i));
            info.name = cam_name;
            info.via_server = (i < static_cast<int>(stream_configs.size())) ? stream_configs[i].via_server : false;

            // Try to find matching camera from server to get motion properties
            bool found_in_server = false;
            for (const auto &server_cam : server_cameras)
            {
                if (server_cam.name == cam_name)
                {
                    info = server_cam;                              // Use server data which has all motion properties
                    info.via_server = stream_configs[i].via_server; // Keep local via_server flag
                    found_in_server = true;
                    break;
                }
            }

            // If not found in server, use local defaults
            if (!found_in_server)
            {
                info.motion_enabled = (i < static_cast<int>(stream_configs.size())) ? stream_configs[i].motion_frame : false;
                info.motion_frame_scale = 1.0f;
                info.noise_threshold = 1.0f;
                info.motion_threshold = 5.0f;
                info.motion_min_hits = 3;
                info.motion_decay = 1;
                info.motion_arrow_scale = 2.5f;
                info.motion_arrow_thickness = 1;
            }

            cameras.push_back(info);
        }
        return cameras;
    };

    auto toggle_motion_callback = [&](const std::string &camera_name, bool enable) -> bool
    {
        bool result = client_network::toggle_motion_detection(client_config.server_endpoint, camera_name, enable);
        if (result)
        {
            // Update local config
            for (auto &config : stream_configs)
            {
                if (config.name == camera_name)
                {
                    config.motion_frame = enable;
                    persist_config();
                    break;
                }
            }
        }
        return result;
    };

    SDL_Texture *motion_frame_texture = nullptr;
    std::vector<unsigned char> motion_frame_jpeg_buffer;

    auto fetch_motion_frame_callback = [&](const std::string &camera_name, void *&texture_out,
                                           int &width_out, int &height_out) -> bool
    {
        if (!renderer)
        {
            return false;
        }

        if (!client_network::fetch_motion_frame_jpeg(client_config.server_endpoint, camera_name, motion_frame_jpeg_buffer))
        {
            return false;
        }

        if (motion_frame_jpeg_buffer.empty())
        {
            return false;
        }

        // Use FFmpeg to decode JPEG
        AVCodecID codec_id = AV_CODEC_ID_MJPEG;
        const AVCodec *codec = avcodec_find_decoder(codec_id);
        if (!codec)
        {
            return false;
        }

        AVCodecContext *codec_ctx = avcodec_alloc_context3(codec);
        if (!codec_ctx)
        {
            return false;
        }

        if (avcodec_open2(codec_ctx, codec, nullptr) < 0)
        {
            avcodec_free_context(&codec_ctx);
            return false;
        }

        AVPacket *pkt = av_packet_alloc();
        AVFrame *frame = av_frame_alloc();
        if (!pkt || !frame)
        {
            if (pkt)
                av_packet_free(&pkt);
            if (frame)
                av_frame_free(&frame);
            avcodec_free_context(&codec_ctx);
            return false;
        }

        pkt->data = motion_frame_jpeg_buffer.data();
        pkt->size = static_cast<int>(motion_frame_jpeg_buffer.size());

        int ret = avcodec_send_packet(codec_ctx, pkt);
        if (ret < 0)
        {
            av_packet_free(&pkt);
            av_frame_free(&frame);
            avcodec_free_context(&codec_ctx);
            return false;
        }

        ret = avcodec_receive_frame(codec_ctx, frame);
        if (ret < 0)
        {
            av_packet_free(&pkt);
            av_frame_free(&frame);
            avcodec_free_context(&codec_ctx);
            return false;
        }

        // Convert to RGB24
        SwsContext *sws_ctx = sws_getContext(
            frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
            frame->width, frame->height, AV_PIX_FMT_RGB24,
            SWS_BILINEAR, nullptr, nullptr, nullptr);

        if (!sws_ctx)
        {
            av_packet_free(&pkt);
            av_frame_free(&frame);
            avcodec_free_context(&codec_ctx);
            return false;
        }

        // Use proper alignment for buffer allocation
        int num_bytes = av_image_get_buffer_size(AV_PIX_FMT_RGB24, frame->width, frame->height, 32);
        if (num_bytes < 0)
        {
            sws_freeContext(sws_ctx);
            av_packet_free(&pkt);
            av_frame_free(&frame);
            avcodec_free_context(&codec_ctx);
            return false;
        }

        uint8_t *rgb_buffer = static_cast<uint8_t *>(av_malloc(num_bytes));
        if (!rgb_buffer)
        {
            sws_freeContext(sws_ctx);
            av_packet_free(&pkt);
            av_frame_free(&frame);
            avcodec_free_context(&codec_ctx);
            return false;
        }

        AVFrame *rgb_frame = av_frame_alloc();
        if (!rgb_frame)
        {
            av_free(rgb_buffer);
            sws_freeContext(sws_ctx);
            av_packet_free(&pkt);
            av_frame_free(&frame);
            avcodec_free_context(&codec_ctx);
            return false;
        }

        int fill_ret = av_image_fill_arrays(rgb_frame->data, rgb_frame->linesize, rgb_buffer,
                                            AV_PIX_FMT_RGB24, frame->width, frame->height, 32);
        if (fill_ret < 0)
        {
            av_frame_free(&rgb_frame);
            av_free(rgb_buffer);
            sws_freeContext(sws_ctx);
            av_packet_free(&pkt);
            av_frame_free(&frame);
            avcodec_free_context(&codec_ctx);
            return false;
        }

        sws_scale(sws_ctx, frame->data, frame->linesize, 0, frame->height,
                  rgb_frame->data, rgb_frame->linesize);

        // Check if texture needs to be recreated (different dimensions)
        if (motion_frame_texture)
        {
            int existing_w = 0, existing_h = 0;
            SDL_QueryTexture(motion_frame_texture, nullptr, nullptr, &existing_w, &existing_h);
            if (existing_w != frame->width || existing_h != frame->height)
            {
                SDL_DestroyTexture(motion_frame_texture);
                motion_frame_texture = nullptr;
            }
        }

        // Create SDL texture if needed
        if (!motion_frame_texture)
        {
            motion_frame_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB24,
                                                     SDL_TEXTUREACCESS_STATIC, frame->width, frame->height);
        }

        bool success = false;
        if (motion_frame_texture)
        {
            int update_ret = SDL_UpdateTexture(motion_frame_texture, nullptr, rgb_buffer, rgb_frame->linesize[0]);
            if (update_ret == 0)
            {
                texture_out = motion_frame_texture;
                width_out = frame->width;
                height_out = frame->height;
                success = true;
            }
            else
            {
                SDL_DestroyTexture(motion_frame_texture);
                motion_frame_texture = nullptr;
            }
        }

        av_free(rgb_buffer);
        av_frame_free(&rgb_frame);
        sws_freeContext(sws_ctx);
        av_packet_free(&pkt);
        av_frame_free(&frame);
        avcodec_free_context(&codec_ctx);

        return success;
    };

    auto add_motion_region_callback = [&](const std::string &camera_name, int x, int y, int w, int h, float angle) -> int
    {
        return client_network::add_motion_region(client_config.server_endpoint, camera_name, x, y, w, h, angle);
    };

    auto remove_motion_region_callback = [&](const std::string &camera_name, int region_id) -> bool
    {
        return client_network::remove_motion_region(client_config.server_endpoint, camera_name, region_id);
    };

    auto clear_motion_regions_callback = [&](const std::string &camera_name) -> bool
    {
        return client_network::clear_motion_regions(client_config.server_endpoint, camera_name);
    };

    auto get_motion_regions_callback = [&](const std::string &camera_name) -> std::vector<ConfigurationPanel::MotionRegion>
    {
        return client_network::get_motion_regions(client_config.server_endpoint, camera_name);
    };

    // Create async network worker for non-blocking network operations
    AsyncNetworkWorker async_network_worker;

    ConfigurationPanel configuration_panel(window_settings, persist_window_settings, add_camera_handler,
                                           client_config.server_endpoint, nullptr, show_metrics_callback,
                                           get_cameras_callback, toggle_motion_callback, fetch_motion_frame_callback,
                                           add_motion_region_callback, remove_motion_region_callback,
                                           clear_motion_regions_callback, get_motion_regions_callback);

    // Connect async worker to configuration panel
    configuration_panel.setAsyncWorker(&async_network_worker);

    for (int i = 0; i < stream_count; ++i)
    {
        if (!open_stream(i, stream_urls[i], i == 0))
        {
            return 5;
        }
        record_stream_open(i);
    }

    if (!reference_dimensions_ready)
    {
        single_w = kDefaultCellWidth;
        single_h = kDefaultCellHeight;
        placeholder_dimensions = true;
    }

    if (stream_count > 0)
    {
        VideoStreamCtx &audio_src = streams[0];
        if (!configure_audio(audio_src))
        {
            return 6;
        }
    }

    for (int i = 0; i < stream_count; ++i)
    {
        start_stream_worker(i);
    }

    // --- SDL Video: one big canvas ---
    canvas_w = std::max(single_w, 1) * GRID_COLS;
    canvas_h = std::max(single_h, 1) * GRID_ROWS;

    win = SDL_CreateWindow(
        "RTSP Grid Player",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        canvas_w, canvas_h,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!win)
    {
        std::cerr << "Failed to create SDL window: " << SDL_GetError() << "\n";
        return 1;
    }
    renderer = SDL_CreateRenderer(
        win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer)
    {
        std::cerr << "Failed to create SDL renderer: " << SDL_GetError() << "\n";
        return 1;
    }

    auto adjust_window_to_canvas = [&](bool allow_maximize)
    {
        if (!win)
        {
            return;
        }

        if (allow_maximize)
        {
            SDL_RestoreWindow(win);
        }

        int display_index = SDL_GetWindowDisplayIndex(win);
        SDL_Rect usable_bounds{};
        if (display_index >= 0 && SDL_GetDisplayUsableBounds(display_index, &usable_bounds) == 0)
        {
            int target_w = std::min(canvas_w, usable_bounds.w);
            int target_h = std::min(canvas_h, usable_bounds.h);
            SDL_SetWindowSize(win, target_w, target_h);
            if (allow_maximize && (canvas_w >= usable_bounds.w || canvas_h >= usable_bounds.h))
            {
                SDL_MaximizeWindow(win);
            }
        }
        else
        {
            SDL_SetWindowSize(win, canvas_w, canvas_h);
        }
    };

    ensure_canvas_dimensions = [&](int desired_single_w, int desired_single_h, bool force) -> bool
    {
        if (!renderer || !win)
        {
            return false;
        }

        desired_single_w = std::max(desired_single_w, 1);
        desired_single_h = std::max(desired_single_h, 1);

        int target_canvas_w = desired_single_w * GRID_COLS;
        int target_canvas_h = desired_single_h * GRID_ROWS;
        if (!force && desired_single_w == single_w && desired_single_h == single_h && texture &&
            canvas_w == target_canvas_w && canvas_h == target_canvas_h)
        {
            return true;
        }

        single_w = desired_single_w;
        single_h = desired_single_h;
        canvas_w = target_canvas_w;
        canvas_h = target_canvas_h;

        int bytes = av_image_get_buffer_size(AV_PIX_FMT_RGB24, canvas_w, canvas_h, 1);
        if (bytes <= 0)
        {
            std::cerr << "Invalid canvas dimensions requested (" << canvas_w << "x" << canvas_h << ")." << "\n";
            return false;
        }

        canvas_buffer.assign(static_cast<size_t>(bytes), 0);
        canvas_data[0] = canvas_buffer.data();
        canvas_data[1] = nullptr;
        canvas_data[2] = nullptr;
        canvas_data[3] = nullptr;
        av_image_fill_linesizes(canvas_linesize, AV_PIX_FMT_RGB24, canvas_w);

        if (texture)
        {
            SDL_DestroyTexture(texture);
            texture = nullptr;
        }
        texture = SDL_CreateTexture(
            renderer, SDL_PIXELFORMAT_RGB24,
            SDL_TEXTUREACCESS_STREAMING, canvas_w, canvas_h);
        if (!texture)
        {
            std::cerr << "Failed to create canvas texture: " << SDL_GetError() << "\n";
            return false;
        }

        std::fill(canvas_buffer.begin(), canvas_buffer.end(), 0);
        adjust_window_to_canvas(force);
        placeholder_dimensions = false;
        return true;
    };

    if (!ensure_canvas_dimensions(single_w, single_h, true))
    {
        std::cerr << "Failed to initialise canvas dimensions." << "\n";
        return 1;
    }

    // --- Dear ImGui setup for a primitive context menu ---
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForSDLRenderer(win, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    bool show_context_menu = false;
    bool menu_hovered = false;
    ImVec2 context_menu_pos = ImVec2(0.0f, 0.0f);
    int context_stream_index = -1;
    int hovered_stream = -1;
    bool reload_all_requested = false;
    int reload_stream_requested = -1;
    bool fullscreen_view = false;
    bool window_is_fullscreen = false;
    int fullscreen_stream = -1;
    bool overlay_always_show_all = false;
    bool show_configuration_panel = false;
    bool show_diagnostics_overlay = false;

    auto stream_index_from_point = [&](int px, int py) -> int
    {
        if (fullscreen_view && fullscreen_stream >= 0 && fullscreen_stream < stream_count)
        {
            return fullscreen_stream;
        }
        int out_w = 0;
        int out_h = 0;
        if (SDL_GetRendererOutputSize(renderer, &out_w, &out_h) != 0 || out_w == 0 || out_h == 0)
        {
            return -1;
        }
        int cell_w = out_w / GRID_COLS;
        int cell_h = out_h / GRID_ROWS;
        if (cell_w <= 0 || cell_h <= 0)
        {
            return -1;
        }
        int col = px / cell_w;
        int row = py / cell_h;
        if (col < 0 || col >= GRID_COLS || row < 0 || row >= GRID_ROWS)
        {
            return -1;
        }
        int idx = row * GRID_COLS + col;
        if (idx >= stream_count)
        {
            return -1;
        }
        return idx;
    };

    auto clear_canvas_slot = [&](int idx)
    {
        if (single_w <= 0 || single_h <= 0 || idx < 0 || idx >= stream_count)
        {
            return;
        }
        int col = idx % GRID_COLS;
        int row = idx / GRID_COLS;
        int dst_x = col * single_w;
        int dst_y = row * single_h;

        uint8_t *dst = canvas_data[0] + dst_y * canvas_linesize[0] + dst_x * 3;
        int dst_pitch = canvas_linesize[0];
        for (int y = 0; y < single_h && (dst_y + y) < canvas_h; ++y)
        {
            std::memset(dst + y * dst_pitch, 0, single_w * 3);
        }
    };

    bool quit = false;
    SDL_Event event;

    // Set up thread info callback now that quit is declared
    auto thread_info_callback = [&]() -> std::vector<ConfigurationPanel::ThreadInfo>
    {
        std::vector<ConfigurationPanel::ThreadInfo> threads;

        // Add info for each stream worker thread
        for (int i = 0; i < stream_count; ++i)
        {
            ConfigurationPanel::ThreadInfo info;
            std::string stream_label = (i < static_cast<int>(stream_names.size()) && !stream_names[i].empty())
                                           ? stream_names[i]
                                           : ("Stream " + std::to_string(i));
            info.name = "Stream Worker: " + stream_label;

            bool has_context = streams[i].fmt_ctx != nullptr;
            bool worker_running = streams[i].worker.joinable();
            bool worker_failed = streams[i].worker_failed.load();

            info.is_active = has_context && worker_running && !worker_failed;

            if (!has_context)
            {
                info.details = "Stream not opened";
            }
            else if (worker_failed)
            {
                info.details = "Worker failed";
            }
            else if (!worker_running)
            {
                info.details = "Worker not started";
            }
            else
            {
                info.details = "Processing video/audio packets";
            }

            threads.push_back(info);
        }

        // Add async network worker thread info
        ConfigurationPanel::ThreadInfo network_thread;
        network_thread.name = "Async Network Worker";
        network_thread.is_active = async_network_worker.isRunning();
        if (async_network_worker.isProcessing())
        {
            network_thread.details = "Processing network request";
        }
        else
        {
            size_t queue_size = async_network_worker.getQueueSize();
            if (queue_size > 0)
            {
                network_thread.details = "Idle (" + std::to_string(queue_size) + " queued)";
            }
            else
            {
                network_thread.details = "Idle (no tasks)";
            }
        }
        threads.push_back(network_thread);

        // Add main thread info
        ConfigurationPanel::ThreadInfo main_thread;
        main_thread.name = "Main Thread";
        main_thread.is_active = !quit;
        main_thread.details = quit ? "Shutting down" : "Event loop, rendering, ImGui";
        threads.push_back(main_thread);

        return threads;
    };
    configuration_panel.setThreadInfoCallback(thread_info_callback);

    // Helper lambda: blit stream i’s RGB frame into its quadrant
    auto blit_stream_to_canvas = [&](int idx)
    {
        if (idx < 0 || idx >= stream_count)
        {
            return;
        }
        VideoStreamCtx &s = streams[idx];
        int col = idx % GRID_COLS;
        int row = idx / GRID_COLS;

        int dst_x = col * single_w;
        int dst_y = row * single_h;
        uint8_t *dst = canvas_data[0] + dst_y * canvas_linesize[0] + dst_x * 3;
        int dst_pitch = canvas_linesize[0];
        bool frame_copied = false;

        {
            std::lock_guard<std::mutex> lock(s.frame_mutex);
            if (!s.vctx || !s.vframe_rgb || s.frame_width <= 0 || s.frame_height <= 0)
            {
                return;
            }
            if (!s.frame_available || s.frame_generation == s.last_consumed_generation)
            {
                return;
            }

            uint8_t *src = s.vframe_rgb->data[0];
            int src_pitch = s.rgb_linesize;
            for (int y = 0; y < s.frame_height && y + dst_y < canvas_h; ++y)
            {
                std::memcpy(dst + y * dst_pitch,
                            src + y * src_pitch,
                            s.frame_width * 3);
            }
            s.last_consumed_generation = s.frame_generation;
            frame_copied = true;
        }

        if (frame_copied)
        {
            std::lock_guard<std::mutex> lock(stream_state_mutex);
            if (idx < static_cast<int>(stream_last_frame_times.size()))
            {
                stream_last_frame_times[idx] = std::chrono::steady_clock::now();
            }
            if (idx < static_cast<int>(stream_stall_reported.size()))
            {
                stream_stall_reported[idx] = false;
            }
        }
    };

    // --- Main loop: round-robin reading ---
#ifdef DEBUG_LOGGING
    std::cerr << "[diag] entering main loop" << "\n";
#endif
    while (!quit)
    {
#ifdef DEBUG_LOGGING
        std::cerr << "[diag] top of loop quit flag: " << quit << "\n";
#endif
        while (SDL_PollEvent(&event))
        {
#ifdef DEBUG_LOGGING
            std::cerr << "[diag] SDL event type: " << event.type << "\n";
#endif
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT)
            {
#ifdef DEBUG_LOGGING
                std::cerr << "[diag] SDL_QUIT received" << "\n";
#endif
                quit = true;
            }
            else if (event.type == SDL_KEYDOWN)
            {
#ifdef DEBUG_LOGGING
                std::cerr << "[diag] SDL keydown: " << SDL_GetKeyName(event.key.keysym.sym) << "\n";
#endif
                if (event.key.keysym.sym == SDLK_q || event.key.keysym.sym == SDLK_ESCAPE)
                    quit = true;
            }
            else if (event.type == SDL_MOUSEBUTTONDOWN)
            {
                if (event.button.button == SDL_BUTTON_RIGHT)
                {
                    context_stream_index = stream_index_from_point(event.button.x, event.button.y);
                    show_context_menu = true;
                    context_menu_pos = ImVec2(static_cast<float>(event.button.x),
                                              static_cast<float>(event.button.y));
                }
                else if (event.button.button == SDL_BUTTON_LEFT)
                {
                    if (show_context_menu && !menu_hovered)
                    {
                        show_context_menu = false;
                    }
                    else if (!show_context_menu && !fullscreen_view)
                    {
                        // Switch audio source on left-click
                        int clicked_stream = stream_index_from_point(event.button.x, event.button.y);
                        if (clicked_stream >= 0 && clicked_stream < stream_count &&
                            clicked_stream != active_audio_stream.load() &&
                            streams[clicked_stream].fmt_ctx)
                        {
                            int prev_stream = active_audio_stream.load();
                            AUDIO_LOG("Switching audio from stream " << prev_stream << " to stream " << clicked_stream);
                            active_audio_stream.store(clicked_stream);
                            if (!configure_audio(streams[clicked_stream]))
                            {
                                std::cerr << "Failed to configure audio for clicked stream\n";
                            }
                            else
                            {
                                audio_controls_last_interaction_time = std::chrono::steady_clock::now();
                                // Show notification
                                std::string to_name = (clicked_stream < static_cast<int>(stream_names.size()) && !stream_names[clicked_stream].empty())
                                                          ? stream_names[clicked_stream]
                                                          : ("Stream " + std::to_string(clicked_stream));
                                audio_switch_notification = "Audio: " + to_name;
                                audio_switch_notification_time = std::chrono::steady_clock::now();
                            }
                        }
                    }
                }
            }
            else if (event.type == SDL_MOUSEMOTION)
            {
                last_pointer_activity_time = std::chrono::steady_clock::now();
                hovered_stream = stream_index_from_point(event.motion.x, event.motion.y);
            }
            else if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_LEAVE)
            {
                hovered_stream = -1;
            }
        }

#ifdef DEBUG_LOGGING
        std::cerr << "[diag] quit flag after events: " << quit << "\n";
#endif

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        const auto frame_now = std::chrono::steady_clock::now();

        int effective_hovered_stream = hovered_stream;
        if (effective_hovered_stream >= 0 && (frame_now - last_pointer_activity_time) > kOverlayAutoHideDuration)
        {
            effective_hovered_stream = -1;
        }
        const double stall_threshold_seconds = std::chrono::duration<double>(kStreamStallThreshold).count();

        for (int i = 0; i < stream_count; ++i)
        {
            blit_stream_to_canvas(i);
        }

        for (int i = 0; i < stream_count; ++i)
        {
            if (!streams[i].pending_reference_update.load())
            {
                continue;
            }

            int new_w = 0;
            int new_h = 0;
            bool have_dimensions = false;
            {
                std::lock_guard<std::mutex> lock(streams[i].frame_mutex);
                if (streams[i].frame_width > 0 && streams[i].frame_height > 0)
                {
                    new_w = streams[i].frame_width;
                    new_h = streams[i].frame_height;
                    have_dimensions = true;
                }
                streams[i].pending_reference_update.store(false);
            }

            if (!have_dimensions)
            {
                continue;
            }

            bool update_reference = !reference_dimensions_ready || i == 0;
            if (update_reference)
            {
                single_w = new_w;
                single_h = new_h;
                reference_dimensions_ready = true;
                placeholder_dimensions = false;
                if (ensure_canvas_dimensions)
                {
                    ensure_canvas_dimensions(single_w, single_h, false);
                }
            }
        }

        for (int i = 0; i < stream_count; ++i)
        {
            if (!streams[i].worker_failed.load())
            {
                continue;
            }

            std::string stream_label = (i < static_cast<int>(stream_names.size()) && !stream_names[i].empty())
                                           ? stream_names[i]
                                           : ("Stream " + std::to_string(i));
#ifdef DEBUG_LOGGING
            std::cerr << "[diag] Worker failure detected for \"" << stream_label << "\"\n";
#endif
            release_stream(streams[i]);
            clear_canvas_slot(i);
            schedule_stream_retry(i, kStreamRetryInitialDelay);
        }

        if (show_context_menu)
        {
            ImGui::SetNextWindowPos(context_menu_pos, ImGuiCond_Always, ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowBgAlpha(0.9f);
            menu_hovered = false;
            if (ImGui::Begin("##context_menu", &show_context_menu,
                             ImGuiWindowFlags_NoDecoration |
                                 ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoSavedSettings))
            {
                if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup |
                                           ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
                {
                    menu_hovered = true;
                }
                ImGui::TextUnformatted("Actions");
                ImGui::Separator();
                bool can_reload_stream = context_stream_index >= 0 && context_stream_index < stream_count;
                if (ImGui::MenuItem("Reload this stream", nullptr, false, can_reload_stream))
                {
                    reload_stream_requested = context_stream_index;
                    show_context_menu = false;
                }
                if (ImGui::MenuItem("Reload all streams"))
                {
                    reload_all_requested = true;
                    show_context_menu = false;
                }
                ImGui::Separator();
                bool can_toggle_overlay = context_stream_index >= 0 && context_stream_index < stream_count;
                if (ImGui::MenuItem("Always show overlay (stream)", nullptr,
                                    can_toggle_overlay ? overlay_always_show_stream[context_stream_index] : false,
                                    can_toggle_overlay))
                {
                    overlay_always_show_stream[context_stream_index] = !overlay_always_show_stream[context_stream_index];
                }
                if (ImGui::MenuItem("Always show overlay (all streams)", nullptr, overlay_always_show_all))
                {
                    overlay_always_show_all = !overlay_always_show_all;
                    std::fill(overlay_always_show_stream.begin(), overlay_always_show_stream.end(), overlay_always_show_all);
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Diagnostics overlay", nullptr, show_diagnostics_overlay))
                {
                    show_diagnostics_overlay = !show_diagnostics_overlay;
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Configuration", nullptr, show_configuration_panel))
                {
                    show_configuration_panel = true;
                    configuration_panel.requestTab(ConfigurationPanel::Tab::General);
                    show_context_menu = false;
                }
                if (ImGui::MenuItem("Add Camera"))
                {
                    show_configuration_panel = true;
                    configuration_panel.requestTab(ConfigurationPanel::Tab::AddCamera);
                    show_context_menu = false;
                }
                // Remove Camera - only enabled if clicking on a valid camera stream
                bool can_remove_camera = context_stream_index >= 0 &&
                                         context_stream_index < static_cast<int>(stream_configs.size());
                if (ImGui::MenuItem("Remove Camera", nullptr, false, can_remove_camera))
                {
                    int idx = context_stream_index;
                    std::string camera_name = stream_configs[idx].name;
                    bool via_server = stream_configs[idx].via_server;

                    // Remove from client
                    stream_configs.erase(stream_configs.begin() + idx);
                    stream_names.erase(stream_names.begin() + idx);
                    persist_config();

                    // Post to server if camera was proxied
                    if (via_server)
                    {
                        async_network_worker.enqueueTask([camera_name, endpoint = client_config.server_endpoint]()
                                                         { client_network::remove_camera(endpoint, camera_name); });
                    }

                    // Request full restart
                    reload_all_requested = true;
                    show_context_menu = false;
                }
                // Motion-frame only enabled if at least one camera is proxied through server
                bool has_server_camera = false;
                for (int i = 0; i < stream_count; ++i)
                {
                    if (i < static_cast<int>(stream_configs.size()) && stream_configs[i].via_server)
                    {
                        has_server_camera = true;
                        break;
                    }
                }
                if (ImGui::MenuItem("Motion-frame", nullptr, false, has_server_camera))
                {
                    show_configuration_panel = true;
                    configuration_panel.requestTab(ConfigurationPanel::Tab::MotionFrame);
                    show_context_menu = false;
                }
                if (ImGui::MenuItem("Show Info"))
                {
                    show_configuration_panel = true;
                    configuration_panel.requestTab(ConfigurationPanel::Tab::Info);
                    show_context_menu = false;
                }
                ImGui::Separator();
                if (!window_is_fullscreen)
                {
                    if (ImGui::MenuItem("Fullscreen window"))
                    {
                        if (SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP) == 0)
                        {
                            window_is_fullscreen = true;
                        }
                        else
                        {
                            std::cerr << "Failed to fullscreen window: " << SDL_GetError() << "\n";
                        }
                        show_context_menu = false;
                    }
                }
                else if (!fullscreen_view)
                {
                    if (ImGui::MenuItem("Exit fullscreen window"))
                    {
                        if (SDL_SetWindowFullscreen(win, 0) == 0)
                        {
                            window_is_fullscreen = false;
                        }
                        else
                        {
                            std::cerr << "Failed to exit fullscreen: " << SDL_GetError() << "\n";
                        }
                        show_context_menu = false;
                    }
                }
                if (!fullscreen_view)
                {
                    bool can_fullscreen = context_stream_index >= 0 && context_stream_index < stream_count;
                    if (ImGui::MenuItem("Fullscreen stream", nullptr, false, can_fullscreen))
                    {
                        if (!window_is_fullscreen)
                        {
                            if (SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP) == 0)
                            {
                                window_is_fullscreen = true;
                            }
                            else
                            {
                                std::cerr << "Failed to enter fullscreen: " << SDL_GetError() << "\n";
                            }
                        }
                        fullscreen_view = true;
                        fullscreen_stream = context_stream_index;
                        hovered_stream = fullscreen_stream;

                        // Switch audio to the fullscreen stream
                        if (context_stream_index != active_audio_stream.load() &&
                            context_stream_index < stream_count &&
                            streams[context_stream_index].fmt_ctx)
                        {
                            int prev_stream = active_audio_stream.load();
                            AUDIO_LOG("Switching audio from stream " << prev_stream << " to stream " << context_stream_index);
                            active_audio_stream.store(context_stream_index);
                            if (!configure_audio(streams[context_stream_index]))
                            {
                                std::cerr << "Failed to configure audio for fullscreen stream\n";
                            }
                            else
                            {
                                audio_controls_last_interaction_time = std::chrono::steady_clock::now();
                                // Show notification
                                std::string to_name = (context_stream_index < static_cast<int>(stream_names.size()) && !stream_names[context_stream_index].empty())
                                                          ? stream_names[context_stream_index]
                                                          : ("Stream " + std::to_string(context_stream_index));
                                audio_switch_notification = "Audio: " + to_name;
                                audio_switch_notification_time = std::chrono::steady_clock::now();
                            }
                        }

                        show_context_menu = false;
                    }
                }
                else
                {
                    if (ImGui::MenuItem("Exit stream fullscreen"))
                    {
                        fullscreen_view = false;
                        fullscreen_stream = -1;
                        hovered_stream = -1;

                        // Switch audio back to stream 0
                        if (active_audio_stream.load() != 0 && stream_count > 0 && streams[0].fmt_ctx)
                        {
                            int prev_stream = active_audio_stream.load();
                            AUDIO_LOG("Switching audio back to stream 0 from stream " << prev_stream);
                            active_audio_stream.store(0);
                            if (!configure_audio(streams[0]))
                            {
                                std::cerr << "Failed to reconfigure audio for stream 0\n";
                            }
                            else
                            {
                                audio_controls_last_interaction_time = std::chrono::steady_clock::now();
                                // Show notification
                                std::string to_name = (!stream_names.empty() && !stream_names[0].empty())
                                                          ? stream_names[0]
                                                          : "Stream 0";
                                audio_switch_notification = "Audio: " + to_name;
                                audio_switch_notification_time = std::chrono::steady_clock::now();
                            }
                        }

                        show_context_menu = false;
                    }
                }
                if (ImGui::MenuItem("Exit"))
                {
                    quit = true;
                    show_context_menu = false;
                }
                ImGui::End();
            }
        }

        if (show_metrics_window)
        {
            ImGui::ShowMetricsWindow(&show_metrics_window);
        }

        if (show_diagnostics_overlay)
        {
            ImGui::SetNextWindowBgAlpha(0.85f);
            ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Diagnostics", &show_diagnostics_overlay,
                             ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoCollapse))
            {
                ImGuiIO &io = ImGui::GetIO();
                ImGui::Text("FPS: %.1f", io.Framerate);
                ImGui::Text("Frame time: %.2f ms", io.Framerate > 0.0f ? 1000.0f / io.Framerate : 0.0f);
                ImGui::Text("Streams: %d", stream_count);
                if (reload_all_requested)
                {
                    ImGui::TextUnformatted("Reload queue: all streams");
                }
                else if (reload_stream_requested >= 0)
                {
                    ImGui::Text("Reload queue: stream %d", reload_stream_requested);
                }
                else
                {
                    ImGui::TextUnformatted("Reload queue: idle");
                }
                ImGui::Separator();
                for (int i = 0; i < stream_count; ++i)
                {
                    std::string stream_label = (i < static_cast<int>(stream_names.size()) && !stream_names[i].empty())
                                                   ? stream_names[i]
                                                   : ("Stream " + std::to_string(i));
                    bool is_open = streams[i].fmt_ctx != nullptr;
                    bool has_timestamp = stream_last_frame_times[i] != std::chrono::steady_clock::time_point{};
                    double last_age = has_timestamp ? std::chrono::duration<double>(frame_now - stream_last_frame_times[i]).count() : 0.0;
                    bool stalled = is_open && has_timestamp && last_age > stall_threshold_seconds;
                    bool awaiting_retry = !is_open && stream_retry_deadlines[i] != std::chrono::steady_clock::time_point{};
                    double retry_in = awaiting_retry ? std::chrono::duration<double>(stream_retry_deadlines[i] - frame_now).count() : 0.0;
                    if (retry_in < 0.0)
                        retry_in = 0.0;
                    ImVec4 header_color = stalled ? ImVec4(0.90f, 0.35f, 0.20f, 1.0f)
                                                  : (is_open ? ImVec4(0.25f, 0.80f, 0.25f, 1.0f)
                                                             : (awaiting_retry ? ImVec4(0.95f, 0.75f, 0.25f, 1.0f)
                                                                               : ImVec4(0.70f, 0.70f, 0.70f, 1.0f)));
                    ImGui::TextColored(header_color, "%d: %s", i, stream_label.c_str());
                    ImGui::Indent();
                    if (is_open)
                    {
                        ImGui::Text("State: %s", stalled ? "stalled" : "active");
                        if (has_timestamp)
                        {
                            ImGui::Text("Last frame: %.1f s ago", last_age);
                        }
                        else
                        {
                            ImGui::TextUnformatted("Last frame: pending");
                        }
                    }
                    else
                    {
                        ImGui::Text("State: %s", awaiting_retry ? "waiting for retry" : "closed");
                        if (awaiting_retry)
                        {
                            ImGui::Text("Retry in: %.1f s", retry_in);
                        }
                    }
                    ImGui::Unindent();
                    if (i + 1 < stream_count)
                    {
                        ImGui::Separator();
                    }
                }
            }
            ImGui::End();
        }

        for (int i = 0; i < stream_count; ++i)
        {
            if (i >= static_cast<int>(stream_last_frame_times.size()))
            {
                break;
            }
            if (!streams[i].fmt_ctx)
            {
                stream_stall_reported[i] = false;
                continue;
            }
            auto last = stream_last_frame_times[i];
            if (last == std::chrono::steady_clock::time_point{})
            {
                continue;
            }
            double age = std::chrono::duration<double>(frame_now - last).count();
            if (age > stall_threshold_seconds)
            {
                if (!stream_stall_reported[i])
                {
                    std::string stream_label = (i < static_cast<int>(stream_names.size()) && !stream_names[i].empty())
                                                   ? stream_names[i]
                                                   : ("Stream " + std::to_string(i));
                    std::ostringstream oss;
                    oss << std::fixed << std::setprecision(1) << age;
#ifdef DEBUG_LOGGING
                    std::cerr << "[diag] Stream \"" << stream_label << "\" stalled for " << oss.str() << "s\n";
#endif
                    stream_stall_reported[i] = true;
                }
            }
            else if (stream_stall_reported[i])
            {
                std::string stream_label = (i < static_cast<int>(stream_names.size()) && !stream_names[i].empty())
                                               ? stream_names[i]
                                               : ("Stream " + std::to_string(i));
                std::ostringstream oss;
                oss << std::fixed << std::setprecision(1) << age;
#ifdef DEBUG_LOGGING
                std::cerr << "[diag] Stream \"" << stream_label << "\" recovered (" << oss.str() << "s since last frame)\n";
#endif
                stream_stall_reported[i] = false;
            }
        }

        std::vector<int> overlay_targets;
        overlay_targets.reserve(stream_count);
        auto add_overlay_target = [&](int idx)
        {
            if (idx < 0 || idx >= stream_count)
                return;
            if (std::find(overlay_targets.begin(), overlay_targets.end(), idx) == overlay_targets.end())
            {
                overlay_targets.push_back(idx);
            }
        };

        if (overlay_always_show_all)
        {
            for (int i = 0; i < stream_count; ++i)
                add_overlay_target(i);
        }
        else
        {
            for (int i = 0; i < stream_count; ++i)
            {
                if (overlay_always_show_stream[i])
                    add_overlay_target(i);
            }
        }
        add_overlay_target(effective_hovered_stream);

        if (!overlay_targets.empty())
        {
            int out_w = 0;
            int out_h = 0;
            if (SDL_GetRendererOutputSize(renderer, &out_w, &out_h) == 0 && out_w > 0 && out_h > 0)
            {
                for (int idx : overlay_targets)
                {
                    ImVec2 overlay_pos;
                    if (fullscreen_view && fullscreen_stream == idx)
                    {
                        overlay_pos = ImVec2(20.0f, 20.0f);
                    }
                    else if (!fullscreen_view)
                    {
                        int cell_w = out_w / GRID_COLS;
                        int cell_h = out_h / GRID_ROWS;
                        int col = idx % GRID_COLS;
                        int row = idx / GRID_COLS;
                        overlay_pos = ImVec2(static_cast<float>(col * cell_w + 12),
                                             static_cast<float>(row * cell_h + 12));
                    }
                    else
                    {
                        continue;
                    }

                    std::string window_name = "##overlay" + std::to_string(idx);
                    ImGui::SetNextWindowPos(overlay_pos, ImGuiCond_Always);
                    ImGui::SetNextWindowBgAlpha(0.75f);
                    ImGuiWindowFlags overlay_flags = ImGuiWindowFlags_NoDecoration |
                                                     ImGuiWindowFlags_AlwaysAutoResize |
                                                     ImGuiWindowFlags_NoMove |
                                                     ImGuiWindowFlags_NoSavedSettings |
                                                     ImGuiWindowFlags_NoInputs;
                    if (ImGui::Begin(window_name.c_str(), nullptr, overlay_flags))
                    {
                        if (idx >= 0 && idx < static_cast<int>(stream_names.size()))
                        {
                            ImGui::TextUnformatted(stream_names[idx].c_str());
                        }
                    }
                    ImGui::End();
                }
            }
        }

        configuration_panel.render(show_configuration_panel);

        // Render audio controls overlay (only when the active stream actually has audio).
        {
            const int audio_stream_idx = active_audio_stream.load();
            const bool audio_active = audio_stream_idx >= 0 && audio_stream_idx < stream_count &&
                                      streams[audio_stream_idx].fmt_ctx &&
                                      streams[audio_stream_idx].audio_stream_index != -1 &&
                                      audio_device_open;

            const bool show_audio_controls = audio_active &&
                                            (effective_hovered_stream == audio_stream_idx ||
                                             (frame_now - audio_controls_last_interaction_time) < kOverlayAutoHideDuration);

            if (show_audio_controls)
            {
                int out_w = 0;
                int out_h = 0;
                if (SDL_GetRendererOutputSize(renderer, &out_w, &out_h) == 0 && out_w > 0 && out_h > 0)
                {
                    ImVec2 overlay_pos;
                    if (fullscreen_view && fullscreen_stream == audio_stream_idx)
                    {
                        overlay_pos = ImVec2(20.0f, 20.0f);
                    }
                    else if (!fullscreen_view)
                    {
                        int cell_w = out_w / GRID_COLS;
                        int cell_h = out_h / GRID_ROWS;
                        int col = audio_stream_idx % GRID_COLS;
                        int row = audio_stream_idx / GRID_COLS;
                        overlay_pos = ImVec2(static_cast<float>(col * cell_w + 12),
                                             static_cast<float>(row * cell_h + 44));
                    }
                    else
                    {
                        overlay_pos = ImVec2(20.0f, 20.0f);
                    }

                    ImGui::SetNextWindowPos(overlay_pos, ImGuiCond_Always);
                    ImGui::SetNextWindowBgAlpha(0.80f);
                    ImGuiWindowFlags audio_overlay_flags = ImGuiWindowFlags_NoDecoration |
                                                           ImGuiWindowFlags_AlwaysAutoResize |
                                                           ImGuiWindowFlags_NoMove |
                                                           ImGuiWindowFlags_NoSavedSettings;

                    if (ImGui::Begin("##audio_controls", nullptr, audio_overlay_flags))
                    {
                        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
                        {
                            audio_controls_last_interaction_time = frame_now;
                        }

                        std::string active_name = (audio_stream_idx < static_cast<int>(stream_names.size()) && !stream_names[audio_stream_idx].empty())
                                                      ? stream_names[audio_stream_idx]
                                                      : ("Stream " + std::to_string(audio_stream_idx));
                        ImGui::Text("Audio: %s", active_name.c_str());

                        bool muted = audio_data.muted.load(std::memory_order_relaxed);
                        if (ImGui::Checkbox("Mute", &muted))
                        {
                            audio_data.muted.store(muted, std::memory_order_relaxed);
                            audio_controls_last_interaction_time = frame_now;
                        }

                        int volume = audio_data.volume_percent.load(std::memory_order_relaxed);
                        if (ImGui::SliderInt("Volume", &volume, 0, 100))
                        {
                            volume = std::clamp(volume, 0, 100);
                            audio_data.volume_percent.store(volume, std::memory_order_relaxed);
                            audio_controls_last_interaction_time = frame_now;
                        }
                    }
                    ImGui::End();
                }
            }
        }

        // Render audio switch notification
        if (!audio_switch_notification.empty())
        {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - audio_switch_notification_time);

            if (elapsed < kNotificationDisplayDuration)
            {
                float alpha = 1.0f;

                // Fade in
                if (elapsed < kNotificationFadeInDuration)
                {
                    alpha = static_cast<float>(elapsed.count()) / static_cast<float>(kNotificationFadeInDuration.count());
                }
                // Fade out
                else if (elapsed > kNotificationDisplayDuration - kNotificationFadeOutDuration)
                {
                    auto fade_time = elapsed - (kNotificationDisplayDuration - kNotificationFadeOutDuration);
                    alpha = 1.0f - (static_cast<float>(fade_time.count()) / static_cast<float>(kNotificationFadeOutDuration.count()));
                }

                alpha = std::max(0.0f, std::min(1.0f, alpha));

                int out_w = 0;
                int out_h = 0;
                if (SDL_GetRendererOutputSize(renderer, &out_w, &out_h) == 0 && out_w > 0 && out_h > 0)
                {
                    ImGui::SetNextWindowPos(ImVec2(out_w * 0.5f, out_h * 0.1f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
                    ImGui::SetNextWindowBgAlpha(0.85f * alpha);
                    ImGuiWindowFlags notification_flags = ImGuiWindowFlags_NoDecoration |
                                                          ImGuiWindowFlags_AlwaysAutoResize |
                                                          ImGuiWindowFlags_NoMove |
                                                          ImGuiWindowFlags_NoSavedSettings |
                                                          ImGuiWindowFlags_NoInputs |
                                                          ImGuiWindowFlags_NoFocusOnAppearing;

                    if (ImGui::Begin("##audio_notification", nullptr, notification_flags))
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, alpha));
                        ImGui::TextUnformatted(audio_switch_notification.c_str());
                        ImGui::PopStyleColor();
                    }
                    ImGui::End();
                }
            }
            else
            {
                audio_switch_notification.clear();
            }
        }

        if (quit)
        {
#ifdef DEBUG_LOGGING
            std::cerr << "[diag] quit set before reload checks" << "\n";
#endif
        }

        if (reload_stream_requested < 0)
        {
            for (int i = 0; i < stream_count; ++i)
            {
                if (streams[i].fmt_ctx)
                {
                    continue;
                }
                if (stream_retry_deadlines[i] != std::chrono::steady_clock::time_point{} && frame_now >= stream_retry_deadlines[i])
                {
                    reload_stream_requested = i;
                    stream_retry_deadlines[i] = std::chrono::steady_clock::time_point{};
                    break;
                }
            }
        }

        if (reload_all_requested || reload_stream_requested >= 0)
        {
            bool need_audio_reset = reload_all_requested || reload_stream_requested == 0;
            if (need_audio_reset && audio_device_open)
            {
                SDL_PauseAudio(1);
            }

            if (reload_all_requested)
            {
                std::fill(canvas_buffer.begin(), canvas_buffer.end(), 0);
                bool stream0_reopened = false;
                std::vector<int> streams_to_restart;
                streams_to_restart.reserve(stream_count);
                for (int i = 0; i < stream_count; ++i)
                {
                    if (!open_stream(i, stream_urls[i], i == 0))
                    {
                        std::cerr << "Failed to reload stream: " << stream_urls[i] << "\n";
                        schedule_stream_retry(i, kStreamRetryInitialDelay);
                        break;
                    }
                    record_stream_open(i);
                    if (ensure_canvas_dimensions && streams[i].vctx)
                    {
                        if (!ensure_canvas_dimensions(streams[i].vctx->width, streams[i].vctx->height, placeholder_dimensions))
                        {
                            std::cerr << "Failed to resize canvas during reload (all)." << "\n";
                        }
                    }
                    streams_to_restart.push_back(i);
                    if (i == 0)
                    {
                        stream0_reopened = true;
                    }
                }
                if (need_audio_reset && stream0_reopened)
                {
                    if (!configure_audio(streams[0]))
                    {
                        std::cerr << "Failed to reconfigure audio\n";
                    }
                }
                for (int idx : streams_to_restart)
                {
                    start_stream_worker(idx);
                }
            }
            else if (reload_stream_requested >= 0 && reload_stream_requested < stream_count)
            {
                clear_canvas_slot(reload_stream_requested);
                if (!open_stream(reload_stream_requested, stream_urls[reload_stream_requested], reload_stream_requested == 0))
                {
                    std::cerr << "Failed to reload stream: " << stream_urls[reload_stream_requested] << "\n";
                    schedule_stream_retry(reload_stream_requested, kStreamRetryInitialDelay);
                }
                else
                {
                    record_stream_open(reload_stream_requested);
                    if (ensure_canvas_dimensions && streams[reload_stream_requested].vctx)
                    {
                        if (!ensure_canvas_dimensions(streams[reload_stream_requested].vctx->width,
                                                      streams[reload_stream_requested].vctx->height,
                                                      placeholder_dimensions))
                        {
                            std::cerr << "Failed to resize canvas during reload (single)." << "\n";
                        }
                    }
                    if (need_audio_reset)
                    {
                        if (!configure_audio(streams[0]))
                        {
                            std::cerr << "Failed to reconfigure audio\n";
                        }
                    }
                    start_stream_worker(reload_stream_requested);
                }
            }

            if (need_audio_reset && audio_device_open)
            {
                SDL_PauseAudio(0);
            }

            reload_all_requested = false;
            reload_stream_requested = -1;
        }

        if (texture && !canvas_buffer.empty() && canvas_linesize[0] > 0)
        {
            SDL_UpdateTexture(texture, nullptr, canvas_buffer.data(), canvas_linesize[0]);
        }

        SDL_RenderClear(renderer);
        if (fullscreen_view && fullscreen_stream >= 0 && fullscreen_stream < stream_count)
        {
            int out_w = 0;
            int out_h = 0;
            if (SDL_GetRendererOutputSize(renderer, &out_w, &out_h) == 0 && out_w > 0 && out_h > 0)
            {
                SDL_Rect src{
                    (fullscreen_stream % GRID_COLS) * single_w,
                    (fullscreen_stream / GRID_COLS) * single_h,
                    single_w,
                    single_h};
                SDL_Rect dst{0, 0, out_w, out_h};
                SDL_RenderCopy(renderer, texture, &src, &dst);
            }
            else
            {
                SDL_RenderCopy(renderer, texture, nullptr, nullptr);
            }
        }
        else
        {
            SDL_RenderCopy(renderer, texture, nullptr, nullptr);

            if (effective_hovered_stream >= 0)
            {
                int out_w = 0;
                int out_h = 0;
                if (SDL_GetRendererOutputSize(renderer, &out_w, &out_h) == 0 && out_w > 0 && out_h > 0)
                {
                    int cell_w = out_w / GRID_COLS;
                    int cell_h = out_h / GRID_ROWS;
                    SDL_Rect overlay{
                        (effective_hovered_stream % GRID_COLS) * cell_w,
                        (effective_hovered_stream / GRID_COLS) * cell_h,
                        cell_w,
                        cell_h};
                    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 96);
                    SDL_RenderFillRect(renderer, &overlay);
                    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
                }
            }
        }

        ImGui::Render();
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);

        SDL_RenderPresent(renderer);

        SDL_Delay(1);

#ifdef DEBUG_LOGGING
        std::cerr << "[diag] end of iteration quit flag: " << quit << "\n";
#endif
    }

    if (audio_device_open)
    {
        SDL_CloseAudio();
        audio_device_open = false;
    }
    if (swr)
    {
        swr_free(&swr);
        swr = nullptr;
    }

    for (auto &stream : streams)
    {
        release_stream(stream);
    }

    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    if (texture)
    {
        SDL_DestroyTexture(texture);
        texture = nullptr;
    }
    if (motion_frame_texture)
    {
        SDL_DestroyTexture(motion_frame_texture);
        motion_frame_texture = nullptr;
    }
    if (renderer)
    {
        SDL_DestroyRenderer(renderer);
        renderer = nullptr;
    }
    if (win)
    {
        SDL_DestroyWindow(win);
        win = nullptr;
    }

    SDL_Quit();
    avformat_network_deinit();

    return 0;
}
