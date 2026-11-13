#pragma once
#define ANDROID_LOG_ERROR 6
#define ANDROID_LOG_INFO 4
inline int __android_log_print(int, char const*, char const*, ...) { return 0; }
