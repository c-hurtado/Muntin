#pragma once

// The icon in the notification area. Its menu turns the main settings on and off, recalls the named layouts,
// and quits. It lives on the thread that starts it, which must run a message loop.
namespace Tray
{
    // The class of its hidden window. The registered message QuitMessageName posted to that window quits, as the menu does.
    constexpr wchar_t WindowClassName[] = L"MuntinTray";
    constexpr wchar_t QuitMessageName[] = L"MuntinQuit";

    // welcome: a balloon tells how to start, for the first run
    void Start(HINSTANCE instance, bool welcome) noexcept;
    void Stop() noexcept;

    // A balloon from the icon. On the thread that started the tray. False when there is no icon to show it.
    bool Notify(const wchar_t* text) noexcept;
}
