# tests

`compactor_test.cpp` drives `ReplayCompactor` (the code that cuts an instant replay out of the buffer) with real
recordings, without the game: it is plain C++ plus snappy.

```
clang-cl /std:c++20 /EHsc /MD /I src /I <snappy>\include tests\compactor_test.cpp src\playback\record\ReplayCompactor.cpp <snappy>\lib\snappy.lib /Fe:compactor_test.exe
compactor_test.exe <outDir> <cacheDir>=<chunk file> [<cacheDir>=<chunk file> ...]
```

Unzip two recordings, join one chunk of each (each with its own `level_chunk_caches`, as two buffer sessions would
have), add a `metadata.json`, zip it with `/` separators and convert it with FlashBridge: the Flashback replay must
match converting the two recordings separately (same chunk sizes, and the cached chunk packets of the result must be
exactly the union of both). That is how the compactor was checked.
