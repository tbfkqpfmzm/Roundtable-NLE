// Precompiled header for roundtable_ui
// Covers STL + spdlog + Qt widgets — the most commonly included headers.
#pragma once

// STL
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// spdlog (included in 68/100+ ui TUs)
#include <spdlog/spdlog.h>

// glm
#include <glm/glm.hpp>

// Qt (most commonly used widgets/core)
#include <QApplication>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>
