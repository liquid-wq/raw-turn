cmake_minimum_required(VERSION 3.20)
project(saturn_ra_monitor CXX C RC)

# ---------------------------------------------------------------------------
# Ziel: EINE Datei. Keine Begleit-DLLs ausser denen, die zu Windows gehoeren.
#
# Deshalb Dear ImGui statt Qt: ImGui wird als Quelltext mitkompiliert, die
# Anbindung laeuft ueber user32/gdi32/opengl32, und die MinGW-Laufzeit wird
# statisch dazugebunden. Zum Vergleich: der Qt-Weg in mega-raw braucht
# windeployqt und liefert rund 73 Dateien aus.
#
# Fremdquellen liegen ABSICHTLICH ausserhalb des Projektordners, damit hier
# ausschliesslich eigener Code liegt.
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)

# ---------------------------------------------------------------------------
# Build-Nummer. EINZIGE Quelle -- bei jeder Aenderung hochsetzen.
#
# Sie landet in buildinfo.h (Anzeige im Fenster) UND im Dateinamen des
# Programms.
# ---------------------------------------------------------------------------
set(BUILD_NR 104)
string(TIMESTAMP BUILD_DATUM "%Y-%m-%d")
configure_file(src/buildinfo.h.in "${CMAKE_BINARY_DIR}/erzeugt/buildinfo.h" @ONLY)

# Programmsymbol als Windows-Ressource, siehe src/app.rc.in.
set(APP_ICON_PFAD "${CMAKE_SOURCE_DIR}/assets/app.ico")
configure_file(src/app.rc.in "${CMAKE_BINARY_DIR}/erzeugt/app.rc" @ONLY)

set(DEPS_DIR "${CMAKE_SOURCE_DIR}/deps" CACHE PATH "Ordner mit imgui/ und rcheevos/")
set(IMGUI_DIR "${DEPS_DIR}/imgui")
set(RCHEEVOS_DIR "${DEPS_DIR}/rcheevos")

foreach(d "${IMGUI_DIR}/imgui.cpp" "${RCHEEVOS_DIR}/include/rc_client.h")
    if(NOT EXISTS "${d}")
        message(FATAL_ERROR "Fehlt: ${d}")
    endif()
endforeach()

# --- Dear ImGui, Kern plus die zwei benoetigten Anbindungen ----------------
set(IMGUI_SOURCES
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_win32.cpp"
    # OpenGL2 (feste Funktionspipeline), NICHT OpenGL3.
    #
    # Auf einem Rechner ohne Grafiktreiber liefert Windows nur
    # "OpenGL 1.1.0 / GDI Generic".
    # Die OpenGL3-Anbindung braucht Shader und stuerzt dort beim ersten Bild
    # mit einer Zugriffsverletzung ab. Die OpenGL2-Anbindung kommt mit 1.1 aus
    # und laeuft damit auf jeder Maschine, mit und ohne echte Grafikkarte.
    "${IMGUI_DIR}/backends/imgui_impl_opengl2.cpp"
)

# --- rcheevos: die echte Bibliothek statt einer eigenen Nachbildung --------
file(GLOB RCHEEVOS_SOURCES
    "${RCHEEVOS_DIR}/src/*.c"
    "${RCHEEVOS_DIR}/src/rcheevos/*.c"
    "${RCHEEVOS_DIR}/src/rapi/*.c"
    "${RCHEEVOS_DIR}/src/rhash/*.c"
)
# Anbindung an fremde Frontends, hier nicht gebraucht.
list(FILTER RCHEEVOS_SOURCES EXCLUDE REGEX "rc_client_raintegration|rc_libretro")

add_executable(saturn_ra_monitor WIN32
    src/main.cpp
    "${CMAKE_BINARY_DIR}/erzeugt/app.rc"
    src/buildinfo.cpp
    src/theme.cpp
    src/connection.cpp
    src/ra_session.cpp
    src/setup_window.cpp
    src/unlock_popup.cpp
    # Kernschicht: ohne jeden Oberflaechen-Bezug, damit sie sich unveraendert
    # auf anderen Zielen uebersetzen laesst -- dieselbe Regel, die sich in
    # mega-raw beim Sprung auf den ESP32 ausgezahlt hat.
    src/core/protocol.cpp
    src/core/mailbox.cpp
    src/core/serial.cpp
    src/core/http.cpp
    src/core/ra.cpp
    src/core/game_index.cpp
    src/core/setup.cpp
    src/core/dialoge.cpp
    src/core/i18n.cpp
    src/core/bps.cpp
    src/core/firmware.cpp
    ${IMGUI_SOURCES}
    ${RCHEEVOS_SOURCES}
)

target_include_directories(saturn_ra_monitor PRIVATE
    src
    "${CMAKE_BINARY_DIR}/erzeugt"
    "${IMGUI_DIR}"
    "${IMGUI_DIR}/backends"
    "${RCHEEVOS_DIR}/include"
    "${RCHEEVOS_DIR}/src"
    "${RCHEEVOS_DIR}/src/rcheevos"
    "${RCHEEVOS_DIR}/src/rhash"
)

target_compile_definitions(saturn_ra_monitor PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    RC_DISABLE_LUA
)

target_link_libraries(saturn_ra_monitor PRIVATE
    opengl32 gdi32 user32 shell32 comdlg32 dwmapi
    setupapi        # Auflisten der seriellen Anschluesse samt USB-Kennung
    winhttp         # HTTPS zu RetroAchievements, siehe src/core/http.cpp
    gdiplus ole32   # Freischalt-Einblendung samt PNG-Badges, siehe src/unlock_popup.cpp
    winmm           # Freischalt-Ton
)

if(MINGW)
    # Der Kern der Sache: keine libstdc++/libgcc/libwinpthread daneben.
    #
    # -municode ist Pflicht, sobald der Einstiegspunkt wWinMain heisst.
    # Ohne das sucht der MinGW-Startcode stur nach WinMain und der Link
    # scheitert mit "undefined reference to `WinMain'", obwohl der Quelltext
    # in Ordnung ist.
    target_link_options(saturn_ra_monitor PRIVATE
        -municode -static -static-libgcc -static-libstdc++)
endif()

# Das fertige Programm landet in bin/, getrennt von den Zwischenstufen in
# build/. Der Ausgabeordner laesst sich mit -DAUSGABE_DIR=<Ordner> aendern.
set(AUSGABE_DIR "${CMAKE_SOURCE_DIR}/bin" CACHE PATH "Ablage der fertigen Programme")
set_target_properties(saturn_ra_monitor PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${AUSGABE_DIR}"
    # Build-Nummer im Dateinamen, siehe BUILD_NR oben.
    OUTPUT_NAME "saroo_monitor_b${BUILD_NR}")



# ---------------------------------------------------------------------------
# Selbsttest der Kernschicht. Konsolenprogramm, kein Fenster.
#
# Prueft, was ohne Geraet und ohne RA-Zugang pruefbar ist. Baut absichtlich
# NUR die core/-Dateien mit -- scheitert das, ist die Trennung verletzt und
# die Kernschicht haengt doch an der Oberflaeche.
# ---------------------------------------------------------------------------
add_executable(selbsttest
    src/selbsttest.cpp
    src/core/protocol.cpp
    src/core/mailbox.cpp
    src/core/serial.cpp
    src/core/http.cpp
    src/core/ra.cpp
    src/core/game_index.cpp
    src/core/setup.cpp
    src/core/dialoge.cpp
    src/core/i18n.cpp
    src/core/bps.cpp
    src/core/firmware.cpp
    ${RCHEEVOS_SOURCES}
)
target_include_directories(selbsttest PRIVATE
    src "${RCHEEVOS_DIR}/include" "${RCHEEVOS_DIR}/src"
    "${RCHEEVOS_DIR}/src/rcheevos" "${RCHEEVOS_DIR}/src/rhash")
target_compile_definitions(selbsttest PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX RC_DISABLE_LUA)
target_link_libraries(selbsttest PRIVATE setupapi winhttp shell32 comdlg32 ole32)
if(MINGW)
    target_link_options(selbsttest PRIVATE -static -static-libgcc -static-libstdc++)
endif()
set_target_properties(selbsttest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")

