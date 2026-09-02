# 快速烧写：跳过 export.ps1 环境激活和 idf.py 的构建检查，直接把 build/ 里的现成镜像
# 用 esptool 写进板子（921600 波特率，写完硬复位）。
#
# 用法：先照常 `idf.py build`，再运行本脚本。
# 注意：本脚本不编译——build/ 里的 bin 过期时烧进去的也是旧的。
# 921600 若在个别 USB 转串口芯片上不稳，把下行 -b 921600 改回 460800 即可。
$ErrorActionPreference = 'Stop'
$py = "E:\esp\.espressif\python_env\idf5.5_py3.12_env\Scripts\python.exe"
$build = Join-Path $PSScriptRoot "..\build"
if (-not (Test-Path (Join-Path $build "flash_args"))) {
    Write-Error "build/flash_args 不存在，请先 idf.py build"
}
Push-Location $build
try {
    & $py -m esptool --chip esp32c3 -b 921600 --before default_reset --after hard_reset write_flash '@flash_args'
} finally {
    Pop-Location
}
