[CmdletBinding()]
param(
    [ValidateSet('x64')]
    [string] $Target = 'x64',
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
    [string] $Configuration = 'RelWithDebInfo'
)

$ErrorActionPreference = 'Stop'

if ( $DebugPreference -eq 'Continue' ) {
    $VerbosePreference = 'Continue'
    $InformationPreference = 'Continue'
}

if ( $env:CI -eq $null ) {
    throw "Package-Windows.ps1 requires CI environment"
}

if ( ! ( [System.Environment]::Is64BitOperatingSystem ) ) {
    throw "Packaging script requires a 64-bit system to build and run."
}

if ( $PSVersionTable.PSVersion -lt '7.2.0' ) {
    Write-Warning 'The packaging script requires PowerShell Core 7. Install or upgrade your PowerShell version: https://aka.ms/pscore6'
    exit 2
}

function Package {
    trap {
        Write-Error $_
        exit 2
    }

    $ScriptHome = $PSScriptRoot
    $ProjectRoot = Resolve-Path -Path "$PSScriptRoot/../.."
    $BuildSpecFile = "${ProjectRoot}/buildspec.json"

    $UtilityFunctions = Get-ChildItem -Path $PSScriptRoot/utils.pwsh/*.ps1 -Recurse

    foreach( $Utility in $UtilityFunctions ) {
        Write-Debug "Loading $($Utility.FullName)"
        . $Utility.FullName
    }

    $BuildSpec = Get-Content -Path ${BuildSpecFile} -Raw | ConvertFrom-Json
    $ProductName = $BuildSpec.name
    $ProductVersion = $BuildSpec.version

    $OutputName = "${ProductName}-${ProductVersion}-windows-${Target}"

    $InstallRoot = "${ProjectRoot}/release/${Configuration}/${ProductName}"
    $StageRoot = "${ProjectRoot}/release/${OutputName}"

    $RemoveArgs = @{
        ErrorAction = 'SilentlyContinue'
        Recurse = $true
        Force = $true
        Path = @(
            "${ProjectRoot}/release/${ProductName}-*-windows-*.zip"
            $StageRoot
        )
    }

    Remove-Item @RemoveArgs

    # Stage the files in the same layout as an OBS installation, so the
    # artifact's contents can be dropped straight into the OBS folder:
    #   obs-plugins/64bit/<plugin>.dll
    #   data/obs-plugins/<plugin>/locale/en-US.ini
    Log-Group "Staging ${ProductName} in the OBS folder layout..."
    $PluginDir = "${StageRoot}/obs-plugins/64bit"
    $DataDir = "${StageRoot}/data/obs-plugins/${ProductName}"

    New-Item -ItemType Directory -Force -Path $PluginDir, $DataDir | Out-Null
    Copy-Item -Path "${InstallRoot}/bin/64bit/*.dll" -Destination $PluginDir
    Copy-Item -Path "${InstallRoot}/data/*" -Destination $DataDir -Recurse

    Get-ChildItem -Path $StageRoot -Recurse -File | ForEach-Object { Write-Information $_.FullName }
    Log-Group
}

Package
