param([ValidateSet('v143','v145')][string]$Toolset = 'v143')
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'Visual Studio Build Tools are not installed. Use the Build Disk Diff workflow in GitHub Actions, or install the C++ desktop workload with ATL and a Windows SDK.'
}
$vsPath = & $vswhere -latest -version '[17.14,)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 Microsoft.VisualStudio.Component.VC.ATL -property installationPath
if (-not $vsPath) { throw 'No C++ compiler with ATL was found. Visual C++ Redistributable alone is not a compiler. Use GitHub Actions to build without installing it locally.' }
$msbuild = Join-Path $vsPath 'MSBuild\Current\Bin\MSBuild.exe'
& $msbuild (Join-Path $repoRoot 'windirstat.sln') /m /t:Build /p:Configuration=Release /p:Platform=x64 /p:LanguageStandard=stdcpplatest "/p:PlatformToolset=$Toolset" /p:TargetName=WinDirStat-Diff_x64
if ($LASTEXITCODE -ne 0) { throw "Windows build failed (exit $LASTEXITCODE)." }
$exe = Join-Path $repoRoot 'build\WinDirStat-Diff_x64.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw "Expected executable was not produced: $exe" }
Write-Host "Built: $exe"
