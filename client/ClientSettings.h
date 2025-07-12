#pragma once
#include <Qt>
// Some client default settings
namespace ClientSettings {

// Log to file in addition to stdout?
inline constexpr bool LOG_TO_FILE = true;
inline const char *LOG_FILE_PATH = "richnvr.log";
inline constexpr bool LOG_OVERWRITE_ON_START = true;

// Video player settings
constexpr int VIDEOPLAYER_FULLSCREEN_KEY = Qt::Key_F12;
constexpr int VIDEOPLAYER_EXIT_FULLSCREEN_KEY = Qt::Key_Escape;
constexpr int VIDEOPLAYER_START_WIDTH = 800;
constexpr int VIDEOPLAYER_START_HEIGHT = 600;
inline constexpr int SEEK_SKIP_SECONDS = 3;
constexpr bool DASHBOARD_MAXIMIZE = true;
constexpr int DASHBOARD_START_WIDTH = 1280;
constexpr int DASHBOARD_START_HEIGHT = 720;

constexpr bool START_WITH_FILEBROWSER = false;
constexpr bool START_WITH_VIDEOPLAYER = false;

inline constexpr const char *kCamFile = "cameras.json";

#ifdef Q_OS_LINUX
inline const char *VIDEOSINK_PRIMARY = "xvimagesink";
#elif defined(Q_OS_WIN)
inline const char *VIDEOSINK_PRIMARY = "d3d11videosink";
#elif defined(Q_OS_MACOS)
inline const char *VIDEOSINK_PRIMARY = "osxvideosink";
#else
inline const char *VIDEOSINK_PRIMARY = "autovideosink";
#endif

inline const QStringList VIDEOSINK_FALLBACKS = {
    "qt6glsink",
    "glimagesink",
    "autovideosink"
};

constexpr int ffmpegSegmentDuration = 10;
constexpr int nmbrFfmpegSegments = 3;
inline std::string PROGRAM_DIR;
inline std::string ffmpegSegmentPattern;
inline float MOTION_THRESHOLD = 20.0f;
constexpr int MOTION_TIME = 7;

// When drawing motion-vectors
inline constexpr float MOTION_ARROW_SCALE = 2.5f;
inline constexpr int MOTION_ARROW_THICKNESS = 1;
inline const QString VIDEO_OUTPUT_FORMAT = "mp4";

inline constexpr int motionFramePollIntervalMs = 20;

constexpr int MOTION_MIN_HITS = 3;
constexpr int MOTION_DECAY = 1;
constexpr size_t MAX_BUFFER_SEGMENTS = 65;

inline const QString FFMPEG_WIN32_PATH = "C:\\ffmpeg\\bin\\ffmpeg.exe";
inline const QString FFPROBE_WIN32_PATH = "C:\\ffmpeg\\bin\\ffprobe.exe";
} // namespace ClientSettings
