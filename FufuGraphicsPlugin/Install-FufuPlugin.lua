install.log("FSR Bridge Lua 安装器已开始执行")

local fsr4_update = "auto"
local fsr4_upscaler_index = "auto"
local fsr4_force_int8 = "auto"
local fsr4_mode = "auto"
local gpu_name = "Unknown"
local gpu_vendor = "Unknown"

local amd_fp8_rules = {
    { vendor = "AMD", family = "RX", series = "9000" },
}

local int8_rules = {
    { vendor = "NVIDIA", family = "GTX", series = "16" },
    { vendor = "NVIDIA", family = "RTX", series = "20" },
    { vendor = "NVIDIA", family = "RTX", series = "30" },
    { vendor = "NVIDIA", family = "RTX", series = "40" },
    { vendor = "NVIDIA", family = "RTX", series = "50" },
    { vendor = "AMD", family = "RX", series = "7000" },
    { vendor = "AMD", name = "740M" },
    { vendor = "AMD", name = "760M" },
    { vendor = "AMD", name = "780M" },
    { vendor = "AMD", name = "8040S" },
    { vendor = "AMD", name = "8050S" },
    { vendor = "AMD", name = "8060S" },
    { vendor = "AMD", name = "840M" },
    { vendor = "AMD", name = "860M" },
    { vendor = "AMD", name = "880M" },
    { vendor = "AMD", name = "890M" },
    { vendor = "Intel", family = "Arc" },
}

-- DLSS is only useful on RTX hardware. GTX 16 is allowed to use FSR4 INT8,
-- but must not receive NVIDIA DLSS runtime files during installation.
local dlss_rules = {
    { vendor = "NVIDIA", family = "RTX", series = "20" },
    { vendor = "NVIDIA", family = "RTX", series = "30" },
    { vendor = "NVIDIA", family = "RTX", series = "40" },
    { vendor = "NVIDIA", family = "RTX", series = "50" },
}
local install_dlss_runtime = false

local function apply_fsr4_policy(mode)
    fsr4_update = "true"
    fsr4_upscaler_index = "0"
    fsr4_force_int8 = mode == "int8" and "true" or "false"
    fsr4_mode = mode
end

local function detect_fsr4_policy()
    if system == nil or system.get_gpu == nil then
        install.log("当前启动器未提供 system.get_gpu，FSR4 策略保持 auto")
        return
    end

    local gpu = system.get_gpu()
    if gpu == nil then
        install.log("system.get_gpu 未返回数据，FSR4 策略保持 auto")
        return
    end
    if gpu.name ~= nil then gpu_name = gpu.name end
    if gpu.vendor ~= nil then gpu_vendor = gpu.vendor end

    if system.gpu_matches_any == nil then
        install.log("当前启动器未提供 GPU 模糊匹配接口，FSR4 策略保持 auto")
        return
    end
    if system.gpu_matches_any(amd_fp8_rules) then
        apply_fsr4_policy("fp8")
    elseif system.gpu_matches_any(int8_rules) then
        apply_fsr4_policy("int8")
    else
        install.log("未识别的 GPU 型号，FSR4 策略保持 auto: " .. gpu_name .. " (" .. gpu_vendor .. ")")
    end
    install_dlss_runtime = system.gpu_matches_any(dlss_rules)
end

detect_fsr4_policy()

install.log("显卡: " .. gpu_name .. " (" .. gpu_vendor .. ")，FSR4 模式: " .. fsr4_mode)

local plugin_id = "FSR-Bridge-Plugin"
local plugins_dir = install.get_plugins_dir()
local plugin_dir = plugins_dir .. "\\" .. plugin_id
local payload_dir = plugin_dir .. "\\payload"
local opti_root_dir = payload_dir .. "\\OptiScaler"
local opti_dir = opti_root_dir
local config_stage_dir = plugin_dir .. "\\._lua_config_stage"
local local_test_package = plugins_dir .. "\\_local_test\\package.zip"
install.log("插件目录: " .. plugin_dir)

install.set_progress(0, "正在准备原神 FSR2 桥接插件")

local result = nil
if install.file_exists(local_test_package) then
    install.log("本地测试模式：检测到本地测试包，开始模拟服务器安装")
    install.create_dir(plugin_dir)
    install.extract(local_test_package, plugin_dir)
    result = { success = true, error = "" }
else
    install.log("未检测到本地测试包，回退到芙芙插件服务下载")
    result = install.download_plugin(plugin_id)
end

if result == nil or not result.success then
    local error_message = (result and result.error) or "官方插件服务未返回成功结果"
    install.log("下载错误:" .. error_message)
    install.show_notification("安装错误", error_message, "error", 5000)
    return
end

if install.file_exists(opti_root_dir .. "\\OptiScaler.dll") then
    opti_dir = opti_root_dir
else
    install.log("未找到 OptiScaler.dll，无法初始化 OptiScaler 配置")
    install.show_notification("安装错误", "插件包缺少 OptiScaler.dll", "error", 5000)
    return
end
install.log("OptiScaler 运行目录: " .. opti_dir)

install.set_progress(82, "正在写入插件配置")
install.write_config(plugin_dir, {
    General = {
        Name = "原神FSR2桥接插件",
        Description = "支持把原神的FSR2转换为FSR4、DLSS、XESS",
        Developer = "シリアCelia",
        File = "FSR-Bridge-Plugin.dll",
        Version = "1.2.1"
    },
    EnableBridge = {
        Name = "启用 FSR Bridge",
        Type = "bool",
        Value = "1"
    },
    EnableOptiScaler = {
        Name = "启用 OptiScaler（需要 Bridge）",
        Type = "bool",
        Value = "1"
    },
    EnableReShade = {
        Name = "启用 ReShade",
        Type = "bool",
        Value = "1"
    },
    IssueFeedback = {
        Name = "问题反馈",
        Type = "string",
        Value = "https://github.com/AizawaHikaru233/genshin_fsr_brigde/issues"
    },
    CommunityGroup = {
        Name = "交流群",
        Type = "string",
        Value = "928147257"
    },
    ResetConfigurations = {
        Name = "重置所有配置文件（自行更换插件版本或出现问题时使用）",
        Type = "bool",
        Value = "1"
    }
})

install.set_progress(90, "正在准备官方默认配置")
install.delete(opti_dir .. "\\OptiScaler.ini")
install.create_dir(config_stage_dir)
install.write_config(config_stage_dir, {
    FSR4Policy = {
        UpscalerIndex = fsr4_upscaler_index,
        Fsr4Update = fsr4_update,
        Fsr4ForceEnableInt8 = fsr4_force_int8
    }
})
install.move_file(config_stage_dir .. "\\config.ini", plugin_dir .. "\\FSR4Policy.ini")
install.delete(config_stage_dir)

local bundled_dlss = payload_dir .. "\\NVIDIA\\DLSS\\nvngx_dlss.dll"
local bundled_dlss_license = payload_dir .. "\\NVIDIA\\DLSS\\nvngx_dlss.license.txt"
if install_dlss_runtime and install.file_exists(bundled_dlss) then
    install.copy_file(bundled_dlss, opti_dir .. "\\nvngx_dlss.dll")
    if install.file_exists(bundled_dlss_license) then
        install.copy_file(bundled_dlss_license, opti_dir .. "\\nvngx_dlss.license.txt")
    end
    install.log("已将 NVIDIA DLSS 组件复制到 OptiScaler 运行目录")
elseif not install_dlss_runtime then
    install.log("当前显卡不是已识别的 RTX，跳过 NVIDIA DLSS 组件复制")
else
    install.log("插件包未包含 NVIDIA DLSS 组件，跳过复制")
end

install.set_progress(100, "安装完成")
install.show_notification("安装成功", "原神FSR2桥接插件已就绪", "success", 5000)
