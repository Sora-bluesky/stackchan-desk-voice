; voice-toggle.ahk  (AutoHotkey v2) - StackChan voice toggle
; StackChan の頭ダブルタップで送られる F13 キーで PC の AI チャットアプリの通話（ボイスモード）を開始・終了します。
; 通話中かどうかを Scroll Lock で StackChan に伝え、アプリ側での終了も反映します。
; 開始ボタンは開いているBotの色に従うため、形状と位置で探し、終了ボタンは赤色で探します。
; 見つからない場合は記録位置を使います。クリック後はマウスを元に戻します。
;
; 使い方
;   1. AutoHotkey v2 をインストールし、このファイルをダブルクリックで起動（タスクトレイに常駐）。
;   2. 対象アプリを開き、F13 でボイスモードを開始・終了します。
;      対象アプリの exe 名は voice-toggle.ini の [target] exe で変更できます（未設定時の既定値はスクリプト内の DefaultExe）。
;   3. 自動で見つからない場合の予備として、マウスをボイスボタンの上に置いて
;      Ctrl + Alt + Shift + G を押す → 位置をウィンドウ内の相対座標として保存（ini に記録）。
;   ・テスト: Ctrl + Alt + Shift + T で F13 と同じ動作をします。
#Requires AutoHotkey v2.0
#SingleInstance Force

IniFile := A_ScriptDir "\voice-toggle.ini"
DefaultExe := "Grok Bot.exe"  ; target app exe when voice-toggle.ini has no [target] exe
CoordMode "Mouse", "Window"
OnExit((*) => SetScrollLockState("Off"))
SetTimer UpdateCallState, 300
UpdateCallState()

LoadCfg() {
    global IniFile
    cfg := Map()
    cfg["exe"]   := IniRead(IniFile, "target", "exe", "")
    cfg["title"] := IniRead(IniFile, "target", "title", "")
    cfg["x"]     := Integer(IniRead(IniFile, "target", "x", "-1"))
    cfg["y"]     := Integer(IniRead(IniFile, "target", "y", "-1"))
    return cfg
}

CaptureWindow(hwnd) {
    dc := 0, memDC := 0, bitmap := 0, oldBitmap := 0
    try {
        if !hwnd || WinGetMinMax("ahk_id " hwnd) = -1
            return false
        WinGetPos , , &width, &height, "ahk_id " hwnd
        if width < 1 || height < 1
            return false
        dc := DllCall("user32\GetDC", "ptr", hwnd, "ptr")
        if !dc
            return false
        memDC := DllCall("gdi32\CreateCompatibleDC", "ptr", dc, "ptr")
        if !memDC
            return false
        bitmap := DllCall("gdi32\CreateCompatibleBitmap", "ptr", dc, "int", width, "int", height, "ptr")
        if !bitmap
            return false
        oldBitmap := DllCall("gdi32\SelectObject", "ptr", memDC, "ptr", bitmap, "ptr")
        if !oldBitmap || !DllCall("user32\PrintWindow", "ptr", hwnd, "ptr", memDC, "uint", 2, "int")
            return false
        ; 画面の重なりに影響されない画像を、上から順に読む32ビット形式にする
        DllCall("gdi32\SelectObject", "ptr", memDC, "ptr", oldBitmap, "ptr")
        oldBitmap := 0
        info := Buffer(40, 0)
        NumPut("uint", 40, info, 0)
        NumPut("int", width, info, 4)
        NumPut("int", -height, info, 8)
        NumPut("ushort", 1, info, 12)
        NumPut("ushort", 32, info, 14)
        pixels := Buffer(width * height * 4, 0)
        if DllCall("gdi32\GetDIBits", "ptr", memDC, "ptr", bitmap, "uint", 0, "uint", height,
                   "ptr", pixels, "ptr", info, "uint", 0, "int") != height
            return false
        return {pixels: pixels, width: width, height: height}
    } catch {
        return false
    } finally {
        if oldBitmap
            DllCall("gdi32\SelectObject", "ptr", memDC, "ptr", oldBitmap, "ptr")
        if bitmap
            DllCall("gdi32\DeleteObject", "ptr", bitmap)
        if memDC
            DllCall("gdi32\DeleteDC", "ptr", memDC)
        if dc
            DllCall("user32\ReleaseDC", "ptr", hwnd, "ptr", dc)
    }
}

VoiceModeActive() {
    try {
        cfg := LoadCfg()
        exe := cfg["exe"] != "" ? cfg["exe"] : DefaultExe
        capture := CaptureWindow(WinExist("ahk_exe " exe))
        if !capture
            return false
        pixels := capture.pixels, width := capture.width, height := capture.height
        left := Floor(width * 0.40), right := width - 10, bottom := Min(220, height - 1)
        if left > right || bottom < 30
            return false
        y := 30
        while y <= bottom {
            x := left
            while x <= right {
                color := NumGet(pixels, (y * width + x) * 4, "uint")
                if Abs(((color >> 16) & 0xFF) - 0xE3) <= 30
                    && Abs(((color >> 8) & 0xFF) - 0x3A) <= 30 && Abs((color & 0xFF) - 0x40) <= 30
                    return true
                x += 3
            }
            y += 3
        }
        return false
    } catch {
        return false
    }
}

FindVoiceStartButton(hwnd, &px, &py) {
    try {
        capture := CaptureWindow(hwnd)
        if !capture
            return false
        pixels := capture.pixels, width := capture.width, height := capture.height
        left := Floor(width * 0.85), top := Floor(height * 0.88)
        rightmostX := -1, rightmostY := -1, sumX := 0, sumY := 0, count := 0
        ; 右端の高彩度画素を起点に、40px以内のまとまりの重心を開始ボタンの中心とする
        x := width - 1
        while x >= left {
            if rightmostX >= 0 && x < rightmostX - 40
                break
            y := top
            while y < height {
                color := NumGet(pixels, (y * width + x) * 4, "uint")
                red := (color >> 16) & 0xFF, green := (color >> 8) & 0xFF, blue := color & 0xFF
                minChannel := Min(red, green, blue)
                if (Max(red, green, blue) - minChannel >= 70 && minChannel < 220) || Max(red, green, blue) < 60 {
                    if rightmostX < 0
                        rightmostX := x, rightmostY := y
                    if (x - rightmostX) ** 2 + (y - rightmostY) ** 2 <= 1600 {
                        sumX += x, sumY += y, count += 1
                    }
                }
                y += 1
            }
            x -= 1
        }
        if count < 200
            return false
        px := Round(sumX / count), py := Round(sumY / count)
        return true
    } catch {
        return false
    }
}

Log(msg) {
    try FileAppend(FormatTime(, "yyyy-MM-dd HH:mm:ss") "." A_MSec " " msg "`n", A_ScriptDir "\voice-toggle.log", "UTF-8")
}

UpdateCallState() {
    active := VoiceModeActive()
    static prev := ""
    if prev != active {
        Log(active ? "state on" : "state off")
        prev := active
    }
    if GetKeyState("ScrollLock", "T") != active
        SetScrollLockState active ? "On" : "Off"
}

; ---- calibration: record the voice button position relative to the active window ----
^!+g:: {
    global IniFile
    hwnd := WinExist("A")
    if !hwnd {
        TrayTip "アクティブウィンドウがありません", "StackChan voice toggle"
        return
    }
    MouseGetPos &mx, &my
    exe := WinGetProcessName(hwnd)
    title := WinGetTitle(hwnd)
    IniWrite exe, IniFile, "target", "exe"
    IniWrite title, IniFile, "target", "title"
    IniWrite mx, IniFile, "target", "x"
    IniWrite my, IniFile, "target", "y"
    TrayTip "記録しました: " exe " (" mx ", " my ")", "StackChan voice toggle"
}

ClickVoiceButton() {
    Log("enter " A_ThisHotkey)
    cfg := LoadCfg()
    exe := cfg["exe"] != "" ? cfg["exe"] : DefaultExe
    hwnd := WinExist("ahk_exe " exe)
    if !hwnd {
        Log("no window exe=" exe)
        TrayTip "対象アプリのウィンドウが見つかりません (" exe ")", "StackChan voice toggle"
        return
    }
    target := "ahk_id " hwnd
    CoordMode "Mouse", "Screen"
    MouseGetPos &ox, &oy
    try {
        WinActivate target
        if !WinWaitActive(target, , 1) {
            active := ""
            try active := WinGetProcessName("A")
            Log("activate failed hwnd=" hwnd " active=" active)
            return
        }
        Sleep 150
        WinGetPos , , &width, &height, target
        Log("win " width "x" height)
        CoordMode "Pixel", "Window"
        CoordMode "Mouse", "Window"
        if PixelSearch(&px, &py, Floor(width * 0.40), 30, width - 10, Min(220, height - 1), 0xE33A40, 30) {
            Log("red end button px=" px " py=" py)
            Click px, py + 18
            TrayTip "終了をクリック", "StackChan voice toggle"
        } else if FindVoiceStartButton(hwnd, &px, &py) {
            Log("start button px=" px " py=" py)
            Click px, py
            TrayTip "開始をクリック", "StackChan voice toggle"
        } else if (cfg["x"] >= width || cfg["y"] >= height) {
            Log("ini fallback outside window")
            return
        } else if (cfg["x"] >= 0 && cfg["y"] >= 0) {
            Log("ini fallback x=" cfg["x"] " y=" cfg["y"])
            Click cfg["x"], cfg["y"]
            TrayTip "記録位置をクリック", "StackChan voice toggle"
        } else {
            Log("unset")
            TrayTip "未設定です。ボイスボタンにマウスを置いて Ctrl+Alt+Shift+G", "StackChan voice toggle"
            return
        }
        SetTimer () => UpdateCallState(), -400
        SetTimer () => UpdateCallState(), -1200
        Sleep 50
    } finally {
        CoordMode "Mouse", "Screen"
        MouseMove ox, oy, 0
        CoordMode "Mouse", "Window"
    }
    ; 前のウィンドウには戻さない（ボイスモード中は対象アプリを前面にしておく方が安全）
}

F13:: ClickVoiceButton()
^!+t:: ClickVoiceButton()
