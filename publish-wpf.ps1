<#
.SYNOPSIS
    ROUNDTABLE Publish Wizard -- WPF standalone GUI.
    Double-click publish-gui.bat to launch.
#>

# ── Load WPF assemblies ───────────────────────────────────────────────────
Add-Type -AssemblyName PresentationFramework, PresentationCore, WindowsBase, System.Xaml

$ScriptDir = Split-Path -Parent $PSCommandPath

# ── State variables ──────────────────────────────────────────────────────
$script:Version         = $null
$script:LastTag         = $null
$script:InstallerPath   = $null
$script:ReleaseUrl      = $null
$script:VersionOk       = $false
$script:CommitDone      = $false
$script:Resumed         = $false
$script:IsResumeRestore = $false
$script:ChangelogNotes   = ""
$script:Cancelled        = $false
$script:PushModeOnly     = $false
$script:WaitingForInput  = $null
$script:StateFilePath   = Join-Path $ScriptDir ".publish-state.json"
$script:StepStatuses    = @("idle","idle","idle","idle","idle","idle","idle","idle")

$stepLabels = @(
    "Check prerequisites",
    "Determine version",
    "Write version.h & commit",
    "Tag & push to GitHub",
    "Build Release EXE",
    "Build installer",
    "Create GitHub Release",
    "Done"
)

$script:StepIcons  = @{ idle = "o"; running = ">"; ok = "+"; fail = "x"; skip = "-" }

# ── Log system ────────────────────────────────────────────────────────────
$script:LogLines = [System.Collections.ArrayList]@()

function Add-Log {
    param([string]$Text, [string]$Color = "WhiteSmoke")
    $script:LogLines.Add(@{ Text = $Text; Color = $Color }) | Out-Null
    $syncHash.Window.Dispatcher.Invoke([Action]{
        if ($syncHash.LogBox) {
            $para = New-Object System.Windows.Documents.Paragraph
            $run  = New-Object System.Windows.Documents.Run($Text)
            $run.Foreground = [System.Windows.Media.BrushConverter]::new().ConvertFromString($Color)
            [void]$para.Inlines.Add($run)
            [void]$syncHash.LogBox.Document.Blocks.Add($para)
            $syncHash.LogBox.ScrollToEnd()
        }
    }, "Normal")
}

# ── Step helpers ─────────────────────────────────────────────────────────
function Set-StepStatus {
    param([int]$Index, [string]$Status)
    if ($Index -ge 0 -and $Index -lt $script:StepStatuses.Count) {
        $script:StepStatuses[$Index] = $Status
    }
    $syncHash.Window.Dispatcher.Invoke([Action]{ Update-StepUI }, "Normal")
}

function Update-StepUI {
    for ($i = 0; $i -lt $stepLabels.Count; $i++) {
        $st = $script:StepStatuses[$i]
        $iconChar = $script:StepIcons[$st]
        $border = $script:StepBorders[$i]
        $iconEl = $script:StepIconsUI[$i]
        $label  = $script:StepLabelsUI[$i]
        if (-not $border) { continue }

        $iconEl.Text = $iconChar

        # Dim publish-only steps (5-8) in Push mode
        $dimmed = $script:PushModeOnly -and $i -ge 4
        $border.Opacity = if ($dimmed) { 0.4 } else { 1.0 }

        switch ($st) {
            "ok" {
                $border.Background = [System.Windows.Media.BrushConverter]::new().ConvertFromString("#FF1A3A2F")
                $iconEl.Foreground = [System.Windows.Media.BrushConverter]::new().ConvertFromString("#FFA6E3A1")
                $label.FontWeight = "Normal"
            }
            "running" {
                $border.Background = [System.Windows.Media.BrushConverter]::new().ConvertFromString("#FF1A2A3F")
                $iconEl.Foreground = [System.Windows.Media.BrushConverter]::new().ConvertFromString("#FF89B4FA")
                $label.FontWeight = "Bold"
            }
            "fail" {
                $border.Background = [System.Windows.Media.BrushConverter]::new().ConvertFromString("#FF3A1E2E")
                $iconEl.Foreground = [System.Windows.Media.BrushConverter]::new().ConvertFromString("#FFF38BA8")
                $label.FontWeight = "Normal"
            }
            default {
                $border.Background = [System.Windows.Media.BrushConverter]::new().ConvertFromString("#00FFFFFF")
                $iconEl.Foreground = [System.Windows.Media.BrushConverter]::new().ConvertFromString("#FF6C7086")
                $label.FontWeight = "Normal"
            }
        }
    }
}

function Test-Cancelled {
    if ($script:Cancelled) {
        Add-Log "[CANCELLED] Operation cancelled by user." -Color Orange
        return $true
    }
    return $false
}

function Save-State {
    $state = @{
        version        = $script:Version
        releaseUrl     = $script:ReleaseUrl
        installerPath  = $script:InstallerPath
        changelogNotes = $script:ChangelogNotes
        completedSteps = @()
        updatedAt      = (Get-Date -Format "o")
    }
    for ($i = 0; $i -lt $stepLabels.Count; $i++) {
        if ($script:StepStatuses[$i] -eq "ok") {
            $state.completedSteps += $i
        }
    }
    Set-Content -Path $script:StateFilePath -Value ($state | ConvertTo-Json -Compress) -Encoding UTF8
}

function Load-State {
    if (-not (Test-Path $script:StateFilePath)) { return $null }
    try { return (Get-Content $script:StateFilePath -Raw -Encoding UTF8 | ConvertFrom-Json) } catch { return $null }
}

function Clear-State {
    if (Test-Path $script:StateFilePath) { Remove-Item $script:StateFilePath -Force }
}

function Get-VersionFromTag {
    $lastTag = & git describe --tags --abbrev=0 2>$null
    if ($lastTag) {
        $parts = ($lastTag -replace '^v') -split '\.'
        return "$($parts[0]).$([int]$parts[1] + 1)"
    }
    return "0.1"
}

# ── Step runner ───────────────────────────────────────────────────────────
function Invoke-Step {
    param([scriptblock]$ScriptBlock, [string]$StepName, [int]$StepIndex)
    Set-StepStatus -Index $StepIndex -Status running
    Add-Log "[$StepName] Starting..." -Color DodgerBlue
    try {
        & $ScriptBlock
        # If step set WaitingForInput, don't mark as done — user needs to confirm first
        if ($script:WaitingForInput) {
            Add-Log "[$StepName] Waiting for input..." -Color Orange
            return $true
        }
        Set-StepStatus -Index $StepIndex -Status ok
        Add-Log "[$StepName] Completed." -Color Green
        Save-State
        return $true
    } catch {
        if ($script:Cancelled) {
            Set-StepStatus -Index $StepIndex -Status idle
            Add-Log "[$StepName] Cancelled." -Color Orange
        } else {
            Set-StepStatus -Index $StepIndex -Status fail
            Add-Log "[$StepName] FAILED: $_" -Color Red
        }
        return $false
    }
}

# ===========================================================================
# STEP FUNCTIONS
# ===========================================================================
function Step1-CheckPrerequisites {
    Add-Log "Checking prerequisites..." -Color Gray
    $tools = @{ Git = "git"; "GitHub CLI (gh)" = "gh"; "Inno Setup (iscc)" = "iscc" }
    $allOk = $true
    foreach ($name in $tools.Keys) {
        $found = Get-Command $tools[$name] -ErrorAction SilentlyContinue
        if ($found) { Add-Log "  [OK] $name found: $($found.Source)" -Color Green }
        else { Add-Log "  [MISSING] $name NOT found" -Color Red; $allOk = $false }
    }
    $null = & gh auth status 2>&1
    if ($LASTEXITCODE -eq 0) { Add-Log "  [OK] GitHub CLI authenticated" -Color Green }
    else { Add-Log "  [MISSING] GitHub CLI not authenticated" -Color Red; $allOk = $false }
    if (-not $allOk) { throw "Prerequisites check failed." }
}

$script:WaitingForInput = $null  # "version" or "commit" — set when a step needs user input

function Step2-DetermineVersion {
    $script:LastTag = & git describe --tags --abbrev=0 2>$null
    $computedVersion = Get-VersionFromTag
    if ($script:LastTag) { Add-Log "Last tag: $($script:LastTag)" -Color Gray } else { Add-Log "Last tag: (none)" -Color Gray }

    if ($script:Resumed -and $script:Version -and $script:Version -eq $computedVersion) {
        Add-Log "Version from saved state: v$($script:Version)" -Color DarkBlue
        return
    }

    $script:Version = $computedVersion
    Add-Log "Next version: v$($script:Version)" -Color DarkBlue

    if ($script:VersionOk) {
        # Already confirmed — skip waiting
        Add-Log "Version confirmed: v$($script:Version)" -Color DarkBlue
        return
    }

    # Show version input and return — user clicks "Run Current Step" again after confirming
    $syncHash.Window.Dispatcher.Invoke([Action]{
        $syncHash.VersionInput.Text = $script:Version
        $syncHash.VersionBar.Visibility = "Visible"
        $syncHash.VersionInput.Focus()
        $syncHash.VersionInput.SelectAll()
    }, "Normal")

    $script:WaitingForInput = "version"
}

function Step3-WriteVersionAndCommit {
    $ver = $script:Version
    $header = @"
// ROUNDTABLE version -- written by publish-gui.ps1
#pragma once

#define ROUNDTABLE_VERSION "$ver"
"@
    Set-Content -Path (Join-Path $ScriptDir "src\version.h") -Value $header -Encoding ASCII
    Add-Log "Written: src\version.h (v$ver)" -Color Green
    Add-Log "Next step: commit and push to GitHub." -Color Gray

    & git diff --quiet HEAD
    $dirty = ($LASTEXITCODE -ne 0)

    if ($dirty) {
        if ($script:CommitDone) {
            Add-Log "Already committed." -Color Green
            return
        }
        $msg = "Release v$ver"
        $script:CommitDone = $false
        $syncHash.Window.Dispatcher.Invoke([Action]{
            $syncHash.CommitMsgInput.Text = $msg
            $syncHash.CommitBar.Visibility = "Visible"
            $syncHash.CommitBtn.Focus()
        }, "Normal")
        Add-Log "Uncommitted changes found - review the commit message and click Commit." -Color Orange
        Add-Log "(This is just for git log - e.g. 'Release v0.13')." -Color Gray
        Add-Log "Put your actual changelog/patch notes in the Release Notes panel below." -Color Gray
        $script:WaitingForInput = "commit"
    } else {
        Add-Log "No uncommitted changes - creating empty commit." -Color Gray
        git commit --allow-empty -m "Release v$ver" 2>&1 | ForEach-Object { Add-Log "  $_" }
        if ($LASTEXITCODE -ne 0) { throw "Empty commit failed." }
    }
}

function Step4-TagAndPush {
    $ver = "v$($script:Version)"
    $existing = git tag -l $ver
    if ($existing) {
        Add-Log "Tag '$ver' already exists locally. Overwriting." -Color Yellow
        git tag -f $ver 2>&1 | ForEach-Object { Add-Log "  $_" }
    } else {
        git tag $ver 2>&1 | ForEach-Object { Add-Log "  $_" }
        if ($LASTEXITCODE -ne 0) { throw "Tag creation failed." }
        Add-Log "Tagged: $ver" -Color Green
    }
    Add-Log "Pushing commit and tag to origin..." -Color DarkBlue
    $pushOutput = git push --atomic origin HEAD $ver 2>&1 | Out-String
    $pushOutput.Trim() -split "`n" | ForEach-Object { Add-Log "  $_" }
    if ($LASTEXITCODE -ne 0) {
        if ($pushOutput -match "! \[rejected\]") {
            Add-Log "Remote tag conflict. Force-pushing..." -Color Yellow
            git push --atomic --force origin HEAD $ver 2>&1 | ForEach-Object { Add-Log "  $_" }
            if ($LASTEXITCODE -ne 0) { throw "Force-push failed." }
        } else { throw "Push failed." }
    }
    Add-Log "Pushed: commit + $ver" -Color Green
}

function Step5-BuildRelease {
    $cmake = if (Test-Path (Join-Path $ScriptDir "tools\cmake\cmake-3.31.6-windows-x86_64\bin\cmake.exe")) {
        Join-Path $ScriptDir "tools\cmake\cmake-3.31.6-windows-x86_64\bin\cmake.exe"
    } else { "cmake"; Add-Log "Using system cmake" -Color Gray }
    $cmakeBinDir = Split-Path -Parent $cmake
    $env:PATH = "${cmakeBinDir};${env:PATH}"

    $cachePath = Join-Path $ScriptDir "build\CMakeCache.txt"
    if (Test-Path $cachePath) {
        $expectedCmd = $cmake -replace '\\', '/'
        $content = Get-Content $cachePath
        for ($i = 0; $i -lt $content.Count; $i++) {
            if ($content[$i] -match '^CMAKE_COMMAND:') {
                $content[$i] = "CMAKE_COMMAND:INTERNAL=$expectedCmd"
                break
            }
        }
        $content | Set-Content $cachePath
    }

    Add-Log "Building roundtable (Release)..." -Color DarkBlue
    & $cmake --build (Join-Path $ScriptDir "build") --config Release --target roundtable --parallel 2>&1 | ForEach-Object { Add-Log "  $_" }
    if ($LASTEXITCODE -ne 0) { throw "Build failed." }
    $builtIcon = Join-Path $ScriptDir "build\icon.ico"
    if (Test-Path $builtIcon) { Copy-Item $builtIcon (Join-Path $ScriptDir "icon.ico") -Force; Add-Log "Updated installer icon" -Color Green }
    Add-Log "Build completed." -Color Green
}

function Step6-BuildInstaller {
    $iscc = (Get-Command iscc -ErrorAction SilentlyContinue).Source
    if (-not $iscc) {
        $paths = @("C:\Program Files (x86)\Inno Setup 6\ISCC.exe", "C:\Program Files\Inno Setup 6\ISCC.exe")
        $iscc = $paths | Where-Object { Test-Path $_ } | Select-Object -First 1
    }
    if (-not $iscc) { Add-Log "Inno Setup not found - skipping installer." -Color Yellow; $script:InstallerPath = $null; return }
    Add-Log "Using: $iscc" -Color Gray
    & $iscc "/DMyAppVersion=$($script:Version)" (Join-Path $ScriptDir "installer.iss") 2>&1 | ForEach-Object { Add-Log "  $_" }
    if ($LASTEXITCODE -ne 0) { throw "Inno Setup failed." }
    $script:InstallerPath = Join-Path $ScriptDir "dist\Roundtable-NLE-v$($script:Version)-Setup.exe"
    if (-not (Test-Path $script:InstallerPath)) { throw "Installer not found." }
    Add-Log "Installer: $($script:InstallerPath)" -Color Green
}

function Step7-CreateRelease {
    $ver = "v$($script:Version)"
    $notesFile = [System.IO.Path]::GetTempFileName()
    $repoUrl = "https://github.com/ROUNDTABLE-TALK/roundtable"
    $remoteUrl = & git remote get-url origin 2>$null
    if ($remoteUrl -match 'github\.com[/:](.+?)(\.git)?$') { $repoUrl = "https://github.com/$($matches[1] -replace '\.git$','')" }

    $notes = "Roundtable-NLE $ver -- Non-Linear Editor for Tier Lists`n`n"
    if ($script:ChangelogNotes) {
        $notes += "== Changelog ==`n`n$($script:ChangelogNotes)`n`n"
        Add-Log "Using your release notes from the 'Release Notes' panel." -Color Gray
    } else {
        Add-Log "No custom release notes found - using default template." -Color Yellow
        Add-Log "Tip: For next time, type changelog in the 'Release Notes' panel." -Color Gray
    }
    $notes += "== Download ==`n`nDownload the installer attached below.`n`n--`n$repoUrl"
    Set-Content $notesFile -Value $notes -Encoding UTF8

    Add-Log "Creating GitHub Release $ver..." -Color DarkBlue
    $relArgs = @("release", "create", $ver)
    if ($script:InstallerPath -and (Test-Path $script:InstallerPath)) { $relArgs += $script:InstallerPath }
    $relArgs += "--title", "Roundtable-NLE $ver", "--notes-file", $notesFile
    & gh $relArgs 2>&1 | ForEach-Object { Add-Log "  $_" }
    if ($LASTEXITCODE -ne 0) {
        & gh release view $ver --json tagName 2>$null
        if ($LASTEXITCODE -eq 0) {
            Add-Log "Release exists. Deleting and recreating..." -Color Yellow
            & gh release delete $ver --yes 2>&1 | Out-Null
            & gh $relArgs 2>&1 | ForEach-Object { Add-Log "  $_" }
            if ($LASTEXITCODE -ne 0) { throw "Release creation failed." }
        } else { throw "Release creation failed." }
    }
    $script:ReleaseUrl = "$repoUrl/releases/tag/$ver"
    Add-Log "Published: $script:ReleaseUrl" -Color Green
}

function Step8-Done {
    Add-Log ("=" * 60) -Color DarkBlue
    Add-Log "  SUCCESS: v$($script:Version) published!" -Color Green
    if ($script:ReleaseUrl) { Add-Log "  $($script:ReleaseUrl)" -Color DarkBlue }
    Add-Log ("=" * 60) -Color DarkBlue
    Clear-State
    $syncHash.Window.Dispatcher.Invoke([Action]{
        $syncHash.BtnOpenBrowser.Visibility = "Visible"
        $syncHash.BtnFinish.Visibility      = "Visible"
        $syncHash.BtnRunStep.IsEnabled      = $false
        $syncHash.BtnPublishAll.IsEnabled   = $false
        $syncHash.BtnPushAll.IsEnabled      = $false
    }, "Normal")
}

# ── Helper: wait for user input using WPF message pump ──────────────
function Wait-ForInput {
    Add-Log "Waiting for you to confirm in the dialog above..." -Color Orange
    while ($script:WaitingForInput -and -not $script:Cancelled) {
        # Pump WPF messages without blocking the UI thread
        $frame = New-Object System.Windows.Threading.DispatcherFrame
        $timer = New-Object System.Windows.Threading.DispatcherTimer
        $timer.Interval = [TimeSpan]::FromMilliseconds(50)
        $timer.Add_Tick({ $frame.Continue = $false; $timer.Stop() })
        $timer.Start()
        [System.Windows.Threading.Dispatcher]::PushFrame($frame)
    }
}

$stepFunctions = @(
    ${function:Step1-CheckPrerequisites},
    ${function:Step2-DetermineVersion},
    ${function:Step3-WriteVersionAndCommit},
    ${function:Step4-TagAndPush},
    ${function:Step5-BuildRelease},
    ${function:Step6-BuildInstaller},
    ${function:Step7-CreateRelease},
    ${function:Step8-Done}
)

# ===========================================================================
# XAML DESIGN
# ===========================================================================

$xaml = @'
<Window xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
        xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
        Title="Roundtable-NLE  |  Release Wizard"
        Height="680" Width="1140" MinWidth="900" MinHeight="600"
        WindowStartupLocation="CenterScreen"
        Background="#1E1E2E"
        FontFamily="Segoe UI" FontSize="13"
        TextElement.Foreground="#FFCDD6F4"
        SnapsToDevicePixels="True"
        UseLayoutRounding="True">

  <Window.Resources>
    <!-- Color palette: Catppuccin Mocha -->
    <Color x:Key="BgDark">#181825</Color>
    <Color x:Key="BgInput">#11111B</Color>
    <Color x:Key="Accent">#89B4FA</Color>
    <Color x:Key="Green">#A6E3A1</Color>
    <Color x:Key="Red">#F38BA8</Color>
    <Color x:Key="Orange">#FAB387</Color>
    <Color x:Key="FgDim">#6C7086</Color>
    <Color x:Key="Fg">#CDD6F4</Color>

    <!-- Scrollbar style -->
    <Style TargetType="ScrollBar">
      <Setter Property="Background" Value="#181825"/>
      <Setter Property="Foreground" Value="#313244"/>
    </Style>

    <!-- Button base style -->
    <Style x:Key="ToolBtn" TargetType="Button">
      <Setter Property="Foreground" Value="#FFCDD6F4"/>
      <Setter Property="Background" Value="#FF2A3A5E"/>
      <Setter Property="BorderThickness" Value="0"/>
      <Setter Property="Padding" Value="14,6"/>
      <Setter Property="FontWeight" Value="Bold"/>
      <Setter Property="FontSize" Value="12.5"/>
      <Setter Property="Cursor" Value="Hand"/>
      <Setter Property="Template">
        <Setter.Value>
          <ControlTemplate TargetType="Button">
            <Border Background="{TemplateBinding Background}" CornerRadius="6"
                    BorderThickness="0" Padding="{TemplateBinding Padding}">
              <ContentPresenter HorizontalAlignment="Center" VerticalAlignment="Center"/>
            </Border>
          </ControlTemplate>
        </Setter.Value>
      </Setter>
      <Style.Triggers>
        <Trigger Property="IsMouseOver" Value="True">
          <Setter Property="Opacity" Value="0.85"/>
        </Trigger>
        <Trigger Property="IsEnabled" Value="False">
          <Setter Property="Opacity" Value="0.4"/>
        </Trigger>
      </Style.Triggers>
    </Style>

    <!-- Input textbox style -->
    <Style x:Key="InputBox" TargetType="TextBox">
      <Setter Property="Background" Value="#FF11111B"/>
      <Setter Property="Foreground" Value="#FFCDD6F4"/>
      <Setter Property="BorderBrush" Value="#FF313244"/>
      <Setter Property="BorderThickness" Value="1"/>
      <Setter Property="Padding" Value="6,3"/>
      <Setter Property="FontSize" Value="13"/>
      <Setter Property="CaretBrush" Value="#FF89B4FA"/>
    </Style>
  </Window.Resources>

  <Grid>
    <Grid.ColumnDefinitions>
      <ColumnDefinition Width="250"/>
      <ColumnDefinition Width="*"/>
    </Grid.ColumnDefinitions>

    <!-- ════ SIDEBAR ════ -->
    <Border Grid.Column="0" Background="#181825" BorderBrush="#2D2D44" BorderThickness="0,0,1,0">
      <Grid Margin="0,0,0,0">
        <Grid.RowDefinitions>
          <RowDefinition Height="Auto"/>
          <RowDefinition Height="*"/>
        </Grid.RowDefinitions>

        <!-- Mode selector -->
        <Border Grid.Row="0" Margin="12,14,12,10" Background="#11111B" CornerRadius="8" Height="34">
          <Grid>
            <Grid.ColumnDefinitions>
              <ColumnDefinition/>
              <ColumnDefinition/>
            </Grid.ColumnDefinitions>
            <Button x:Name="BtnModePush" Grid.Column="0" Content="  Push"
                    Background="#11111B" Foreground="#6C7086"
                    BorderThickness="0" FontWeight="Bold" FontSize="13"
                    Margin="2" Cursor="Hand"/>
            <Button x:Name="BtnModePublish" Grid.Column="1" Content="  Publish"
                    Background="#2A3A5E" Foreground="#89B4FA"
                    BorderThickness="0" FontWeight="Bold" FontSize="13"
                    Margin="2" Cursor="Hand"/>
          </Grid>
        </Border>

        <!-- Step list (built programmatically in code) -->
        <ScrollViewer Grid.Row="1" VerticalScrollBarVisibility="Auto"
                       Margin="0,0,0,8" Background="Transparent">
          <StackPanel x:Name="StepListPanel" Margin="0,4,0,0"/>
        </ScrollViewer>
      </Grid>
    </Border>

    <!-- ════ CONTENT ════ -->
    <Grid Grid.Column="1" Margin="14,12,14,10">
      <Grid.RowDefinitions>
        <RowDefinition Height="*"/>
        <RowDefinition Height="Auto"/>
        <RowDefinition Height="Auto"/>
        <RowDefinition Height="Auto"/>
        <RowDefinition Height="Auto"/>
      </Grid.RowDefinitions>

      <!-- Log output -->
      <Border Grid.Row="0" Background="#11111B" CornerRadius="8"
              BorderBrush="#2D2D44" BorderThickness="1" Margin="0,0,0,8">
        <ScrollViewer VerticalScrollBarVisibility="Auto">
          <RichTextBox x:Name="LogBox" Background="Transparent"
                       BorderThickness="0" IsReadOnly="True"
                       FontFamily="Consolas" FontSize="12"
                       Foreground="#FFBAC2DE"
                       IsDocumentEnabled="True"
                       VerticalScrollBarVisibility="Hidden"/>
        </ScrollViewer>
      </Border>

      <!-- Version input bar -->
      <Border x:Name="VersionBar" Grid.Row="1" Background="#181825"
              CornerRadius="8" Padding="12,6" Margin="0,0,0,6"
              BorderBrush="#2D2D44" BorderThickness="1"
              Visibility="Collapsed">
        <StackPanel Orientation="Horizontal">
          <TextBlock Text="Version number:" VerticalAlignment="Center"
                     Foreground="#FFCDD6F4" FontWeight="SemiBold"
                     Margin="0,0,8,0" FontSize="12"/>
          <TextBox x:Name="VersionInput" Width="100" Style="{StaticResource InputBox}"/>
          <Button x:Name="VersionConfirmBtn" Content="Confirm"
                  Style="{StaticResource ToolBtn}"
                  Background="#89B4FA" Foreground="#1E1E2E"
                  Margin="8,0,0,0" Padding="12,4"/>
          <TextBlock VerticalAlignment="Center"
                     Foreground="#FF585B70" FontSize="11" FontStyle="Italic"
                     Margin="18,0,0,0">
            <Run Text="Git tag: v"/><Run x:Name="CommitPreview" Text="X.Y"/><Run Text="  |  Commit msg: "/><Run Text="Release v"/><Run Text="X.Y"/>
          </TextBlock>
        </StackPanel>
      </Border>

      <!-- Commit input bar (shown by Step3) -->
      <Border x:Name="CommitBar" Grid.Row="2" Background="#181825"
              CornerRadius="8" Padding="12,6" Margin="0,0,0,6"
              BorderBrush="#2D2D44" BorderThickness="1"
              Visibility="Collapsed">
        <StackPanel Orientation="Horizontal">
          <TextBlock VerticalAlignment="Center"
                     Foreground="#FFCDD6F4" FontWeight="SemiBold"
                     Margin="0,0,8,0" FontSize="12">
            <Run Text="Git commit message:"/>
            <Run Text=" (one-liner, appears in git log)" Foreground="#FF585B70" FontSize="10" FontStyle="Italic"/>
          </TextBlock>
          <TextBox x:Name="CommitMsgInput" Width="300" Style="{StaticResource InputBox}"/>
          <Button x:Name="CommitBtn" Content="Commit"
                  Style="{StaticResource ToolBtn}"
                  Background="#89B4FA" Foreground="#1E1E2E"
                  Margin="8,0,0,0" Padding="12,4"/>
        </StackPanel>
      </Border>

      <!-- Notes panel -->
      <Border x:Name="NotesPanel" Grid.Row="3" Background="#181825"
              CornerRadius="8" Margin="0,0,0,6"
              BorderBrush="#2D2D44" BorderThickness="1"
              Visibility="Visible">
        <Grid>
          <Grid.RowDefinitions>
            <RowDefinition Height="Auto"/>
            <RowDefinition Height="Auto"/>
            <RowDefinition Height="Auto"/>
          </Grid.RowDefinitions>

          <!-- Notes header -->
          <Border x:Name="NotesHeader" Grid.Row="0" Background="#1E1E30" CornerRadius="8,8,0,0"
                  Padding="12,6" Cursor="Hand">
            <Grid>
              <Grid.ColumnDefinitions>
                <ColumnDefinition Width="Auto"/>
                <ColumnDefinition Width="*"/>
                <ColumnDefinition Width="Auto"/>
              </Grid.ColumnDefinitions>
              <TextBlock Grid.Column="0" Text="Release Notes"
                         FontWeight="Bold" FontSize="13"
                         VerticalAlignment="Center"/>
              <Border Grid.Column="0" Margin="8,0,0,0"
                      Background="#2A3A5E" CornerRadius="10"
                      Padding="8,2" VerticalAlignment="Center">
                <TextBlock Text="Used in Step 7" FontSize="10"
                           Foreground="#89B4FA"/>
              </Border>
              <TextBlock x:Name="NotesToggle" Grid.Column="2"
                         Text="v" FontSize="10"
                         Foreground="#FF6C7086"
                         VerticalAlignment="Center" Cursor="Hand"
                         Margin="0,0,4,0"/>
            </Grid>
          </Border>

          <!-- Notes editor body -->
          <Border x:Name="NotesBody" Grid.Row="1" Background="#11111B"
                  Height="150" Visibility="Visible">
            <Grid>
              <Grid.RowDefinitions>
                <RowDefinition Height="*"/>
                <RowDefinition Height="Auto"/>
              </Grid.RowDefinitions>
              <TextBox x:Name="NotesEditor" Grid.Row="0"
                       Background="#11111B" Foreground="#FFCDD6F4"
                       BorderThickness="0" FontFamily="Consolas"
                       FontSize="12.5" AcceptsReturn="True"
                       TextWrapping="Wrap" Padding="10,8"
                       VerticalScrollBarVisibility="Auto"
                       SpellCheck.IsEnabled="True"/>
              <Border Grid.Row="1" Background="#1A1A28" Padding="10,4">
                <Grid>
                  <Grid.ColumnDefinitions>
                    <ColumnDefinition Width="*"/>
                    <ColumnDefinition Width="Auto"/>
                  </Grid.ColumnDefinitions>
                  <TextBlock x:Name="NotesStats" Grid.Column="0"
                             Text="0 lines, 0 chars"
                             Foreground="#FF6C7086" FontSize="11"/>
                  <TextBlock Grid.Column="1" Text="Spellcheck active - inserted into GitHub Release (Step 7)"
                             Foreground="#FF585B70" FontSize="11"
                             FontStyle="Italic"/>
                </Grid>
              </Border>
            </Grid>
          </Border>
        </Grid>
      </Border>

      <!-- Toolbar -->
      <Border Grid.Row="4" Background="#181825" CornerRadius="8"
              Padding="8,6" BorderBrush="#2D2D44" BorderThickness="1">
        <StackPanel Orientation="Horizontal">
          <Button x:Name="BtnRunStep" Content="Run Current Step"
                  Style="{StaticResource ToolBtn}"
                  Background="#2A3A5E" Foreground="#89B4FA"
                  Margin="0,0,6,0"/>
          <Button x:Name="BtnPushAll" Content="Run All Steps (Push)"
                  Style="{StaticResource ToolBtn}"
                  Background="#1E3A2F" Foreground="#A6E3A1"
                  Visibility="Collapsed" Margin="0,0,6,0"/>
          <Button x:Name="BtnPublishAll" Content="Run All Steps (Publish)"
                  Style="{StaticResource ToolBtn}"
                  Background="#2A3A5E" Foreground="#89B4FA"
                  Margin="0,0,6,0"/>
          <Button x:Name="BtnStartFresh" Content="Start Fresh"
                  Style="{StaticResource ToolBtn}"
                  Background="#3A2A1E" Foreground="#FAB387"
                  Margin="0,0,6,0"/>
          <Button x:Name="BtnOpenBrowser" Content="Open in Browser"
                  Style="{StaticResource ToolBtn}"
                  Background="#1E2A3F" Foreground="#89B4FA"
                  Visibility="Collapsed" Margin="0,0,6,0"/>
          <Button x:Name="BtnFinish" Content="Close"
                  Style="{StaticResource ToolBtn}"
                  Background="#2A3A2A" Foreground="#A6E3A1"
                  Visibility="Collapsed" Margin="0,0,6,0"/>
          <Button x:Name="BtnCancel" Content="Cancel"
                  Style="{StaticResource ToolBtn}"
                  Background="#3A1E2E" Foreground="#F38BA8"
                  HorizontalAlignment="Right"/>
        </StackPanel>
      </Border>
    </Grid>
  </Grid>
</Window>
'@

# ===========================================================================
# LOAD WINDOW
# ===========================================================================
$reader = New-Object System.Xml.XmlNodeReader ([xml]$xaml)
$window = [System.Windows.Markup.XamlReader]::Load($reader)

# ── Sync hash for easy control access ─────────────────────────────────────
$syncHash = @{ Window = $window }
$window.FindName("LogBox")              | ForEach-Object { $syncHash["LogBox"] = $_ }
$window.FindName("VersionInput")         | ForEach-Object { $syncHash["VersionInput"] = $_ }
$window.FindName("VersionBar")           | ForEach-Object { $syncHash["VersionBar"] = $_ }
$window.FindName("VersionConfirmBtn")    | ForEach-Object { $syncHash["VersionConfirmBtn"] = $_ }
$window.FindName("CommitPreview")        | ForEach-Object { $syncHash["CommitPreview"] = $_ }
$window.FindName("CommitBar")            | ForEach-Object { $syncHash["CommitBar"] = $_ }
$window.FindName("CommitMsgInput")       | ForEach-Object { $syncHash["CommitMsgInput"] = $_ }
$window.FindName("CommitBtn")            | ForEach-Object { $syncHash["CommitBtn"] = $_ }
$window.FindName("NotesPanel")           | ForEach-Object { $syncHash["NotesPanel"] = $_ }
$window.FindName("NotesBody")            | ForEach-Object { $syncHash["NotesBody"] = $_ }
$window.FindName("NotesEditor")          | ForEach-Object { $syncHash["NotesEditor"] = $_ }
$window.FindName("NotesToggle")          | ForEach-Object { $syncHash["NotesToggle"] = $_ }
$window.FindName("NotesHeader")          | ForEach-Object { $syncHash["NotesHeader"] = $_ }
$window.FindName("NotesStats")           | ForEach-Object { $syncHash["NotesStats"] = $_ }
$window.FindName("BtnRunStep")           | ForEach-Object { $syncHash["BtnRunStep"] = $_ }
$window.FindName("BtnPushAll")           | ForEach-Object { $syncHash["BtnPushAll"] = $_ }
$window.FindName("BtnPublishAll")        | ForEach-Object { $syncHash["BtnPublishAll"] = $_ }
$window.FindName("BtnStartFresh")        | ForEach-Object { $syncHash["BtnStartFresh"] = $_ }
$window.FindName("BtnOpenBrowser")       | ForEach-Object { $syncHash["BtnOpenBrowser"] = $_ }
$window.FindName("BtnFinish")            | ForEach-Object { $syncHash["BtnFinish"] = $_ }
$window.FindName("BtnCancel")            | ForEach-Object { $syncHash["BtnCancel"] = $_ }
$window.FindName("BtnModePush")          | ForEach-Object { $syncHash["BtnModePush"] = $_ }
$window.FindName("BtnModePublish")       | ForEach-Object { $syncHash["BtnModePublish"] = $_ }

# ── Build step list programmatically ──────────────────────────────────────
$syncHash.StepListPanel = $window.FindName("StepListPanel")
$script:StepBorders  = @()
$script:StepIconsUI  = @()
$script:StepLabelsUI = @()

$thn8_2 = [System.Windows.Thickness]::new(8,2,8,2)
$thn8_6 = [System.Windows.Thickness]::new(8,6,8,6)
$thn4_0 = [System.Windows.Thickness]::new(4,0,0,0)
$cr6    = [System.Windows.CornerRadius]::new(6)
$gl28   = [System.Windows.GridLength]::new(28)
$glStar = [System.Windows.GridLength]::new(1, [System.Windows.GridUnitType]::Star)
$cBold  = [System.Windows.FontWeights]::Bold

for ($i = 0; $i -lt $stepLabels.Count; $i++) {
    $border = New-Object System.Windows.Controls.Border
    $border.Margin = $thn8_2
    $border.Padding = $thn8_6
    $border.CornerRadius = $cr6
    $border.Background = [System.Windows.Media.Brushes]::Transparent
    $border.BorderThickness = [System.Windows.Thickness]::new(0)

    $grid = New-Object System.Windows.Controls.Grid
    $col1 = New-Object System.Windows.Controls.ColumnDefinition
    $col1.Width = $gl28
    $col2 = New-Object System.Windows.Controls.ColumnDefinition
    $col2.Width = $glStar
    [void]$grid.ColumnDefinitions.Add($col1)
    [void]$grid.ColumnDefinitions.Add($col2)

    $icon = New-Object System.Windows.Controls.TextBlock
    $icon.Text = "o"
    $icon.FontSize = 13
    $icon.FontWeight = $cBold
    $icon.Foreground = [System.Windows.Media.BrushConverter]::new().ConvertFromString("#FF6C7086")
    $icon.VerticalAlignment = "Center"
    $icon.HorizontalAlignment = "Center"
    [System.Windows.Controls.Grid]::SetColumn($icon, 0)
    [void]$grid.Children.Add($icon)

    $label = New-Object System.Windows.Controls.TextBlock
    $label.Text = "$($i+1). $($stepLabels[$i])"
    $label.FontSize = 13
    $label.Foreground = [System.Windows.Media.BrushConverter]::new().ConvertFromString("#FFCDD6F4")
    $label.VerticalAlignment = "Center"
    $label.Margin = $thn4_0
    [System.Windows.Controls.Grid]::SetColumn($label, 1)
    [void]$grid.Children.Add($label)

    $border.Child = $grid
    [void]$syncHash.StepListPanel.Children.Add($border)

    $script:StepBorders  += $border
    $script:StepIconsUI  += $icon
    $script:StepLabelsUI += $label
}

# ── Mode switching ────────────────────────────────────────────────────────
$syncHash.BtnModePush.Add_Click({
    $script:PushModeOnly = $true
    $syncHash.BtnModePush.Background = [System.Windows.Media.BrushConverter]::new().ConvertFromString("#FF2A3A2A")
    $syncHash.BtnModePush.Foreground = [System.Windows.Media.BrushConverter]::new().ConvertFromString("#FFA6E3A1")
    $syncHash.BtnModePublish.Background = [System.Windows.Media.BrushConverter]::new().ConvertFromString("#FF11111B")
    $syncHash.BtnModePublish.Foreground = [System.Windows.Media.BrushConverter]::new().ConvertFromString("#FF6C7086")
    $syncHash.BtnPushAll.Visibility = "Visible"
    $syncHash.BtnPublishAll.Visibility = "Collapsed"
    Update-StepUI
})

$syncHash.BtnModePublish.Add_Click({
    $script:PushModeOnly = $false
    $syncHash.BtnModePublish.Background = [System.Windows.Media.BrushConverter]::new().ConvertFromString("#FF2A3A5E")
    $syncHash.BtnModePublish.Foreground = [System.Windows.Media.BrushConverter]::new().ConvertFromString("#FF89B4FA")
    $syncHash.BtnModePush.Background = [System.Windows.Media.BrushConverter]::new().ConvertFromString("#FF11111B")
    $syncHash.BtnModePush.Foreground = [System.Windows.Media.BrushConverter]::new().ConvertFromString("#FF6C7086")
    $syncHash.BtnPushAll.Visibility = "Collapsed"
    $syncHash.BtnPublishAll.Visibility = "Visible"
    Update-StepUI
})

# ── Version confirmation ──────────────────────────────────────────────────
$syncHash.VersionConfirmBtn.Add_Click({
    $val = $syncHash.VersionInput.Text.Trim()
    if ($val -match '^\d+\.\d+$') {
        $script:Version = $val
        $script:VersionOk = $true
        $script:WaitingForInput = $null
        Add-Log "Version confirmed: v$val" -Color DarkBlue
        $syncHash.VersionBar.Visibility = "Collapsed"
        Add-Log "Now write your changelog in the Release Notes panel (below) - it goes into Step 7." -Color Gray
        Add-Log "Continuing..." -Color Gray
    } else {
        [System.Windows.MessageBox]::Show("Version must be X.Y (e.g. 0.16)", "Invalid", "OK", "Warning")
    }
})

$syncHash.VersionInput.Add_TextChanged({
    $v = $syncHash.VersionInput.Text.Trim()
    if ([string]::IsNullOrEmpty($v)) { $v = "X.Y" }
    $syncHash.CommitPreview.Text = $v
})

# ── Commit ────────────────────────────────────────────────────────────────
$syncHash.CommitBtn.Add_Click({
    $msg = $syncHash.CommitMsgInput.Text.Trim()
    if ([string]::IsNullOrEmpty($msg)) { $msg = "Release v$($script:Version)" }
    Add-Log "Committing: $msg" -Color DarkBlue
    git add -A 2>&1 | ForEach-Object { Add-Log "  $_" }
    git commit -m $msg 2>&1 | ForEach-Object { Add-Log "  $_" }
    if ($LASTEXITCODE -eq 0) {
        Add-Log "Committed: $msg" -Color Green
        $script:CommitDone = $true
        $script:WaitingForInput = $null
        $syncHash.CommitBar.Visibility = "Collapsed"
        Add-Log "Continuing..." -Color Gray
    } else {
        Add-Log "Commit failed. Fix and click Commit again." -Color Red
    }
})

# ── Notes toggle (starts expanded) ────────────────────────────────────────
$script:NotesExpanded = $true
function Toggle-Notes {
    $script:NotesExpanded = -not $script:NotesExpanded
    $syncHash.NotesBody.Visibility = if ($script:NotesExpanded) { "Visible" } else { "Collapsed" }
    $syncHash.NotesToggle.Text = if ($script:NotesExpanded) { "^" } else { "v" }
}
$syncHash.NotesHeader.Add_MouseLeftButtonUp({ Toggle-Notes })
$syncHash.NotesToggle.Add_MouseLeftButtonUp({ Toggle-Notes })

# ── Notes stats ───────────────────────────────────────────────────────────
$syncHash.NotesEditor.Add_TextChanged({
    $text = $syncHash.NotesEditor.Text
    $lines = ($text -split "`n").Count
    $chars = $text.Length
    $words = if ($text.Trim()) { ($text.Trim() -split "\s+").Count } else { 0 }
    $syncHash.NotesStats.Text = "$lines lines, $chars chars, $words words"
    $script:ChangelogNotes = $text
})

# ── Step list already built above; Update-StepUI uses arrays directly
$window.Add_Loaded({
    Update-StepUI
})

# ── Run step ──────────────────────────────────────────────────────────────
$syncHash.BtnRunStep.Add_Click({
    $syncHash.BtnRunStep.IsEnabled = $false
    $syncHash.BtnPushAll.IsEnabled = $false
    $syncHash.BtnPublishAll.IsEnabled = $false
    $script:Cancelled = $false
    try {
        # Keep running steps until we hit one that's done, or user cancels
        while (-not $script:Cancelled) {
            # Find next incomplete step
            $idx = -1
            for ($i = 0; $i -lt $script:StepStatuses.Count; $i++) {
                if ($script:StepStatuses[$i] -eq "ok") { continue }
                if ($script:PushModeOnly -and $i -ge 4) { continue }
                $idx = $i
                break
            }
            if ($idx -lt 0) { Add-Log "All steps completed." -Color Green; break }

            $ok = Invoke-Step -ScriptBlock $stepFunctions[$idx] -StepName $stepLabels[$idx] -StepIndex $idx
            if (-not $ok) { break }

            # If step needs input, wait for user to confirm
            if ($script:WaitingForInput) {
                Wait-ForInput
                if ($script:Cancelled) { break }
                # Re-run the same step (now input is ready)
                continue
            }
            # Step completed, loop to run next step
        }
    } catch {
        if (-not $script:Cancelled) { Add-Log "Step error: $_" -Color Red }
    } finally {
        $syncHash.BtnRunStep.IsEnabled = $true
        $syncHash.BtnPushAll.IsEnabled = $true
        $syncHash.BtnPublishAll.IsEnabled = $true
    }
})

# ── Run all (Push) ────────────────────────────────────────────────────────
$syncHash.BtnPushAll.Add_Click({
    $syncHash.BtnRunStep.IsEnabled = $false
    $syncHash.BtnPushAll.IsEnabled = $false
    $syncHash.BtnPublishAll.IsEnabled = $false
    $script:Cancelled = $false
    try {
        for ($i = 0; $i -lt 4; $i++) {
            if (Test-Cancelled) { break }
            if ($script:StepStatuses[$i] -eq "ok") { continue }

            $ok = Invoke-Step -ScriptBlock $stepFunctions[$i] -StepName $stepLabels[$i] -StepIndex $i
            if (-not $ok) { break }

            # If step needs input, wait for user to confirm, then retry this step
            if ($script:WaitingForInput) {
                Wait-ForInput
                if ($script:Cancelled) { break }
                $i--  # retry this step now that input is ready
            }
        }
    } finally {
        $syncHash.BtnRunStep.IsEnabled = $true
        $syncHash.BtnPushAll.IsEnabled = $true
        $syncHash.BtnPublishAll.IsEnabled = $true
    }
})

# ── Run all (Publish) ─────────────────────────────────────────────────────
$syncHash.BtnPublishAll.Add_Click({
    $syncHash.BtnRunStep.IsEnabled = $false
    $syncHash.BtnPushAll.IsEnabled = $false
    $syncHash.BtnPublishAll.IsEnabled = $false
    $script:Cancelled = $false
    try {
        for ($i = 0; $i -lt $stepFunctions.Count; $i++) {
            if (Test-Cancelled) { break }
            if ($script:StepStatuses[$i] -eq "ok") { continue }

            $ok = Invoke-Step -ScriptBlock $stepFunctions[$i] -StepName $stepLabels[$i] -StepIndex $i
            if (-not $ok) { break }

            # If step needs input (e.g. version confirm, commit), wait for user
            if ($script:WaitingForInput) {
                Wait-ForInput
                if ($script:Cancelled) { break }
                $i--  # retry this step now that input is ready
            }
        }
    } finally {
        $syncHash.BtnRunStep.IsEnabled = $true
        $syncHash.BtnPushAll.IsEnabled = $true
        $syncHash.BtnPublishAll.IsEnabled = $true
    }
})

# ── Cancel ────────────────────────────────────────────────────────────────
$syncHash.BtnCancel.Add_Click({
    $script:Cancelled = $true
    Save-State
    Add-Log "[CANCELLING] User requested cancellation..." -Color Orange
})

# ── Start Fresh ───────────────────────────────────────────────────────────
$syncHash.BtnStartFresh.Add_Click({
    Clear-State
    for ($i = 0; $i -lt $script:StepStatuses.Count; $i++) { $script:StepStatuses[$i] = "idle" }
    $script:Version = $null; $script:VersionOk = $false; $script:CommitDone = $false
    $script:ReleaseUrl = $null; $script:Resumed = $false; $script:ChangelogNotes = ""
    $script:Cancelled = $false; $script:WaitingForInput = $null
    $syncHash.NotesEditor.Text = ""
    $syncHash.VersionBar.Visibility = "Collapsed"
    $syncHash.CommitBar.Visibility = "Collapsed"
    $syncHash.BtnOpenBrowser.Visibility = "Collapsed"
    $syncHash.BtnFinish.Visibility = "Collapsed"
    $syncHash.BtnRunStep.IsEnabled = $true
    $syncHash.BtnPublishAll.IsEnabled = $true
    $syncHash.BtnPushAll.IsEnabled = $true
    Update-StepUI
    Add-Log "State cleared. Starting fresh." -Color Orange
})

# ── Open in Browser ───────────────────────────────────────────────────────
$syncHash.BtnOpenBrowser.Add_Click({
    if ($script:ReleaseUrl) { Start-Process $script:ReleaseUrl }
})

# ── Finish / Close ───────────────────────────────────────────────────────
$syncHash.BtnFinish.Add_Click({
    Clear-State
    $window.Close()
})

# ===========================================================================
# RESUME CHECK
# ===========================================================================
$savedState = Load-State
if ($savedState -and $savedState.completedSteps -and $savedState.completedSteps.Count -gt 0) {
    $doneCount = $savedState.completedSteps.Count
    $verLabel = if ($savedState.version) { "v$($savedState.version)" } else { "(unknown)" }
    $msg = "Resume previous publish session?`n`n  Version: $verLabel`n  Completed: $doneCount of 8 steps"
    $dlg = [System.Windows.MessageBox]::Show($msg, "Resume Publish?", "YesNo", "Question")
    if ($dlg -eq "Yes") {
        $script:Version = $savedState.version
        $script:ReleaseUrl = $savedState.releaseUrl
        $script:ChangelogNotes = if ($savedState.changelogNotes) { $savedState.changelogNotes } else { "" }
        $script:Resumed = $true
        if ($script:ChangelogNotes) { $syncHash.NotesEditor.Text = $script:ChangelogNotes }
        foreach ($idx in $savedState.completedSteps) {
            if ($idx -lt $script:StepStatuses.Count) { $script:StepStatuses[$idx] = "ok" }
        }
    } else {
        Clear-State
    }
} elseif ($savedState) { Clear-State }

# ===========================================================================
# SHOW
# ===========================================================================
Add-Log "Welcome to the Roundtable-NLE Release Wizard!" -Color Cyan
if ($script:Resumed) {
    $v = if ($script:Version) { "v$($script:Version)" } else { "" }
    Add-Log "Resumed previous session ($v)." -Color Orange
}
Add-Log "Select a mode (Push / Publish), then run steps." -Color Gray

# Force initial step UI update after window loads
$window.Add_Loaded({
    Update-StepUI
})

$window.ShowDialog() | Out-Null
