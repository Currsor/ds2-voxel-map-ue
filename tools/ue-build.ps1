# UE 5.4 构建助手（DS2VoxelMap 工程）
# 用法:
#   .\tools\ue-build.ps1                                   # 默认编译 Editor Development Win64
#   .\tools\ue-build.ps1 -Target DS2VoxelMap -Config Development
#   .\tools\ue-build.ps1 -Extra @('-projectfiles')         # 生成 VS 工程文件
#
# 说明: 本机系统只装了 .NET 10，而 UBT 需要 .NET 6，因此使用 UE 5.4 自带的 .NET 6.0.302。
param(
    [string]$Target   = "DS2VoxelMapEditor",
    [string]$Config   = "Development",
    [string]$Platform = "Win64",
    [string[]]$Extra  = @()
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot

$dotnetDir = 'D:\Program Files\Epic Games\UE_5.4\Engine\Binaries\ThirdParty\DotNet\6.0.302\windows'
$ubt       = 'D:\Program Files\Epic Games\UE_5.4\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.dll'

if (-not (Test-Path "$dotnetDir\dotnet.exe")) { throw "找不到 UE 自带的 dotnet: $dotnetDir" }
if (-not (Test-Path $ubt))                  { throw "找不到 UnrealBuildTool: $ubt" }

$env:DOTNET_ROOT               = $dotnetDir
$env:DOTNET_MULTILEVEL_LOOKUP  = '0'
$env:DOTNET_ROLL_FORWARD       = 'LatestMajor'
$env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
$env:PATH = "$dotnetDir;$env:PATH"

$project = Join-Path $projectRoot 'DS2VoxelMap.uproject'
Write-Output "==> dotnet $ubt $Target $Platform $Config -project=$project"
& "$dotnetDir\dotnet.exe" $ubt $Target $Platform $Config "-project=$project" "-waitmutex" "-progress" @Extra
exit $LASTEXITCODE
