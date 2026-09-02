param(
    [switch]$Clean,
    [switch]$Flash
)

# o-platform 一键构建脚本（固化最优环境配置 + 文件锁自动重试）
# 用法：
#   .\tools\build.ps1              # 增量构建
#   .\tools\build.ps1 -Clean       # 先 fullclean 再构建
#   .\tools\build.ps1 -Flash       # 构建成功后烧写 COM6
# 特性：
#   1. 工具链显式指向 E:\esp\.espressif（C 盘旧副本已删除，勿再引用）。
#   2. CCACHE_DIR 指向 E:\esp\ccache，避免 C 盘 95% 满导致的 ENOSPC 连环失败，
#      并长期复用编译缓存（官方 ai-passport 用 IDF_CCACHE_ENABLE 走同一套加速）。
#   3. 自动重试 Defender 文件锁（ranlib/ar/ninja remove 的 Permission denied，
#      属瞬时锁，重试 4 次基本必过）。
#   4. 构建失败会打印日志路径，便于定位。

# 注意：不设 $ErrorActionPreference='Stop'。export.ps1 内部 activate.py 的普通输出
# 在 PS5.1 下被当作 native stderr，Stop 会把它当致命错误中断。用 Continue + 手动判错。

# ---------- 环境 ----------
$env:IDF_TOOLS_PATH      = "E:\esp\.espressif"
$env:IDF_PYTHON_ENV_PATH = "E:\esp\.espressif\python_env\idf5.5_py3.12_env"
$env:CCACHE_DIR          = "E:\esp\ccache"

$IDF_PATH   = "E:\code\code tools\esp-idf-v5.5.3"
$PROJECT    = "E:\code\ai passport\o-platform"

# ---------- 激活 ESP-IDF ----------
Write-Host "[build] activating ESP-IDF 5.5.3 ..." -ForegroundColor Cyan
# 先把 python venv 加进 PATH，确保 export.ps1 能找到 python
$pyScripts = Join-Path $env:IDF_PYTHON_ENV_PATH "Scripts"
if (Test-Path $pyScripts) { $env:PATH = "$pyScripts;$env:PATH" }
Push-Location $IDF_PATH
try {
    . .\export.ps1
} finally {
    Pop-Location
}
if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
    Write-Host "[build] ESP-IDF 激活失败：找不到 idf.py" -ForegroundColor Red
    exit 1
}

# ---------- fullclean (optional) ----------
if ($Clean) {
    Write-Host "[build] fullclean ..." -ForegroundColor Yellow
    Push-Location $PROJECT
    try {
        idf.py fullclean
    } finally {
        Pop-Location
    }
}

# ---------- build (with file-lock retry) ----------
$maxRetry = 4
for ($attempt = 1; $attempt -le $maxRetry; $attempt++) {
    Write-Host "[build] attempt $attempt/$maxRetry ..." -ForegroundColor Cyan
    $ok = $false
    Push-Location $PROJECT
    try {
        idf.py build
        $ok = ($LASTEXITCODE -eq 0)
    } finally {
        Pop-Location
    }

    if ($ok) {
        Write-Host "[build] build OK" -ForegroundColor Green
        break
    }

    if ($attempt -lt $maxRetry) {
        Write-Host "[build] failed (likely Defender file lock), retry in 2s ..." -ForegroundColor Red
        Start-Sleep -Seconds 2
    } else {
        Write-Host "[build] max retries reached, see build\log\idf_py_stderr_output_*" -ForegroundColor Red
        exit 1
    }
}

# ---------- optional flash ----------
if ($Flash) {
    Write-Host "[build] flashing COM6 ..." -ForegroundColor Cyan
    & (Join-Path $PSScriptRoot "flash.ps1")
}

Write-Host "[build] done. bin at $PROJECT\build\o-platform.bin" -ForegroundColor Green
