; SPDX-License-Identifier: GPL-3.0-or-later
; Quillreel's Windows installer. Built by packaging/build-windows.sh, which
; defines VERSION, SIZE_KB, OUTFILE, SRC (the source tree) and FILES_DIR (holding
; files-install.nsh / files-uninstall.nsh, the exact file list).

Unicode true
!include "MUI2.nsh"
!include "x64.nsh"
!include "FileFunc.nsh"

!define APP "Quillreel"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP}"
!define PROGID "Quillreel.BinkVideo"

Name "${APP} ${VERSION}"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\${APP}"
InstallDirRegKey HKLM "Software\${APP}" "InstallDir"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
ManifestDPIAware true

VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${APP}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "FileDescription" "${APP} ${VERSION} Setup"
VIAddVersionKey "LegalCopyright" "GPL-3.0-or-later"

!define MUI_ICON "${SRC}\data\windows\quillreel.ico"
!define MUI_UNICON "${SRC}\data\windows\quillreel.ico"
!define MUI_ABORTWARNING

!define MUI_LICENSEPAGE_TEXT_TOP "Quillreel is free software under the GNU General Public License."
!define MUI_LICENSEPAGE_TEXT_BOTTOM "You don't have to accept the license to use Quillreel; it sets the terms for sharing and changing it."
!define MUI_LICENSEPAGE_BUTTON "$(^NextBtn)"
!insertmacro MUI_PAGE_LICENSE "${SRC}\LICENSE"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_TEXT "Quillreel is installed.$\r$\n$\r$\nBink 2 video (.bk2) needs RAD's bink2w64.dll, which Quillreel does not include. Add it from a game that uses Bink 2 with Help > Bink 2 Video > Install DLL. Without it, .bk2 files can be inspected and their audio played and converted."
!define MUI_FINISHPAGE_RUN "$INSTDIR\quillreel-gui.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Start Quillreel"
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Function .onInit
    ${IfNot} ${RunningX64}
        MessageBox MB_ICONSTOP "Quillreel needs 64-bit Windows."
        Abort
    ${EndIf}
    SetRegView 64
    SetShellVarContext all  ; Start menu and desktop shortcuts for all users
FunctionEnd

Section "Quillreel" SecCore
    SectionIn RO
    ; An earlier version's uninstaller removes exactly the files it installed,
    ; so files a new version no longer has don't stay behind.
    ReadRegStr $0 HKLM "${UNINST_KEY}" "InstallLocation"
    ${If} $0 != ""
    ${AndIf} ${FileExists} "$0\uninstall.exe"
        ExecWait '"$0\uninstall.exe" /S /KEEPREG _?=$0'
        Delete "$0\uninstall.exe"
    ${EndIf}

    SetOutPath "$INSTDIR"
    !include "${FILES_DIR}\files-install.nsh"
    WriteUninstaller "$INSTDIR\uninstall.exe"

    WriteRegStr HKLM "Software\${APP}" "InstallDir" "$INSTDIR"
    WriteRegStr HKLM "${UNINST_KEY}" "DisplayName" "${APP}"
    WriteRegStr HKLM "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
    WriteRegStr HKLM "${UNINST_KEY}" "Publisher" "${APP}"
    WriteRegStr HKLM "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\quillreel-gui.exe"
    WriteRegStr HKLM "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
    WriteRegStr HKLM "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
    WriteRegStr HKLM "${UNINST_KEY}" "URLInfoAbout" "https://github.com/b1g-b8lls/Quillreel"
    WriteRegDWORD HKLM "${UNINST_KEY}" "EstimatedSize" ${SIZE_KB}
    WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
    WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1
    ; Lets Run (Win+R) and "start" find the GUI by name.
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\App Paths\quillreel-gui.exe" "" "$INSTDIR\quillreel-gui.exe"
SectionEnd

Section "Start menu shortcut" SecStartMenu
    CreateShortcut "$SMPROGRAMS\${APP}.lnk" "$INSTDIR\quillreel-gui.exe"
SectionEnd

Section /o "Desktop shortcut" SecDesktop
    CreateShortcut "$DESKTOP\${APP}.lnk" "$INSTDIR\quillreel-gui.exe"
SectionEnd

!macro Associate EXT
    WriteRegStr HKLM "Software\Classes\${EXT}\OpenWithProgids" "${PROGID}" ""
    ; Becomes the default only if nothing else has claimed the extension.
    ReadRegStr $0 HKLM "Software\Classes\${EXT}" ""
    ${If} $0 == ""
        WriteRegStr HKLM "Software\Classes\${EXT}" "" "${PROGID}"
    ${EndIf}
!macroend

Section "Open .bik and .bk2 files with Quillreel" SecAssoc
    WriteRegStr HKLM "Software\Classes\${PROGID}" "" "Bink video"
    WriteRegStr HKLM "Software\Classes\${PROGID}\DefaultIcon" "" "$INSTDIR\quillreel-gui.exe,0"
    WriteRegStr HKLM "Software\Classes\${PROGID}\shell\open\command" "" '"$INSTDIR\quillreel-gui.exe" "%1"'
    !insertmacro Associate ".bik"
    !insertmacro Associate ".bk2"
    System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'  ; SHCNE_ASSOCCHANGED
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
    !insertmacro MUI_DESCRIPTION_TEXT ${SecCore} "The Quillreel application (quillreel-gui.exe) and command-line tool (quillreel.exe)."
    !insertmacro MUI_DESCRIPTION_TEXT ${SecStartMenu} "Add Quillreel to the Start menu."
    !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} "Put a Quillreel shortcut on the desktop."
    !insertmacro MUI_DESCRIPTION_TEXT ${SecAssoc} "Offer Quillreel for Bink video files in Explorer's Open with menu, and make it the default for them if no other program is."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

; ------------------------------------------------------------------ uninstall

!macro Unassociate EXT
    DeleteRegValue HKLM "Software\Classes\${EXT}\OpenWithProgids" "${PROGID}"
    DeleteRegKey /ifempty HKLM "Software\Classes\${EXT}\OpenWithProgids"
    ReadRegStr $0 HKLM "Software\Classes\${EXT}" ""
    ${If} $0 == "${PROGID}"
        DeleteRegValue HKLM "Software\Classes\${EXT}" ""
    ${EndIf}
    DeleteRegKey /ifempty HKLM "Software\Classes\${EXT}"
!macroend

Function un.onInit
    SetRegView 64
    SetShellVarContext all  ; Start menu and desktop shortcuts for all users
FunctionEnd

Section "Uninstall"
    !include "${FILES_DIR}\files-uninstall.nsh"
    Delete "$INSTDIR\uninstall.exe"
    RMDir "$INSTDIR"

    ; /KEEPREG: run by a newer installer, which writes these again.
    ClearErrors
    ${GetOptions} $CMDLINE "/KEEPREG" $0
    IfErrors 0 done
    Delete "$SMPROGRAMS\${APP}.lnk"
    Delete "$DESKTOP\${APP}.lnk"
    DeleteRegKey HKLM "Software\Classes\${PROGID}"
    !insertmacro Unassociate ".bik"
    !insertmacro Unassociate ".bk2"
    System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
    DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\App Paths\quillreel-gui.exe"
    DeleteRegKey HKLM "${UNINST_KEY}"
    DeleteRegKey HKLM "Software\${APP}"
    ; The user's own bink2w64.dll (%LOCALAPPDATA%\Quillreel) and settings stay.
    done:
SectionEnd
