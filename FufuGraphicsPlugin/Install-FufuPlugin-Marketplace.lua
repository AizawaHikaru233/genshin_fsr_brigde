install.log("FSR Bridge Lua 商城安装器已开始执行")

-- 纯文件层安装：不调用任何 GPU 检测接口（芙芙沙箱 GPU API 在部分多适配器环境会中断脚本）。
-- FSR4 策略与 DLSS 依赖均由 bootstrap（FufuGraphicsPlugin.dll）启动时处理：
--   - 策略：Device-ID 精确分类写入 OptiScaler.ini（本脚本删除旧配置后由 bootstrap 重建）
--   - DLSS：bootstrap 检测 OptiScaler 目录缺 nvngx_dlss.dll 且为 RTX 时，从 payload\NVIDIA\DLSS 补齐
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
        Description = "支持把原神的FSR2转换为FSR4、DLSS、XESS",
        Developer = "シリアCelia",
        File = "FSR-Bridge-Plugin.dll",
        Version = "1.2.3"
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
-- 删除旧配置，bootstrap 下次启动时按 default_config 重建：
--   OptiScaler.ini（托管设置 + Device-ID 策略分类）、FSR4Policy.ini 不再需要
if install.file_exists(plugin_dir .. "\\FSR4Policy.ini") then
    install.delete(plugin_dir .. "\\FSR4Policy.ini")
end
if install.file_exists(opti_dir .. "\\OptiScaler.ini") then
    install.delete(opti_dir .. "\\OptiScaler.ini")
end

install.set_progress(100, "安装完成")
install.show_notification("安装成功", "原神FSR2桥接插件已就绪", "success", 5000)
