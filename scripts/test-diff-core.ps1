param([int]$BenchmarkFiles = 0)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'C++ Build Tools were not found. Run this test with GitHub Actions.' }
$vsPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw 'C++ compiler not found.' }
$devCmd = Join-Path $vsPath 'Common7\Tools\VsDevCmd.bat'
$outputDir = Join-Path $repoRoot 'build\diff-tests'
New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
$testExe = Join-Path $outputDir 'test-diff-core.exe'
$compile = 'call "{0}" -no_logo -arch=x64 -host_arch=x64 && cl /nologo /std:c++17 /EHsc /O2 /W4 /WX /utf-8 /I"{1}" "{2}" "{3}" /Fe:"{4}" /Fo:"{5}\\"' -f $devCmd, (Join-Path $repoRoot 'windirstat'), (Join-Path $repoRoot 'windirstat\SnapshotDiffCore.cpp'), (Join-Path $repoRoot 'tests\disk-diff\TestCore.cpp'), $testExe, $outputDir
& $env:ComSpec /d /c $compile
if ($LASTEXITCODE -ne 0) { throw "Core test compilation failed (exit $LASTEXITCODE)." }
& $testExe (Join-Path $repoRoot 'tests\disk-diff\fixtures')
if ($LASTEXITCODE -ne 0) { throw "Core tests failed (exit $LASTEXITCODE)." }
if ($BenchmarkFiles -gt 0) {
    & $testExe --benchmark $BenchmarkFiles
    if ($LASTEXITCODE -ne 0) { throw "Benchmark failed (exit $LASTEXITCODE)." }
}
