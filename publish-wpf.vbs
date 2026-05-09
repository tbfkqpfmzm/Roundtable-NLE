' ROUNDTABLE Publish Wizard (WPF) — Launcher
' Double-click this file to launch the WPF-based publish GUI with no console window.

Dim shell, scriptDir
Set shell = CreateObject("WScript.Shell")
scriptDir = CreateObject("Scripting.FileSystemObject").GetParentFolderName(WScript.ScriptFullName)
shell.Run "powershell -ExecutionPolicy Bypass -NoProfile -File """ & scriptDir & "\publish-wpf.ps1""", 0, False
