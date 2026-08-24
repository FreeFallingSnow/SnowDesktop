#include "pch.h"

// CMake's desktop vcxproj does not discover this C++/WinRT generated source
// as a compile item.  Keep the generated implementation in the binary tree
// and compile it through a stable source-tree bridge.
#include <XamlMetaDataProvider.cpp>
