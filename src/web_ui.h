// Wi-Fi access point + single-page web UI + JSON API.
#pragma once

#include <mutex>

#include "engine.h"

// The engine and the mutex that serialises access to it (shared with the
// engine task in main.cpp).
void webBegin(Engine& engine, std::mutex& engineMutex);
void webLoop();
