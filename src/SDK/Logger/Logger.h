#pragma once

#if defined(__GNUC__) || defined(__clang__)
#define OVR_PRINTF_FORMAT(FormatIndex, FirstArgIndex) __attribute__((format(gnu_printf, FormatIndex, FirstArgIndex)))
#else
#define OVR_PRINTF_FORMAT(FormatIndex, FirstArgIndex)
#endif

namespace Logger
{
    bool Init();
    void Write(const char* Prefix, const char* Format, ...) OVR_PRINTF_FORMAT(2, 3);
}

#define Log(...) Logger::Write("[+]", __VA_ARGS__)
#define LogW(...) Logger::Write("[!]", __VA_ARGS__)
#define LogE(...) Logger::Write("[-]", __VA_ARGS__)
