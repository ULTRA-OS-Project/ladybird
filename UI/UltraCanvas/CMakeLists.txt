# UltraCanvas GUI backend for Ladybird.
#
# Selected with -DLADYBIRD_GUI_FRAMEWORK=UltraCanvas (or `./Meta/ladybird.py ...
# --gui UltraCanvas`). This replaces the vendor's demo-app build that shipped at
# this path; the original is preserved alongside as CMakeLists.vendor-demoapp.txt.
#
# The uploaded UltraCanvas toolkit lives in the nested UltraCanvas/ subdirectory
# (UI/UltraCanvas/UltraCanvas/) and is compiled in-tree as a static library whose
# CMake target is `UltraCanvas`.

# --- Neutralize Ladybird's global compile flags for the UltraCanvas subtree ---
# The top-level build applies -fno-exceptions and -Werror -Wall -Wextra to every
# target (Meta/CMake/compile_options.cmake). UltraCanvas relies on C++ exceptions
# and does not build cleanly under -Werror. These directory-scoped options are
# appended AFTER the inherited ones; for conflicting flags the last one wins, so
# -fexceptions overrides -fno-exceptions and -Wno-error/-w defuse -Werror. They
# apply both to the nested UltraCanvas library and to our glue sources below,
# which include UltraCanvas headers that use exceptions and RTTI.
add_compile_options(
    $<$<COMPILE_LANGUAGE:C,CXX>:-fexceptions>
    $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-error>
    $<$<COMPILE_LANGUAGE:C,CXX>:-w>
)

# --- Trim the UltraCanvas build to what the browser chrome needs ---
# Disable every optional plugin plus networking, audio, video, GL surfaces and the
# database module: the browser UI uses none of them, and turning them off keeps the
# dependency surface and build time down (and sidesteps the cargo/tesseract/MuPDF/microtex
# toolchains those plugins pull in).
#
# ULTRACANVAS_ENABLE_DATABASE is especially important to turn OFF: its UltraDatabase module
# links the *system* libsqlite3, which collides with Ladybird's vcpkg libsqlite3 in the final
# executable. With two sqlite libraries loaded, LibDatabase runs against the wrong one and every
# prepared statement fails at startup with "SQL logic error" (only reproduces here, not in Qt).
set(ULTRACANVAS_BUILD_SHARED   OFF CACHE BOOL "" FORCE)
set(ULTRACANVAS_ENABLE_GL      OFF CACHE BOOL "" FORCE)
set(ULTRACANVAS_ENABLE_AUDIO   OFF CACHE BOOL "" FORCE)
set(ULTRACANVAS_ENABLE_VIDEO   OFF CACHE BOOL "" FORCE)
set(ULTRACANVAS_ENABLE_NET     OFF CACHE BOOL "" FORCE)
set(ULTRACANVAS_ENABLE_DATABASE OFF CACHE BOOL "" FORCE)
foreach(_uc_plugin
        CDR VECTOR XAR SVG PDF OCR VECTORIZER LATEX
        SMTP IMAP POP3 JMAP WEBDAV LDAP TELNET RTSP MQTT AMQP COAP
        SNMP SSH MDNS RTMP GRPC SIP RTP)
    set(ULTRACANVAS_PLUGIN_${_uc_plugin} OFF CACHE BOOL "" FORCE)
endforeach()

add_subdirectory(UltraCanvasSources/UltraCanvas)

# --- The ladybird executable ---
add_executable(ladybird main.cpp)
target_sources(ladybird PRIVATE
    Application.cpp
    Autocomplete.cpp
    Bookmarks.cpp
    Downloads.cpp
    EventLoopImplementationUltraCanvas.cpp
    UltraCanvasPlatform.cpp
    WebContentView.cpp
    BrowserWindow.cpp
)
target_link_libraries(ladybird PRIVATE UltraCanvas)

create_ladybird_bundle(ladybird)
