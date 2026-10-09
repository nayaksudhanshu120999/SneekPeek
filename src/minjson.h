#pragma once
// SneekPeek - minimal JSON "..." string decoder. Header-only, no Win32
// dependency beyond MultiByteToWideChar, so unit tests exercise it directly.
// Handles \" \\ \n \t \r and \uXXXX escapes, including UTF-16 surrogate
// pairs (lone surrogates are rejected).
#include <windows.h>
#include <string>

inline bool DecodeJsonString(const char*& p, const char* end, std::wstring& out) {
    // *p must point at the opening quote. Decodes UTF-8 bytes, then converts.
    std::string raw;
    p++; // opening quote
    auto pushCodepoint = [&](unsigned v) {
        if (v < 0x80) raw += (char)v;
        else if (v < 0x800) {
            raw += (char)(0xC0 | (v >> 6));
            raw += (char)(0x80 | (v & 63));
        } else if (v < 0x10000) {
            raw += (char)(0xE0 | (v >> 12));
            raw += (char)(0x80 | ((v >> 6) & 63));
            raw += (char)(0x80 | (v & 63));
        } else {
            raw += (char)(0xF0 | (v >> 18));
            raw += (char)(0x80 | ((v >> 12) & 63));
            raw += (char)(0x80 | ((v >> 6) & 63));
            raw += (char)(0x80 | (v & 63));
        }
    };
    auto hexVal = [](char h, unsigned& d) {
        if (h >= '0' && h <= '9') d = (unsigned)(h - '0');
        else if (h >= 'a' && h <= 'f') d = (unsigned)(h - 'a' + 10);
        else if (h >= 'A' && h <= 'F') d = (unsigned)(h - 'A' + 10);
        else return false;
        return true;
    };
    while (p < end && *p != '"') {
        if (*p == '\\' && p + 1 < end) {
            p++;
            char e = *p++;
            if (e == 'u' && p + 4 <= end) {
                unsigned v = 0;
                for (int k = 0; k < 4; k++) {
                    unsigned d = 0;
                    if (!hexVal(*p++, d)) return false;
                    v = (v << 4) | d;
                }
                if (v >= 0xD800 && v <= 0xDBFF) {
                    // high surrogate: must be followed by \uDC00-\uDFFF
                    if (p + 6 > end || p[0] != '\\' || p[1] != 'u') return false;
                    p += 2;
                    unsigned lo = 0;
                    for (int k = 0; k < 4; k++) {
                        unsigned d = 0;
                        if (!hexVal(*p++, d)) return false;
                        lo = (lo << 4) | d;
                    }
                    if (lo < 0xDC00 || lo > 0xDFFF) return false;
                    v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00);
                } else if (v >= 0xDC00 && v <= 0xDFFF) {
                    return false; // lone low surrogate
                }
                pushCodepoint(v);
            } else if (e == 'n') raw += '\n';
            else if (e == 't') raw += '\t';
            else if (e == 'r') raw += '\r';
            else raw += e;
        } else {
            raw += *p++;
        }
    }
    if (p < end) p++; // closing quote
    int n = MultiByteToWideChar(CP_UTF8, 0, raw.c_str(), (int)raw.size(), NULL, 0);
    if (n <= 0) return false;
    out.assign((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, raw.c_str(), (int)raw.size(), out.data(), n);
    return true;
}
