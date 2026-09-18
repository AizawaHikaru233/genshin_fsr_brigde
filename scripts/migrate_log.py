"""把 Dx11FsrBridge.cpp 的 log_line(...) 调用点机械迁移为显式等级的 LOG_*(分类, ...)。

设计：
  * 只改"以字符串字面量开头"的调用（约 178 处）——这类调用点名（调用点标识）确定，
    分类与等级可以按前缀可靠映射。以变量/表达式开头的调用点保持 log_line（默认 INFO/core）。
  * 通过括号配对定位语句结尾，因此单行、多行拼接两种形态都能处理。
  * 括号不配对的（log_line 与别的结构混在一条语句里）跳过并报告，人工复核。
  * 迁移后目录：由前缀查表得出；等级：错误类前缀 ERROR、节流/每帧类 DEBUG、其余 INFO。

用法：python migrate_log.py <file> [--apply]
不带 --apply 只打印统计（dry-run）。
"""

import re
import sys
from collections import Counter

# 前缀 -> (分类, 等级)
# 等级规则：失败/不支持 = error；每帧/节流细节 = debug；状态/一次性 = info
PREFIX_MAP = [
    # 探针
    ("texture_trace", ("probe", "info")),
    ("texture_create", ("probe", "debug")),
    ("rtv_bind_target", ("probe", "debug")),
    ("final_scene", ("probe", "info")),
    ("similarity", ("probe", "info")),
    ("hdr_output_desc_probe", ("probe", "debug")),
    ("hdr_environment_probe", ("probe", "debug")),
    ("hdr_composite", ("probe", "debug")),
    ("hdr_output_desc_spoof", ("hdr", "info")),
    # HDR / 色彩
    ("hdr_sdr_tone_map", ("hdr", "debug")),
    ("hdr_swapchain", ("hdr", "info")),
    ("hdr_diagnostics", ("hdr", "debug")),
    ("tone_map", ("hdr", "debug")),
    ("dxgi_hdr_force", ("hdr", "debug")),
    # 超分后端 / FSR2
    ("ffx12_dispatch", ("upscale", "debug")),
    ("ffx12_path", ("upscale", "debug")),
    ("ffx12_adapter", ("upscale", "debug")),
    ("ffx12_result", ("upscale", "info")),
    ("ffx12_failed", ("upscale", "error")),
    ("ffx12_token", ("upscale", "debug")),
    ("ffx12_enter", ("upscale", "debug")),
    ("ffx12_jit", ("upscale", "debug")),
    ("ffx12_instance", ("upscale", "info")),
    ("ffx12_native_dump", ("upscale", "debug")),
    ("ffx12_probe", ("upscale", "debug")),
    ("ffx12_mem_dump", ("upscale", "debug")),
    ("ffx12_cb0", ("upscale", "debug")),
    ("ffx12_textures", ("upscale", "debug")),
    ("ffx12_motion", ("upscale", "debug")),
    ("ffx12_depth", ("upscale", "debug")),
    ("ffx12_reactive", ("upscale", "debug")),
    ("ffx12_gpu", ("upscale", "info")),
    ("ffx12_sdk", ("upscale", "info")),
    ("ffx12_version", ("upscale", "info")),
    ("ffx12_", ("upscale", "debug")),
    ("fsr2_on_demand_identify_fail", ("upscale", "debug")),
    ("fsr2_translation_candidate", ("upscale", "info")),
    ("fsr2_translation_failure", ("upscale", "error")),
    ("fsr2_translation_blocked", ("upscale", "warn")),
    ("fsr2_translation", ("upscale", "debug")),
    ("fsr2_il2cpp_hook_failed", ("hook", "error")),
    ("fsr2_il2cpp_hook_installed", ("hook", "info")),
    ("fsr2_il2cpp_rva", ("hook", "info")),
    ("fsr2_family", ("upscale", "debug")),
    ("fsr2_runtime_status", ("upscale", "debug")),
    ("fsr2_upscaler_stall", ("upscale", "warn")),
    ("fsr2_jitter", ("upscale", "debug")),
    ("fsr2_stage", ("upscale", "debug")),
    ("fsr2_input_dump", ("upscale", "debug")),
    ("fsr2_input", ("upscale", "debug")),
    ("fsr2_output_dump", ("upscale", "debug")),
    ("fsr2_motion_mask_guard", ("upscale", "debug")),
    ("fsr2_depth_guard", ("upscale", "debug")),
    ("fsr2_neutral_exposure", ("upscale", "debug")),
    ("fsr2_transient_capture", ("probe", "debug")),
    ("fsr2_optiscaler", ("coexist", "info")),
    ("fsr2_", ("upscale", "debug")),
    ("target_upscaler", ("upscale", "debug")),
    ("target_jitter", ("upscale", "debug")),
    ("mode2_", ("upscale", "debug")),
    # 钩子
    ("hooked ", ("hook", "info")),
    ("hook_", ("hook", "info")),
    ("iat_scan", ("hook", "debug")),
    ("detour", ("hook", "info")),
    ("GetProcAddress", ("hook", "debug")),
    ("LoadLibrary", ("hook", "debug")),
    ("d3d11_loaded", ("hook", "debug")),
    ("context_device_texture_hook", ("hook", "debug")),
    ("texture_device_interface_hook", ("hook", "debug")),
    ("il2cpp_", ("hook", "info")),
    # 菜单
    ("render_scale_menu", ("menu", "info")),
    ("render_scale", ("menu", "info")),
    # 共存
    ("dlssg_", ("coexist", "info")),
    ("optiscaler", ("coexist", "info")),
    ("fsr2_interface_query", ("coexist", "debug")),
    # 帧生成
    ("fg_", ("fg", "info")),
    ("frame_gen", ("fg", "info")),
    # 配置 / 生命周期 / 其它
    ("build_profile", ("core", "info")),
    ("warning", ("core", "warn")),
    ("dxgi_swapchain_result", ("hook", "debug")),
    ("dxgi_swapchain_request", ("hook", "debug")),
    ("native_ldr_swapchain", ("hdr", "info")),
    ("dxgi_", ("hook", "debug")),
]

ERROR_HINTS = ("failed", "failure", "_fail", "unsupported", "unavailable", "refused", "refusing")
WARN_HINTS = ("warning", "stall", "blocked", "mismatch")


def classify(prefix: str):
    for key, value in PREFIX_MAP:
        if prefix.startswith(key):
            return value
    cat, lvl = "core", "info"
    low = prefix.lower()
    if any(h in low for h in ERROR_HINTS):
        lvl = "error"
    elif any(h in low for h in WARN_HINTS):
        lvl = "warn"
    return cat, lvl


LEVEL_FN = {"error": "LOG_ERROR", "warn": "LOG_WARN", "info": "LOG_INFO", "debug": "LOG_DEBUG",
            "trace": "LOG_TRACE"}


def find_stmt_end(text: str, start: int) -> int:
    """从 log_line( 的 '(' 处出发做括号配对，返回语句分号后的下标；失败返回 -1。"""
    depth = 0
    i = start
    n = len(text)
    in_str = False
    in_char = False
    while i < n:
        c = text[i]
        if in_str:
            if c == "\\":
                i += 2
                continue
            if c == '"':
                in_str = False
        elif in_char:
            if c == "\\":
                i += 2
                continue
            if c == "'":
                in_char = False
        else:
            if c == '"':
                in_str = True
            elif c == "'":
                in_char = True
            elif c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
                if depth == 0:
                    j = i + 1
                    while j < n and text[j] in " \t\r\n":
                        j += 1
                    if j < n and text[j] == ";":
                        return j + 1
                    return -1
        i += 1
    return -1


def main():
    path = sys.argv[1]
    apply = "--apply" in sys.argv
    with open(path, "r", encoding="utf-8", newline="") as fh:
        text = fh.read()

    out = []
    pos = 0
    stats = Counter()
    skipped = []

    pattern = re.compile(r'log_line\(\s*(?:std::string\()?\s*"([A-Za-z0-9_]+)')
    for m in pattern.finditer(text):
        # 调用点必须落在 pos 之后（避免重叠）
        if m.start() < pos:
            continue
        open_paren = text.index("(", m.start())
        end = find_stmt_end(text, open_paren)
        if end < 0:
            skipped.append((text.count("\n", 0, m.start()) + 1, m.group(1)))
            stats["skipped_unbalanced"] += 1
            continue
        prefix = m.group(1)
        cat, lvl = classify(prefix)
        stmt = text[m.start():end]
        # 去掉结尾分号，稍后补回
        assert stmt.rstrip().endswith(";")
        body = stmt.rstrip()[:-1]
        # log_line(  ->  LOG_*(cat,
        body = body.replace("log_line(", f"{LEVEL_FN[lvl]}(blog::cat::{cat}, ", 1)
        # 如果原来是 log_line(std::string("...") 形态，保留 std::string 包裹
        out.append(text[pos:m.start()])
        out.append(body + ";")
        pos = end
        stats[f"{cat}/{lvl}"] += 1
        stats["converted"] += 1

    out.append(text[pos:])
    result = "".join(out)

    print(f"file: {path}")
    print(f"converted: {stats['converted']}")
    for key in sorted(k for k in stats if k not in ("converted", "skipped_unbalanced")):
        print(f"  {key:24s} {stats[key]}")
    if skipped:
        print(f"skipped (unbalanced, 需人工): {len(skipped)}")
        for line, prefix in skipped[:20]:
            print(f"  line {line}: {prefix}")

    if apply:
        with open(path, "w", encoding="utf-8", newline="") as fh:
            fh.write(result)
        print("APPLIED")


if __name__ == "__main__":
    main()
