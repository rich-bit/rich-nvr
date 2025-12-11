#include "ConfigurationPanel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <utility>

#include "AsyncNetworkWorker.h"
#include "ClientNetworking.h"
#include "third_party/imgui/imgui.h"

ConfigurationPanel::ConfigurationPanel(ConfigurationWindowSettings &window_settings,
                                       PersistCallback persist_callback,
                                       AddCameraCallback add_camera_callback,
                                       const std::string &default_server_endpoint,
                                       ThreadInfoCallback thread_info_callback,
                                       ShowMetricsCallback show_metrics_callback,
                                       GetCamerasCallback get_cameras_callback,
                                       std::function<bool(const std::string &, bool)> toggle_motion_callback,
                                       std::function<bool(const std::string &, void *&, int &, int &)> fetch_motion_frame_callback,
                                       std::function<int(const std::string &, int, int, int, int, float)> add_motion_region_callback,
                                       std::function<bool(const std::string &, int)> remove_motion_region_callback,
                                       std::function<bool(const std::string &)> clear_motion_regions_callback,
                                       std::function<std::vector<MotionRegion>(const std::string &)> get_motion_regions_callback)
    : auto_reconnect_(true),
      show_fps_overlay_(false),
      master_volume_(0.75f),
      alert_volume_(0.9f),
      motion_detection_enabled_(true),
      motion_threshold_(0.45f),
      motion_window_size_(12),
      active_tab_(Tab::General),
      requested_tab_(Tab::General),
      needs_tab_selection_(false),
      window_settings_(window_settings),
      persist_callback_(std::move(persist_callback)),
      add_camera_callback_(std::move(add_camera_callback)),
      thread_info_callback_(std::move(thread_info_callback)),
      show_metrics_callback_(std::move(show_metrics_callback)),
      get_cameras_callback_(std::move(get_cameras_callback)),
      toggle_motion_callback_(std::move(toggle_motion_callback)),
      fetch_motion_frame_callback_(std::move(fetch_motion_frame_callback)),
      add_motion_region_callback_(std::move(add_motion_region_callback)),
      remove_motion_region_callback_(std::move(remove_motion_region_callback)),
      clear_motion_regions_callback_(std::move(clear_motion_regions_callback)),
      get_motion_regions_callback_(std::move(get_motion_regions_callback)),
      window_size_dirty_(false),
      add_camera_via_server_(true),
      add_camera_segment_(false),
      add_camera_overlay_(false),
      add_camera_motion_frame_(true),
      add_camera_live555_proxy_(true),
      add_camera_motion_frame_scale_(1.0f),
      add_camera_noise_threshold_(0.3f),
      add_camera_motion_threshold_(0.5f),
      add_camera_motion_min_hits_(4),
      add_camera_motion_decay_(20),
      add_camera_motion_arrow_scale_(1.0f),
      add_camera_motion_arrow_thickness_(2),
      add_camera_status_success_(false),
      server_health_checking_(false),
      server_health_available_(false),
      server_health_camera_count_(0),
      server_health_uptime_(0),
      server_health_rtsp_port_(0),
      last_health_check_time_(0.0f),
      selected_camera_index_(0),
      motion_frame_enabled_(false),
      motion_frame_texture_(nullptr),
      motion_frame_width_(0),
      motion_frame_height_(0),
      last_motion_frame_fetch_(0.0f),
      drawing_motion_region_(false),
      region_draw_start_x_(0), region_draw_start_y_(0),
      region_draw_end_x_(0), region_draw_end_y_(0),
      pending_region_angle_(0.0f),
      selected_region_index_(0),
      last_region_fetch_time_(0.0f),
      show_record_motion_warning_(false),
      dont_show_record_motion_warning_(false),
      async_worker_(nullptr)
{
    add_camera_name_.fill(0);
    add_camera_rtsp_.fill(0);
    server_endpoint_.fill(0);

    std::snprintf(add_camera_name_.data(), add_camera_name_.size(), "%s", "Camera");
    std::snprintf(add_camera_rtsp_.data(), add_camera_rtsp_.size(), "%s", "rtsp://");

    const char *endpoint = default_server_endpoint.empty() ? "http://localhost:8080" : default_server_endpoint.c_str();
    std::snprintf(server_endpoint_.data(), server_endpoint_.size(), "%s", endpoint);
}

void ConfigurationPanel::render(bool &open)
{
    if (!open)
    {
        return;
    }

    const float min_window_width = 360.0f;
    const float min_window_height = 240.0f;
    const float max_window_width = 1920.0f;
    const float max_window_height = 1080.0f;
    window_settings_.width = std::clamp(window_settings_.width, min_window_width, max_window_width);
    window_settings_.height = std::clamp(window_settings_.height, min_window_height, max_window_height);
    
    // Set initial size only once, then allow manual resizing
    ImGui::SetNextWindowSize(ImVec2(window_settings_.width, window_settings_.height), ImGuiCond_Once);
    ImGui::SetNextWindowSizeConstraints(ImVec2(min_window_width, min_window_height), ImVec2(max_window_width, max_window_height));

    // Disable window dragging only when Motion-frame tab is active
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoCollapse;
    if (active_tab_ == Tab::MotionFrame)
    {
        window_flags |= ImGuiWindowFlags_NoMove;
    }

    if (!ImGui::Begin("Configuration", &open, window_flags))
    {
        ImGui::End();
        return;
    }
    
    // Track actual window size changes from manual resize
    ImVec2 current_size = ImGui::GetWindowSize();
    if (std::abs(current_size.x - window_settings_.width) > 1.0f || 
        std::abs(current_size.y - window_settings_.height) > 1.0f)
    {
        window_settings_.width = current_size.x;
        window_settings_.height = current_size.y;
        window_size_dirty_ = true;
    }

    if (ImGui::BeginTabBar("ConfigurationTabs"))
    {
        renderGeneralTab(needs_tab_selection_ && requested_tab_ == Tab::General);
        renderAudioTab(needs_tab_selection_ && requested_tab_ == Tab::Audio);
        renderAddCameraTab(needs_tab_selection_ && requested_tab_ == Tab::AddCamera);
        renderMotionFrameTab(needs_tab_selection_ && requested_tab_ == Tab::MotionFrame);
        renderInfoTab(needs_tab_selection_ && requested_tab_ == Tab::Info);
        ImGui::EndTabBar();
    }

    if (needs_tab_selection_)
    {
        needs_tab_selection_ = false;
    }

    ImGui::End();
    
    // Show record on motion warning dialog
    if (show_record_motion_warning_)
    {
        ImGui::OpenPopup("Record on Motion Warning");
        show_record_motion_warning_ = false;
    }
    
    if (ImGui::BeginPopupModal("Record on Motion Warning", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextWrapped("This will constantly record video segments and wait for motion events.");
        ImGui::Spacing();
        ImGui::TextWrapped("Warning: This involves constant disk writes which can cause significant");
        ImGui::TextWrapped("wear on your storage drive. Use only if you understand that your drive");
        ImGui::TextWrapped("will work continuously when this feature is enabled.");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        
        ImGui::Checkbox("Don't show this warning again", &dont_show_record_motion_warning_);
        
        ImGui::Spacing();
        if (ImGui::Button("OK", ImVec2(120, 0)))
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0)))
        {
            // Uncheck the checkbox if user cancels
            add_camera_segment_ = false;
            ImGui::CloseCurrentPopup();
        }
        
        ImGui::EndPopup();
    }

    if (window_size_dirty_ && persist_callback_)
    {
        persist_callback_(window_settings_);
        window_size_dirty_ = false;
    }
}

void ConfigurationPanel::requestTab(Tab tab)
{
    requested_tab_ = tab;
    needs_tab_selection_ = true;
}

void ConfigurationPanel::setThreadInfoCallback(ThreadInfoCallback callback)
{
    thread_info_callback_ = std::move(callback);
}

void ConfigurationPanel::renderGeneralTab(bool set_selected)
{
    ImGuiTabItemFlags flags = set_selected ? ImGuiTabItemFlags_SetSelected : 0;
    if (ImGui::BeginTabItem("General", nullptr, flags))
    {
        active_tab_ = Tab::General;
        ImGui::Checkbox("Auto reconnect streams", &auto_reconnect_);
        ImGui::Checkbox("Show FPS overlay", &show_fps_overlay_);

        bool metrics_changed = ImGui::Checkbox("Show ImGui Metrics", &window_settings_.show_imgui_metrics);
        if (metrics_changed)
        {
            if (show_metrics_callback_)
            {
                show_metrics_callback_(window_settings_.show_imgui_metrics);
            }
            window_size_dirty_ = true;
        }

        ImGui::TextUnformatted("Toggle basic client behaviour.");
        ImGui::Separator();
        
        ImGui::Text("Window Size: %.0fx%.0f", window_settings_.width, window_settings_.height);
        ImGui::TextDisabled("Drag the bottom-right corner to resize this window.");
        
        ImGui::EndTabItem();
    }
}

void ConfigurationPanel::renderAudioTab(bool set_selected)
{
    ImGuiTabItemFlags flags = set_selected ? ImGuiTabItemFlags_SetSelected : 0;
    if (ImGui::BeginTabItem("Audio", nullptr, flags))
    {
        active_tab_ = Tab::Audio;
        ImGui::SliderFloat("Master volume", &master_volume_, 0.0f, 1.0f);
        ImGui::SliderFloat("Alert volume", &alert_volume_, 0.0f, 1.0f);
        ImGui::TextUnformatted("Wire these into your audio mixer when ready.");
        ImGui::EndTabItem();
    }
}

void ConfigurationPanel::renderAddCameraTab(bool set_selected)
{
    ImGuiTabItemFlags flags = set_selected ? ImGuiTabItemFlags_SetSelected : 0;
    if (ImGui::BeginTabItem("Add Camera", nullptr, flags))
    {
        active_tab_ = Tab::AddCamera;

        ImGui::Checkbox("Connect through RichServer", &add_camera_via_server_);
        ImGui::InputText("RTSP address", add_camera_rtsp_.data(), add_camera_rtsp_.size());

        ImGui::BeginDisabled(!add_camera_via_server_);
        ImGui::InputText("Camera name", add_camera_name_.data(), add_camera_name_.size());
        ImGui::InputText("Server endpoint", server_endpoint_.data(), server_endpoint_.size());
        
        // Server health check (async, every 3 seconds)
        if (add_camera_via_server_ && async_worker_)
        {
            float current_time = ImGui::GetTime();
            if (current_time - last_health_check_time_ > 3.0f && !server_health_checking_)
            {
                last_health_check_time_ = current_time;
                server_health_checking_ = true;
                
                std::string endpoint(server_endpoint_.data());
                async_worker_->enqueueTask([this, endpoint]() {
                    auto health = client_network::check_server_health(endpoint);
                    server_health_available_ = health.available;
                    server_health_camera_count_ = health.camera_count;
                    server_health_uptime_ = health.uptime_seconds;
                    server_health_rtsp_port_ = health.rtsp_proxy_port;
                    server_health_error_ = health.error_message;
                    server_health_checking_ = false;
                });
            }
            
            // Display health status
            ImGui::SameLine();
            if (server_health_checking_)
            {
                ImGui::TextDisabled("Checking...");
            }
            else if (server_health_available_)
            {
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Available");
                
                // Show health info as tooltip
                if (ImGui::IsItemHovered())
                {
                    ImGui::BeginTooltip();
                    ImGui::Text("Server Health:");
                    ImGui::Separator();
                    ImGui::Text("Cameras: %d", server_health_camera_count_);
                    ImGui::Text("Uptime: %ld seconds", server_health_uptime_);
                    ImGui::Text("RTSP Port: %d", server_health_rtsp_port_);
                    ImGui::EndTooltip();
                }
            }
            else
            {
                ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Not available");
                if (!server_health_error_.empty() && ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("%s", server_health_error_.c_str());
                }
            }
        }
        
        if (ImGui::Checkbox("Record on Motion", &add_camera_segment_))
        {
            if (add_camera_segment_ && !dont_show_record_motion_warning_)
            {
                show_record_motion_warning_ = true;
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Continuously records video segments and saves only when motion is detected.");
        
        ImGui::Checkbox("Timestamp overlay", &add_camera_overlay_);
        ImGui::Checkbox("Motion frame", &add_camera_motion_frame_);
        ImGui::Checkbox("LIVE555 proxy", &add_camera_live555_proxy_);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("Motion Detection Settings:");
        ImGui::DragFloat("Motion frame scale", &add_camera_motion_frame_scale_, 0.01f, 0.1f, 2.0f, "%.2f");
        ImGui::DragFloat("Noise threshold", &add_camera_noise_threshold_, 0.01f, 0.0f, 5.0f, "%.2f");
        ImGui::DragFloat("Motion threshold", &add_camera_motion_threshold_, 0.01f, 0.0f, 5.0f, "%.2f");
        if (ImGui::InputInt("Motion min hits", &add_camera_motion_min_hits_))
        {
            if (add_camera_motion_min_hits_ < 0)
                add_camera_motion_min_hits_ = 0;
        }
        if (ImGui::InputInt("Motion decay", &add_camera_motion_decay_))
        {
            if (add_camera_motion_decay_ < 0)
                add_camera_motion_decay_ = 0;
        }
        ImGui::DragFloat("Motion arrow scale", &add_camera_motion_arrow_scale_, 0.01f, 0.0f, 5.0f, "%.2f");
        if (ImGui::InputInt("Motion arrow thickness", &add_camera_motion_arrow_thickness_))
        {
            if (add_camera_motion_arrow_thickness_ < 0)
                add_camera_motion_arrow_thickness_ = 0;
        }
        ImGui::EndDisabled();

        if (!add_camera_via_server_)
        {
            ImGui::TextDisabled("Advanced options are available when routing through RichServer.");
        }

        if (ImGui::Button("Save"))
        {
            if (!add_camera_callback_)
            {
                add_camera_status_success_ = false;
                add_camera_status_ = "Add camera handler unavailable.";
            }
            else
            {
                AddCameraRequest request;
                request.connect_via_server = add_camera_via_server_;
                request.rtsp_url = add_camera_rtsp_.data();
                request.name = add_camera_name_.data();
                request.server_endpoint = server_endpoint_.data();
                request.segment = add_camera_segment_;
                request.recording = false;  // Always disabled
                request.overlay = add_camera_overlay_;
                request.motion_frame = add_camera_motion_frame_;
                request.gstreamer_proxy = false;  // Always disabled
                request.live555_proxy = add_camera_live555_proxy_;
                request.segment_bitrate = 2000;  // Default value
                request.segment_speed_preset = "medium";
                request.proxy_bitrate = 1500;  // Default value
                request.proxy_speed_preset = "veryfast";
                request.motion_frame_width = 0;  // Use default from server
                request.motion_frame_height = 0;  // Use default from server
                request.motion_frame_scale = add_camera_motion_frame_scale_;
                request.noise_threshold = add_camera_noise_threshold_;
                request.motion_threshold = add_camera_motion_threshold_;
                request.motion_min_hits = add_camera_motion_min_hits_;
                request.motion_decay = add_camera_motion_decay_;
                request.motion_arrow_scale = add_camera_motion_arrow_scale_;
                request.motion_arrow_thickness = add_camera_motion_arrow_thickness_;

                auto result = add_camera_callback_(request);
                add_camera_status_success_ = result.success;
                add_camera_status_ = result.message;
            }
        }

        if (!add_camera_status_.empty())
        {
            ImGui::Spacing();
            ImVec4 color = add_camera_status_success_ ? ImVec4(0.2f, 0.8f, 0.2f, 1.0f)
                                                      : ImVec4(0.9f, 0.3f, 0.3f, 1.0f);
            ImGui::TextColored(color, "%s", add_camera_status_.c_str());
        }

        ImGui::EndTabItem();
    }
}

void ConfigurationPanel::renderMotionFrameTab(bool set_selected)
{
    ImGuiTabItemFlags flags = set_selected ? ImGuiTabItemFlags_SetSelected : 0;
    if (ImGui::BeginTabItem("Motion-frame", nullptr, flags))
    {
        active_tab_ = Tab::MotionFrame;

        if (!get_cameras_callback_)
        {
            ImGui::TextDisabled("Camera list unavailable.");
            ImGui::EndTabItem();
            return;
        }

        auto cameras = get_cameras_callback_();
        std::vector<CameraInfo> server_cameras;
        for (const auto &cam : cameras)
        {
            if (cam.via_server)
            {
                server_cameras.push_back(cam);
            }
        }

        if (server_cameras.empty())
        {
            ImGui::TextDisabled("No cameras proxied through RichServer.");
            ImGui::TextUnformatted("Add cameras via 'Add Camera' tab with 'Connect through RichServer' enabled.");
            ImGui::EndTabItem();
            return;
        }

        // Clamp selected index
        if (selected_camera_index_ < 0 || selected_camera_index_ >= static_cast<int>(server_cameras.size()))
        {
            selected_camera_index_ = 0;
        }

        // Camera dropdown
        ImGui::Text("Select Camera:");
        if (ImGui::BeginCombo("##camera_select", server_cameras[selected_camera_index_].name.c_str()))
        {
            for (int i = 0; i < static_cast<int>(server_cameras.size()); ++i)
            {
                bool is_selected = (selected_camera_index_ == i);
                if (ImGui::Selectable(server_cameras[i].name.c_str(), is_selected))
                {
                    selected_camera_index_ = i;
                }
                if (is_selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
                
                // Right-click context menu for camera removal
                if (ImGui::BeginPopupContextItem())
                {
                    if (ImGui::MenuItem("Remove Camera"))
                    {
                        if (async_worker_)
                        {
                            std::string camera_name = server_cameras[i].name;
                            std::string endpoint = std::string(server_endpoint_.data());
                            async_worker_->enqueueTask([camera_name, endpoint]() {
                                client_network::remove_camera(endpoint, camera_name);
                            });
                            
                            // Reset selected index if removing current selection
                            if (i == selected_camera_index_)
                            {
                                selected_camera_index_ = 0;
                            }
                        }
                    }
                    ImGui::EndPopup();
                }
            }
            ImGui::EndCombo();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const auto &selected_camera = server_cameras[selected_camera_index_];
        bool motion_enabled = selected_camera.motion_enabled;

        // Motion toggle
        ImGui::Text("Motion Detection Status: %s", motion_enabled ? "Enabled" : "Disabled");

        bool toggled = false;
        if (motion_enabled)
        {
            if (ImGui::Button("Disable Motion Detection"))
            {
                if (toggle_motion_callback_ && async_worker_)
                {
                    std::string camera_name = selected_camera.name;
                    async_worker_->enqueueTask([this, camera_name]() {
                        toggle_motion_callback_(camera_name, false);
                    });
                    toggled = true;
                }
            }
        }
        else
        {
            if (ImGui::Button("Enable Motion Detection"))
            {
                if (toggle_motion_callback_ && async_worker_)
                {
                    std::string camera_name = selected_camera.name;
                    async_worker_->enqueueTask([this, camera_name]() {
                        toggle_motion_callback_(camera_name, true);
                    });
                    toggled = true;
                }
            }
        }
        
        ImGui::SameLine();
        ImGui::Spacing();
        ImGui::SameLine();
        
        // Record on Motion toggle
        bool segment_enabled = selected_camera.segment_recording;
        
        if (segment_enabled)
        {
            if (ImGui::Button("Disable Record on Motion"))
            {
                if (async_worker_)
                {
                    std::string camera_name = selected_camera.name;
                    std::string endpoint = std::string(server_endpoint_.data());
                    async_worker_->enqueueTask([camera_name, endpoint]() {
                        client_network::toggle_segment_recording(endpoint, camera_name, false);
                    });
                }
            }
        }
        else
        {
            if (ImGui::Button("Enable Record on Motion"))
            {
                if (!dont_show_record_motion_warning_)
                {
                    show_record_motion_warning_ = true;
                }
                else if (async_worker_)
                {
                    std::string camera_name = selected_camera.name;
                    std::string endpoint = std::string(server_endpoint_.data());
                    async_worker_->enqueueTask([camera_name, endpoint]() {
                        client_network::toggle_segment_recording(endpoint, camera_name, true);
                    });
                }
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Enable continuous segment recording that saves only when motion is detected.\nWarning: This involves constant disk writes.");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Motion frame display
        if (motion_enabled && !toggled)
        {
            ImGui::Text("Motion Frame:");

            // Fetch motion frame periodically (every 0.5 seconds)
            float current_time = ImGui::GetTime();
            if (current_time - last_motion_frame_fetch_ > 0.5f)
            {
                if (fetch_motion_frame_callback_)
                {
                    fetch_motion_frame_callback_(selected_camera.name, motion_frame_texture_,
                                                 motion_frame_width_, motion_frame_height_);
                    last_motion_frame_fetch_ = current_time;
                }
            }

            if (motion_frame_texture_ && motion_frame_width_ > 0 && motion_frame_height_ > 0)
            {
                // Fetch regions for selected camera periodically (async to avoid blocking UI)
                if (get_motion_regions_callback_ && async_worker_ && (current_time - last_region_fetch_time_ > 2.0f))
                {
                    std::string camera_name = selected_camera.name;
                    async_worker_->enqueueTask([this, camera_name]() {
                        auto regions = get_motion_regions_callback_(camera_name);
                        std::lock_guard<std::mutex> lock(motion_regions_mutex_);
                        motion_regions_ = std::move(regions);
                    });
                    last_region_fetch_time_ = current_time;
                }

                // Display the motion frame texture with region drawing capability
                float max_width = 640.0f;
                float aspect = static_cast<float>(motion_frame_width_) / static_cast<float>(motion_frame_height_);
                float display_width = std::min(max_width, static_cast<float>(motion_frame_width_));
                float display_height = display_width / aspect;
                float scale_x = display_width / static_cast<float>(motion_frame_width_);
                float scale_y = display_height / static_cast<float>(motion_frame_height_);

                ImVec2 image_pos = ImGui::GetCursorScreenPos();
                ImGui::Image(motion_frame_texture_, ImVec2(display_width, display_height));
                
                // Define image bounds for mouse interaction
                ImVec2 image_min = image_pos;
                ImVec2 image_max(image_pos.x + display_width, image_pos.y + display_height);
                ImVec2 mouse_pos = ImGui::GetMousePos();
                bool mouse_in_image = mouse_pos.x >= image_min.x && mouse_pos.x <= image_max.x &&
                                      mouse_pos.y >= image_min.y && mouse_pos.y <= image_max.y;

                // Handle mouse interaction for drawing regions
                if (mouse_in_image)
                {
                    ImVec2 relative_pos(mouse_pos.x - image_pos.x, mouse_pos.y - image_pos.y);

                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !drawing_motion_region_)
                    {
                        drawing_motion_region_ = true;
                        region_draw_start_x_ = relative_pos.x;
                        region_draw_start_y_ = relative_pos.y;
                        region_draw_end_x_ = relative_pos.x;
                        region_draw_end_y_ = relative_pos.y;
                        ImGui::SetWindowFocus();
                    }
                }

                if (drawing_motion_region_)
                {
                    region_draw_end_x_ = mouse_pos.x - image_pos.x;
                    region_draw_end_y_ = mouse_pos.y - image_pos.y;

                    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
                    {
                        drawing_motion_region_ = false;
                    }
                }

                // Draw existing regions (with mutex protection)
                ImDrawList *draw_list = ImGui::GetWindowDrawList();
                {
                    std::lock_guard<std::mutex> lock(motion_regions_mutex_);
                    for (const auto &region : motion_regions_)
                    {
                        float rx = image_pos.x + region.x * scale_x;
                        float ry = image_pos.y + region.y * scale_y;
                        float rw = region.w * scale_x;
                        float rh = region.h * scale_y;
                        
                        // Draw rotated rectangle if angle is set
                        if (std::abs(region.angle) > 0.001f)
                        {
                            float cx = rx + rw / 2.0f;
                            float cy = ry + rh / 2.0f;
                            float angle_rad = region.angle * 3.14159265f / 180.0f;
                            float cos_a = std::cos(angle_rad);
                            float sin_a = std::sin(angle_rad);
                            
                            // Calculate rotated corners
                            ImVec2 corners[4];
                            float hw = rw / 2.0f;
                            float hh = rh / 2.0f;
                            corners[0] = ImVec2(cx + (-hw * cos_a - (-hh) * sin_a), cy + (-hw * sin_a + (-hh) * cos_a));
                            corners[1] = ImVec2(cx + (hw * cos_a - (-hh) * sin_a), cy + (hw * sin_a + (-hh) * cos_a));
                            corners[2] = ImVec2(cx + (hw * cos_a - hh * sin_a), cy + (hw * sin_a + hh * cos_a));
                            corners[3] = ImVec2(cx + (-hw * cos_a - hh * sin_a), cy + (-hw * sin_a + hh * cos_a));
                            
                            for (int i = 0; i < 4; i++)
                                draw_list->AddLine(corners[i], corners[(i + 1) % 4], IM_COL32(0, 255, 0, 255), 2.0f);
                        }
                        else
                        {
                            draw_list->AddRect(ImVec2(rx, ry), ImVec2(rx + rw, ry + rh),
                                               IM_COL32(0, 255, 0, 255), 0.0f, 0, 2.0f);
                        }
                        draw_list->AddText(ImVec2(rx + 5, ry + 5), IM_COL32(255, 255, 255, 255), region.name.c_str());
                    }
                }

                // Draw current drawing rectangle (yellow while dragging)
                if (drawing_motion_region_)
                {
                    float x1 = image_pos.x + std::min(region_draw_start_x_, region_draw_end_x_);
                    float y1 = image_pos.y + std::min(region_draw_start_y_, region_draw_end_y_);
                    float x2 = image_pos.x + std::max(region_draw_start_x_, region_draw_end_x_);
                    float y2 = image_pos.y + std::max(region_draw_start_y_, region_draw_end_y_);
                    draw_list->AddRect(ImVec2(x1, y1), ImVec2(x2, y2),
                                       IM_COL32(255, 255, 0, 255), 0.0f, 0, 2.0f);
                }

                // Draw pending region in blue with rotation preview
                bool has_drawn_region = !drawing_motion_region_ &&
                                        (std::abs(region_draw_end_x_ - region_draw_start_x_) > 5.0f) &&
                                        (std::abs(region_draw_end_y_ - region_draw_start_y_) > 5.0f);
                
                if (has_drawn_region)
                {
                    float x1 = std::min(region_draw_start_x_, region_draw_end_x_);
                    float y1 = std::min(region_draw_start_y_, region_draw_end_y_);
                    float x2 = std::max(region_draw_start_x_, region_draw_end_x_);
                    float y2 = std::max(region_draw_start_y_, region_draw_end_y_);
                    float rw = x2 - x1;
                    float rh = y2 - y1;
                    
                    // Draw rotated pending region in blue
                    if (std::abs(pending_region_angle_) > 0.001f)
                    {
                        float cx = image_pos.x + x1 + rw / 2.0f;
                        float cy = image_pos.y + y1 + rh / 2.0f;
                        float angle_rad = pending_region_angle_ * 3.14159265f / 180.0f;
                        float cos_a = std::cos(angle_rad);
                        float sin_a = std::sin(angle_rad);
                        
                        // Calculate rotated corners
                        ImVec2 corners[4];
                        float hw = rw / 2.0f;
                        float hh = rh / 2.0f;
                        corners[0] = ImVec2(cx + (-hw * cos_a - (-hh) * sin_a), cy + (-hw * sin_a + (-hh) * cos_a));
                        corners[1] = ImVec2(cx + (hw * cos_a - (-hh) * sin_a), cy + (hw * sin_a + (-hh) * cos_a));
                        corners[2] = ImVec2(cx + (hw * cos_a - hh * sin_a), cy + (hw * sin_a + hh * cos_a));
                        corners[3] = ImVec2(cx + (-hw * cos_a - hh * sin_a), cy + (-hw * sin_a + hh * cos_a));
                        
                        for (int i = 0; i < 4; i++)
                            draw_list->AddLine(corners[i], corners[(i + 1) % 4], IM_COL32(100, 150, 255, 255), 2.0f);
                    }
                    else
                    {
                        draw_list->AddRect(ImVec2(image_pos.x + x1, image_pos.y + y1),
                                         ImVec2(image_pos.x + x2, image_pos.y + y2),
                                         IM_COL32(100, 150, 255, 255), 0.0f, 0, 2.0f);
                    }
                }

                ImGui::Text("Size: %dx%d", motion_frame_width_, motion_frame_height_);
                ImGui::TextDisabled("Click and drag on the image to draw a motion region.");

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // Region management buttons
                ImGui::Text("Motion Region Management:");
                
                // Angle slider for pending region
                if (has_drawn_region)
                {
                    ImGui::PushItemWidth(200);
                    ImGui::SliderFloat("Region Angle", &pending_region_angle_, -180.0f, 180.0f, "%.1f°");
                    ImGui::PopItemWidth();
                    ImGui::SameLine();
                    ImGui::TextDisabled("(?)");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Rotate the motion region. 0° = no rotation.");
                    ImGui::Spacing();
                }

                ImGui::BeginDisabled(!has_drawn_region);
                if (ImGui::Button("Save Motion Region"))
                {
                    if (add_motion_region_callback_ && async_worker_ && has_drawn_region)
                    {
                        // Convert display coordinates to actual image coordinates
                        int x = static_cast<int>(std::min(region_draw_start_x_, region_draw_end_x_) / scale_x);
                        int y = static_cast<int>(std::min(region_draw_start_y_, region_draw_end_y_) / scale_y);
                        int w = static_cast<int>(std::abs(region_draw_end_x_ - region_draw_start_x_) / scale_x);
                        int h = static_cast<int>(std::abs(region_draw_end_y_ - region_draw_start_y_) / scale_y);
                        
                        std::string camera_name = selected_camera.name;
                        float angle = pending_region_angle_;
                        
                        // Reset drawing state immediately (UI feedback)
                        region_draw_start_x_ = 0;
                        region_draw_start_y_ = 0;
                        region_draw_end_x_ = 0;
                        region_draw_end_y_ = 0;
                        pending_region_angle_ = 0.0f;
                        
                        // Execute network calls in background
                        async_worker_->enqueueTask([this, camera_name, x, y, w, h, angle]() {
                            int region_id = add_motion_region_callback_(camera_name, x, y, w, h, angle);
                            
                            // Immediately fetch updated regions if successful
                            if (region_id != -1 && get_motion_regions_callback_)
                            {
                                auto regions = get_motion_regions_callback_(camera_name);
                                std::lock_guard<std::mutex> lock(motion_regions_mutex_);
                                motion_regions_ = std::move(regions);
                            }
                        });
                    }
                }
                ImGui::EndDisabled();

                if (has_drawn_region)
                {
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel Drawing"))
                    {
                        region_draw_start_x_ = 0;
                        region_draw_start_y_ = 0;
                        region_draw_end_x_ = 0;
                        region_draw_end_y_ = 0;
                        pending_region_angle_ = 0.0f;
                    }
                }

                ImGui::Spacing();

                // Region list and removal
                size_t region_count;
                {
                    std::lock_guard<std::mutex> lock(motion_regions_mutex_);
                    region_count = motion_regions_.size();
                }
                ImGui::Text("Existing Regions (%zu):", region_count);
                
                if (region_count > 0)
                {
                    std::lock_guard<std::mutex> lock(motion_regions_mutex_);

                    // Clamp selected region index
                    if (selected_region_index_ < 0 || selected_region_index_ >= static_cast<int>(motion_regions_.size()))
                    {
                        selected_region_index_ = 0;
                    }

                    const char* combo_preview = selected_region_index_ >= 0 && selected_region_index_ < static_cast<int>(motion_regions_.size())
                                                ? motion_regions_[selected_region_index_].name.c_str()
                                                : "Select region";

                    if (ImGui::BeginCombo("##region_select", combo_preview))
                    {
                        for (int i = 0; i < static_cast<int>(motion_regions_.size()); ++i)
                        {
                            bool is_selected = (selected_region_index_ == i);
                            std::string label = motion_regions_[i].name + " (" +
                                                std::to_string(motion_regions_[i].w) + "x" +
                                                std::to_string(motion_regions_[i].h) + ")";
                            if (ImGui::Selectable(label.c_str(), is_selected))
                            {
                                selected_region_index_ = i;
                            }
                            if (is_selected)
                            {
                                ImGui::SetItemDefaultFocus();
                            }
                        }
                        ImGui::EndCombo();
                    }

                    ImGui::SameLine();
                    if (ImGui::Button("Remove Region"))
                    {
                        if (remove_motion_region_callback_ && async_worker_ && 
                            selected_region_index_ >= 0 && selected_region_index_ < static_cast<int>(motion_regions_.size()))
                        {
                            int region_id = motion_regions_[selected_region_index_].id;
                            std::string camera_name = selected_camera.name;
                            
                            async_worker_->enqueueTask([this, camera_name, region_id]() {
                                if (remove_motion_region_callback_(camera_name, region_id))
                                {
                                    // Refresh regions
                                    if (get_motion_regions_callback_)
                                    {
                                        auto regions = get_motion_regions_callback_(camera_name);
                                        std::lock_guard<std::mutex> lock(motion_regions_mutex_);
                                        motion_regions_ = std::move(regions);
                                    }
                                }
                            });
                        }
                    }
                }
                else
                {
                    ImGui::TextDisabled("No motion regions defined.");
                }

                // Clear All button - always available
                if (ImGui::Button("Clear All Regions"))
                {
                    if (clear_motion_regions_callback_ && async_worker_)
                    {
                        std::string camera_name = selected_camera.name;
                        
                        async_worker_->enqueueTask([this, camera_name]() {
                            if (clear_motion_regions_callback_(camera_name))
                            {
                                std::lock_guard<std::mutex> lock(motion_regions_mutex_);
                                motion_regions_.clear();
                                selected_region_index_ = 0;
                            }
                        });
                    }
                }
                
                // Motion Detection Properties
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::Text("Motion Detection Properties:");
                
                // Create temporary variables for editing
                static float temp_motion_frame_scale = selected_camera.motion_frame_scale;
                static float temp_noise_threshold = selected_camera.noise_threshold;
                static float temp_motion_threshold = selected_camera.motion_threshold;
                static int temp_motion_min_hits = selected_camera.motion_min_hits;
                static int temp_motion_decay = selected_camera.motion_decay;
                static float temp_motion_arrow_scale = selected_camera.motion_arrow_scale;
                static int temp_motion_arrow_thickness = selected_camera.motion_arrow_thickness;
                
                // Update temp values when camera changes
                static int last_selected_camera = selected_camera_index_;
                if (last_selected_camera != selected_camera_index_)
                {
                    temp_motion_frame_scale = selected_camera.motion_frame_scale;
                    temp_noise_threshold = selected_camera.noise_threshold;
                    temp_motion_threshold = selected_camera.motion_threshold;
                    temp_motion_min_hits = selected_camera.motion_min_hits;
                    temp_motion_decay = selected_camera.motion_decay;
                    temp_motion_arrow_scale = selected_camera.motion_arrow_scale;
                    temp_motion_arrow_thickness = selected_camera.motion_arrow_thickness;
                    last_selected_camera = selected_camera_index_;
                }
                
                bool properties_changed = false;
                
                // Check if there are any motion regions defined
                bool has_motion_regions = false;
                {
                    std::lock_guard<std::mutex> lock(motion_regions_mutex_);
                    has_motion_regions = !motion_regions_.empty();
                }
                
                ImGui::Text("Current Values:");
                ImGui::PushItemWidth(150);
                
                // Disable motion frame scale slider if motion regions exist
                if (has_motion_regions)
                {
                    ImGui::BeginDisabled();
                }
                
                if (ImGui::SliderFloat("Motion Frame Scale", &temp_motion_frame_scale, 0.1f, 2.0f, "%.2f"))
                    properties_changed = true;
                
                // Add tooltip to the disabled slider itself
                if (has_motion_regions && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                {
                    ImGui::SetTooltip("Cannot change scale while motion regions are defined.\nClear all regions first to adjust this setting.");
                }
                
                ImGui::SameLine();
                ImGui::TextDisabled("(?)");
                if (ImGui::IsItemHovered())
                {
                    if (has_motion_regions)
                    {
                        ImGui::SetTooltip("Cannot change scale while motion regions are defined.\nClear all regions first to adjust this setting.");
                    }
                    else
                    {
                        ImGui::SetTooltip("Scale factor for motion frame size. Lower = faster processing.");
                    }
                }
                
                if (has_motion_regions)
                {
                    ImGui::EndDisabled();
                }
                
                if (ImGui::SliderFloat("Noise Threshold", &temp_noise_threshold, 0.0f, 10.0f, "%.2f"))
                    properties_changed = true;
                ImGui::SameLine();
                ImGui::TextDisabled("(?)");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Minimum pixel movement to consider (filters out noise).");
                
                if (ImGui::SliderFloat("Motion Threshold", &temp_motion_threshold, 0.0f, 50.0f, "%.2f"))
                    properties_changed = true;
                ImGui::SameLine();
                ImGui::TextDisabled("(?)");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Average motion required to trigger detection.");
                
                if (ImGui::SliderInt("Motion Min Hits", &temp_motion_min_hits, 1, 20))
                    properties_changed = true;
                ImGui::SameLine();
                ImGui::TextDisabled("(?)");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Number of consecutive frames with motion to trigger.");
                
                if (ImGui::SliderInt("Motion Decay", &temp_motion_decay, 0, 10))
                    properties_changed = true;
                ImGui::SameLine();
                ImGui::TextDisabled("(?)");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("How quickly motion counter decreases when no motion.");
                
                if (ImGui::SliderFloat("Arrow Scale", &temp_motion_arrow_scale, 1.0f, 10.0f, "%.1f"))
                    properties_changed = true;
                ImGui::SameLine();
                ImGui::TextDisabled("(?)");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Visual scale for motion arrows on frame.");
                
                if (ImGui::SliderInt("Arrow Thickness", &temp_motion_arrow_thickness, 1, 5))
                    properties_changed = true;
                ImGui::SameLine();
                ImGui::TextDisabled("(?)");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Line thickness for motion arrows.");
                
                ImGui::PopItemWidth();
                
                ImGui::Spacing();
                if (ImGui::Button("Apply Changes") || (properties_changed && ImGui::IsKeyPressed(ImGuiKey_Enter)))
                {
                    if (async_worker_)
                    {
                        std::string camera_name = selected_camera.name;
                        std::string endpoint(server_endpoint_.data());
                        
                        // Copy values to non-static variables for lambda capture
                        float motion_frame_scale = temp_motion_frame_scale;
                        float noise_threshold = temp_noise_threshold;
                        float motion_threshold = temp_motion_threshold;
                        int motion_min_hits = temp_motion_min_hits;
                        int motion_decay = temp_motion_decay;
                        float motion_arrow_scale = temp_motion_arrow_scale;
                        int motion_arrow_thickness = temp_motion_arrow_thickness;
                        
                        async_worker_->enqueueTask([endpoint, camera_name, motion_frame_scale, noise_threshold, 
                                                     motion_threshold, motion_min_hits, motion_decay, 
                                                     motion_arrow_scale, motion_arrow_thickness]() {
                            client_network::update_camera_properties(endpoint, camera_name,
                                                                      motion_frame_scale, noise_threshold, motion_threshold,
                                                                      motion_min_hits, motion_decay, 
                                                                      motion_arrow_scale, motion_arrow_thickness);
                        });
                        
                        properties_changed = false;
                    }
                }
                
                if (properties_changed)
                {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "* Unsaved changes");
                }
            }
            else
            {
                ImGui::TextDisabled("No motion frame available.");
                ImGui::TextUnformatted("Motion frame will appear when motion is detected.");
            }
        }
        else if (!motion_enabled)
        {
            ImGui::TextDisabled("Enable motion detection to view motion frames.");
        }

        ImGui::EndTabItem();
    }
}

void ConfigurationPanel::renderInfoTab(bool set_selected)
{
    ImGuiTabItemFlags flags = set_selected ? ImGuiTabItemFlags_SetSelected : 0;
    if (ImGui::BeginTabItem("Info", nullptr, flags))
    {
        active_tab_ = Tab::Info;

        ImGui::TextUnformatted("Active Threads");
        ImGui::Separator();

        if (thread_info_callback_)
        {
            auto threads = thread_info_callback_();

            if (threads.empty())
            {
                ImGui::TextDisabled("No thread information available.");
            }
            else
            {
                ImGui::Text("Total threads: %zu", threads.size());
                ImGui::Spacing();

                if (ImGui::BeginTable("ThreadTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
                {
                    ImGui::TableSetupColumn("Thread Name", ImGuiTableColumnFlags_WidthFixed, 200.0f);
                    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                    ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableHeadersRow();

                    for (const auto &thread : threads)
                    {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted(thread.name.c_str());

                        ImGui::TableSetColumnIndex(1);
                        ImVec4 status_color = thread.is_active ? ImVec4(0.2f, 0.8f, 0.2f, 1.0f) : ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
                        ImGui::TextColored(status_color, "%s", thread.is_active ? "Active" : "Stopped");

                        ImGui::TableSetColumnIndex(2);
                        ImGui::TextUnformatted(thread.details.c_str());
                    }

                    ImGui::EndTable();
                }
            }
        }
        else
        {
            ImGui::TextDisabled("Thread information callback not configured.");
        }

        ImGui::EndTabItem();
    }
}
