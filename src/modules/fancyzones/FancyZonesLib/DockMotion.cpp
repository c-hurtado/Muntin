#include "pch.h"
#include "DockMotion.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <future>
#include <numbers>
#include <thread>

#include <dwmapi.h>

#include <common/logger/logger.h>

#include <FancyZonesLib/DesktopScroll.h>

namespace
{
    constexpr wchar_t WindowClassName[] = L"MuntinDockMotion";

    constexpr UINT WM_MOTION_HOLD = WM_APP + 1; // lparam: the windows
    constexpr UINT WM_MOTION_RELEASE = WM_APP + 2; // wparam: whether any window moved
    constexpr UINT WM_MOTION_RUN = WM_APP + 3;
    constexpr UINT WM_MOTION_STOP = WM_APP + 4;

    // The default of Tricky Tiles. Stiffness and damping come from them as in SwiftUI.
    constexpr double DurationSeconds = 0.29;
    constexpr double Bounce = 0.15;
    constexpr double Omega = 2 * std::numbers::pi / DurationSeconds;
    constexpr double Decay = Omega * (1.0 - Bounce);

    constexpr ULONGLONG LandMillis = 120; // A busy app moves its window late, and a window that hasn't moved yet looks at rest
    constexpr double RestPixels = 0.5;
    constexpr double RestSpeed = 5.0; // Pixels per second
    constexpr double LongestStep = 1.0 / 30;

    struct Sprite
    {
        HWND window{};
        HTHUMBNAIL id{};
        std::array<double, 4> place{}; // Left, top, right, bottom
        std::array<double, 4> speed{};
        RECT drawn{}; // DWM is only told what changed
    };

    bool IsAnimated(HWND window) noexcept
    {
        int cloaked{};
        DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        return IsWindowVisible(window) && !IsIconic(window) && !cloaked && !DesktopScroll::IsParked(window);
    }

    std::array<double, 4> PlaceOf(const RECT& rect) noexcept
    {
        return { static_cast<double>(rect.left), static_cast<double>(rect.top), static_cast<double>(rect.right), static_cast<double>(rect.bottom) };
    }

    // Bottom of the z-order first
    std::vector<HWND> InZOrder(const std::vector<HWND>& windows)
    {
        std::vector<HWND> all;
        EnumWindows([](HWND window, LPARAM param) {
            reinterpret_cast<std::vector<HWND>*>(param)->push_back(window);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&all));
        std::reverse(all.begin(), all.end());
        std::erase_if(all, [&](HWND window) { return std::find(windows.begin(), windows.end(), window) == windows.end(); });
        return all;
    }
}

void DockMotion::Advance(double& displacement, double& velocity, double seconds) noexcept
{
    const double frequency = std::sqrt(Omega * Omega - Decay * Decay);
    const double fade = std::exp(-Decay * seconds);
    const double cosine = std::cos(frequency * seconds);
    const double sine = std::sin(frequency * seconds);
    const double place = fade * (displacement * cosine + (velocity + Decay * displacement) / frequency * sine);
    velocity = fade * (velocity * cosine - (Decay * velocity + Omega * Omega * displacement) / frequency * sine);
    displacement = place;
}

struct DockMotion::Impl
{
    static inline Impl* self{};

    HINSTANCE instance{};
    HWND window{};
    HWND overlay{};
    POINT origin{}; // Of the overlay
    std::thread thread;
    std::vector<Sprite> sprites;
    int holds{};
    ULONGLONG releasedAt{};

    void Run(std::promise<void>& ready) noexcept
    {
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        self = this;

        WNDCLASSEXW wcex{};
        wcex.cbSize = sizeof(WNDCLASSEX);
        wcex.lpfnWndProc = WndProc;
        wcex.hInstance = instance;
        wcex.lpszClassName = WindowClassName;
        RegisterClassExW(&wcex);

        window = CreateWindowExW(0, WindowClassName, L"", WS_POPUP, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
        ready.set_value();
        if (!window)
        {
            Logger::error(L"Failed to create the dock motion window");
            return;
        }

        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            DispatchMessageW(&message);
        }

        DestroyWindow(window);
        self = nullptr;
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) noexcept
    {
        switch (message)
        {
        case WM_MOTION_HOLD:
            self->HoldWindows(*reinterpret_cast<const std::vector<HWND>*>(lparam));
            return 0;
        case WM_MOTION_RELEASE:
            self->holds = (std::max)(self->holds - 1, 0);
            if (wparam)
            {
                self->releasedAt = GetTickCount64();
            }

            return 0;
        case WM_MOTION_RUN:
            self->Animate();
            return 0;
        case WM_MOTION_STOP:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }
    }

    void HoldWindows(const std::vector<HWND>& windows) noexcept
    {
        ++holds;
        const bool moving = overlay != nullptr;
        if (!moving)
        {
            origin = { GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN) };
            overlay = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, WindowClassName, L"", WS_POPUP, origin.x, origin.y, GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN), nullptr, nullptr, instance, nullptr);
        }

        if (!overlay)
        {
            return;
        }

        for (const auto candidate : InZOrder(windows))
        {
            const bool known = std::any_of(sprites.begin(), sprites.end(), [&](const Sprite& sprite) { return sprite.window == candidate; });
            RECT rect{};
            HTHUMBNAIL id{};
            if (!known && IsAnimated(candidate) && GetWindowRect(candidate, &rect) && SUCCEEDED(DwmRegisterThumbnail(overlay, candidate, &id)))
            {
                sprites.push_back({ .window = candidate, .id = id, .place = PlaceOf(rect) });
            }
        }

        Draw();
        DwmFlush();
        if (!moving)
        {
            SetWindowPos(overlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            DwmFlush();
            PostMessageW(window, WM_MOTION_RUN, 0, 0);
        }
    }

    void Draw() noexcept
    {
        for (auto& sprite : sprites)
        {
            const RECT place{ std::lround(sprite.place[0]) - origin.x, std::lround(sprite.place[1]) - origin.y, std::lround(sprite.place[2]) - origin.x, std::lround(sprite.place[3]) - origin.y };
            if (!EqualRect(&place, &sprite.drawn))
            {
                sprite.drawn = place;
                DWM_THUMBNAIL_PROPERTIES properties{ .dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_OPACITY | DWM_TNP_VISIBLE, .opacity = 255, .fVisible = TRUE };
                properties.rcDestination = place;
                DwmUpdateThumbnailProperties(sprite.id, &properties);
            }
        }
    }

    // False once the thumbnail isn't needed anymore
    static bool Step(Sprite& sprite, double seconds) noexcept
    {
        RECT rect{};
        if (!GetWindowRect(sprite.window, &rect))
        {
            return false;
        }

        const auto target = PlaceOf(rect);
        bool resting = true;
        for (size_t side = 0; side < target.size(); ++side)
        {
            double displacement = sprite.place.at(side) - target.at(side);
            Advance(displacement, sprite.speed.at(side), seconds);
            const bool sideResting = std::abs(displacement) < RestPixels && std::abs(sprite.speed.at(side)) < RestSpeed;
            sprite.place.at(side) = sideResting ? target.at(side) : target.at(side) + displacement;
            resting = resting && sideResting;
        }

        return !resting;
    }

    // False once the thread has to stop
    static bool Pump() noexcept
    {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT)
            {
                PostQuitMessage(0);
                return false;
            }

            DispatchMessageW(&message);
        }

        return true;
    }

    void Animate() noexcept
    {
        using Clock = std::chrono::steady_clock;

        auto last = Clock::now();
        while (Pump())
        {
            const auto now = Clock::now();
            const double seconds = (std::min)(std::chrono::duration<double>(now - last).count(), LongestStep);
            last = now;
            if (holds == 0)
            {
                const bool landed = GetTickCount64() - releasedAt >= LandMillis;
                std::erase_if(sprites, [&](Sprite& sprite) {
                    const bool done = !Step(sprite, seconds) && landed;
                    if (done)
                    {
                        DwmUnregisterThumbnail(sprite.id);
                    }

                    return done;
                });

                if (sprites.empty())
                {
                    break;
                }
            }

            Draw();
            DwmFlush();
        }

        // One more frame, so that the thumbnails of the last windows are gone with their real windows in sight
        DwmFlush();
        for (const auto& sprite : sprites)
        {
            DwmUnregisterThumbnail(sprite.id);
        }

        sprites.clear();
        DestroyWindow(overlay);
        overlay = nullptr;
    }
};

DockMotion::DockMotion(HINSTANCE instance) :
    m_impl(std::make_unique<Impl>())
{
    m_impl->instance = instance;

    std::promise<void> ready;
    m_impl->thread = std::thread([this, &ready] { m_impl->Run(ready); });
    ready.get_future().wait();
}

DockMotion::~DockMotion()
{
    if (m_impl->window)
    {
        PostMessageW(m_impl->window, WM_MOTION_STOP, 0, 0);
    }

    m_impl->thread.join();
}

void DockMotion::Hold(const std::vector<HWND>& windows) noexcept
{
    if (m_impl->window)
    {
        // Not posted: the windows must not move before this returns
        SendMessageW(m_impl->window, WM_MOTION_HOLD, 0, reinterpret_cast<LPARAM>(&windows));
    }
}

void DockMotion::Release(bool moved) noexcept
{
    if (m_impl->window)
    {
        PostMessageW(m_impl->window, WM_MOTION_RELEASE, moved, 0);
    }
}
