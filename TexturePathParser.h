#pragma once


namespace ngg {
namespace mw {

class TextureHashTable;

namespace paths {

// Parse JSON and build hash→path map (FAST - no texture loading!)
// Matches original ASI behavior: sub_1005DD50 only parses JSON
void ParseTexturePaths(TextureHashTable* hashTable, bool& pathsLoaded);

} // namespace paths
} // namespace mw
} // namespace ngg

