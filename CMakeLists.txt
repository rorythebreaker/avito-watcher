cmake_minimum_required(VERSION 3.20)
project(AvitoWatcher VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "Avito Watcher targets Windows only.")
endif()

# Link the CRT statically so the released binary runs on a clean machine
# without the Visual C++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Everything except the window lives in a library so the self-test can link
# against exactly the code the application runs.
add_library(avitowatcher_core STATIC
    src/util/strings.cpp
    src/util/json.cpp
    src/util/paths.cpp
    src/util/log.cpp

    src/core/models.cpp
    src/core/config.cpp
    src/core/secrets.cpp
    src/core/store.cpp
    src/core/autostart.cpp

    src/net/http.cpp
    src/net/tls.cpp
    src/net/websocket.cpp
    src/net/browser.cpp
    src/net/fetcher.cpp

    src/avito/parser.cpp
    src/avito/similarity.cpp
    src/avito/engine.cpp

    src/notify/notify.cpp
    src/notify/telegram.cpp
    src/notify/mail.cpp
)

add_executable(AvitoWatcher WIN32
    src/main.cpp

    src/ui/theme.cpp
    src/ui/icon.cpp
    src/ui/images.cpp
    src/ui/dialog.cpp
    src/ui/feed_view.cpp
    src/ui/task_dialog.cpp
    src/ui/settings_dialog.cpp
    src/ui/main_window.cpp

    res/app.rc
)

target_include_directories(avitowatcher_core PUBLIC src)
target_include_directories(AvitoWatcher PRIVATE src)

set(AVITO_DEFINES
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    SECURITY_WIN32
)
target_compile_definitions(avitowatcher_core PUBLIC ${AVITO_DEFINES})
target_compile_definitions(AvitoWatcher PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    SECURITY_WIN32
)

set(AVITO_LIBS winhttp ws2_32 crypt32 secur32 shlwapi comctl32
               gdiplus shell32 user32 gdi32 ole32 uuid advapi32 msimg32
               dwmapi uxtheme)
target_link_libraries(avitowatcher_core PUBLIC ${AVITO_LIBS})
target_link_libraries(AvitoWatcher PRIVATE avitowatcher_core ${AVITO_LIBS})

if(MSVC)
    target_compile_options(avitowatcher_core PRIVATE /W4 /permissive- /utf-8 /EHsc)
    target_compile_options(AvitoWatcher PRIVATE /W4 /permissive- /utf-8 /EHsc)
    # Ask for the modern common controls so the tray and dialogs look native.
    target_link_options(AvitoWatcher PRIVATE
        "/MANIFESTDEPENDENCY:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'")
else()
    target_compile_options(avitowatcher_core PRIVATE -Wall -Wextra)
    target_compile_options(AvitoWatcher PRIVATE -Wall -Wextra -municode)
    target_link_options(AvitoWatcher PRIVATE -municode -static -static-libgcc -static-libstdc++)
endif()

set_target_properties(AvitoWatcher PROPERTIES
    OUTPUT_NAME "AvitoWatcher"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Console self-test: parses saved Avito pages and, with --live, checks a real
# request end to end. Build with -DBUILD_TESTS=ON.
option(BUILD_TESTS "Build the self-test executable" OFF)
if(BUILD_TESTS)
    add_executable(AvitoWatcherSelfTest tests/selftest.cpp)
    target_link_libraries(AvitoWatcherSelfTest PRIVATE avitowatcher_core ${AVITO_LIBS})
    if(MSVC)
        target_compile_options(AvitoWatcherSelfTest PRIVATE /W4 /permissive- /utf-8 /EHsc)
    endif()
    set_target_properties(AvitoWatcherSelfTest PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()
