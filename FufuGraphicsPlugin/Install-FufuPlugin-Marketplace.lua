install.log("FSR Bridge Lua 商城安装器已开始执行")

local dlss_rules = {
    { vendor = "NVIDIA", family = "RTX", series = "20" },
    { vendor = "NVIDIA", family = "RTX", series = "30" },
    { vendor = "NVIDIA", family = "RTX", series = "40" },
    { vendor = "NVIDIA", family = "RTX", series = "50" },
}
local install_dlss_runtime = false
if system ~= nil and system.gpu_matches_any ~= nil then
    install_dlss_runtime = system.gpu_matches_any(dlss_rules) == true
end

local plugin_id = "FSR-Bridge-Plugin"
local plugins_dir = install.get_plugins_dir()
local plugin_dir = plugins_dir .. "\\" .. plugin_id
local payload_dir = plugin_dir .. "\\payload"
local opti_root_dir = payload_dir .. "\\OptiScaler"
local opti_dir = opti_root_dir
install.log("插件目录: " .. plugin_dir)

install.set_progress(0, "正在准备原神 FSR2 桥接插件")

local result = install.download_plugin(plugin_id)
if result == nil or not result.success then
    local message = result and result.error or "官方插件服务未返回成功结果"
    install.log("下载错误: " .. message)
    install.show_notification("安装错误", message, "error", 5000)
    return
end

if install.file_exists(opti_root_dir .. "\\OptiScaler.dll") then
    opti_dir = opti_root_dir
else
    local message = "插件包缺少 OptiScaler.dll"
    install.log(message)
    install.show_notification("安装错误", message, "error", 5000)
    return
end
install.log("OptiScaler 运行目录: " .. opti_dir)

install.set_progress(82, "正在写入插件配置")
install.write_config(plugin_dir, {
    General = {
        Name = "原神FSR2桥接插件",
        Description = "支持把原神的FSR2转换为FSR4（A卡7000/9000）、DLSS/XeSS/FSR4 INT8（其余显卡）",
        Developer = "シリアCelia",
        File = "FSR-Bridge-Plugin.dll",
        Version = "2.0.0"
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

install.set_progress(90, "正在准备组件初始配置")
if install.file_exists(plugin_dir .. "\\FSR4Policy.ini") then
    install.delete(plugin_dir .. "\\FSR4Policy.ini")
end
if install.file_exists(opti_dir .. "\\OptiScaler.ini") then
    install.delete(opti_dir .. "\\OptiScaler.ini")
end

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
