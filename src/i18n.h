#pragma once
#include <string>

// Tiny runtime translation layer (English source text -> Portuguese).
void setPortuguese(bool on);
bool isPortuguese();

// Translates a label, a format string or a multi-line text. Returns the input unchanged in English mode.
// The result is in the ANSI code page, ready for the *A Win32 functions.
std::string tr(const std::string& en);

// Translates a ", "-separated tag list such as "Text, Code, Reasoning".
std::string trTags(const std::string& en);
