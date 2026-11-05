param(
    [string]$MsvcRoot,
    [string]$SdkRoot = "${env:ProgramFiles(x86)}\Windows Kits\10",
    [string]$SdkVersion = '10.0.26100.0',
    [string]$BuildDir = 'build-win32',
    [int]$Jobs = 6
)

$ErrorActionPreference = 'Stop'
if ($MsvcRoot) {
    $MsvcRoot = (Resolve-Path -LiteralPath $MsvcRoot).Path
    $SdkRoot = (Resolve-Path -LiteralPath $SdkRoot).Path
    $compilerBin = Join-Path $MsvcRoot 'bin/Hostx64/x86'
    $sdkBin = Join-Path $SdkRoot "bin/$SdkVersion/x64"
    foreach ($required in @("$compilerBin/cl.exe", "$sdkBin/rc.exe", "$SdkRoot/Lib/$SdkVersion/um/x86/kernel32.lib")) {
        if (!(Test-Path -LiteralPath $required)) {
            throw "Required Win32 build tool or SDK file missing: $required"
        }
    }
    $env:PATH = "$compilerBin;$sdkBin;$env:PATH"
    $env:INCLUDE = "$MsvcRoot/include;$SdkRoot/Include/$SdkVersion/ucrt;$SdkRoot/Include/$SdkVersion/shared;$SdkRoot/Include/$SdkVersion/um;$SdkRoot/Include/$SdkVersion/winrt"
    $env:LIB = "$MsvcRoot/lib/x86;$SdkRoot/Lib/$SdkVersion/ucrt/x86;$SdkRoot/Lib/$SdkVersion/um/x86"
}

if (!(Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    throw 'Use an x86 Native Tools PowerShell, or pass -MsvcRoot.'
}

Push-Location -LiteralPath $PSScriptRoot
try {
    cmake -S . -B $BuildDir -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_C_COMPILER=cl.exe" "-DCMAKE_CXX_COMPILER=cl.exe"
    if ($LASTEXITCODE) { throw "CMake configuration failed ($LASTEXITCODE)" }
    cmake --build $BuildDir --parallel $Jobs
    if ($LASTEXITCODE) { throw "Win32 build failed ($LASTEXITCODE)" }
} finally {
    Pop-Location
}
