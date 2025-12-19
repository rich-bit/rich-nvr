#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct ImVec2;

namespace client_network {
    struct ServerThreadInfo;
}

struct ConfigurationWindowSettings {
    float width = 720.0f;
    float height = 520.0f;
    bool show_imgui_metrics = false;
};

struct AddCameraRequest {
    bool connect_via_server = false;
    std::string name;
    std::string rtsp_url;
    std::string server_endpoint;
    bool segment = false;
    bool recording = false;
    bool overlay = false;
    bool motion_frame = false;
    bool gstreamer_proxy = false;
    bool live555_proxy = false;
    int segment_bitrate = 0;
    std::string segment_speed_preset;
    int proxy_bitrate = 0;
    std::string proxy_speed_preset;
    int motion_frame_width = 0;
    int motion_frame_height = 0;
    float motion_frame_scale = 0.0f;
    float noise_threshold = 0.0f;
    float motion_threshold = 0.0f;
    int motion_min_hits = 0;
    int motion_decay = 0;
    float motion_arrow_scale = 0.0f;
    int motion_arrow_thickness = 0;
};

struct AddCameraResult {
    bool success = false;
    std::string message;
};

struct ProbeStreamResult {
    bool success = false;
    int width = 0;
    int height = 0;
    bool has_audio = false;
    std::string error_message;
};

class ConfigurationPanel {
public:
    enum class Tab {
        General,
        Audio,
        AddCamera,
        MotionFrame,
        Info
    };

    struct ThreadInfo {
        std::string name;
        bool is_active;
        std::string details;
    };
    
    struct MotionRegion {
        int id;
        std::string name;
        int x, y, w, h;
        float angle;
    };

    using PersistCallback = std::function<void(const ConfigurationWindowSettings&)>;
    using AddCameraCallback = std::function<AddCameraResult(const AddCameraRequest&)>;
    using ProbeStreamCallback = std::function<ProbeStreamResult(const std::string&)>;
    using ThreadInfoCallback = std::function<std::vector<ThreadInfo>()>;
    using ShowMetricsCallback = std::function<void(bool)>;
    
    struct CameraInfo {
        std::string name;
        bool via_server;
        bool motion_enabled;
        bool segment_recording;
        float motion_frame_scale;
        float noise_threshold;
        float motion_threshold;
        int motion_min_hits;
        int motion_decay;
        float motion_arrow_scale;
        int motion_arrow_thickness;
    };
    using GetCamerasCallback = std::function<std::vector<CameraInfo>()>;

    ConfigurationPanel(ConfigurationWindowSettings& window_settings,
                       PersistCallback persist_callback,
                       AddCameraCallback add_camera_callback,
                       ProbeStreamCallback probe_stream_callback,
                       const std::string& default_server_endpoint,
                       ThreadInfoCallback thread_info_callback,
                       ShowMetricsCallback show_metrics_callback,
                       GetCamerasCallback get_cameras_callback,
                       std::function<bool(const std::string&, bool)> toggle_motion_callback,
                       std::function<bool(const std::string&, void*&, int&, int&)> fetch_motion_frame_callback,
                       std::function<int(const std::string&, int, int, int, int, float)> add_motion_region_callback,
                       std::function<bool(const std::string&, int)> remove_motion_region_callback,
                       std::function<bool(const std::string&)> clear_motion_regions_callback,
                       std::function<std::vector<MotionRegion>(const std::string&)> get_motion_regions_callback);

    void render(bool& open);
    void requestTab(Tab tab);
    void setThreadInfoCallback(ThreadInfoCallback callback);
    void setAsyncWorker(class AsyncNetworkWorker* worker) { async_worker_ = worker; }
    
    // Public accessor for motion frame worker (for thread info display)
    class AsyncNetworkWorker* getMotionFrameWorker() const { return motion_frame_worker_.get(); }

private:
    void renderGeneralTab(bool set_selected);
    void renderAudioTab(bool set_selected);
    void renderAddCameraTab(bool set_selected);
    void renderMotionFrameTab(bool set_selected);
    void renderInfoTab(bool set_selected);
    
    // Helper to decode JPEG from buffer using global shared data
    void decode_motion_frame_from_buffer_(const std::vector<unsigned char>& jpeg_buffer,
                                          void*& texture_out, int& width_out, int& height_out);

    bool auto_reconnect_;
    bool show_fps_overlay_;
    float master_volume_;
    float alert_volume_;

    bool motion_detection_enabled_;
    float motion_threshold_;
    int motion_window_size_;

    // Motion frame viewer state
    int selected_camera_index_;
    bool motion_frame_enabled_;
    void* motion_frame_texture_;
    int motion_frame_width_;
    int motion_frame_height_;
    float last_motion_frame_fetch_;
    bool motion_frame_fetch_in_progress_;
    float motion_frame_fetch_interval_;
    
    // Motion frame data buffer (for async transfer)
    std::vector<unsigned char> motion_frame_data_;
    int pending_motion_frame_width_;
    int pending_motion_frame_height_;
    bool has_pending_motion_frame_;
    mutable std::mutex motion_frame_mutex_;
    
    // Motion region drawing state
    bool drawing_motion_region_;
    float region_draw_start_x_, region_draw_start_y_;
    float region_draw_end_x_, region_draw_end_y_;
    float pending_region_angle_;
    int selected_region_index_;
    std::vector<MotionRegion> motion_regions_;
    mutable std::mutex motion_regions_mutex_;
    float last_region_fetch_time_;
    
    class AsyncNetworkWorker* async_worker_;
    std::unique_ptr<class AsyncNetworkWorker> motion_frame_worker_;  // Dedicated worker for motion frames

    Tab active_tab_;
    Tab requested_tab_;
    bool needs_tab_selection_;

    ConfigurationWindowSettings& window_settings_;
    PersistCallback persist_callback_;
    AddCameraCallback add_camera_callback_;
    ProbeStreamCallback probe_stream_callback_;
    ThreadInfoCallback thread_info_callback_;
    ShowMetricsCallback show_metrics_callback_;
    GetCamerasCallback get_cameras_callback_;
    bool window_size_dirty_;
    
    std::function<bool(const std::string&, bool)> toggle_motion_callback_;
    std::function<bool(const std::string&, void*&, int&, int&)> fetch_motion_frame_callback_;
    std::function<int(const std::string&, int, int, int, int, float)> add_motion_region_callback_;
    std::function<bool(const std::string&, int)> remove_motion_region_callback_;
    std::function<bool(const std::string&)> clear_motion_regions_callback_;
    std::function<std::vector<MotionRegion>(const std::string&)> get_motion_regions_callback_;

    // Add camera tab state
    bool add_camera_via_server_;
    bool add_camera_segment_;
    bool add_camera_overlay_;
    bool add_camera_motion_frame_;
    bool add_camera_live555_proxy_;
    float add_camera_motion_frame_scale_;
    float add_camera_noise_threshold_;
    float add_camera_motion_threshold_;
    int add_camera_motion_min_hits_;
    int add_camera_motion_decay_;
    float add_camera_motion_arrow_scale_;
    int add_camera_motion_arrow_thickness_;
    std::array<char, 64> add_camera_name_;
    std::array<char, 256> add_camera_rtsp_;
    std::array<char, 128> server_endpoint_;
    bool add_camera_status_success_;
    std::string add_camera_status_;
    
    // Probe stream state
    bool probe_in_progress_;
    ProbeStreamResult last_probe_result_;
    
    // Proxy initiation state
    bool proxy_initiate_in_progress_;
    bool proxy_initiated_successfully_;
    std::string proxy_initiate_message_;
    std::string proxied_rtsp_url_;
    bool proxy_probe_in_progress_;
    float last_proxy_probe_time_;
    
    // Server health check state
    bool server_health_checking_;
    bool server_health_available_;
    int server_health_camera_count_;
    long server_health_uptime_;
    int server_health_rtsp_port_;
    std::string server_health_error_;
    float last_health_check_time_;
    
    // Record on motion warning
    bool show_record_motion_warning_;
    bool dont_show_record_motion_warning_;
    
    // Window control
    bool close_after_save_;
    
    // Server camera list for motion-frame tab
    std::vector<CameraInfo> server_cameras_;
    float last_server_camera_fetch_time_;
    bool server_camera_fetch_in_progress_;
    
    // Async server thread info (non-blocking)
    std::atomic<bool> server_thread_info_fetch_in_progress_;
    std::vector<client_network::ServerThreadInfo> cached_server_threads_;
    mutable std::mutex server_thread_cache_mutex_;
    float last_server_thread_info_fetch_;
    float server_check_interval_;
};
