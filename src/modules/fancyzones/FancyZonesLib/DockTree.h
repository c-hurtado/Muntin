#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

// Dock layout of one work area: a binary tree of splits whose leaves are windows.
// Rects are in work area coordinates.
namespace Dock
{
    enum class Edge
    {
        Left,
        Top,
        Right,
        Bottom
    };

    struct Node
    {
        // Leaf
        HWND window{}; // nullptr: an empty slot
        std::wstring app; // Process path. On an empty slot: the app the slot is kept for
        std::wstring launch; // What starts the app (a path, or shell:AppsFolder\<id> for a packaged app). Set in named layouts.
        bool hidden{}; // Minimized or hidden window: keeps its place in the tree, but takes no space
        ULONGLONG placedAt{}; // Tick count of the first time the window was moved to its place, 0 until then

        // Split
        bool sideBySide{}; // true: first is on the left of second, false: first is above second
        float ratio{ 0.5f }; // Share of the split taken by first
        std::unique_ptr<Node> first;
        std::unique_ptr<Node> second;

        bool IsLeaf() const noexcept { return !first; }
    };

    // Where a dragged window lands
    struct Target
    {
        Node* node{}; // nullptr: the whole work area
        Edge edge{};
        bool fill{}; // node is an empty slot taken over by the window
        RECT preview{};
    };

    class Tree
    {
    public:
        std::unique_ptr<Node> root;

        Tree Clone() const;
        std::vector<Node*> Leaves() const;
        Node* Find(HWND window) const;

        // edgeBand: distance in pixels from an edge of the area within which the window docks against the whole area.
        // windowEdgeShare: share of a docked window, from each of its edges, within which the window docks against it.
        // The middle of a docked window is no target, so a window can be dropped there without docking.
        // edgesOnly: only the left and right edge of the area are targets, like the edges that Windows snaps to
        // (the top one maximizes, the bottom one is the taskbar).
        std::optional<Target> TargetFromPoint(const RECT& area, POINT pt, int gap, LONG edgeBand = 48, float windowEdgeShare = 0.3f, bool edgesOnly = false);
        void Insert(HWND window, const std::wstring& app, const Target& target);
        // Where a new window goes without a drag, like Hyprland's dwindle layout: an empty slot if there is one, otherwise it splits
        // the focused window along its longer side and takes the right or bottom half. Without a focused window it splits the largest one.
        std::optional<Target> AutoTarget(const RECT& area, int gap, HWND focused);
        // Docks the window on that edge of the area. An empty slot on screen along that edge is filled instead of splitting the area,
        // an open one before one kept for an app.
        void InsertAtEdge(HWND window, const std::wstring& app, const RECT& area, int gap, Edge edge);
        // The window takes the empty slots beside it, the other windows keep their size along those splits.
        // A window that isn't docked first fills the largest empty slot on screen. False when nothing changed.
        bool Expand(HWND window, const std::wstring& app, const RECT& area, int gap);
        // Swaps the window with the leaf next to it on that side. False when there is none.
        bool Move(HWND window, const RECT& area, int gap, Edge direction);
        // The freed space goes to the neighbors
        bool Remove(HWND window);
        // Moves the splits bounding the window by the edges the user moved, from the rect the window had to the new one
        bool Resize(HWND window, const RECT& area, int gap, const RECT& fromRect, const RECT& toRect);
        // Turns the split that holds the window from side by side to one above the other, or back. False when no split divides its space.
        bool ToggleSplit(HWND window, const RECT& area, int gap);
        // Moves the nearest split of that direction so that the window gets delta pixels more (less when negative). False when nothing moved.
        bool Grow(HWND window, const RECT& area, int gap, bool sideBySide, LONG delta);

        std::vector<std::pair<HWND, RECT>> WindowRects(const RECT& area, int gap) const;
    };

    // The side of the area that a docked window sits on: left or right, unless it spans nearly the whole width
    inline Edge SideOf(const RECT& rect, const RECT& area) noexcept
    {
        constexpr double spansArea = 0.9;

        const double width = static_cast<double>(rect.right) - rect.left;
        const double areaWidth = static_cast<double>(area.right) - area.left;
        if (width < spansArea * areaWidth)
        {
            return rect.left + rect.right < area.left + area.right ? Edge::Left : Edge::Right;
        }

        return rect.top + rect.bottom < area.top + area.bottom ? Edge::Top : Edge::Bottom;
    }
}
