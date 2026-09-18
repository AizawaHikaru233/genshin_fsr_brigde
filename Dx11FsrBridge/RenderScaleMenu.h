#pragma once

#include <Windows.h>

#include <string>

using render_scale_menu_log_fn = void (*)(const std::string &message);

// Starts the Genshin render-scale menu integration on a worker thread.
// Safe to call more than once; only the first call starts the module.
// enabled=false（ini [Dx11FsrBridge] RenderScaleMenu=0）时不启动任何工作线程，
// 游戏保持原生渲染精度档位与行为。
void initialize_render_scale_menu(HMODULE bridge_module, render_scale_menu_log_fn log_callback, bool enabled);
