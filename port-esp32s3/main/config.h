#pragma once
/*
 * Upstream 44vba never committed this header -- the repo's .gitignore swallows
 * it, so port-esp32s3 does not compile as published. main.cpp and os.c include
 * it unconditionally but reference nothing from it, so an empty translation
 * unit is enough to restore the build.
 */
