#pragma once

#include <string>

namespace eb {
// Shared application body. The native entry points provide UTF-8 arguments and
// platform-specific console behavior before entering the same frontend/game path.
int run_application(int argc, char** argv);

#ifdef _WIN32
// Explorer launches have no terminal to display failures. The Windows entry
// enables dialogs only for those interactive launches, never for headless tools.
void show_desktop_error(const std::string& message);
#elif defined(__APPLE__) && defined(__has_include) && __has_include(<TargetConditionals.h>)
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
// Port/eame: an iOS app has no terminal either; the error is shown in an alert
// (desktop_application.cpp) and written to Documents/port-eame-log.txt.
void show_desktop_error(const std::string& message);
#else
inline void show_desktop_error(const std::string&) {}
#endif
#else
inline void show_desktop_error(const std::string&) {}
#endif
} // namespace eb
