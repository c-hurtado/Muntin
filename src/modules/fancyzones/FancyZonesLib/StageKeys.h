#pragma once

// Stage mode keys (Phase 5, keys half): pure logic, no windows, no hook, no settings. The keyboard hook (FancyZones::OnKeyDown) and
// StageMode feed it facts and act on what it answers. Inputs are injectable so the tests need nothing on screen.
//
//  - a key table as data: modifiers + virtual key -> action (DefaultBindings; a settings file could replace it later)
//  - which window a key acts on: the foreground window when Stage holds it; while the foreground is the shell, the taskbar, an overlay
//    or Muntin itself, the last held foreground (port of StageProto's "last managed foreground", commit 921a309); else none and the
//    keystroke passes through unchanged
//  - Gate: swallows exactly the chord's key-down (and its auto-repeats) and the matching key-up, so no key is left stuck down
//  - NextMonitor: left/right neighbour by screen position, wrapping

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace StageKeys
{
    enum class Action
    {
        None,
        RotateBack, // the whole stage list rotates one step: the master goes to the end of the stack (StageProto: Left)
        RotateForward, // the reverse: the last window becomes master (Right)
        Promote, // the focused window becomes master
        Demote, // the focused window goes to the end of the stack (the next one becomes master)
        MoveToPrevMonitor,
        MoveToNextMonitor,
        CycleLayout, // stage -> pair -> master
    };

    // Modifier bits of a chord
    constexpr uint32_t ModWin = 1;
    constexpr uint32_t ModShift = 2;
    constexpr uint32_t ModCtrl = 4;
    constexpr uint32_t ModAlt = 8;

    inline constexpr uint32_t Mods(bool win, bool shift, bool ctrl, bool alt) noexcept
    {
        return (win ? ModWin : 0u) | (shift ? ModShift : 0u) | (ctrl ? ModCtrl : 0u) | (alt ? ModAlt : 0u);
    }

    struct Binding
    {
        uint32_t mods{};
        uint32_t vk{};
        Action action{};
    };

    // Virtual key codes written out (this header does not include windows.h)
    constexpr uint32_t VkLeft = 0x25;
    constexpr uint32_t VkUp = 0x26;
    constexpr uint32_t VkRight = 0x27;
    constexpr uint32_t VkDown = 0x28;
    constexpr uint32_t VkL = 0x4C;

    // The default mapping (user decision 11/23). Win+L locks Windows and cannot be taken, so the layout cycle is Win+Shift+L.
    inline const std::vector<Binding>& DefaultBindings()
    {
        static const std::vector<Binding> bindings{
            { ModWin, VkLeft, Action::RotateBack },
            { ModWin, VkRight, Action::RotateForward },
            { ModWin, VkUp, Action::Promote },
            { ModWin, VkDown, Action::Demote },
            { ModWin | ModShift, VkLeft, Action::MoveToPrevMonitor },
            { ModWin | ModShift, VkRight, Action::MoveToNextMonitor },
            { ModWin | ModShift, VkL, Action::CycleLayout },
        };
        return bindings;
    }

    // Exact match on the modifiers: Win+Ctrl+Left is not Win+Left
    inline Action Lookup(const std::vector<Binding>& table, uint32_t mods, uint32_t vk)
    {
        for (const auto& b : table)
        {
            if (b.mods == mods && b.vk == vk)
            {
                return b.action;
            }
        }

        return Action::None;
    }

    // Held down: only the rotation repeats (smoothly). The others act once per press.
    inline constexpr bool Repeats(Action action) noexcept
    {
        return action == Action::RotateBack || action == Action::RotateForward;
    }

    inline constexpr const wchar_t* Name(Action action) noexcept
    {
        switch (action)
        {
        case Action::RotateBack: return L"rotate back";
        case Action::RotateForward: return L"rotate forward";
        case Action::Promote: return L"promote";
        case Action::Demote: return L"demote";
        case Action::MoveToPrevMonitor: return L"move to previous monitor";
        case Action::MoveToNextMonitor: return L"move to next monitor";
        case Action::CycleLayout: return L"cycle layout";
        default: return L"none";
        }
    }

    // ---- which window the keys act on ----

    struct ForegroundFacts
    {
        uintptr_t window{}; // the foreground window (its root), 0 = none
        bool held{}; // Stage manages it
        bool transient{}; // shell, taskbar, an overlay, Muntin's own window, or no foreground at all: not the user going elsewhere
    };

    // Windows that briefly take the foreground without the user having left their window (taskbar, Start, overlays of other apps)
    inline bool IsTransientClass(const std::wstring& windowClass)
    {
        static const wchar_t* const classes[] = {
            L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"Progman", L"WorkerW", L"NotifyIconOverflowWindow",
            L"TopLevelWindowForOverflowXamlIsland", L"Windows.UI.Core.CoreWindow", L"XamlExplorerHostIslandWindow",
            L"ForegroundStaging", L"MultitaskingViewFrame", L"XamlWindow",
        };
        for (const auto* c : classes)
        {
            if (windowClass == c)
            {
                return true;
            }
        }

        return false;
    }

    // WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT: other apps' overlay helpers (921a309)
    inline constexpr bool IsOverlayStyle(uint64_t exStyle) noexcept
    {
        return (exStyle & (0x80ull | 0x08000000ull | 0x20ull)) != 0;
    }

    inline bool IsTransient(const std::wstring& windowClass, uint64_t exStyle, bool ownWindow)
    {
        return ownWindow || IsTransientClass(windowClass) || IsOverlayStyle(exStyle);
    }

    // Remembers the last held foreground window. Note() at every foreground change, Resolve() when a key comes in.
    class Tracker
    {
    public:
        void Note(const ForegroundFacts& fg) noexcept
        {
            if (fg.held)
            {
                m_last = fg.window;
            }
            else if (!fg.transient)
            {
                m_last = 0; // The user went to a window Stage does not manage: its keys are not ours
            }
        }

        // The window a key acts on, 0 = none (the key passes through). lastStillHeld: is that window still managed?
        template<typename Held>
        uintptr_t Resolve(const ForegroundFacts& fg, Held&& lastStillHeld) const
        {
            if (fg.held)
            {
                return fg.window;
            }

            if (fg.transient && m_last != 0 && lastStillHeld(m_last))
            {
                return m_last;
            }

            return 0;
        }

        void Reset() noexcept { m_last = 0; }
        uintptr_t Last() const noexcept { return m_last; }

    private:
        uintptr_t m_last{};
    };

    // ---- swallowing ----

    // What the keyboard hook does with one key-down / key-up. Only a chord that has an action AND a target is swallowed, together with its
    // auto-repeats and its key-up. Everything else passes through (Win itself is never swallowed: Windows sees it go down and up).
    class Gate
    {
    public:
        struct Decision
        {
            bool swallow{};
            bool fire{}; // post the action
        };

        // action: what the table says for this chord, None when the chord is not ours or there is no window to act on.
        // Repeats of a swallowed key are swallowed even when the action turned None meanwhile (focus moved, Shift released): no stuck key.
        Decision OnDown(Action action, uint32_t vk)
        {
            if (m_down.contains(vk))
            {
                const bool fire = action != Action::None && Repeats(action) && m_pending < MaxPending;
                m_pending += fire ? 1 : 0;
                return { true, fire };
            }

            if (action == Action::None)
            {
                return {};
            }

            m_down.insert(vk);
            m_pending++;
            return { true, true };
        }

        // The key-up of a swallowed key is swallowed too
        bool OnUp(uint32_t vk) { return m_down.erase(vk) > 0; }

        // The main thread handled one posted action: repeats coalesce while two or more are waiting
        void Completed() noexcept
        {
            if (m_pending > 0)
            {
                m_pending--;
            }
        }

        // Drops keys that are no longer physically down (their key-up never reached the hook: secure desktop, hook timeout), except `current`
        // (the key of the event being handled, whose async state may not be updated yet).
        template<typename PhysicallyDown>
        void Prune(PhysicallyDown&& down, uint32_t current)
        {
            for (auto it = m_down.begin(); it != m_down.end();)
            {
                it = (*it != current && !down(*it)) ? m_down.erase(it) : std::next(it);
            }
        }

        bool IsDown(uint32_t vk) const { return m_down.contains(vk); }
        size_t Pending() const noexcept { return m_pending; }
        void Reset()
        {
            m_down.clear();
            m_pending = 0;
        }

        static constexpr size_t MaxPending = 2;

    private:
        std::set<uint32_t> m_down;
        size_t m_pending{};
    };

    // ---- minimized windows, mask key ----

    // After a rotation the first window of the order is the master. Minimized windows keep their place in the order but cannot be shown:
    // rotate on in the same direction until a live window leads. rotate: one step, returns false when nothing changed; leads(i): is order[0] usable.
    // All windows minimized (or a single step impossible): the order is put back and false is returned.
    template<typename Order, typename Rotate, typename Usable>
    bool RotateToLive(Order& order, Rotate&& rotate, Usable&& usable)
    {
        const Order before = order;
        for (size_t i = 0; i < order.size(); i++)
        {
            if (!rotate(order))
            {
                break;
            }

            if (usable(order.front()))
            {
                return true;
            }
        }

        order = before;
        return false;
    }

    // The window that should take the focus: the first one in the order that is not minimized, 0 when there is none
    template<typename Order, typename Iconic>
    int FocusChoice(const Order& order, Iconic&& iconic)
    {
        for (const auto id : order)
        {
            if (!iconic(id))
            {
                return id;
            }
        }

        return 0;
    }

    // The 0xE8 mask key (stops Win from opening Start) goes out once per Win press, and only while Win is still physically down:
    // sent after Win went up it would be wasted and would wrongly count as this press's mask.
    inline constexpr bool ShouldMask(bool alreadySent, bool winPhysicallyDown) noexcept
    {
        return !alreadySent && winPhysicallyDown;
    }

    // ---- next monitor ----

    struct MonitorPos
    {
        int left{};
        int top{};
    };

    // Index (into monitors) of the left (dir < 0) or right (dir > 0) neighbour of `from` by screen position: left edge, ties by top
    // edge. Wraps: Left on the leftmost goes to the rightmost. -1 when there is no other monitor or `from` is out of range.
    inline int NextMonitor(const std::vector<MonitorPos>& monitors, int from, int dir)
    {
        if (monitors.size() < 2 || from < 0 || from >= static_cast<int>(monitors.size()))
        {
            return -1;
        }

        std::vector<int> order(monitors.size());
        for (size_t i = 0; i < order.size(); i++)
        {
            order[i] = static_cast<int>(i);
        }

        for (size_t i = 1; i < order.size(); i++) // stable insertion sort: tiny list
        {
            for (size_t j = i; j > 0; j--)
            {
                const auto& a = monitors[order[j - 1]];
                const auto& b = monitors[order[j]];
                if (std::make_pair(b.left, b.top) < std::make_pair(a.left, a.top))
                {
                    std::swap(order[j - 1], order[j]);
                }
                else
                {
                    break;
                }
            }
        }

        const int n = static_cast<int>(order.size());
        int at = 0;
        while (order[at] != from)
        {
            at++;
        }

        return order[((at + (dir > 0 ? 1 : -1)) % n + n) % n];
    }
}
