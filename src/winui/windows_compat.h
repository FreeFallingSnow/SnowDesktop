#pragma once

#include <windows.h>

// The legacy Win32 alias expands inside WinUI's Storyboard::GetCurrentTime
// declarations. Remove it before any XAML projection is included, including
// public settings headers consumed without the WinUI precompiled header.
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif
