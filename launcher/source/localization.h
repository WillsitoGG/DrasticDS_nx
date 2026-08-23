#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace LauncherLocalization
{
struct Language { const char* code; const char* name; };

void Initialize(std::string_view preference);
std::string_view Translate(std::string_view source);
std::string_view Preference();
std::string_view CurrentLanguage();
std::string DisplayName();
const std::vector<Language>& Languages();
int FindLanguage(std::string_view code);
}

