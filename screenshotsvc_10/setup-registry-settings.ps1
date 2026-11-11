# Run from an elevated PowerShell once when installing/upgrading ScreenshotService.
# Grants standard users access to change ScreenshotService's machine-wide settings
# without running the application itself as administrator.
$ErrorActionPreference = 'Stop'

$principal = [System.Security.Principal.WindowsPrincipal]::new(
    [System.Security.Principal.WindowsIdentity]::GetCurrent()
)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this script from PowerShell opened with Run as administrator.'
}

$subKey = 'SOFTWARE\Gallery Inc\ScreenshotSvc'
$path = "Registry::HKEY_LOCAL_MACHINE\$subKey"
New-Item -Path $path -Force | Out-Null

$acl = Get-Acl -LiteralPath $path
$usersSid = [System.Security.Principal.SecurityIdentifier]::new('S-1-5-32-545') # BUILTIN\Users
$rights = [System.Security.AccessControl.RegistryRights]::ReadKey -bor `
          [System.Security.AccessControl.RegistryRights]::SetValue -bor `
          [System.Security.AccessControl.RegistryRights]::CreateSubKey
$rule = [System.Security.AccessControl.RegistryAccessRule]::new(
    $usersSid,
    $rights,
    [System.Security.AccessControl.InheritanceFlags]::ContainerInherit,
    [System.Security.AccessControl.PropagationFlags]::None,
    [System.Security.AccessControl.AccessControlType]::Allow
)
$acl.SetAccessRule($rule)
Set-Acl -LiteralPath $path -AclObject $acl

# Ensure the encoder capability cache exists and inherits the permissions above.
New-Item -Path (Join-Path $path 'EncoderCache') -Force | Out-Null
Write-Host "Registry settings ready at HKLM\$subKey"
Write-Host 'The next ScreenshotService start will import values from the old settings.ini once, if present.'
