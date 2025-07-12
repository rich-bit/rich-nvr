#pragma once
#include "Dashboard.h"
#include <iostream>
#include <nlohmann/json.hpp>
#include "conversations.h"

inline bool loadCamerasFromServer(const QByteArray &body, Dashboard &dash) {
  using json = nlohmann::json;

  if (body.isEmpty()) {
    std::cout << "[GUI] cameras.json body empty";
    return false;
  }

  try {
    json cams = json::parse(body.constData());

    if (cams.empty()) {
      std::cout << "[GUI] Server returned empty camera list.";
      return true; // nothing to add – not an error
    }

    for (const auto &cam : cams) {
      QString id = QString::fromStdString(cam.value("id", ""));
      QString url = QString::fromStdString(cam.value("url", ""));
      int w = cam.value("width", 640);
      int h = cam.value("height", 360);
      float s = cam.value("scale", 1.0f);

      if (!id.isEmpty() && !url.isEmpty())
        dash.addCamera(url, id, int(s * w), int(s * h));
    }
  } catch (const std::exception &e) {
    std::cout << "[GUI] Failed to parse cameras.json:"
              << QString::fromUtf8(e.what());
    return false;
  }
  return true;
}
