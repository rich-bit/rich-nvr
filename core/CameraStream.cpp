#include "CameraStream.h"
#include "PathUtils.h"
#include "SegmentWorker.h"
#include "VideoExporter.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>

CameraStream::CameraStream(
    const std::string &name, const std::string &uri, Settings &settings,
    bool segment, bool recording, bool overlay, bool motion_frame,
    bool gstreamerEncodedProxy, bool live555Proxied, int proxy_bitrate,
    std::string proxy_speed_preset, int segment_bitrate,
    std::string segment_speed_preset, cv::Size motion_frame_size,
    float motion_frame_scale, float noise_threshold, float motion_threshold,
    int motion_min_hits, int motion_decay, float motion_arrow_scale,
    int motion_arrow_thickness, std::string video_output_format)
    : name_(name), uri_(uri), settings_(settings), mount_point_("/" + name),
      segment_(segment), recording_(recording), overlay_(overlay),
      motion_frame_(motion_frame),
      gstreamerEncodedProxy_(gstreamerEncodedProxy),
      live555Proxied_(live555Proxied), proxy_bitrate_(proxy_bitrate),
      proxy_speed_preset_(proxy_speed_preset),
      segment_bitrate_(segment_bitrate),
      segment_speed_preset_(segment_speed_preset),
      motion_frame_size_(motion_frame_size),
      motion_frame_scale_(motion_frame_scale),
      noise_threshold_(noise_threshold), motion_threshold_(motion_threshold),
      motion_min_hits_(motion_min_hits), motion_decay_(motion_decay),
      motion_arrow_scale_(motion_arrow_scale),
      motion_arrow_thickness_(motion_arrow_thickness),
      video_output_format_(video_output_format)
{

  // Probe stream for audio
  ProbeRtspAudio(uri_, pr_, /*timeout_ms=*/1500);
  std::cout << "[CameraStream CTR] Stream " << uri_ 
            << " probed: " << (pr_.probed ? "yes" : "no")
            << ", has audio: " << (pr_.has_audio ? "yes" : "no") << std::endl;

  std::string base_dir = core::PathUtils::getExecutableDir();
  std::string safe_name = core::PathUtils::sanitizeCameraName(name);
  output_path_ = base_dir + "/media/" + safe_name;
  std::string segment_dir = output_path_ + "/tmp/";

  core::PathUtils::ensureDirExists(output_path_);
  core::PathUtils::ensureDirExists(segment_dir);

  // segment_path = segment_dir + "segment-%03d.mp4";
  segment_path = segment_dir + "segment-%03d.mkv";

  segmentWorker_ = std::make_unique<SegmentWorker>(segment_dir, 500);
}

CameraStream::~CameraStream() { stop(); }

void CameraStream::start()
{
  if (running_)
    return;

  std::string pipeline_desc;

  if (pr_.probed && pr_.has_audio)
  {

    pipeline_desc = buildPipelineWithAudio();
    std::cout << "Pipeline string: " << pipeline_desc << std::endl;
  }
  else
  {
    pipeline_desc = buildPipelineWithoutAudio();
    std::cout << "Pipeline string: " << pipeline_desc << std::endl;
  }

  GError *error = nullptr;
  pipeline_ = gst_parse_launch(pipeline_desc.c_str(), &error);

  if (!pipeline_)
  {
    std::cerr << "Failed to create pipeline: "
              << (error ? error->message : "Unknown error") << std::endl;
    if (error)
      g_error_free(error);

    running_ = false;
    return;
  }

  gst_element_set_state(static_cast<GstElement *>(pipeline_),
                        GST_STATE_PLAYING);

  if (motion_frame_)
  {
    motion_sink_ = gst_bin_get_by_name(GST_BIN(pipeline_), "motion_sink");
    // Optionally check for null and log error if not found
    if (!motion_sink_)
    {
      std::cerr << "appsink 'motion_sink' not found in pipeline!" << std::endl;
    }

    startMotionLoop();
  }

  // Recreate segmentWorker if needed before starting
  if (segment_ && !segmentWorker_)
  {
    std::string base_dir = core::PathUtils::getExecutableDir();
    std::string safe_name = core::PathUtils::sanitizeCameraName(name_);
    std::string segment_dir = base_dir + "/media/" + safe_name + "/tmp/";
    segmentWorker_ = std::make_unique<SegmentWorker>(segment_dir, 500);
  }

  if (segment_)
    segmentWorker_->start();

  running_ = true;
}

void CameraStream::stop()
{
  running_ = false; // Signal the motion loop to exit
  motion_running_ = false;

  // Join the thread if running
  if (motion_thread_.joinable())
    motion_thread_.join();

  if (pipeline_)
  {
    gst_element_set_state(static_cast<GstElement *>(pipeline_), GST_STATE_NULL);
    gst_object_unref(static_cast<GstElement *>(pipeline_));
    pipeline_ = nullptr;
  }
  if (motion_sink_)
  {
    gst_object_unref(motion_sink_);
    motion_sink_ = nullptr;
  }

  if (segmentWorker_)
  {
    segmentWorker_->stop();
    segmentWorker_.reset();
  }
}

void CameraStream::enableSegmentRecording()
{
  if (segment_)
    return;
  segment_ = true;
  rebuild();
}
void CameraStream::disableSegmentRecording()
{
  if (!segment_)
    return;
  segment_ = false;
  rebuild();
}

void CameraStream::enableFullRecording(const std::string &filename)
{
  if (recording_ && recordFile_ == filename)
    return;
  recording_ = true;
  recordFile_ = filename;
  rebuild();
}
void CameraStream::disableFullRecording()
{
  if (!recording_)
    return;
  recording_ = false;
  recordFile_.clear();
  rebuild();
}

void CameraStream::enableTimestampOverlay()
{
  if (overlay_)
    return;
  overlay_ = true;
  rebuild();
}
void CameraStream::disableTimestampOverlay()
{
  if (!overlay_)
    return;
  overlay_ = false;
  rebuild();
}

void CameraStream::enableMotionFrameSaving(const std::string &outPath)
{
  // if (motion_frame_ && motionFile_ == outPath)
  if (motion_frame_)
    return;

  motion_frame_ = true;
  // motionFile_ = outPath;
  rebuild();
}
void CameraStream::disableMotionFrameSaving()
{
  if (!motion_frame_)
    return;
  motion_frame_ = false;
  motionFile_.clear();
  rebuild();
}

void CameraStream::rebuild()
{
  bool was_running = running_;
  stop();
  if (was_running)
    start();
}

std::string CameraStream::getMountPoint() const { return mount_point_; }

std::string CameraStream::buildPipelineWithoutAudio() const
{
  const int rtspsrcLatency = 150;

  std::cout << "[CameraStream] Build pipeline without audio." << std::endl;

  std::string p;

  // 1) splitmuxsink only when segmenting
  if (segment_)
  {
    p += "splitmuxsink name=smux muxer-factory=matroskamux "
         "location=" +
         segment_path +
         " "
         "max-size-time=10000000000 max-files=3 async-finalize=true ";
  }

  // 2) RTSP source
  p += "rtspsrc location=" + uri_ +
       " protocols=tcp latency=" + std::to_string(rtspsrcLatency) +
       " ntp-sync=true name=src ";

  // 3) VIDEO (force the video pad): depay -> parse -> tee
  p += "src. ! application/x-rtp,media=video,encoding-name=H264 "
       "! queue ! rtph264depay "
       "! h264parse config-interval=1 ! tee name=vt ";

  // 3a) Optional motion frames (decoded)
  if (motion_frame_)
  {
    p += "vt. ! queue ! avdec_h264 ! videoconvert ! videoscale "
         "! video/x-raw,format=BGR "
         "! appsink name=motion_sink emit-signals=false max-buffers=1 "
         "drop=true sync=false ";
  }

  // 3b) Encoded video to mux (only if segmenting)
  if (segment_)
  {
    p += "vt. ! queue ! video/x-h264,stream-format=avc,alignment=au "
         "! smux.video ";
  }

  return p;
}

std::string CameraStream::buildPipelineWithAudio() const
{
  const int rtspsrcLatency = 150;

  std::cout << "[CameraStream] Build pipeline with audio." << std::endl;

  std::string p;

  // 1) Declare splitmuxsink up front ONLY if segmenting is enabled
  if (segment_)
  {
    p += "splitmuxsink name=smux muxer-factory=matroskamux "
         "location=" +
         segment_path +
         " " // e.g. /.../segment-%05d.mkv
         "max-size-time=10000000000 max-files=3 async-finalize=true ";
  }

  // 2) RTSP source
  p += "rtspsrc location=" + uri_ +
       " protocols=tcp latency=" + std::to_string(rtspsrcLatency) +
       " ntp-sync=true name=src ";

  // 3) VIDEO: depay -> parse -> tee
  p += "src. ! queue ! rtph264depay "
       "! h264parse config-interval=1 ! tee name=vt ";

  // 3a) Motion frames (decoded) — optional
  if (motion_frame_)
  {
    p += "vt. ! queue ! avdec_h264 ! videoconvert ! videoscale "
         "! video/x-raw,format=BGR "
         "! appsink name=motion_sink emit-signals=false max-buffers=1 "
         "drop=true sync=false ";
  }

  // 3b) Encoded video to mux (only if segmenting)
  if (segment_)
  {
    p += "vt. ! queue ! video/x-h264,stream-format=avc,alignment=au "
         "! smux.video ";
  }

  // 4) AUDIO: depay -> parse -> caps -> mux (only if segmenting)
  if (segment_)
  {
    p += "src. ! queue ! rtpmp4gdepay ! aacparse "
         "! audio/mpeg,mpegversion=4,stream-format=raw,rate=48000,channels=2 "
         "! queue ! smux.audio_0 ";
  }

  return p;
}

void CameraStream::startMotionLoop()
{
  motion_running_ = true;

  std::cout << "Initiate motion-loop, scale: " << motion_frame_scale_
            << ", segment: " << segment_ << std::endl;

  motion_thread_ = std::thread([this]
                               {
    // Some vars outside actual loop
    cv::Mat prevGray;
    int motionHitCount = 0;
    std::chrono::seconds motion_hold_duration(motion_hold_duration_);

    while (motion_running_) {
      GstSample *sample = nullptr;
      bool segment_enabled = segment_.load();  // Copy once per iteration

      if (motion_sink_)
        sample = gst_app_sink_pull_sample(GST_APP_SINK(motion_sink_));

      if (!sample) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        continue;
      }

      GstBuffer *buffer = gst_sample_get_buffer(sample);
      GstCaps *caps = gst_sample_get_caps(sample);

      GstMapInfo map;
      if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        gst_sample_unref(sample);
        continue;
      }

      // Extract frame dimensions and format from caps
      int width = 0, height = 0;
      const GstStructure *caps_struct = gst_caps_get_structure(caps, 0);
      gst_structure_get_int(caps_struct, "width", &width);
      gst_structure_get_int(caps_struct, "height", &height);
      const gchar *format = gst_structure_get_string(caps_struct, "format");

      cv::Mat mat;
      if (format) {
        if (strcmp(format, "BGR") == 0) {
          mat = cv::Mat(height, width, CV_8UC3, (char *)map.data,
                        cv::Mat::AUTO_STEP);
        } else if (strcmp(format, "RGB") == 0) {
          mat = cv::Mat(height, width, CV_8UC3, (char *)map.data,
                        cv::Mat::AUTO_STEP);
          cv::cvtColor(mat, mat, cv::COLOR_RGB2BGR);
        } else if (strcmp(format, "I420") == 0) {
          // I420 = YUV420p, width x height, 1.5 bytes per pixel
          cv::Mat yuv(height + height / 2, width, CV_8UC1, (char *)map.data);
          cv::Mat bgr;
          cv::cvtColor(yuv, bgr, cv::COLOR_YUV2BGR_I420);
          mat = bgr;
        } else {
          // Handle other formats as needed
          std::cerr << "Unsupported pixel format for motion detection: "
                    << format << std::endl;
          gst_buffer_unmap(buffer, &map);
          gst_sample_unref(sample);
          continue;
        }

        // At this point, 'mat' is a valid OpenCV BGR image!
        // === RUN MOTION DETECTION HERE ===
        // For demo: save as JPEG (comment this out in real loop!)
        // cv::imwrite("/tmp/motion.jpg", mat);

        // Optionally: store the last frame (as a member variable)
        // last_motion_frame_ = mat.clone();

        // --- MOTION ANALYSIS LOGIC ---
        cv::Mat resized = mat;
        // Step 1: Resize to explicit dimensions if set
        if (motion_frame_size_.width > 0 && motion_frame_size_.height > 0) {
          cv::resize(mat, resized, motion_frame_size_, 0, 0, cv::INTER_LINEAR);
        }

        // Step 2: Apply scale if < 1.0 (or any value != 1.0)
        if (motion_frame_scale_ > 0.0f && motion_frame_scale_ != 1.0f) {
          cv::Mat scaled;
          cv::resize(resized, scaled, cv::Size(), motion_frame_scale_,
                     motion_frame_scale_, cv::INTER_LINEAR);
          resized = scaled; // overwrite
        }

        cv::Mat gray;
        cv::cvtColor(resized, gray, cv::COLOR_BGR2GRAY);

        // Only analyze motion if previous gray exists (skip on very first
        // frame)
        if (!prevGray.empty()) {
          std::vector<cv::Point2f> prevPts, nextPts;
          // Find good features to track in previous gray frame
          cv::goodFeaturesToTrack(prevGray, prevPts, 100, 0.01, 10);

          if (!prevPts.empty()) {
            std::vector<uchar> status;
            std::vector<float> err;
            // Calculate optical flow between previous and current gray frames
            cv::calcOpticalFlowPyrLK(prevGray, gray, prevPts, nextPts, status,
                                     err);

            float totalMotion = 0;
            int validCount = 0;
            cv::Mat vis = resized.clone();
            
            // Draw motion regions on visualization
            for (const auto& region : motion_regions_) {
              if (region.angle == 0.0f) {
                // Draw regular rectangle for non-rotated regions
                cv::rectangle(vis, region.rect, cv::Scalar(255, 0, 0), 2);
              } else {
                // Draw rotated rectangle
                cv::RotatedRect rotRect = region.getRotatedRect();
                cv::Point2f vertices[4];
                rotRect.points(vertices);
                for (int i = 0; i < 4; i++) {
                  cv::line(vis, vertices[i], vertices[(i + 1) % 4], cv::Scalar(255, 0, 0), 2);
                }
              }
              cv::putText(vis, "Region " + std::to_string(region.id), 
                         cv::Point(region.rect.x, region.rect.y - 10),
                         cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(255, 0, 0), 1);
            }

            for (size_t i = 0; i < prevPts.size(); ++i) {
              if (status[i]) {
                // Check if point is within any motion region (if regions are defined)
                bool pointInRegion = motion_regions_.empty(); // If no regions, analyze entire frame
                
                if (!motion_regions_.empty()) {
                  for (const auto& region : motion_regions_) {
                    if (region.angle == 0.0f) {
                      // Use simple rectangle containment for non-rotated regions
                      if (region.rect.contains(cv::Point2i(prevPts[i]))) {
                        pointInRegion = true;
                        break;
                      }
                    } else {
                      // Use rotated rectangle containment
                      cv::RotatedRect rotRect = region.getRotatedRect();
                      std::vector<cv::Point2f> vertices(4);
                      rotRect.points(vertices.data());
                      
                      // Check if point is inside the rotated rectangle using pointPolygonTest
                      if (cv::pointPolygonTest(vertices, prevPts[i], false) >= 0) {
                        pointInRegion = true;
                        break;
                      }
                    }
                  }
                }
                
                if (pointInRegion) {
                  float dist = cv::norm(nextPts[i] - prevPts[i]);
                  if (dist > noise_threshold_) // Filter out some irrelevent dists
                                               // (noise).
                  {
                    totalMotion += dist;
                    validCount++;

                    // Draw arrowed lines to show direction of motion
                    cv::Point2f dir = nextPts[i] - prevPts[i];
                    cv::Point2f scaledEnd =
                        prevPts[i] + 5.0 * dir; // scale arrow for visibility
                    cv::arrowedLine(vis, prevPts[i], scaledEnd,
                                    cv::Scalar(0, 255, 0), 2);
                  }
                }
              }
            }

            float avgMotion = 0;

            // Calculate average motion score
            if (validCount > 0) {
              avgMotion = totalMotion / validCount;
            }

            // Always draw motion value on visualization
            std::ostringstream oss;
            oss << "Motion: " << avgMotion;
            cv::putText(vis, oss.str(), cv::Point(10, 30),
                        cv::FONT_HERSHEY_SIMPLEX, 1.0, cv::Scalar(0, 0, 255),
                        2);
            
            // Always update the motion frame to show current state
            last_motion_frame_ = vis.clone();

            // Check if motion exceeds threshold for detection logic
            if (avgMotion > motion_threshold_) {
              ++motionHitCount;
              if (motionHitCount >= motion_min_hits_) {
                std::cout << "[Motion] avg displacement: " << avgMotion
                          << std::endl;
                lastMotionTime_ = std::chrono::steady_clock::now();
              }
            } else {
              // Decay hit count gently, don't zero immediately
              if (motionHitCount > 0)
                motionHitCount -= motion_decay_;
            }

            motionDetected_ =
                std::chrono::steady_clock::now() - lastMotionTime_ <=
                motion_hold_duration;

            if (motionDetected_ != prevMotionDetected_)
              std::cout << (motionDetected_ ? "[Motion] started."
                                            : "[Motion] stopped.");

            // This applies only if motion-record = on
            if (segment_enabled) {

              if (motionDetected_)
                segmentWorker_->SaveCurrentSegment();

              bool motionTransition = (!motionDetected_ && prevMotionDetected_);
              if (motionTransition)
                segmentWorker_->setState(
                    SegmentWorker::WorkerState::
                        FinishRequested); // Finish current segment for us

              // We asked segmentworker to finish in previous tick, now theres
              // new motion
              if (segmentWorker_->getState() ==
                      SegmentWorker::WorkerState::FinishRequested &&
                  motionDetected_) {
                std::cout << "[Motion] Segmentworker asked to finalize, but "
                             "there was new motion!"
                          << std::endl;
                segmentWorker_->setState(
                    SegmentWorker::WorkerState::Working); // Back to work! Not
                // time to finialize
                // video yet
              }

              if (segmentWorker_->getState() ==
                  SegmentWorker::WorkerState::Finalized) // Or if we have to
                                                         // many
              // segments
              // Export final output file !
              {
                std::cout << "[Motion] Time to finish video" << std::endl;

                auto segments = segmentWorker_->getAndResetMotionSegments();

                if (!segments.empty()) {
                  std::string outputFilename = getTimestampedFilename(); // e.g.
                  // motion-2025-07-29_21-15-43.mp4
                  exportInBackground(segments, output_path_, outputFilename);
                } else
                  std::cout << "[Motion] No segments!!" << std::endl;

                segmentWorker_->setState(SegmentWorker::WorkerState::Working);
              }
            }
          }
          prevMotionDetected_ = motionDetected_;
        }
        prevGray = gray.clone(); // Save for next loop
      }

      gst_buffer_unmap(buffer, &map);
      gst_sample_unref(sample);
    }
    std::cout << "Left motion-loop" << std::endl; });
}

void CameraStream::exportInBackground(
    const std::vector<std::filesystem::path> &segments,
    const std::filesystem::path &outputFolder,
    const std::string &outputFilename)
{
  std::thread([=]()
              {
    bool ok =
        VideoExporter::exportSegments(segments, outputFolder, outputFilename);
    if (ok) {
      std::cout << "[MotionLoop] Export completed: " << outputFilename
                << std::endl;
    } else {
      std::cerr << "[MotionLoop] Export failed for " << outputFilename
                << std::endl;
    } })
      .detach(); // No join, just fire-and-forget
}

std::string CameraStream::getTimestampedFilename(const std::string &prefix,
                                                 const std::string &ext)
{
  auto now = std::chrono::system_clock::now();
  std::time_t now_c = std::chrono::system_clock::to_time_t(now);

  std::tm tm;
  localtime_r(&now_c, &tm); // Use localtime_s on Windows

  std::ostringstream oss;
  oss << prefix << std::put_time(&tm, "%Y-%m-%d_%H-%M-%S") << ext;

  return oss.str(); // e.g., motion-2025-07-29_21-15-43.mp4
}
struct Ctx
{
  std::mutex *m;
  std::condition_variable *cv;
  std::atomic<bool> *done;
  AudioProbeResult *out;
};

static void on_pad_added(GstElement * /*src*/, GstPad *pad,
                         gpointer user_data)
{
  auto *ctx = static_cast<Ctx *>(user_data);

  // Prefer current caps; fall back to query
  GstCaps *caps = gst_pad_get_current_caps(pad);
  if (!caps)
    caps = gst_pad_query_caps(pad, nullptr);
  if (!caps)
    return;

  const GstStructure *s = gst_caps_get_structure(caps, 0);
  const char *media = gst_structure_get_string(s, "media");
  if (media && g_str_equal(media, "audio"))
  {
    ctx->out->has_audio = true;
    if (const char *enc = gst_structure_get_string(s, "encoding-name"))
      ctx->out->encoding = enc;

    int rate = 0, channels = 0;
    gst_structure_get_int(s, "clock-rate", &rate); // RTP caps field
    gst_structure_get_int(s, "channels", &channels);
    if (rate)
      ctx->out->rate = rate;
    if (channels)
      ctx->out->channels = channels;

    {
      std::lock_guard<std::mutex> lk(*ctx->m);
      ctx->done->store(true);
    }
    ctx->cv->notify_one();
  }
  gst_caps_unref(caps);
}

static void on_no_more_pads(GstElement * /*src*/, gpointer user_data)
{
  auto *ctx = static_cast<Ctx *>(user_data);
  {
    std::lock_guard<std::mutex> lk(*ctx->m);
    ctx->done->store(true);
  }
  ctx->cv->notify_one();
}

bool CameraStream::ProbeRtspAudio(const std::string &uri, AudioProbeResult &out,
                                  int timeout_ms)
{
  // Pipeline: just rtspsrc; we don't even link pads.
  GstElement *pipeline = gst_pipeline_new("probe-pipe");
  GstElement *src = gst_element_factory_make("rtspsrc", "probe-src");
  if (!pipeline || !src)
  {
    if (pipeline)
      gst_object_unref(pipeline);
    return false;
  }

  // Force TCP so results match your normal pipeline
  // NOTE: 4 == GST_RTSP_LOWER_TRANS_TCP (avoids extra headers/libs)
  g_object_set(src, "location", uri.c_str(), "protocols", 4, NULL);

  gst_bin_add(GST_BIN(pipeline), src);

  // Simple sync between GLib callback thread and this thread
  std::mutex m;
  std::condition_variable cv;
  std::atomic<bool> done{false};

  Ctx ctx{&m, &cv, &done, &out};

  // When pads appear, check caps for media=audio and encoding-name
  gulong h_pad =
      g_signal_connect(src, "pad-added", G_CALLBACK(on_pad_added), &ctx);
  gulong h_nmp =
      g_signal_connect(src, "no-more-pads", G_CALLBACK(on_no_more_pads), &ctx);

  // Go to PAUSED so rtspsrc DESCRIBEs and exposes pads
  gst_element_set_state(pipeline, GST_STATE_PAUSED);

  // Wait (bounded) until we know the answer
  {
    std::unique_lock<std::mutex> lk(m);
    cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                [&]
                { return done.load(); });
  }

  g_signal_handler_disconnect(src, h_pad);
  g_signal_handler_disconnect(src, h_nmp);

  // Cleanup
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(pipeline);

  // If we timed out, we still return whatever we found (likely no audio)
  return true;
}

int CameraStream::addMotionRegion(const cv::Rect &rect, float angle)
{
  int id = next_region_id_++;
  motion_regions_.emplace_back(id, rect, angle);
  std::cout << "[MotionRegion] Added region " << id << " at (" << rect.x << "," << rect.y
            << ") size " << rect.width << "x" << rect.height << " angle " << angle << "°" << std::endl;
  return id;
}

bool CameraStream::removeMotionRegion(int id)
{
  auto it = std::find_if(motion_regions_.begin(), motion_regions_.end(),
                         [id](const MotionRegion &region)
                         { return region.id == id; });

  if (it != motion_regions_.end())
  {
    std::cout << "[MotionRegion] Removed region " << id << std::endl;
    motion_regions_.erase(it);
    return true;
  }

  std::cout << "[MotionRegion] Region " << id << " not found for removal" << std::endl;
  return false;
}

void CameraStream::clearMotionRegions()
{
  std::cout << "[MotionRegion] Cleared " << motion_regions_.size() << " regions" << std::endl;
  motion_regions_.clear();
}