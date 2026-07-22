install.log("FSR Bridge Lua 安装器已开始执行")

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

local has_system = system ~= nil
local has_gpu_api = has_system and system.get_gpu ~= nil

if has_system and has_gpu_api then
    local gpu = system.get_gpu()
    if gpu ~= nil then
        gpu_name = gpu.name or "Unknown"
        gpu_vendor = gpu.vendor or "Unknown"
        if gpu_vendor == "AMD" and (gpu_name == "AMD Radeon RX 9060" or gpu_name == "AMD Radeon RX 9060 XT" or gpu_name == "AMD Radeon RX 9070" or gpu_name == "AMD Radeon RX 9070 XT") then
            fsr4_update = "true"
            fsr4_upscaler_index = "0"
            fsr4_force_int8 = "false"
            fsr4_mode = "fp8"
        elseif gpu_vendor == "NVIDIA" and (gpu_name == "NVIDIA GeForce RTX 2060" or gpu_name == "NVIDIA GeForce RTX 2070" or gpu_name == "NVIDIA GeForce RTX 2080" or gpu_name == "NVIDIA GeForce RTX 3050" or gpu_name == "NVIDIA GeForce RTX 3060" or gpu_name == "NVIDIA GeForce RTX 3070" or gpu_name == "NVIDIA GeForce RTX 3080" or gpu_name == "NVIDIA GeForce RTX 3090" or gpu_name == "NVIDIA GeForce RTX 4060" or gpu_name == "NVIDIA GeForce RTX 4070" or gpu_name == "NVIDIA GeForce RTX 4080" or gpu_name == "NVIDIA GeForce RTX 4090" or gpu_name == "NVIDIA GeForce RTX 5060" or gpu_name == "NVIDIA GeForce RTX 5070" or gpu_name == "NVIDIA GeForce RTX 5080" or gpu_name == "NVIDIA GeForce RTX 5090") then
            fsr4_update = "true"
            fsr4_upscaler_index = "0"
            fsr4_force_int8 = "true"
            fsr4_mode = "int8"
        elseif gpu_vendor == "AMD" and (gpu_name == "AMD Radeon RX 7600" or gpu_name == "AMD Radeon RX 7600 XT" or gpu_name == "AMD Radeon RX 7700 XT" or gpu_name == "AMD Radeon RX 7800 XT" or gpu_name == "AMD Radeon RX 7900 GRE" or gpu_name == "AMD Radeon RX 7900 XT" or gpu_name == "AMD Radeon RX 7900 XTX" or gpu_name == "AMD Radeon 740M" or gpu_name == "AMD Radeon 760M" or gpu_name == "AMD Radeon 780M" or gpu_name == "AMD Radeon 8050S" or gpu_name == "AMD Radeon 8060S" or gpu_name == "AMD Radeon 880M" or gpu_name == "AMD Radeon 890M") then
            fsr4_update = "true"
            fsr4_upscaler_index = "0"
            fsr4_force_int8 = "true"
            fsr4_mode = "int8"
        elseif gpu_vendor == "Intel" and (gpu_name == "Intel(R) Arc(TM) A380 Graphics" or gpu_name == "Intel(R) Arc(TM) A580 Graphics" or gpu_name == "Intel(R) Arc(TM) A750 Graphics" or gpu_name == "Intel(R) Arc(TM) A770 Graphics" or gpu_name == "Intel(R) Arc(TM) B570" or gpu_name == "Intel(R) Arc(TM) B580" or gpu_name == "Intel(R) Arc(TM) Graphics" or gpu_name == "Intel(R) Arc(TM) 130V GPU" or gpu_name == "Intel(R) Arc(TM) 140V GPU") then
            fsr4_update = "true"
            fsr4_upscaler_index = "0"
            fsr4_force_int8 = "true"
            fsr4_mode = "int8"
        end
    end
else
    install.log("当前启动器未提供 system.get_gpu，FSR4 GPU 策略保持 auto")
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
        Version = "1.1.3"
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
