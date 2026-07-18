#pragma once

#include <Windows.h>

#include <string>

using render_scale_menu_log_fn = void (*)(const std::string &message);

// Starts the Genshin render-scale menu integration on a worker thread.
// Safe to call more than once; only the first call starts the module.
void initialize_render_scale_menu(HMODULE bridge_module, render_scale_menu_log_fn log_callback);
