<#
.SYNOPSIS
Creates a passwordless local Jukebox account that runs Neon Jukebox instead of the Windows desktop.
.DESCRIPTION
Windows 10/11 Pro, Enterprise or Education, 64-bit. Place this single script next to
neon_jukebox.exe and the assets folder. It requests administrator elevation itself.
It does not enable automatic Windows sign-in or change other users' shells.
The small GUI launcher is compiled from source embedded in this file; no downloads.
Existing users named Jukebox are accepted only when a previous setup receipt owns their SID.
The account must be signed out when installing, updating or restoring its desktop.
.EXAMPLE
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Install-Jukebox.ps1
.EXAMPLE
.\Install-Jukebox.ps1 -AppDirectory 'D:\Neon Jukebox' -WhatIf
.EXAMPLE
.\Install-Jukebox.ps1 -RestoreDesktop
.EXAMPLE
.\Install-Jukebox.ps1 -ResetSources
#>
[CmdletBinding(SupportsShouldProcess)]
param(
    [string]$AppDirectory,
    [switch]$RestoreDesktop,
    [switch]$ResetSources,
    [switch]$NoDialog
)

$ErrorActionPreference = 'Stop'
$accountName = 'Jukebox'
$installRoot = Join-Path $env:ProgramData 'NeonJukebox'
$receiptPath = Join-Path $env:ProgramData 'NeonJukebox.kiosk-setup.json'
$shellRelativeKey = 'Software\Microsoft\Windows\CurrentVersion\Policies\System'
$managedAccount = $false
$temporaryPassword = $null
$success = $false

function Show-Result([string]$message, [bool]$failed = $false) {
    Write-Host $message
    if (-not $NoDialog) {
        Add-Type -AssemblyName System.Windows.Forms
        $icon = if ($failed) { [Windows.Forms.MessageBoxIcon]::Error } else { [Windows.Forms.MessageBoxIcon]::Information }
        [Windows.Forms.MessageBox]::Show($message, 'Neon Jukebox Kurulumu', [Windows.Forms.MessageBoxButtons]::OK, $icon) | Out-Null
    }
}

function Save-Receipt {
    $receipt | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $receiptPath -Encoding UTF8
    $acl = [Security.AccessControl.FileSecurity]::new()
    $acl.SetAccessRuleProtection($true, $false)
    foreach ($item in @(@('S-1-5-18','FullControl'), @('S-1-5-32-544','FullControl'), @('S-1-5-32-545','Read'))) {
        $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new([Security.Principal.SecurityIdentifier]::new($item[0]), [Security.AccessControl.FileSystemRights]$item[1], [Security.AccessControl.AccessControlType]::Allow))
    }
    Set-Acl -LiteralPath $receiptPath -AclObject $acl
}

function Protect-ApplicationDirectory([string]$path) {
    $acl = [Security.AccessControl.DirectorySecurity]::new()
    $acl.SetAccessRuleProtection($true, $false)
    foreach ($item in @(@('S-1-5-18','FullControl'), @('S-1-5-32-544','FullControl'), @('S-1-5-32-545','ReadAndExecute'))) {
        $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new([Security.Principal.SecurityIdentifier]::new($item[0]), [Security.AccessControl.FileSystemRights]$item[1], [Security.AccessControl.InheritanceFlags]'ContainerInherit,ObjectInherit', [Security.AccessControl.PropagationFlags]::None, [Security.AccessControl.AccessControlType]::Allow))
    }
    Set-Acl -LiteralPath $path -AclObject $acl
}

function Set-BlankPassword {
    $entry = [ADSI]("WinNT://" + $env:COMPUTERNAME + '/' + $accountName + ',user')
    $entry.SetPassword('')
    $entry.Put('UserFlags', ([int]$entry.Properties['UserFlags'].Value -bor 0x20 -bor 0x10000))
    $entry.Put('PasswordExpired', 0)
    $entry.SetInfo()
    Set-LocalUser -Name $accountName -PasswordNeverExpires $true
}

function Reset-MediaSources([string]$profilePath, [string]$applicationPath) {
    $preferenceRoot = Join-Path $profilePath 'AppData\Roaming\NeonJukebox\NeonJukebox'
    $settingsPath = Join-Path $preferenceRoot 'settings.json'
    $queuePath = Join-Path $preferenceRoot 'queue.json'
    $libraryRoot = Join-Path $applicationPath 'library'
    $libraryPath = Join-Path $libraryRoot 'library.json'
    $settings = if (Test-Path -LiteralPath $settingsPath) { Get-Content -LiteralPath $settingsPath -Raw | ConvertFrom-Json } else { [pscustomobject]@{} }
    $backup = Join-Path $preferenceRoot ('backups\sources-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0,8))
    New-Item -ItemType Directory -Path $backup -Force | Out-Null
    New-Item -ItemType Directory -Path $libraryRoot -Force | Out-Null
    $originalFiles = @($settingsPath, $queuePath, $libraryPath | Where-Object { Test-Path -LiteralPath $_ })
    foreach ($path in $originalFiles) { Copy-Item -LiteralPath $path -Destination $backup }
    try {
        foreach ($property in @{musicRoots=@();videoRoots=@();currentTrackId='';playbackPositionMs=0;playbackWasActive=$false;currentTrackManual=$false;ambientMediaKind=$null}.GetEnumerator()) {
            $settings | Add-Member -NotePropertyName $property.Key -NotePropertyValue $property.Value -Force
        }
        $settings | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $settingsPath -Encoding UTF8
        '[]' | Set-Content -LiteralPath $queuePath -Encoding UTF8
        [ordered]@{schemaVersion=5;musicRoots=@();videoRoots=@();tracks=@();scannedAtMs=0} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $libraryPath -Encoding UTF8
        $saved = Get-Content -LiteralPath $settingsPath -Raw | ConvertFrom-Json
        if (@($saved.musicRoots).Count -ne 0 -or @($saved.videoRoots).Count -ne 0) { throw 'Kaynak sifirlama dogrulanamadi.' }
    } catch {
        foreach ($path in $originalFiles) { Copy-Item -LiteralPath (Join-Path $backup (Split-Path -Leaf $path)) -Destination $path -Force }
        throw
    }
    return $backup
}

try {
    if ($ResetSources -and $RestoreDesktop) { throw '-ResetSources ve -RestoreDesktop birlikte kullanilamaz.' }
    if (-not [Environment]::Is64BitOperatingSystem) { throw '64 bit Windows gereklidir.' }
    $edition = (Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion').EditionID
    if ($edition -notmatch 'Professional|Enterprise|Education|IoT') { throw 'Bu kurulum Windows Pro, Enterprise veya Education gerektirir.' }
    if (-not $RestoreDesktop -and -not $ResetSources) {
        if (-not $AppDirectory) {
            foreach ($candidate in @($PSScriptRoot, (Join-Path $PSScriptRoot '..\build\Release'))) {
                if (Test-Path -LiteralPath (Join-Path $candidate 'neon_jukebox.exe')) { $AppDirectory = $candidate; break }
            }
        }
        if (-not $AppDirectory) { throw 'Betiği neon_jukebox.exe ve assets klasorunun yanina koyun veya -AppDirectory ile uygulama klasorunu belirtin.' }
        $AppDirectory = (Resolve-Path -LiteralPath $AppDirectory).Path
        if (-not (Test-Path -LiteralPath (Join-Path $AppDirectory 'neon_jukebox.exe')) -or -not (Test-Path -LiteralPath (Join-Path $AppDirectory 'assets'))) { throw 'Uygulama klasorunde neon_jukebox.exe ve assets bulunamadi.' }
    }
    $operation = if ($RestoreDesktop) { 'Restore the desktop for the managed Jukebox account' } elseif ($ResetSources) { 'Back up and clear Jukebox music/video sources, catalogue and queue' } else { 'Install the Jukebox account and custom shell' }
    if (-not $PSCmdlet.ShouldProcess($accountName, $operation)) { return }

    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator) -or -not [Environment]::Is64BitProcess) {
        $powershell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
        if (-not [Environment]::Is64BitProcess) { $powershell = Join-Path $env:SystemRoot 'Sysnative\WindowsPowerShell\v1.0\powershell.exe' }
        $invoke = "& '" + $PSCommandPath.Replace("'", "''") + "'"
        if ($AppDirectory) { $invoke += " -AppDirectory '" + $AppDirectory.Replace("'", "''") + "'" }
        if ($RestoreDesktop) { $invoke += ' -RestoreDesktop' }
        if ($ResetSources) { $invoke += ' -ResetSources' }
        if ($NoDialog) { $invoke += ' -NoDialog' }
        $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($invoke))
        $elevated = Start-Process -FilePath $powershell -Verb RunAs -WindowStyle Hidden -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-EncodedCommand',$encoded) -PassThru
        $elevated.WaitForExit()
        exit $elevated.ExitCode
    }

    $receipt = if (Test-Path -LiteralPath $receiptPath) { Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json } else { $null }
    $user = Get-LocalUser -Name $accountName -ErrorAction SilentlyContinue
    if ($user -and (-not $receipt -or $receipt.AccountSid -ne $user.SID.Value -or $receipt.ManagedBy -ne 'NeonJukeboxKiosk-v1')) { throw 'Jukebox adinda mevcut, bu betige ait olmayan bir hesap var. Bu hesap degistirilmedi.' }
    if ($receipt -and -not $user) { throw 'Kayitli kurulumun hesabi bulunamadi. Eski kurulum kaydi incelenmelidir.' }
    if (($RestoreDesktop -or $ResetSources) -and -not $user) { throw 'Islem yapilacak Jukebox kurulumu bulunamadi.' }
    if ($user) {
        $userProfile = Get-CimInstance Win32_UserProfile | Where-Object { $_.SID -eq $user.SID.Value }
        if ($userProfile -and $userProfile.Loaded) { throw 'Jukebox hesabindan once OTURUMU KAPATIN; yalnizca kullanici degistirmek yeterli degildir.' }
        $adminGroup = Get-LocalGroup -SID 'S-1-5-32-544'
        if (Get-LocalGroupMember -Group $adminGroup | Where-Object { $_.SID.Value -eq $user.SID.Value }) { throw 'Jukebox hesabi yonetici olmus. Kurulum durduruldu.' }
    }
    if (-not $user -and (Test-Path -LiteralPath $installRoot)) { throw 'Hedef klasor zaten var ve bu betige ait degil. Icerigi degistirilmedi.' }

    if ($ResetSources) {
        $profileRecord = Get-ItemProperty -LiteralPath ('HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList\' + $user.SID.Value)
        $profilePath = [Environment]::ExpandEnvironmentVariables($profileRecord.ProfileImagePath)
        $backup = Reset-MediaSources $profilePath $installRoot
        Show-Result "Jukebox muzik ve video kaynaklari sifirlandi. PIN, tema ve medya dosyalari korundu. Sonraki giriste yeni kaynaklari secin.`r`nYedek: $backup"
        return
    }

    $nativeSource = @'
using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Security.Principal;
using System.IO;
using Microsoft.Win32;
using Microsoft.Win32.SafeHandles;
using System.ComponentModel;
public static class JukeboxProvisioning {
    [DllImport("userenv.dll", CharSet=CharSet.Unicode, ExactSpelling=true)]
    public static extern int CreateProfile([MarshalAs(UnmanagedType.LPWStr)] string sid, [MarshalAs(UnmanagedType.LPWStr)] string userName, [Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder profilePath, uint pathLength);
    [DllImport("advapi32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    public static extern bool LogonUser(string user, string domain, string password, int type, int provider, out IntPtr token);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool CloseHandle(IntPtr handle);
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
    public struct ProfileInfo {
        public int Size; public int Flags;
        public string UserName; public string ProfilePath; public string DefaultPath;
        public string ServerName; public string PolicyPath; public IntPtr Profile;
    }
    [StructLayout(LayoutKind.Sequential, Pack=1)]
    private struct TokenPrivilege { public int Count; public long Luid; public int Attributes; }
    [DllImport("advapi32.dll", SetLastError=true)]
    private static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
    [DllImport("advapi32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    private static extern bool LookupPrivilegeValue(string system, string name, out long luid);
    [DllImport("advapi32.dll", SetLastError=true)]
    private static extern bool AdjustTokenPrivileges(IntPtr token, bool disable, ref TokenPrivilege state, int size, IntPtr previous, IntPtr length);
    [DllImport("userenv.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    private static extern bool LoadUserProfile(IntPtr token, ref ProfileInfo info);
    [DllImport("userenv.dll", SetLastError=true)]
    private static extern bool UnloadUserProfile(IntPtr token, IntPtr profile);
    private static void EnablePrivilege(string name) {
        IntPtr processToken;
        if (!OpenProcessToken(System.Diagnostics.Process.GetCurrentProcess().Handle, 0x28, out processToken)) throw new Win32Exception();
        try {
            long luid;
            if (!LookupPrivilegeValue(null, name, out luid)) throw new Win32Exception();
            TokenPrivilege state = new TokenPrivilege { Count=1, Luid=luid, Attributes=2 };
            if (!AdjustTokenPrivileges(processToken, false, ref state, 0, IntPtr.Zero, IntPtr.Zero) || Marshal.GetLastWin32Error() != 0) throw new Win32Exception();
        } finally { CloseHandle(processToken); }
    }
    public static string ConfigureShell(string user, string password, string command) {
        EnablePrivilege("SeBackupPrivilege"); EnablePrivilege("SeRestorePrivilege");
        IntPtr token;
        if (!LogonUser(user, Environment.MachineName, password, 2, 0, out token)) throw new Win32Exception();
        ProfileInfo info = new ProfileInfo { Size=Marshal.SizeOf(typeof(ProfileInfo)), Flags=1, UserName=user };
        bool loaded=false;
        try {
            if (!LoadUserProfile(token, ref info)) throw new Win32Exception();
            loaded=true;
            using (var handle = new SafeRegistryHandle(info.Profile, false))
            using (var root = RegistryKey.FromHandle(handle))
            using (var key = root.CreateSubKey(@"Software\Microsoft\Windows\CurrentVersion\Policies\System")) {
                key.SetValue("Shell", command, RegistryValueKind.String);
                key.Flush();
                return (string)key.GetValue("Shell");
            }
        } finally {
            try {
                if (loaded && !UnloadUserProfile(token, info.Profile)) throw new Win32Exception();
            } finally { CloseHandle(token); }
        }
    }
    public static string Probe(string user, string password, string path, bool writable) {
        IntPtr token;
        if (!LogonUser(user, Environment.MachineName, password, 2, 0, out token))
            return "LOGON_ERROR:" + Marshal.GetLastWin32Error();
        try {
            using (WindowsImpersonationContext context = WindowsIdentity.Impersonate(token)) {
                if (writable) {
                    string probe = Path.Combine(path, ".jukebox-permission-probe-" + Guid.NewGuid().ToString("N"));
                    File.WriteAllText(probe, "permission check");
                    File.Delete(probe);
                } else if (File.Exists(path)) {
                    using (FileStream stream = File.Open(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite)) { stream.ReadByte(); }
                } else {
                    using (var entries = Directory.EnumerateFileSystemEntries(path).GetEnumerator()) { entries.MoveNext(); }
                }
            }
            return "OK";
        } catch (Exception error) { return "ERROR: " + error.Message; }
        finally { CloseHandle(token); }
    }
}
'@
    Add-Type -TypeDefinition $nativeSource
    $launcherSource = @'
using System;
using System.Diagnostics;
using System.IO;
using System.Windows.Forms;
using Microsoft.Win32;

internal sealed class JukeboxContext : ApplicationContext
{
    private readonly Timer timer = new Timer();
    private readonly string executable = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "neon_jukebox.exe");
    private Process jukebox;
    private DateTime nextStart = DateTime.MinValue;
    private bool ending;

    internal JukeboxContext()
    {
        timer.Interval = 1000;
        timer.Tick += Tick;
        SystemEvents.SessionEnding += SessionEnding;
        timer.Start();
    }

    private static void Log(string message)
    {
        try
        {
            string folder = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "NeonJukebox");
            Directory.CreateDirectory(folder);
            File.AppendAllText(Path.Combine(folder, "shell.log"), DateTime.Now.ToString("s") + " " + message + Environment.NewLine);
        }
        catch { }
    }

    private void Tick(object sender, EventArgs e)
    {
        if (ending) return;
        if (jukebox != null)
        {
            if (!jukebox.HasExited) return;
            Log("Application exited: " + jukebox.ExitCode);
            jukebox.Dispose();
            jukebox = null;
            nextStart = DateTime.UtcNow.AddSeconds(3);
        }
        if (DateTime.UtcNow < nextStart) return;
        try
        {
            jukebox = Process.Start(new ProcessStartInfo(executable) {
                WorkingDirectory = AppDomain.CurrentDomain.BaseDirectory,
                UseShellExecute = false
            });
            Log("Application started: " + jukebox.Id);
        }
        catch (Exception error)
        {
            Log("Launch failed: " + error.Message);
            nextStart = DateTime.UtcNow.AddSeconds(30);
        }
    }

    private void SessionEnding(object sender, SessionEndingEventArgs e)
    {
        ending = true;
        timer.Stop();
        if (jukebox != null && !jukebox.HasExited) jukebox.CloseMainWindow();
        ExitThread();
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            SystemEvents.SessionEnding -= SessionEnding;
            timer.Dispose();
            if (jukebox != null) jukebox.Dispose();
        }
        base.Dispose(disposing);
    }

    [STAThread]
    private static void Main()
    {
        using (var context = new JukeboxContext()) Application.Run(context);
    }
}
'@
    $globalBefore = Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon' | Select-Object Shell,AutoAdminLogon
    $temporaryPassword = [Guid]::NewGuid().ToString('N') + 'aA9!' + [Guid]::NewGuid().ToString('N')
    if (-not $user) {
        $user = New-LocalUser -Name $accountName -Password (ConvertTo-SecureString $temporaryPassword -AsPlainText -Force) -PasswordNeverExpires -AccountNeverExpires -Disabled -Description 'Neon Jukebox - dedicated standard kiosk account'
        $receipt = [pscustomobject][ordered]@{
            ManagedBy='NeonJukeboxKiosk-v1'; AccountSid=$user.SID.Value; AccountName=$accountName
            InstallRoot=$installRoot; ProfilePath=$null; Status='Preparing'; Shell=$null
            Error=$null; UpdatedAt=$null; ApplicationSHA256=$null; AccessChecks=@()
        }
        Save-Receipt
    }
    $managedAccount = $true
    Disable-LocalUser -Name $accountName
    Set-LocalUser -Name $accountName -Password (ConvertTo-SecureString $temporaryPassword -AsPlainText -Force) -PasswordNeverExpires $true
    $receipt.Status='Preparing'; $receipt.Error=$null; Save-Receipt
    $usersGroup = Get-LocalGroup -SID 'S-1-5-32-545'
    if (-not (Get-LocalGroupMember -Group $usersGroup | Where-Object { $_.SID.Value -eq $user.SID.Value })) { Add-LocalGroupMember -Group $usersGroup -Member $user }

    $profileKey = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList\' + $user.SID.Value
    $profileRecord = Get-ItemProperty -LiteralPath $profileKey -ErrorAction SilentlyContinue
    if (-not $profileRecord) {
        $buffer = [Text.StringBuilder]::new(260)
        $hr = [JukeboxProvisioning]::CreateProfile($user.SID.Value, $accountName, $buffer, 260)
        if ($hr -ne 0) { throw ('Kullanici profili olusturulamadi: 0x{0:X8}' -f $hr) }
        $profileRecord = Get-ItemProperty -LiteralPath $profileKey
    }
    $profilePath = [Environment]::ExpandEnvironmentVariables($profileRecord.ProfileImagePath)
    $receipt.ProfilePath=$profilePath; Save-Receipt

    if (-not $RestoreDesktop) {
        New-Item -ItemType Directory -Path $installRoot -Force | Out-Null
        Protect-ApplicationDirectory $installRoot
        if (-not $AppDirectory.TrimEnd('\').Equals($installRoot.TrimEnd('\'),[StringComparison]::OrdinalIgnoreCase)) {
            Copy-Item -LiteralPath (Join-Path $AppDirectory 'neon_jukebox.exe') -Destination $installRoot -Force
            Copy-Item -LiteralPath (Join-Path $AppDirectory 'assets') -Destination $installRoot -Recurse -Force
            Get-ChildItem -LiteralPath $AppDirectory -File -Filter '*.dll' | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $installRoot -Force }
        }
        $sourceFile = Join-Path $installRoot 'JukeboxShell.cs'
        $launcherSource | Set-Content -LiteralPath $sourceFile -Encoding UTF8
        $compiler = Join-Path $env:SystemRoot 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
        & $compiler /nologo /target:winexe /platform:x64 /reference:System.Windows.Forms.dll (('/out:' + (Join-Path $installRoot 'JukeboxShell.exe'))) $sourceFile
        if ($LASTEXITCODE -ne 0) { throw 'Jukebox baslaticisi derlenemedi.' }
        $libraryRoot = Join-Path $installRoot 'library'
        New-Item -ItemType Directory -Path $libraryRoot -Force | Out-Null
        $libraryAcl = Get-Acl -LiteralPath $libraryRoot
        $libraryAcl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($user.SID, [Security.AccessControl.FileSystemRights]::Modify, [Security.AccessControl.InheritanceFlags]'ContainerInherit,ObjectInherit', [Security.AccessControl.PropagationFlags]::None, [Security.AccessControl.AccessControlType]::Allow))
        Set-Acl -LiteralPath $libraryRoot -AclObject $libraryAcl
        $preferenceRoot = Join-Path $profilePath 'AppData\Roaming\NeonJukebox\NeonJukebox'
        New-Item -ItemType Directory -Path $preferenceRoot -Force | Out-Null
    }

    # A random password is used only for local provisioning and is never saved.
    # Windows rejects programmatic logons with blank passwords. The final account
    # password is cleared; the machine-wide blank-password policy is untouched.
    Enable-LocalUser -Name $accountName
    $shellCommand = if ($RestoreDesktop) { 'explorer.exe' } else { '"' + (Join-Path $installRoot 'JukeboxShell.exe') + '"' }
    $receipt.Shell = [JukeboxProvisioning]::ConfigureShell($accountName, $temporaryPassword, $shellCommand)
    if ($receipt.Shell -ne $shellCommand) { throw 'Kullanici kabugu dogrulanamadi.' }
    $receipt.AccessChecks=@()
    if (-not $RestoreDesktop) {
        foreach ($check in @(@((Join-Path $installRoot 'neon_jukebox.exe'),$false), @((Join-Path $installRoot 'JukeboxShell.exe'),$false), @($libraryRoot,$true), @($preferenceRoot,$true))) {
            $outcome = [JukeboxProvisioning]::Probe($accountName, $temporaryPassword, $check[0], $check[1])
            $receipt.AccessChecks += [pscustomobject]@{Path=$check[0];Write=$check[1];Result=$outcome}
            if ($outcome -ne 'OK') { throw ('Dosya erisimi dogrulanamadi: ' + $outcome) }
        }
        $receipt.ApplicationSHA256=(Get-FileHash -LiteralPath (Join-Path $installRoot 'neon_jukebox.exe') -Algorithm SHA256).Hash
    }
    $globalAfter = Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon' | Select-Object Shell,AutoAdminLogon
    if ($globalBefore.Shell -ne $globalAfter.Shell -or $globalBefore.AutoAdminLogon -ne $globalAfter.AutoAdminLogon) { throw 'Genel Windows oturum ayarlari beklenmedik sekilde degisti.' }
    Set-BlankPassword
    $temporaryPassword=$null
    Enable-LocalUser -Name $accountName
    $receipt.Status = if ($RestoreDesktop) { 'DesktopRestored' } else { 'Complete' }
    $receipt.UpdatedAt=(Get-Date).ToString('o')
    Save-Receipt
    $success=$true
    if ($RestoreDesktop) {
        Show-Result "Jukebox hesabi normal Windows masaustune geri alindi. Hesap ve uygulama dosyalari korundu."
    } else {
        Show-Result "Kurulum tamamlandi. Giris ekranindan Jukebox hesabini secin; parola gerekmiyor. Neon Jukebox masaustu yerine acilir. Uygulama kapanirsa yeniden baslatilir.`r`n`r`nIlk acilista uygulama PIN'ini ve muzik/video kaynaklarini belirleyin. Ag paylasimlari icin Jukebox hesabina erisim gerekebilir.`r`n`r`nDiger hesabiniza donmek icin Ctrl+Alt+Delete > Kullanici degistir. Kurulum kaydi: $receiptPath"
    }
} catch {
    $message=$_.Exception.Message
    if ($managedAccount) {
        Disable-LocalUser -Name $accountName -ErrorAction SilentlyContinue
        try { Set-BlankPassword } catch { }
        if ($receipt) { $receipt.Status='Failed'; $receipt.Error=$message; $receipt.UpdatedAt=(Get-Date).ToString('o'); Save-Receipt }
        $message += "`r`nJukebox hesabi kurulum tamamlanana kadar devre disi birakildi. Ayni betik yeniden calistirilabilir."
    }
    Show-Result $message $true
    exit 1
} finally {
    $temporaryPassword=$null
}
