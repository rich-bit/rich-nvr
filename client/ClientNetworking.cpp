#include "ClientNetworking.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

#include <httplib.h>
#include <nlohmann/json.hpp>

// Debug flag for motion frame operations
// Set to true to enable verbose logging for motion regions and motion frames
static constexpr bool DEBUG_MOTION_FRAME = false;

namespace client_network {
namespace {

std::string sanitize_camera_name(const std::string &name) {
    std::string safe;
    safe.reserve(name.size());
    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') {
            safe += c;
        }
    }
    return safe;
}

struct EndpointParts {
    std::string scheme;
    std::string host;
    int port = 0;
    std::string base_path;
};

EndpointParts parse_endpoint(const std::string &endpoint) {
    EndpointParts parts;
    parts.scheme = "http";
    parts.port = 80;
    parts.base_path = "/";

    std::string working = endpoint;
    if (working.empty()) {
        return parts;
    }

    if (auto scheme_pos = working.find("://"); scheme_pos != std::string::npos) {
        parts.scheme = working.substr(0, scheme_pos);
        working.erase(0, scheme_pos + 3);
    }

    auto slash_pos = working.find('/');
    std::string host_segment = slash_pos == std::string::npos ? working : working.substr(0, slash_pos);
    if (slash_pos != std::string::npos) {
        parts.base_path = working.substr(slash_pos);
        if (parts.base_path.empty()) {
            parts.base_path = "/";
        }
    }

    if (parts.scheme == "https") {
        parts.port = 443;
    }

    if (!host_segment.empty()) {
        auto parse_port = [](const std::string &text) -> int {
            try {
                return std::stoi(text);
            } catch (...) {
                return 0;
            }
        };
        if (host_segment.front() == '[') {
            auto closing = host_segment.find(']');
            if (closing != std::string::npos) {
                parts.host = host_segment.substr(1, closing - 1);
                if (closing + 1 < host_segment.size() && host_segment[closing + 1] == ':') {
                    parts.port = parse_port(host_segment.substr(closing + 2));
                }
            }
        } else {
            auto colon_pos = host_segment.find(':');
            if (colon_pos != std::string::npos) {
                parts.host = host_segment.substr(0, colon_pos);
                parts.port = parse_port(host_segment.substr(colon_pos + 1));
            } else {
                parts.host = host_segment;
            }
        }
    }

    if (parts.port == 0) {
        parts.port = (parts.scheme == "https") ? 443 : 80;
    }

    if (parts.base_path.empty()) {
        parts.base_path = "/";
    }

    return parts;
}

std::string join_paths(const std::string &base, const std::string &suffix) {
    std::string normalized_base = base;
    if (normalized_base.empty()) {
        normalized_base = "/";
    }
    if (normalized_base.back() == '/' && normalized_base.size() > 1) {
        normalized_base.pop_back();
    }

    std::string normalized_suffix = suffix;
    if (normalized_suffix.empty()) {
        normalized_suffix = "/";
    } else if (normalized_suffix.front() != '/') {
        normalized_suffix.insert(normalized_suffix.begin(), '/');
    }

    if (normalized_base == "/") {
        return normalized_suffix;
    }
    return normalized_base + normalized_suffix;
}

std::string format_float(float value) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3) << value;
    return oss.str();
}

} // namespace

std::string extract_host_from_endpoint(const std::string &endpoint) {
    if (endpoint.empty()) {
        return {};
    }
    std::string host_part = endpoint;
    if (auto scheme_pos = host_part.find("://"); scheme_pos != std::string::npos) {
        host_part.erase(0, scheme_pos + 3);
    }
    if (auto slash_pos = host_part.find('/'); slash_pos != std::string::npos) {
        host_part.erase(slash_pos);
    }
    if (host_part.empty()) {
        return {};
    }
    if (!host_part.empty() && host_part.front() == '[') {
        auto closing = host_part.find(']');
        if (closing != std::string::npos) {
            return host_part.substr(0, closing + 1);
        }
    }
    if (auto colon_pos = host_part.find(':'); colon_pos != std::string::npos) {
        host_part.erase(colon_pos);
    }
    return host_part;
}

std::string build_proxy_rtsp_url(const std::string &endpoint, const std::string &camera_name) {
    std::string host = extract_host_from_endpoint(endpoint);
    if (host.empty()) {
        return {};
    }

    std::string sanitized = sanitize_camera_name(camera_name);
    if (sanitized.empty()) {
        sanitized = "camera";
    }

    return "rtsp://" + host + ":8554/cam/" + sanitized;
}

AddCameraResult send_add_camera_request(const AddCameraRequest &request, std::string &response_body) {
    AddCameraResult result;
    result.success = false;

    EndpointParts parts = parse_endpoint(request.server_endpoint);
    if (parts.host.empty()) {
        result.message = "Invalid server endpoint.";
        return result;
    }

    httplib::Params params;
    params.emplace("name", request.name);
    params.emplace("uri", request.rtsp_url);
    params.emplace("segment", request.segment ? "1" : "0");
    params.emplace("recording", request.recording ? "1" : "0");
    params.emplace("overlay", request.overlay ? "1" : "0");
    params.emplace("motion_frame", request.motion_frame ? "1" : "0");
    params.emplace("gstreamerEncodedProxy", request.gstreamer_proxy ? "1" : "0");
    params.emplace("live555proxied", request.live555_proxy ? "1" : "0");
    if (request.segment_bitrate > 0) {
        params.emplace("segment_bitrate", std::to_string(request.segment_bitrate));
    }
    if (!request.segment_speed_preset.empty()) {
        params.emplace("segment_speed_preset", request.segment_speed_preset);
    }
    if (request.proxy_bitrate > 0) {
        params.emplace("proxy_bitrate", std::to_string(request.proxy_bitrate));
    }
    if (!request.proxy_speed_preset.empty()) {
        params.emplace("proxy_speed_preset", request.proxy_speed_preset);
    }
    if (request.motion_frame_width > 0) {
        params.emplace("motion_frame_w", std::to_string(request.motion_frame_width));
    }
    if (request.motion_frame_height > 0) {
        params.emplace("motion_frame_h", std::to_string(request.motion_frame_height));
    }
    if (request.motion_frame_scale > 0.0f) {
        params.emplace("motion_frame_scale", format_float(request.motion_frame_scale));
    }
    if (request.noise_threshold > 0.0f) {
        params.emplace("noise_threshold", format_float(request.noise_threshold));
    }
    if (request.motion_threshold > 0.0f) {
        params.emplace("motion_threshold", format_float(request.motion_threshold));
    }
    if (request.motion_min_hits > 0) {
        params.emplace("motion_min_hits", std::to_string(request.motion_min_hits));
    }
    if (request.motion_decay > 0) {
        params.emplace("motion_decay", std::to_string(request.motion_decay));
    }
    if (request.motion_arrow_scale > 0.0f) {
        params.emplace("motion_arrow_scale", format_float(request.motion_arrow_scale));
    }
    if (request.motion_arrow_thickness > 0) {
        params.emplace("motion_arrow_thickness", std::to_string(request.motion_arrow_thickness));
    }

    std::string path = join_paths(parts.base_path, "add_camera");

    auto perform_request = [&](auto &client_impl) -> AddCameraResult {
        AddCameraResult inner_result;
        inner_result.success = false;

        client_impl.set_read_timeout(10, 0);
        client_impl.set_write_timeout(10, 0);
        client_impl.set_follow_location(true);

        auto res = client_impl.Post(path.c_str(), params);
        if (!res) {
            inner_result.message = "Request failed";
            inner_result.message += ": ";
            inner_result.message += httplib::to_string(res.error());
            return inner_result;
        }

        response_body = res->body;
        if (res->status < 200 || res->status >= 300) {
            inner_result.message = response_body.empty() ? ("Server returned status " + std::to_string(res->status))
                                                         : response_body;
            return inner_result;
        }

        inner_result.success = true;
        inner_result.message = response_body.empty() ? "Camera added via RichServer." : response_body;
        return inner_result;
    };

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    if (parts.scheme == "https") {
        httplib::SSLClient client_impl(parts.host, parts.port);
        client_impl.enable_server_certificate_verification(false);
        return perform_request(client_impl);
    }
#else
    if (parts.scheme == "https") {
        result.message = "HTTPS support not available.";
        return result;
    }
#endif

    if (parts.scheme != "http") {
        result.message = "Unsupported scheme: " + parts.scheme;
        return result;
    }

    httplib::Client client_impl(parts.host, parts.port);
    return perform_request(client_impl);
}

bool toggle_motion_detection(const std::string &endpoint, const std::string &camera_name, bool enable) {
    EndpointParts parts = parse_endpoint(endpoint);
    if (parts.host.empty()) {
        return false;
    }

    httplib::Params params;
    params.emplace("name", camera_name);
    params.emplace("value", enable ? "on" : "off");

    std::string path = join_paths(parts.base_path, "toggle_motion");

    auto perform_request = [&](auto &client_impl) -> bool {
        client_impl.set_read_timeout(5, 0);
        client_impl.set_write_timeout(5, 0);

        auto res = client_impl.Post(path.c_str(), params);
        if (!res) {
            return false;
        }

        return (res->status >= 200 && res->status < 300);
    };

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    if (parts.scheme == "https") {
        httplib::SSLClient client_impl(parts.host, parts.port);
        client_impl.enable_server_certificate_verification(false);
        return perform_request(client_impl);
    }
#endif

    if (parts.scheme != "http") {
        return false;
    }

    httplib::Client client_impl(parts.host, parts.port);
    return perform_request(client_impl);
}

bool fetch_motion_frame_jpeg(const std::string &endpoint, const std::string &camera_name, std::vector<unsigned char> &jpeg_data) {
    EndpointParts parts = parse_endpoint(endpoint);
    if (parts.host.empty()) {
        return false;
    }

    std::string path = join_paths(parts.base_path, "motion_frame");
    path += "?name=" + camera_name;

    auto perform_request = [&](auto &client_impl) -> bool {
        client_impl.set_read_timeout(5, 0);
        client_impl.set_write_timeout(5, 0);

        auto res = client_impl.Get(path.c_str());
        if (!res) {
            return false;
        }

        if (res->status != 200) {
            return false;
        }

        jpeg_data.assign(res->body.begin(), res->body.end());
        return !jpeg_data.empty();
    };

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    if (parts.scheme == "https") {
        httplib::SSLClient client_impl(parts.host, parts.port);
        client_impl.enable_server_certificate_verification(false);
        return perform_request(client_impl);
    }
#endif

    if (parts.scheme != "http") {
        return false;
    }

    httplib::Client client_impl(parts.host, parts.port);
    return perform_request(client_impl);
}

int add_motion_region(const std::string &endpoint, const std::string &camera_name, int x, int y, int w, int h, float angle) {
    EndpointParts parts = parse_endpoint(endpoint);
    if (parts.host.empty()) {
        return -1;
    }

    httplib::Params params;
    params.emplace("name", camera_name);
    params.emplace("x", std::to_string(x));
    params.emplace("y", std::to_string(y));
    params.emplace("w", std::to_string(w));
    params.emplace("h", std::to_string(h));
    if (angle != 0.0f) {
        params.emplace("angle", format_float(angle));
    }

    std::string path = join_paths(parts.base_path, "add_motion_region");

    auto perform_request = [&](auto &client_impl) -> int {
        client_impl.set_read_timeout(5, 0);
        client_impl.set_write_timeout(5, 0);

        auto res = client_impl.Post(path.c_str(), params);
        if (!res || res->status != 200) {
            return -1;
        }

        try {
            nlohmann::json j = nlohmann::json::parse(res->body);
            if (j.contains("region_id")) {
                return j["region_id"].get<int>();
            }
        } catch (...) {
            return -1;
        }
        return -1;
    };

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    if (parts.scheme == "https") {
        httplib::SSLClient client_impl(parts.host, parts.port);
        client_impl.enable_server_certificate_verification(false);
        return perform_request(client_impl);
    }
#endif

    if (parts.scheme != "http") {
        return -1;
    }

    httplib::Client client_impl(parts.host, parts.port);
    return perform_request(client_impl);
}

bool remove_motion_region(const std::string &endpoint, const std::string &camera_name, int region_id) {
    EndpointParts parts = parse_endpoint(endpoint);
    if (parts.host.empty()) {
        return false;
    }

    httplib::Params params;
    params.emplace("name", camera_name);
    params.emplace("region_id", std::to_string(region_id));

    std::string path = join_paths(parts.base_path, "remove_motion_region");

    auto perform_request = [&](auto &client_impl) -> bool {
        client_impl.set_read_timeout(5, 0);
        client_impl.set_write_timeout(5, 0);

        auto res = client_impl.Post(path.c_str(), params);
        return (res && res->status >= 200 && res->status < 300);
    };

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    if (parts.scheme == "https") {
        httplib::SSLClient client_impl(parts.host, parts.port);
        client_impl.enable_server_certificate_verification(false);
        return perform_request(client_impl);
    }
#endif

    if (parts.scheme != "http") {
        return false;
    }

    httplib::Client client_impl(parts.host, parts.port);
    return perform_request(client_impl);
}

bool clear_motion_regions(const std::string &endpoint, const std::string &camera_name) {
    EndpointParts parts = parse_endpoint(endpoint);
    if (parts.host.empty()) {
        return false;
    }

    httplib::Params params;
    params.emplace("name", camera_name);

    std::string path = join_paths(parts.base_path, "clear_motion_regions");

    auto perform_request = [&](auto &client_impl) -> bool {
        client_impl.set_read_timeout(5, 0);
        client_impl.set_write_timeout(5, 0);

        auto res = client_impl.Post(path.c_str(), params);
        return (res && res->status >= 200 && res->status < 300);
    };

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    if (parts.scheme == "https") {
        httplib::SSLClient client_impl(parts.host, parts.port);
        client_impl.enable_server_certificate_verification(false);
        return perform_request(client_impl);
    }
#endif

    if (parts.scheme != "http") {
        return false;
    }

    httplib::Client client_impl(parts.host, parts.port);
    return perform_request(client_impl);
}

std::vector<ConfigurationPanel::MotionRegion> get_motion_regions(const std::string &endpoint, const std::string &camera_name) {
    std::vector<ConfigurationPanel::MotionRegion> regions;
    
    EndpointParts parts = parse_endpoint(endpoint);
    if (parts.host.empty()) {
        return regions;
    }

    std::string path = join_paths(parts.base_path, "get_motion_regions");
    path += "?name=" + camera_name;

    auto perform_request = [&](auto &client_impl) -> std::vector<ConfigurationPanel::MotionRegion> {
        std::vector<ConfigurationPanel::MotionRegion> result;
        
        client_impl.set_read_timeout(5, 0);
        client_impl.set_write_timeout(5, 0);

        auto res = client_impl.Get(path.c_str());
        if (!res || res->status != 200) {
            if (DEBUG_MOTION_FRAME) {
                std::cout << "[get_motion_regions] Request failed - status: " 
                          << (res ? std::to_string(res->status) : "no response") << std::endl;
            }
            return result;
        }

        if (DEBUG_MOTION_FRAME) {
            std::cout << "[get_motion_regions] Response from server:\n" << res->body << std::endl;
        }

        try {
            nlohmann::json j = nlohmann::json::parse(res->body);
            if (j.contains("regions") && j["regions"].is_array()) {
                if (DEBUG_MOTION_FRAME) {
                    std::cout << "[get_motion_regions] Found " << j["regions"].size() << " regions" << std::endl;
                }
                for (const auto& region_json : j["regions"]) {
                    ConfigurationPanel::MotionRegion region;
                    region.id = region_json.value("id", 0);
                    region.name = region_json.value("name", "");
                    region.x = region_json.value("x", 0);
                    region.y = region_json.value("y", 0);
                    region.w = region_json.value("w", 0);
                    region.h = region_json.value("h", 0);
                    region.angle = region_json.value("angle", 0.0f);
                    if (DEBUG_MOTION_FRAME) {
                        std::cout << "  Region " << region.id << ": " << region.name 
                                  << " at (" << region.x << "," << region.y 
                                  << ") size " << region.w << "x" << region.h << std::endl;
                    }
                    result.push_back(region);
                }
            } else {
                if (DEBUG_MOTION_FRAME) {
                    std::cout << "[get_motion_regions] No 'regions' array in response" << std::endl;
                }
            }
        } catch (const std::exception& e) {
            if (DEBUG_MOTION_FRAME) {
                std::cout << "[get_motion_regions] JSON parse error: " << e.what() << std::endl;
            }
            return result;
        }
        
        return result;
    };

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    if (parts.scheme == "https") {
        httplib::SSLClient client_impl(parts.host, parts.port);
        client_impl.enable_server_certificate_verification(false);
        return perform_request(client_impl);
    }
#endif

    if (parts.scheme != "http") {
        return regions;
    }

    httplib::Client client_impl(parts.host, parts.port);
    return perform_request(client_impl);
}

bool update_camera_properties(const std::string &endpoint, const std::string &camera_name,
                               float motion_frame_scale, float noise_threshold, float motion_threshold,
                               int motion_min_hits, int motion_decay, float motion_arrow_scale, int motion_arrow_thickness) {
    EndpointParts parts = parse_endpoint(endpoint);
    if (parts.host.empty()) {
        return false;
    }

    httplib::Params params;
    params.emplace("name", camera_name);
    params.emplace("motion_frame_scale", format_float(motion_frame_scale));
    params.emplace("noise_threshold", format_float(noise_threshold));
    params.emplace("motion_threshold", format_float(motion_threshold));
    params.emplace("motion_min_hits", std::to_string(motion_min_hits));
    params.emplace("motion_decay", std::to_string(motion_decay));
    params.emplace("motion_arrow_scale", format_float(motion_arrow_scale));
    params.emplace("motion_arrow_thickness", std::to_string(motion_arrow_thickness));

    std::string path = join_paths(parts.base_path, "update_camera_properties");

    auto perform_request = [&](auto &client_impl) -> bool {
        client_impl.set_read_timeout(5, 0);
        client_impl.set_write_timeout(5, 0);

        auto res = client_impl.Post(path.c_str(), params);
        return (res && res->status >= 200 && res->status < 300);
    };

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    if (parts.scheme == "https") {
        httplib::SSLClient client_impl(parts.host, parts.port);
        client_impl.enable_server_certificate_verification(false);
        return perform_request(client_impl);
    }
#endif

    if (parts.scheme != "http") {
        return false;
    }

    httplib::Client client_impl(parts.host, parts.port);
    return perform_request(client_impl);
}

bool toggle_segment_recording(const std::string &endpoint, const std::string &camera_name, bool enable) {
    EndpointParts parts = parse_endpoint(endpoint);
    if (parts.host.empty()) {
        return false;
    }

    httplib::Params params;
    params.emplace("name", camera_name);
    params.emplace("segment_recording", enable ? "1" : "0");

    std::string path = join_paths(parts.base_path, "update_camera_properties");

    auto perform_request = [&](auto &client_impl) -> bool {
        client_impl.set_read_timeout(5, 0);
        client_impl.set_write_timeout(5, 0);

        auto res = client_impl.Post(path.c_str(), params);
        return (res && res->status >= 200 && res->status < 300);
    };

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    if (parts.scheme == "https") {
        httplib::SSLClient client_impl(parts.host, parts.port);
        client_impl.enable_server_certificate_verification(false);
        return perform_request(client_impl);
    }
#endif

    if (parts.scheme != "http") {
        return false;
    }

    httplib::Client client_impl(parts.host, parts.port);
    return perform_request(client_impl);
}

bool remove_camera(const std::string &endpoint, const std::string &camera_name) {
    EndpointParts parts = parse_endpoint(endpoint);
    if (parts.host.empty()) {
        return false;
    }

    httplib::Params params;
    params.emplace("name", camera_name);

    std::string path = join_paths(parts.base_path, "remove_camera");

    auto perform_request = [&](auto &client_impl) -> bool {
        client_impl.set_read_timeout(5, 0);
        client_impl.set_write_timeout(5, 0);

        auto res = client_impl.Post(path.c_str(), params);
        return (res && res->status >= 200 && res->status < 300);
    };

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    if (parts.scheme == "https") {
        httplib::SSLClient client_impl(parts.host, parts.port);
        client_impl.enable_server_certificate_verification(false);
        return perform_request(client_impl);
    }
#endif

    if (parts.scheme != "http") {
        return false;
    }

    httplib::Client client_impl(parts.host, parts.port);
    return perform_request(client_impl);
}

std::vector<ConfigurationPanel::CameraInfo> get_cameras_from_server(const std::string &endpoint) {
    std::vector<ConfigurationPanel::CameraInfo> cameras;
    
    EndpointParts parts = parse_endpoint(endpoint);
    if (parts.host.empty()) {
        return cameras;
    }

    std::string path = join_paths(parts.base_path, "get_cameras");

    auto perform_request = [&](auto &client_impl) -> std::vector<ConfigurationPanel::CameraInfo> {
        std::vector<ConfigurationPanel::CameraInfo> result;
        
        client_impl.set_read_timeout(5, 0);
        client_impl.set_write_timeout(5, 0);

        auto res = client_impl.Get(path.c_str());
        if (!res || res->status != 200) {
            return result;
        }

        try {
            nlohmann::json j = nlohmann::json::parse(res->body);
            if (j.is_array()) {
                for (const auto& cam_json : j) {
                    ConfigurationPanel::CameraInfo info;
                    info.name = cam_json.value("name", "");
                    info.via_server = true; // Cameras from server are always via_server
                    info.motion_enabled = cam_json.value("motion_frame", false);
                    info.segment_recording = cam_json.value("segment", false);
                    info.motion_frame_scale = cam_json.value("motion_frame_scale", 1.0f);
                    info.noise_threshold = cam_json.value("noise_threshold", 1.0f);
                    info.motion_threshold = cam_json.value("motion_threshold", 5.0f);
                    info.motion_min_hits = cam_json.value("motion_min_hits", 3);
                    info.motion_decay = cam_json.value("motion_decay", 1);
                    info.motion_arrow_scale = cam_json.value("motion_arrow_scale", 2.5f);
                    info.motion_arrow_thickness = cam_json.value("motion_arrow_thickness", 1);
                    result.push_back(info);
                }
            }
        } catch (const std::exception& e) {
            if (DEBUG_MOTION_FRAME) {
                std::cout << "[get_cameras_from_server] JSON parse error: " << e.what() << std::endl;
            }
            return result;
        }
        
        return result;
    };

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    if (parts.scheme == "https") {
        httplib::SSLClient client_impl(parts.host, parts.port);
        client_impl.enable_server_certificate_verification(false);
        return perform_request(client_impl);
    }
#endif

    if (parts.scheme != "http") {
        return cameras;
    }

    httplib::Client client_impl(parts.host, parts.port);
    return perform_request(client_impl);
}

ServerHealthInfo check_server_health(const std::string &endpoint) {
    ServerHealthInfo health;
    health.available = false;
    health.http_port = 0;
    health.rtsp_proxy_port = 0;
    health.camera_count = 0;
    health.uptime_seconds = 0;
    
    auto parts = parse_endpoint(endpoint);
    if (parts.host.empty()) {
        health.error_message = "Invalid endpoint";
        return health;
    }

    auto perform_request = [&](auto& client) -> ServerHealthInfo {
        client.set_connection_timeout(2, 0);  // 2 second timeout
        client.set_read_timeout(2, 0);
        
        auto res = client.Get("/health");
        if (!res) {
            health.error_message = "Connection failed";
            return health;
        }
        
        if (res->status != 200) {
            health.error_message = "Server returned status " + std::to_string(res->status);
            return health;
        }
        
        try {
            nlohmann::json j = nlohmann::json::parse(res->body);
            health.available = j.value("ok", false);
            health.http_port = j.value("http_port", 0);
            health.rtsp_proxy_port = j.value("rtsp_proxy_port", 0);
            health.camera_count = j.value("camera_count", 0);
            health.uptime_seconds = j.value("uptime_s", 0);
            health.error_message = "";
        } catch (const std::exception& e) {
            health.error_message = "JSON parse error";
            return health;
        }
        
        return health;
    };

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    if (parts.scheme == "https") {
        httplib::SSLClient ssl_client_impl(parts.host, parts.port);
        return perform_request(ssl_client_impl);
    }
#endif

    httplib::Client client_impl(parts.host, parts.port);
    return perform_request(client_impl);
}

} // namespace client_network
