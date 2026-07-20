-- 芙芙启动器本地测试版：优先使用 Plugins\_local_test\package.zip，缺少时回退到商城下载。
install.log("FSR Bridge Lua 本地测试安装器已开始执行")

local plugin_id = "FSR-Bridge-Plugin"
local plugins_dir = install.get_plugins_dir()
local plugin_dir = plugins_dir .. "\\" .. plugin_id
local payload_dir = plugin_dir .. "\\payload"
local opti_root_dir = payload_dir .. "\\OptiScaler"
local opti_dir = opti_root_dir
local config_stage_dir = plugin_dir .. "\\._lua_config_stage"
local local_test_package = plugins_dir .. "\\_local_test\\package.zip"

local fsr4_update = "auto"
local fsr4_upscaler_index = "auto"
local fsr4_force_int8 = "auto"
local fsr4_mode = "auto"
local gpu_name = "Unknown"
local gpu_vendor = "Unknown"

local function contains(text, token)
    return string.find(string.upper(text), token, 1, true) ~= nil
end

local function matches(text, pattern)
    return string.match(string.upper(text), pattern) ~= nil
end

if system ~= nil and system.get_gpu ~= nil and string ~= nil then
    local gpu = system.get_gpu()
    if gpu ~= nil then
        gpu_name = gpu.name or "Unknown"
        gpu_vendor = gpu.vendor or "Unknown"
        local name = string.upper(gpu_name)

        local amd_fp8 = gpu_vendor == "AMD" and matches(name, "RX%s*9%d%d%d")
        local nvidia_int8 = gpu_vendor == "NVIDIA" and (
            matches(name, "RTX%s*20%d%d") or
            matches(name, "RTX%s*30%d%d") or
            matches(name, "RTX%s*40%d%d") or
            matches(name, "RTX%s*50%d%d")
        )
        local amd_discrete_int8 = gpu_vendor == "AMD" and matches(name, "RX%s*[67]%d%d%d")
        local amd_integrated_int8 = gpu_vendor == "AMD" and (
            contains(name, "RADEON 610M") or
            contains(name, "RADEON 660M") or
            contains(name, "RADEON 680M") or
            contains(name, "RADEON 740M") or
            contains(name, "RADEON 760M") or
            contains(name, "RADEON 780M") or
            contains(name, "RADEON 880M") or
            contains(name, "RADEON 890M") or
            contains(name, "RADEON 8050S") or
            contains(name, "RADEON 8060S")
        )
        local intel_int8 = gpu_vendor == "Intel" and contains(name, "ARC")

        if amd_fp8 then
            fsr4_update = "true"
            fsr4_upscaler_index = "0"
            fsr4_force_int8 = "false"
            fsr4_mode = "fp8"
        elseif nvidia_int8 or amd_discrete_int8 or amd_integrated_int8 or intel_int8 then
            fsr4_update = "true"
            fsr4_upscaler_index = "0"
            fsr4_force_int8 = "true"
            fsr4_mode = "int8"
        end
    end
else
    install.log("当前启动器未提供 system/string 查询能力，FSR4 GPU 策略保持 auto")
end

install.log("显卡: " .. gpu_name .. " (" .. gpu_vendor .. ")，FSR4 模式: " .. fsr4_mode)

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

if not result.success then
    install.log("下载错误:" .. result.error)
    install.show_notification("安装错误", result.error, "error", 5000)
    return
end

if install.file_exists(opti_root_dir .. "\\OptiScaler\\OptiScaler.dll") then
    opti_dir = opti_root_dir .. "\\OptiScaler"
elseif install.file_exists(opti_root_dir .. "\\OptiScaler.dll") then
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
        Version = "1.1.2"
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
if install.file_exists(bundled_dlss) then
    install.copy_file(bundled_dlss, opti_dir .. "\\nvngx_dlss.dll")
    if install.file_exists(bundled_dlss_license) then
        install.copy_file(bundled_dlss_license, opti_dir .. "\\nvngx_dlss.license.txt")
    end
    install.log("已将 NVIDIA DLSS 组件复制到 OptiScaler 运行目录")
else
    install.log("插件包未包含 NVIDIA DLSS 组件，跳过复制")
end

install.set_progress(100, "安装完成")
install.show_notification("安装成功", "原神FSR2桥接插件已就绪", "success", 5000)
