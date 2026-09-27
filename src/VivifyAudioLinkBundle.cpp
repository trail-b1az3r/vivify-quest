// The AudioLink bundle (assets/audiolink/AudioLink.bundle), linked into the
// library as-is. CMake passes its path in VIVIFY_AUDIOLINK_BUNDLE; a build
// without it (the host syntax check) links an empty one, and AudioLink stays
// off with a log line saying so.
#ifdef VIVIFY_AUDIOLINK_BUNDLE
asm(".section .rodata\n"
    ".global vivify_audiolink_bundle_start\n"
    ".balign 16\n"
    "vivify_audiolink_bundle_start:\n"
    ".incbin \"" VIVIFY_AUDIOLINK_BUNDLE "\"\n"
    ".global vivify_audiolink_bundle_end\n"
    "vivify_audiolink_bundle_end:\n"
    ".previous\n");
#else
extern "C" unsigned char const vivify_audiolink_bundle_start[1] = {0};
extern "C" unsigned char const vivify_audiolink_bundle_end[1] = {0};
#endif
