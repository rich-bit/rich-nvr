#include "AddStreamDialog.h"
#include "PathUtils.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QVBoxLayout>
#include <glib.h>
#include <gst/gst.h>
#include <gst/pbutils/gstdiscoverer.h>
#include <gst/pbutils/pbutils.h> // for discoverer API
#include <iostream>

AddStreamDialog::AddStreamDialog(QWidget *parent) : QDialog(parent) {
  setupUi();
  setWindowTitle("Add Stream");

  connect(checkResButton, &QPushButton::clicked, this,
          &AddStreamDialog::onCheckResolution);
  connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
  connect(saveButton, &QPushButton::clicked, this,
          &AddStreamDialog::onSaveClicked);

  connect(sizeRadio, &QRadioButton::toggled, this,
          &AddStreamDialog::onSizeOrScaleToggled);
  connect(scaleRadio, &QRadioButton::toggled, this,
          &AddStreamDialog::onSizeOrScaleToggled);

  onSizeOrScaleToggled(); // set initial enabled states
}

void AddStreamDialog::setupUi() {
  auto layout = new QVBoxLayout(this);

  // --- Camera ID ---
  layout->addWidget(new QLabel("Camera ID:"));
  idEdit = new QLineEdit(this);
  idEdit->setPlaceholderText("e.g. frontdoor");
  layout->addWidget(idEdit);

  // --- URL ---
  layout->addWidget(new QLabel("Stream URL:"));
  urlEdit = new QLineEdit(this);
  layout->addWidget(urlEdit);

  // Remote server (host[:port] or full http://host:port)
  layout->addWidget(new QLabel("Remote server:"));
  serverEdit = new QLineEdit(this);
  serverEdit->setPlaceholderText(
      "e.g. 192.168.1.50:8080 or http://nvr.local:8080");
  serverEdit->setText("127.0.0.1:8080"); // default, tweak if you like
  layout->addWidget(serverEdit);

  // --- Flags row ---
  auto flagsRow = new QHBoxLayout();
  segmentCheck = new QCheckBox("Segment (record)", this);
  motionFrameCheck = new QCheckBox("Motion Frame", this);
  liveProxiedCheck = new QCheckBox("Live555 proxied", this);
  liveProxiedCheck->setChecked(true);
  flagsRow->addWidget(segmentCheck);
  flagsRow->addWidget(motionFrameCheck);
  flagsRow->addWidget(liveProxiedCheck);
  flagsRow->addStretch();
  layout->addLayout(flagsRow);

  // --- Resolution check ---
  checkResButton = new QPushButton("Check Resolution", this);
  resLabel = new QLabel("Resolution: Not checked", this);
  layout->addWidget(checkResButton);
  layout->addWidget(resLabel);

  layout->addWidget(new QLabel("Video dashboard size:"));
  dashboardSizeCombo = new QComboBox(this);
  dashboardSizeCombo->addItem("1 (100%)", 1.0);
  dashboardSizeCombo->addItem("3/4 (75%)", 0.75);
  dashboardSizeCombo->addItem("1/2 (50%)", 0.50);
  dashboardSizeCombo->addItem("1/4 (25%)", 0.25);
  dashboardSizeCombo->addItem("1/8 (12.5%)", 0.125);
  dashboardSizeCombo->setCurrentIndex(2);
  layout->addWidget(dashboardSizeCombo);

  // --- Size OR Scale selection ---
  layout->addWidget(new QLabel("Motion Frame Size OR Scale:"));
  sizeRadio = new QRadioButton("Use size (W x H)", this);
  scaleRadio = new QRadioButton("Use scale", this);
  // sizeRadio->setChecked(true);
  scaleRadio->setChecked(true);

  auto choiceRow = new QHBoxLayout();
  choiceRow->addWidget(sizeRadio);
  choiceRow->addWidget(scaleRadio);
  choiceRow->addStretch();
  layout->addLayout(choiceRow);

  // Size editors
  auto sizeRow = new QHBoxLayout();
  widthSpin = new QSpinBox(this);
  heightSpin = new QSpinBox(this);
  widthSpin->setRange(0, 8192);
  heightSpin->setRange(0, 8192);
  widthSpin->setSuffix(" px");
  heightSpin->setSuffix(" px");
  widthSpin->setValue(0);
  heightSpin->setValue(0);
  sizeRow->addWidget(new QLabel("W:"));
  sizeRow->addWidget(widthSpin);
  sizeRow->addSpacing(12);
  sizeRow->addWidget(new QLabel("H:"));
  sizeRow->addWidget(heightSpin);
  sizeRow->addStretch();
  layout->addLayout(sizeRow);

  // Scale editor
  auto scaleRow = new QHBoxLayout();
  scaleSpin = new QDoubleSpinBox(this);
  scaleSpin->setDecimals(3);
  scaleSpin->setSingleStep(0.05);
  scaleSpin->setRange(0.05, 8.0);
  scaleSpin->setValue(0.70); // default if using scale
  scaleRow->addWidget(new QLabel("Scale:"));
  scaleRow->addWidget(scaleSpin);
  scaleRow->addStretch();
  layout->addLayout(scaleRow);

  initiateButton = new QPushButton("Initiate", this);
  layout->addWidget(initiateButton);

  // --- Analysis params (with your defaults) ---
  layout->addWidget(new QLabel("Motion/Analysis Settings:"));

  auto grid = new QGridLayout();
  int r = 0;

  noiseSpin = new QDoubleSpinBox(this);
  noiseSpin->setDecimals(3);
  noiseSpin->setRange(0.0, 1000.0);
  noiseSpin->setSingleStep(0.1);
  noiseSpin->setValue(1.0);

  motionThreshSpin = new QDoubleSpinBox(this);
  motionThreshSpin->setDecimals(3);
  motionThreshSpin->setRange(0.0, 10000.0);
  motionThreshSpin->setSingleStep(0.5);
  motionThreshSpin->setValue(10.0);

  minHitsSpin = new QSpinBox(this);
  minHitsSpin->setRange(0, 1000);
  minHitsSpin->setValue(3);

  decaySpin = new QSpinBox(this);
  decaySpin->setRange(0, 1000);
  decaySpin->setValue(1);

  arrowScaleSpin = new QDoubleSpinBox(this);
  arrowScaleSpin->setDecimals(2);
  arrowScaleSpin->setRange(0.0, 100.0);
  arrowScaleSpin->setSingleStep(0.1);
  arrowScaleSpin->setValue(2.5);

  arrowThickSpin = new QDoubleSpinBox(this);
  arrowThickSpin->setDecimals(2);
  arrowThickSpin->setRange(0.0, 50.0);
  arrowThickSpin->setSingleStep(0.1);
  arrowThickSpin->setValue(1.0);

  grid->addWidget(new QLabel("Noise threshold:"), r, 0);
  grid->addWidget(noiseSpin, r, 1);
  r++;
  grid->addWidget(new QLabel("Motion threshold:"), r, 0);
  grid->addWidget(motionThreshSpin, r, 1);
  r++;
  grid->addWidget(new QLabel("Motion min hits:"), r, 0);
  grid->addWidget(minHitsSpin, r, 1);
  r++;
  grid->addWidget(new QLabel("Motion decay:"), r, 0);
  grid->addWidget(decaySpin, r, 1);
  r++;
  grid->addWidget(new QLabel("Arrow scale:"), r, 0);
  grid->addWidget(arrowScaleSpin, r, 1);
  r++;
  grid->addWidget(new QLabel("Arrow thickness:"), r, 0);
  grid->addWidget(arrowThickSpin, r, 1);
  r++;

  layout->addLayout(grid);

  // Buttons
  auto btns = new QHBoxLayout();
  cancelButton = new QPushButton("Cancel", this);
  saveButton = new QPushButton("Save", this);
  btns->addStretch();
  btns->addWidget(cancelButton);
  btns->addWidget(saveButton);
  layout->addLayout(btns);
}

// -------- getters ----------
QString AddStreamDialog::cameraId() const { return idEdit->text(); }
QString AddStreamDialog::streamUrl() const { return urlEdit->text(); }

bool AddStreamDialog::segment() const { return segmentCheck->isChecked(); }
bool AddStreamDialog::motionFrame() const {
  return motionFrameCheck->isChecked();
}
bool AddStreamDialog::liveProxied() const {
  return liveProxiedCheck->isChecked();
}

bool AddStreamDialog::useMotionFrameSize() const {
  return sizeRadio->isChecked();
}

QSize AddStreamDialog::motionFrameSize() const {
  return QSize(widthSpin->value(), heightSpin->value());
}

float AddStreamDialog::motionFrameScale() const {
  return static_cast<float>(scaleSpin->value());
}

float AddStreamDialog::noiseThreshold() const {
  return static_cast<float>(noiseSpin->value());
}
float AddStreamDialog::motionThreshold() const {
  return static_cast<float>(motionThreshSpin->value());
}
int AddStreamDialog::motionMinHits() const { return minHitsSpin->value(); }
int AddStreamDialog::motionDecay() const { return decaySpin->value(); }
float AddStreamDialog::motionArrowScale() const {
  return static_cast<float>(arrowScaleSpin->value());
}
float AddStreamDialog::motionArrowThickness() const {
  return static_cast<float>(arrowThickSpin->value());
}
float AddStreamDialog::dashboardDisplayScale() const {
  return static_cast<float>(dashboardSizeCombo->currentData().toDouble());
}
QString AddStreamDialog::targetServer() const {
  return serverEdit->text().trimmed();
}
// -------- validation & actions ----------
void AddStreamDialog::onSizeOrScaleToggled() {
  const bool useSize = sizeRadio->isChecked();
  widthSpin->setEnabled(useSize);
  heightSpin->setEnabled(useSize);
  scaleSpin->setEnabled(!useSize);
}

void AddStreamDialog::onSaveClicked() {
  const QString url = urlEdit->text().trimmed();
  const QString camId = idEdit->text().trimmed();

  if (camId.isEmpty()) {
    QMessageBox::warning(this, "Missing Camera ID",
                         "Please enter a unique Camera ID.");
    return;
  }
  QRegularExpression re("^[A-Za-z0-9._-]+$");
  if (!re.match(camId).hasMatch()) {
    QMessageBox::warning(
        this, "Invalid ID",
        "Camera ID may only contain letters, digits, '.', '_' or '-'.");
    return;
  }

  // Basic sanity for size/scale
  if (useMotionFrameSize()) {
    if (widthSpin->value() <= 0 || heightSpin->value() <= 0) {
      QMessageBox::warning(this, "Invalid size",
                           "Please provide a positive width and height, or "
                           "choose Scale instead.");
      return;
    }
  } else {
    if (scaleSpin->value() <= 0.0) {
      QMessageBox::warning(this, "Invalid scale",
                           "Scale must be greater than zero.");
      return;
    }
  }

  if (!isValidStream(url)) {
    QMessageBox::warning(this, "Invalid stream",
                         "Could not open that URI.\nPlease check the address.");
    return;
  }

  accept();
}
static bool probe_rtsp_tcp_simple(const QString &uri, int timeoutMs) {
  GstElement *pipeline = gst_pipeline_new("rtsp-probe");
  GstElement *src = gst_element_factory_make("rtspsrc", "src");
  GstElement *sink = gst_element_factory_make("fakesink", "sink");
  if (!pipeline || !src || !sink) {
    if (pipeline)
      gst_object_unref(pipeline);
    std::cerr << "[RTSP-Probe] element create failed\n";
    return false;
  }

  g_object_set(sink, "sync", FALSE, NULL);
  g_object_set(src, "location", uri.toUtf8().constData(), "latency", 300, NULL);
  // Use integer value instead of enum to avoid Docker container issues
  g_object_set(src, "protocols", 4, NULL);  // 4 = GST_RTSP_LOWER_TRANS_TCP

  gst_bin_add_many(GST_BIN(pipeline), src, sink, NULL);

  // rtspsrc has dynamic pads; link them to fakesink on the fly
  g_signal_connect(
      src, "pad-added",
      G_CALLBACK(+[](GstElement *, GstPad *newpad, gpointer user_data) {
        GstElement *fsink = GST_ELEMENT(user_data);
        GstPad *sinkpad = gst_element_get_static_pad(fsink, "sink");
        if (!sinkpad)
          return;
        if (!gst_pad_is_linked(sinkpad))
          gst_pad_link(newpad, sinkpad);
        gst_object_unref(sinkpad);
      }),
      sink);

  GstBus *bus = gst_element_get_bus(pipeline);
  gst_element_set_state(pipeline, GST_STATE_PLAYING);

  // Wait for success (ASYNC_DONE / PAUSED/PLAYING) or fail (ERROR/EOS) or
  // timeout
  const guint64 deadline =
      (guint64)std::max(timeoutMs, 8000) * 1000000ULL; // ns
  guint64 waited = 0, slice = 50 * 1000000ULL;
  bool ok = false;

  while (waited < deadline) {
    GstMessage *msg = gst_bus_timed_pop_filtered(
        bus, slice,
        (GstMessageType)(GST_MESSAGE_ERROR | GST_MESSAGE_EOS |
                         GST_MESSAGE_ASYNC_DONE | GST_MESSAGE_STATE_CHANGED));
    if (!msg) {
      waited += slice;
      continue;
    }

    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
      GError *e = nullptr;
      gchar *d = nullptr;
      gst_message_parse_error(msg, &e, &d);
      std::cerr << "[RTSP-Probe] ERROR: " << (e ? e->message : "?")
                << " dbg=" << (d ? d : "") << "\n";
      if (e)
        g_error_free(e);
      if (d)
        g_free(d);
      gst_message_unref(msg);
      goto done;
    }
    case GST_MESSAGE_EOS:
      gst_message_unref(msg);
      goto done;

    case GST_MESSAGE_ASYNC_DONE:
      ok = true;
      gst_message_unref(msg);
      goto done;

    case GST_MESSAGE_STATE_CHANGED: {
      if (GST_MESSAGE_SRC(msg) == GST_OBJECT(pipeline)) {
        GstState o, n, p;
        gst_message_parse_state_changed(msg, &o, &n, &p);
        if (n == GST_STATE_PAUSED || n == GST_STATE_PLAYING) {
          ok = true;
          gst_message_unref(msg);
          goto done;
        }
      }
      break;
    }
    default:
      break;
    }
    gst_message_unref(msg);
  }

done:
  gst_element_set_state(pipeline, GST_STATE_NULL);
  if (bus)
    gst_object_unref(bus);
  gst_object_unref(pipeline);
  return ok;
}

bool AddStreamDialog::isValidStream(const QString &uri, int timeoutMs) {
  std::cout << "[Discoverer] validating URI: " << uri.toStdString() << "\n";

  const bool isRtsp = uri.startsWith("rtsp://", Qt::CaseInsensitive);
  if (isRtsp) {
    const bool ok = probe_rtsp_tcp_simple(uri, timeoutMs);
    std::cout << "[Validate] RTSP probe (TCP) => "
              << (ok ? "VALID\n" : "INVALID\n");
    return ok;
  }

  // Non-RTSP → keep your discoverer path, but give it a sensible timeout +
  // newline logs
  GError *err = nullptr;
  const GstClockTime timeout =
      (GstClockTime)std::max(timeoutMs, 5000) * GST_MSECOND;
  GstDiscoverer *disc = gst_discoverer_new(timeout, &err);
  if (!disc) {
    std::cerr << "[Discoverer] new failed: " << (err ? err->message : "unknown")
              << "\n";
    if (err)
      g_error_free(err);
    return false;
  }
  GstDiscovererInfo *info =
      gst_discoverer_discover_uri(disc, uri.toUtf8().constData(), &err);
  if (!info) {
    std::cerr << "[Discoverer] discover_uri null: "
              << (err ? err->message : "unknown") << "\n";
    if (err)
      g_error_free(err);
    g_object_unref(disc);
    return false;
  }

  // (optional) keep your resolution-detect block here for files/http
  const auto res = gst_discoverer_info_get_result(info);
  const bool valid = (res == GST_DISCOVERER_OK);
  std::cout << "[Discoverer] Result: " << (valid ? "OK" : "NOT OK") << "\n";

  gst_discoverer_info_unref(info);
  g_object_unref(disc);
  return valid;
}

void AddStreamDialog::onCheckResolution() {
  const QString uri = urlEdit->text().trimmed();
  std::cout << "[Discoverer] Attempting to check resolution for URI: " << uri.toStdString() << std::endl;
  
  if (uri.isEmpty()) {
    resLabel->setText("Resolution: Enter URL first");
    std::cout << "[Discoverer] No URI provided" << std::endl;
    return;
  }
  
  // WSL bypass: Skip GStreamer discovery as it's unreliable in WSL
  if (core::PathUtils::isWSLEnvironment()) {
    std::cout << "[Discoverer] WSL detected - skipping GStreamer discovery (unreliable in WSL)" << std::endl;
    resLabel->setText("Resolution: WSL detected. Set manually below (default: 1920x1080)");
    
    // Set common default resolution for convenience
    videoW = 1920;
    videoH = 1080;
    widthSpin->setValue(videoW);
    heightSpin->setValue(videoH);
    sizeRadio->setChecked(true);
    
    std::cout << "[Discoverer] WSL: Set default 1920x1080. User can adjust as needed." << std::endl;
    return;
  }
  
  GError *err = nullptr;
  GstDiscoverer *disc = gst_discoverer_new(5 * GST_SECOND, &err);
  if (!disc) {
    QString errMsg = err ? QString::fromUtf8(err->message) : "unknown error";
    std::cout << "[Discoverer] Failed to create discoverer: " << errMsg.toStdString() << std::endl;
    resLabel->setText("Resolution: Discoverer creation failed");
    if (err)
      g_error_free(err);
    return;
  }

  std::cout << "[Discoverer] Created discoverer, attempting discovery..." << std::endl;
  GstDiscovererInfo *info =
      gst_discoverer_discover_uri(disc, uri.toUtf8().constData(), &err);
  if (!info) {
    QString errMsg = err ? QString::fromUtf8(err->message) : "unknown error";
    std::cout << "[Discoverer] discover_uri failed: " << errMsg.toStdString() << std::endl;
    resLabel->setText(QString("Resolution: Discovery failed (%1)").arg(errMsg));
    if (err)
      g_error_free(err);
    g_object_unref(disc);
    return;
  }

  std::cout << "[Discoverer] Discovery successful, checking for video streams..." << std::endl;
  
  const GList *streams = gst_discoverer_info_get_video_streams(info);
  if (streams) {
    std::cout << "[Discoverer] Found video stream, extracting resolution..." << std::endl;
    const auto *vid =
        static_cast<const GstDiscovererVideoInfo *>(streams->data);
    videoW = gst_discoverer_video_info_get_width(vid);
    videoH = gst_discoverer_video_info_get_height(vid);
    resLabel->setText(QString("Resolution: %1x%2").arg(videoW).arg(videoH));
    widthSpin->setValue(videoW);
    heightSpin->setValue(videoH);
    sizeRadio->setChecked(true); // prefer exact size after detection
    std::cout << "[Discoverer] Successfully detected resolution: " << videoW << "x" << videoH << std::endl;
  } else {
    std::cout << "[Discoverer] No video streams found in discovery info" << std::endl;
    resLabel->setText("Resolution: No video stream found");
  }

  if (info)
    gst_discoverer_info_unref(info);
  g_object_unref(disc);
  if (err)
    g_error_free(err);
}
