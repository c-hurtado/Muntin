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

#include <FancyZonesLib/Arrange.h>
#include <FancyZonesLib/DesktopScroll.h>
#include <FancyZonesLib/Settings.h>
#include <FancyZonesLib/StageMotion.h>

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

    constexpr double PullBack = 0.06; // Ease: the pull back is this share of the way at its deepest, but the ease already goes on, so the dip is about 2%
    constexpr double PullBackEnd = 0.3; // Of the ease, where the pull back is over
    constexpr double PullBackSeconds = 0.08; // Spring: how long the pull back lasts before it sets off
    constexpr double SpringPullBack = 0.04; // Of the way, at the deepest...
    constexpr double SpringPullBackMost = 40.0; // ...and at most this many pixels

    struct Sprite
    {
        HWND window{};
        HTHUMBNAIL id{};
        std::array<double, 4> place{}; // Left, top, right, bottom
        std::array<double, 4> speed{};
        RECT drawn{}; // DWM is only told what changed
        bool started{};
        std::array<double, 4> from{}; // Ease: where the move set off from, to where it is going
        std::array<double, 4> goal{};
        double elapsed{}; // Seconds since
        bool pullBack{};
        StageMotion::Kind kind{ StageMotion::Kind::Travel }; // FadeIn / FadeOut: stays at `place`, only the opacity moves
        bool fixedEnd{}; // the sprite goes to `end` (not to where the real window is)
        std::array<double, 4> end{};
        int opacity{ 255 }; // 0 to 255
        int drawnOpacity{ -1 };
        bool ghost{}; // the fade-out half of a crossfaded move: a second sprite of the same window, at its old rect
        int startOpacity{ 255 }; // ghost: the opacity its fade out starts from (a quick second move starts from a half-faded sprite)
        bool landsAtEnd{}; // the real window waits out of sight: moved to `end` once the sprite is at rest, the sprite goes once it is there
        std::array<double, 4> waitsAt{}; // landsAtEnd: where the stage parked the window to wait (it is only landed from there)
        std::array<double, 4> droppedAt{}; // landsAtEnd: where it was when held (the stage's async move to waitsAt may not have arrived yet)
        ULONGLONG landSent{}; // when it was sent to `end` (0: not yet)
        bool landSeen{}; // seen at `end`: one more frame, so the window is drawn there before its sprite goes
    };

    bool SameRect(const RECT& rect, const std::array<double, 4>& place) noexcept
    {
        return rect.left == std::lround(place[0]) && rect.top == std::lround(place[1]) && rect.right == std::lround(place[2]) && rect.bottom == std::lround(place[3]);
    }

    // A sprite at rest whose real window waits out of sight (landsAtEnd): moves the window to the sprite's end, and says whether the sprite
    // may go. StageMotion::LandStep decides: a window that is no longer where the stage left it to wait (the stage or the user placed it
    // since, or the stage let it go) is not moved.
    bool LandAtEnd(Sprite& sprite) noexcept
    {
        StageMotion::LandFacts facts;
        facts.window = IsWindow(sprite.window) != FALSE;
        if (facts.window)
        {
            RECT rect{};
            const bool known = GetWindowRect(sprite.window, &rect) != FALSE;
            facts.sent = sprite.landSent != 0;
            facts.seen = sprite.landSeen;
            facts.rectIsEnd = known && SameRect(rect, sprite.end);
            facts.rectIsWaitsAt = known && SameRect(rect, sprite.waitsAt);
            facts.rectIsFrom = known && SameRect(rect, sprite.droppedAt);
            facts.iconic = IsIconic(sprite.window) != FALSE;
            facts.zoomed = IsZoomed(sprite.window) != FALSE;
            facts.held = Stage::IsHeld(sprite.window);
            facts.sinceSentMs = facts.sent ? GetTickCount64() - sprite.landSent : 0;
        }

        switch (StageMotion::LandStep(facts))
        {
        case StageMotion::Landing::Send:
        {
            sprite.landSent = (std::max)(GetTickCount64(), 1ULL);
            const auto& e = sprite.end;
            // Async: a window that hangs must not hang the glide. No z-order change: it is where the stage left it.
            SetWindowPos(sprite.window, nullptr, std::lround(e[0]), std::lround(e[1]), std::lround(e[2] - e[0]), std::lround(e[3] - e[1]), SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS);
            return false;
        }
        case StageMotion::Landing::Seen:
            sprite.landSeen = true;
            return false; // drawn at its place in the next frame (the loop flushes), the sprite goes after that
        case StageMotion::Landing::Wait:
            return false;
        case StageMotion::Landing::Abandon:
            Logger::info(L"DockMotion: hwnd={:x} was placed elsewhere during its glide: not landed", reinterpret_cast<uintptr_t>(sprite.window));
            return true;
        case StageMotion::Landing::Done:
        default:
            return true;
        }
    }

    bool IsAnimated(HWND window) noexcept
    {
        int cloaked{};
        DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        return IsWindowVisible(window) && !IsIconic(window) && !cloaked && !Arrange::HandsOffForMotion(window);
    }

    std::array<double, 4> PlaceOf(const RECT& rect) noexcept
    {
        return { static_cast<double>(rect.left), static_cast<double>(rect.top), static_cast<double>(rect.right), static_cast<double>(rect.bottom) };
    }

    std::array<double, 4> PlaceOf(const StageLayouts::LRect& rect) noexcept
    {
        return { static_cast<double>(rect.L), static_cast<double>(rect.T), static_cast<double>(rect.R), static_cast<double>(rect.B) };
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

double DockMotion::Ease(double progress, bool anticipation) noexcept
{
    progress = std::clamp(progress, 0.0, 1.0);
    double share = (1.0 - std::cos(std::numbers::pi * progress)) / 2;
    if (anticipation && progress < PullBackEnd)
    {
        const double pull = std::sin(std::numbers::pi * progress / PullBackEnd);
        share -= PullBack * pull * pull;
    }

    return share;
}

bool DockMotion::RestartsEase(const std::array<double, 4>& goal, const std::array<double, 4>& target, double progress) noexcept
{
    double most = 0;
    for (size_t side = 0; side < goal.size(); ++side)
    {
        most = (std::max)(most, std::abs(target.at(side) - goal.at(side)));
    }

    return progress >= 1.0 || most > EaseRestartPixels;
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
            self->HoldWindows(*reinterpret_cast<const std::vector<Held>*>(lparam), reinterpret_cast<std::vector<HWND>*>(wparam));
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

    // taken (may be null): the windows whose landing this hold took (landsAtEnd with a sprite)
    void HoldWindows(const std::vector<Held>& held, std::vector<HWND>* taken) noexcept
    {
        std::vector<HWND> windows;
        for (const auto& h : held)
        {
            windows.push_back(h.window);
        }

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

        const auto ordered = InZOrder(windows);
        std::vector<StageMotion::Spec> specs;
        for (const auto candidate : ordered)
        {
            specs.push_back(std::find_if(held.begin(), held.end(), [&](const Held& h) { return h.window == candidate; })->spec);
        }

        // Every ghost (the fade-out half of a crossfaded move) is registered before any new live sprite, so DWM stacks it below them
        for (const auto& registration : StageMotion::RegistrationOrder(specs))
        {
            const auto candidate = ordered[registration.index];
            const auto& fullSpec = specs[registration.index];
            const auto existing = std::find_if(sprites.begin(), sprites.end(), [&](const Sprite& sprite) { return sprite.window == candidate && !sprite.ghost; });
            const bool known = existing != sprites.end();
            if (registration.ghost)
            {
                if (!known && !IsAnimated(candidate))
                {
                    continue;
                }

                // A quick second move: the ghost starts where the live sprite is seen now, at its opacity
                const auto start = StageMotion::GhostStart(fullSpec, known ? std::optional<StageMotion::Look>(StageMotion::Look{ existing->place, existing->opacity }) : std::nullopt);
                HTHUMBNAIL ghostId{};
                if (start && SUCCEEDED(DwmRegisterThumbnail(overlay, candidate, &ghostId)))
                {
                    // It replaces this window's older ghost; without a new one, an older ghost fades out on its own
                    std::erase_if(sprites, [&](const Sprite& sprite) {
                        const bool replaced = sprite.ghost && sprite.window == candidate;
                        if (replaced)
                        {
                            DwmUnregisterThumbnail(sprite.id);
                        }

                        return replaced;
                    });

                    Sprite ghost{ .window = candidate, .id = ghostId, .place = start->place };
                    ghost.kind = StageMotion::Kind::FadeOut;
                    ghost.ghost = true;
                    ghost.startOpacity = start->opacity;
                    ghost.opacity = start->opacity;
                    sprites.push_back(ghost);
                }

                continue;
            }

            const auto parts = StageMotion::FadeThroughParts(fullSpec);
            const StageMotion::Spec& spec = parts.empty() ? fullSpec : parts[1];
            if (known)
            {
                // A quick second move of a window that still has a sprite: its spec replaces the old one (no stale target, no fade the wrong way)
                const bool special = spec.kind != StageMotion::Kind::Travel || spec.from || spec.to;
                if (special || existing->kind != StageMotion::Kind::Travel || existing->fixedEnd)
                {
                    existing->kind = spec.kind;
                    existing->fixedEnd = spec.to.has_value();
                    existing->end = spec.to ? PlaceOf(*spec.to) : existing->end;
                    existing->opacity = StageMotion::OpacityByte(spec.kind, 0);
                    if (spec.from)
                    {
                        existing->place = PlaceOf(*spec.from);
                        existing->speed = {};
                    }

                    existing->started = false;
                    existing->elapsed = 0;
                    existing->pullBack = false;
                }

                // A new move of a window that waits out of sight: the new spec says who lands it now (a plain move: the caller does)
                existing->landsAtEnd = spec.landsAtEnd && spec.to && spec.from && spec.waitsAt && spec.kind == StageMotion::Kind::Travel;
                existing->waitsAt = spec.waitsAt ? PlaceOf(*spec.waitsAt) : std::array<double, 4>{};
                existing->droppedAt = spec.from ? PlaceOf(*spec.from) : std::array<double, 4>{};
                existing->landSent = 0;
                existing->landSeen = false;
                if (existing->landsAtEnd && taken)
                {
                    taken->push_back(candidate);
                }
            }

            RECT rect{};
            HTHUMBNAIL id{};
            if (!known && IsAnimated(candidate) && GetWindowRect(candidate, &rect) && SUCCEEDED(DwmRegisterThumbnail(overlay, candidate, &id)))
            {
                Sprite sprite{ .window = candidate, .id = id, .place = spec.from ? PlaceOf(*spec.from) : PlaceOf(rect) };
                sprite.kind = spec.kind;
                sprite.fixedEnd = spec.to.has_value();
                sprite.end = spec.to ? PlaceOf(*spec.to) : sprite.end;
                sprite.opacity = StageMotion::OpacityByte(spec.kind, 0); // a fade in starts invisible: not a frame of it before the first step
                sprite.landsAtEnd = spec.landsAtEnd && spec.to && spec.from && spec.waitsAt && spec.kind == StageMotion::Kind::Travel;
                sprite.waitsAt = spec.waitsAt ? PlaceOf(*spec.waitsAt) : std::array<double, 4>{};
                sprite.droppedAt = spec.from ? PlaceOf(*spec.from) : std::array<double, 4>{};
                sprites.push_back(sprite);
                if (sprite.landsAtEnd && taken)
                {
                    taken->push_back(candidate);
                }
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
            if (!EqualRect(&place, &sprite.drawn) || sprite.opacity != sprite.drawnOpacity)
            {
                sprite.drawn = place;
                sprite.drawnOpacity = sprite.opacity;
                DWM_THUMBNAIL_PROPERTIES properties{ .dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_OPACITY | DWM_TNP_VISIBLE, .opacity = static_cast<BYTE>(sprite.opacity), .fVisible = TRUE };
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

        if (sprite.kind != StageMotion::Kind::Travel)
        {
            // A fade: the sprite sits still, its opacity runs over the ease time
            sprite.started = true;
            sprite.elapsed += seconds;
            const double progress = (std::min)(sprite.elapsed / EaseSeconds, 1.0);
            sprite.opacity = sprite.ghost ? StageMotion::GhostOpacityByte(sprite.startOpacity, progress) : StageMotion::OpacityByte(sprite.kind, progress);
            return progress < 1.0;
        }

        const auto target = sprite.fixedEnd ? sprite.end : PlaceOf(rect);
        const bool ease = FancyZonesSettings::settings().dockMotionCurve == 1;
        if (!sprite.started)
        {
            sprite.started = true;
            sprite.pullBack = FancyZonesSettings::settings().dockAnticipation;
            sprite.from = sprite.place;
            sprite.goal = target;
            sprite.elapsed = 0;
        }
        else if (ease && target != sprite.goal)
        {
            // A far new target starts a new ease from where the sprite is; a small shift of the same one just moves its end
            if (RestartsEase(sprite.goal, target, sprite.elapsed / EaseSeconds))
            {
                sprite.pullBack = false;
                sprite.from = sprite.place;
                sprite.elapsed = 0;
            }

            sprite.goal = target;
        }

        if (ease)
        {
            sprite.elapsed += seconds;
            const double progress = (std::min)(sprite.elapsed / EaseSeconds, 1.0);
            const double share = Ease(progress, sprite.pullBack);
            for (size_t side = 0; side < target.size(); ++side)
            {
                sprite.place.at(side) = sprite.from.at(side) + (sprite.goal.at(side) - sprite.from.at(side)) * share;
            }

            return progress < 1.0 && sprite.from != sprite.goal;
        }

        if (sprite.pullBack)
        {
            // Before a spring sets off: out from the target and back, the same share of the way on every side, so the size holds
            sprite.elapsed += seconds;
            if (sprite.elapsed < PullBackSeconds)
            {
                double most = 0;
                for (size_t side = 0; side < target.size(); ++side)
                {
                    most = (std::max)(most, std::abs(sprite.from.at(side) - sprite.goal.at(side)));
                }

                const double share = most > 0 ? (std::min)(SpringPullBack, SpringPullBackMost / most) * std::sin(std::numbers::pi * sprite.elapsed / PullBackSeconds) : 0.0;
                for (size_t side = 0; side < target.size(); ++side)
                {
                    sprite.place.at(side) = sprite.from.at(side) + (sprite.from.at(side) - sprite.goal.at(side)) * share;
                }

                return true;
            }

            sprite.pullBack = false;
            sprite.place = sprite.from;
        }

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
                    const bool done = !Step(sprite, seconds) && landed && (!sprite.landsAtEnd || LandAtEnd(sprite));
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

std::vector<HWND> DockMotion::Hold(const std::vector<Held>& windows) noexcept
{
    std::vector<HWND> taken;
    if (m_impl->window)
    {
        // Not posted: the windows must not move before this returns
        try
        {
            taken.reserve(windows.size()); // no allocation on the motion thread can throw then
        }
        catch (...)
        {
            return {};
        }

        SendMessageW(m_impl->window, WM_MOTION_HOLD, reinterpret_cast<WPARAM>(&taken), reinterpret_cast<LPARAM>(&windows));
    }

    return taken;
}

void DockMotion::Hold(const std::vector<HWND>& windows) noexcept
{
    std::vector<Held> held;
    for (const auto window : windows)
    {
        held.push_back({ window, {} });
    }

    Hold(held);
}

void DockMotion::Release(bool moved) noexcept
{
    if (m_impl->window)
    {
        PostMessageW(m_impl->window, WM_MOTION_RELEASE, moved, 0);
    }
}
