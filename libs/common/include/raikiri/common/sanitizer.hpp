#pragma once

// tsan's runtime defines the global new and delete, so a test counting allocations cannot replace them under it
#if defined(__SANITIZE_THREAD__)
#define RAIKIRI_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define RAIKIRI_TSAN 1
#endif
#endif
#ifndef RAIKIRI_TSAN
#define RAIKIRI_TSAN 0
#endif
