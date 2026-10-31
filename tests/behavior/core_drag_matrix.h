#pragma once

// External real-file ingress. Other core drag domains remain required.
namespace drag_matrix
{
struct Gesture { const char* name; DWORD keys; DWORD effect; };
struct Case
{
    bool asynchronous;
    bool directory;
    Gesture release;
    DWORD allowed;
    bool enterShift;
    bool cancel;
    std::string_view target = "desktop";
    bool icon = false;
};
inline constexpr std::string_view targets[]{"desktop", "container-collection", "container-files",
    "container-mapping", "container-collection-group", "container-file-group", "container-mapping-group"};
inline constexpr bool iconValues[]{false, true};
inline constexpr bool asyncValues[]{false, true};
inline constexpr bool directoryValues[]{false, true};
inline constexpr Gesture gestures[]{
    {"copy", MK_CONTROL, DROPEFFECT_COPY},
    {"move", MK_SHIFT, DROPEFFECT_MOVE},
    {"link", MK_ALT, DROPEFFECT_LINK},
};
inline constexpr DWORD capabilities[]{0, 1, 2, 3, 4, 5, 6, 7};
inline constexpr bool enterValues[]{false, true};
inline constexpr bool cancelValues[]{false, true};

inline std::string Id(const Case& c)
{
    return std::string("external-") + std::string(c.target) +
        (c.target == "desktop" ? "" : c.icon ? ".icon" : ".expanded") + "." + (c.asynchronous ? "async" : "sync") +
        "." + (c.directory ? "file-directory" : "file") + "." + (c.cancel ? "no-release" : c.release.name) +
        ".cap" + std::to_string(c.allowed) + "." + (c.enterShift ? "enter-shift" : "enter-plain") +
        "." + (c.cancel ? "leave" : "drop");
}
inline constexpr std::size_t RawCount()
{
    return std::size(targets) * std::size(iconValues) * std::size(asyncValues) * std::size(directoryValues) * std::size(gestures) *
        std::size(capabilities) * std::size(enterValues) * std::size(cancelValues);
}
inline std::vector<Case> Enumerate()
{
    std::vector<Case> result;
    for (const auto target : targets) for (bool icon : iconValues)
    for (bool async : asyncValues) for (bool directory : directoryValues)
        for (const auto& gesture : gestures) for (DWORD allowed : capabilities)
            for (bool enter : enterValues) for (bool cancel : cancelValues)
            {
                // Desktop is not a single-cell widget representation.
                if (target == "desktop" && icon) continue;
                // Leave has no release event; retain one canonical no-release value.
                if (!cancel || gesture.effect == gestures[0].effect)
                    result.push_back({async, directory, gesture, allowed, enter, cancel, target, icon});
            }
    return result;
}
}
