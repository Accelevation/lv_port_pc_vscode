cmake_minimum_required(VERSION 3.12.4)
project(lvgl C CXX)

set(HMI_ROOT_DIR "${PROJECT_SOURCE_DIR}/../..")
set(HMI_SHARED_UI_DIR "${HMI_ROOT_DIR}/src/ui")

set(LVGL_PRO_PROJECT_DIR
    ""
    CACHE PATH "Path to LVGL Pro Project folder")

# FreeRTOS is always built in — every target (sim, h745-hmi, h757-hmi) runs
# the FreeRTOS two-task model, and src/ui/runtime.cpp is the RTOS bridge
# layer, not a file that happens to use FreeRTOS.
message(STATUS "FreeRTOS is enabled (MSVC-MingW Windows simulator port)")

# Sim data producer: "dummy" (internal wander task) or "socket" (TCP transport
# on :5555 feeding the v1 codec). Default keeps zero-setup runs working.
set(PRODUCER "dummy" CACHE STRING "Sim data producer: dummy | socket")

# The vendored kernel ships both the GCC/POSIX port (uses pthreads + SIGALRM,
# won't compile under MSVC) and the MSVC-MingW Windows simulator port. We use
# the latter so the same sim builds under both MSVC and MinGW on Windows.
set(FREERTOS_PORT_DIR "${PROJECT_SOURCE_DIR}/FreeRTOS/portable/MSVC-MingW")

include_directories(${PROJECT_SOURCE_DIR}/FreeRTOS/include)
include_directories(${FREERTOS_PORT_DIR})
include_directories(${PROJECT_SOURCE_DIR}/config)

set(FREERTOS_SOURCES
    ${PROJECT_SOURCE_DIR}/FreeRTOS/croutine.c
    ${PROJECT_SOURCE_DIR}/FreeRTOS/event_groups.c
    ${PROJECT_SOURCE_DIR}/FreeRTOS/list.c
    ${PROJECT_SOURCE_DIR}/FreeRTOS/queue.c
    ${PROJECT_SOURCE_DIR}/FreeRTOS/stream_buffer.c
    ${PROJECT_SOURCE_DIR}/FreeRTOS/tasks.c
    ${PROJECT_SOURCE_DIR}/FreeRTOS/timers.c
    ${PROJECT_SOURCE_DIR}/FreeRTOS/portable/MemMang/heap_4.c
    ${FREERTOS_PORT_DIR}/port.c
)

# Main include files of the project
include_directories(${PROJECT_SOURCE_DIR}/main/inc)

# Define options for LVGL with default values (OFF)
option(LV_USE_DRAW_SDL "Use SDL draw unit" OFF)
option(LV_USE_LIBPNG "Use libpng to decode PNG" OFF)
option(LV_USE_LIBJPEG_TURBO "Use libjpeg turbo to decode JPEG" OFF)
option(LV_USE_FFMPEG "Use libffmpeg to display video using lv_ffmpeg" OFF)
option(LV_USE_FREETYPE "Use freetype library" OFF)

# Set C and C++ standards
set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set the output path for the executable
set(EXECUTABLE_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/bin)
set(WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

# Find and include SDL2 library
find_package(SDL2 REQUIRED)

# Add compile definitions based on the selected options
add_compile_definitions($<$<BOOL:${LV_USE_DRAW_SDL}>:LV_USE_DRAW_SDL=1>)
add_compile_definitions($<$<BOOL:${LV_USE_LIBPNG}>:LV_USE_LIBPNG=1>)
add_compile_definitions($<$<BOOL:${LV_USE_LIBJPEG_TURBO}>:LV_USE_LIBJPEG_TURBO=1>)
add_compile_definitions($<$<BOOL:${LV_USE_FFMPEG}>:LV_USE_FFMPEG=1>)

# Add LVGL subdirectory
add_subdirectory(lvgl)
target_include_directories(lvgl PUBLIC ${PROJECT_SOURCE_DIR} ${SDL2_INCLUDE_DIRS})

set(MAIN_SOURCES
    src/mouse_cursor_icon.c
    src/hal/hal.c
    src/settings_backend_file.c
    ${HMI_SHARED_UI_DIR}/assets/logo.c
    ${HMI_SHARED_UI_DIR}/assets/montserrat_bold_96.c
    ${HMI_SHARED_UI_DIR}/navigation.cpp
    ${HMI_SHARED_UI_DIR}/runtime.cpp
    ${HMI_SHARED_UI_DIR}/theme.cpp
    ${HMI_SHARED_UI_DIR}/ui_fmt.cpp
    ${HMI_SHARED_UI_DIR}/ui.cpp
    ${HMI_SHARED_UI_DIR}/views/dashboard_view.cpp
    ${HMI_SHARED_UI_DIR}/views/summary_view.cpp
    ${HMI_SHARED_UI_DIR}/views/breakers_view.cpp
    ${HMI_SHARED_UI_DIR}/views/telemetry_view.cpp
    ${HMI_SHARED_UI_DIR}/views/config_view.cpp
    ${HMI_SHARED_UI_DIR}/views/alarms_view.cpp
    ${HMI_SHARED_UI_DIR}/views/network_settings_dialog.cpp
    ${HMI_SHARED_UI_DIR}/views/clock_dialog.cpp
    ${HMI_SHARED_UI_DIR}/widgets/action_button.cpp
    ${HMI_SHARED_UI_DIR}/widgets/app_shell.cpp
    ${HMI_SHARED_UI_DIR}/widgets/dialog.cpp
    ${HMI_SHARED_UI_DIR}/widgets/icon_button.cpp
    ${HMI_SHARED_UI_DIR}/widgets/metric_box.cpp
    ${HMI_SHARED_UI_DIR}/widgets/nav_item.cpp
    ${HMI_SHARED_UI_DIR}/widgets/panel_card.cpp
    ${HMI_SHARED_UI_DIR}/widgets/primitives.cpp
    ${HMI_SHARED_UI_DIR}/widgets/screen.cpp
    ${HMI_ROOT_DIR}/src/comms/comms.c
    ${HMI_ROOT_DIR}/src/comms/frame_codec_v1.c
    ${HMI_ROOT_DIR}/src/canbus/canbus.c
    ${HMI_ROOT_DIR}/src/demo/demo_source.c
    ${HMI_ROOT_DIR}/src/settings/settings.c
)
set(MAIN_LIBS lvgl lvgl::examples lvgl::demos lvgl::thorvg ${SDL2_LIBRARIES})

list(APPEND MAIN_SOURCES src/freertos_main.c)

if(PRODUCER STREQUAL "socket")
    list(APPEND MAIN_SOURCES src/socket_transport.c)
endif()

list(APPEND MAIN_SOURCES ${FREERTOS_SOURCES})

if(NOT MSVC)
    list(APPEND MAIN_LIBS m pthread)
endif()

# Custom target to run the executable
add_custom_target(run COMMAND ${EXECUTABLE_OUTPUT_PATH}/main DEPENDS main)

# Conditionally include and link SDL2_image if LV_USE_DRAW_SDL is enabled
if(LV_USE_DRAW_SDL)
    set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake")
    find_package(SDL2_image REQUIRED)
    target_include_directories(lvgl PUBLIC ${SDL2_IMAGE_INCLUDE_DIRS})
    list(APPEND MAIN_LIBS ${SDL2_IMAGE_LIBRARIES})
endif()

# Conditionally include and link libpng if LV_USE_LIBPNG is enabled
if(LV_USE_LIBPNG)
    find_package(PNG REQUIRED)
    target_include_directories(lvgl PUBLIC ${PNG_INCLUDE_DIRS})
    list(APPEND MAIN_LIBS ${PNG_LIBRARIES})
endif()

# Conditionally include and link libjpeg-turbo if LV_USE_LIBJPEG_TURBO is enabled
if(LV_USE_LIBJPEG_TURBO)
    find_package(JPEG REQUIRED)
    target_include_directories(lvgl PUBLIC ${JPEG_INCLUDE_DIRS})
    list(APPEND MAIN_LIBS ${JPEG_LIBRARIES})
endif()

# Conditionally include and link FFmpeg libraries if LV_USE_FFMPEG is enabled
if(LV_USE_FFMPEG)
    list(APPEND MAIN_LIBS avformat avcodec avutil swscale z)
endif()

# Conditionally include and link FreeType if LV_USE_FREETYPE is enabled
if(LV_USE_FREETYPE)
    find_package(Freetype REQUIRED)
    target_include_directories(lvgl PUBLIC ${FREETYPE_INCLUDE_DIRS})
    list(APPEND MAIN_LIBS ${FREETYPE_LIBRARIES})
endif()

if(LVGL_PRO_PROJECT_DIR)
  if(EXISTS "${LVGL_PRO_PROJECT_DIR}/CMakeLists.txt")
    add_subdirectory(${LVGL_PRO_PROJECT_DIR} ui)
    list(APPEND MAIN_LIBS lib-ui)

    target_compile_definitions(lib-ui PUBLIC LV_LVGL_H_INCLUDE_SIMPLE)
    target_include_directories(lib-ui PUBLIC
      ${LVGL_PRO_PROJECT_DIR}  # local headers like ui.h, generated files, etc.
      ${CMAKE_SOURCE_DIR}/lvgl # include "lvgl.h" works
    )
    message(STATUS "LVGL Pro Project enabled from: ${LVGL_PRO_PROJECT_DIR}")
  else()
    message(
      WARNING
        "LVGL_PRO_PROJECT_DIR specified but no CMakeLists.txt found at: ${LVGL_PRO_PROJECT_DIR}"
    )
  endif()
endif()

add_executable(main ${MAIN_SOURCES})

# --- Host unit test for the portable frame codec (no LVGL/SDL/RTOS) ---
add_executable(test_codec
    ${HMI_ROOT_DIR}/src/comms/frame_codec_v1.c
    ${HMI_ROOT_DIR}/src/comms/comms.c
    ${HMI_ROOT_DIR}/src/comms/test/test_frame_codec_v1.c
)
target_include_directories(test_codec PRIVATE
    ${HMI_ROOT_DIR}/src/comms
    ${HMI_SHARED_UI_DIR}
)
if (WIN32 AND MSVC)
    target_link_options(test_codec PRIVATE "/SUBSYSTEM:CONSOLE")
endif()

# --- Host unit test for the portable CAN decode module (no LVGL/SDL/RTOS) ---
add_executable(test_canbus
    ${HMI_ROOT_DIR}/src/canbus/canbus.c
    ${HMI_ROOT_DIR}/src/canbus/test/test_canbus.c
)
target_include_directories(test_canbus PRIVATE
    ${HMI_ROOT_DIR}/src/canbus
    ${HMI_SHARED_UI_DIR}
)
if (WIN32 AND MSVC)
    target_link_options(test_canbus PRIVATE "/SUBSYSTEM:CONSOLE")
endif()

# --- Host unit test for the shared UI formatting helpers (no LVGL/SDL/RTOS) ---
add_executable(test_ui_fmt
    ${HMI_SHARED_UI_DIR}/ui_fmt.cpp
    ${HMI_SHARED_UI_DIR}/test/test_ui_fmt.cpp
)
target_include_directories(test_ui_fmt PRIVATE ${HMI_SHARED_UI_DIR})
if (WIN32 AND MSVC)
    target_link_options(test_ui_fmt PRIVATE "/SUBSYSTEM:CONSOLE")
endif()

# --- Host unit test for the portable demo source (no LVGL/SDL/RTOS) ---
add_executable(test_demo_source
    ${HMI_ROOT_DIR}/src/demo/demo_source.c
    ${HMI_ROOT_DIR}/src/demo/test/test_demo_source.c
)
target_include_directories(test_demo_source PRIVATE
    ${HMI_ROOT_DIR}/src/demo
    ${HMI_SHARED_UI_DIR}
)
if (WIN32 AND MSVC)
    target_link_options(test_demo_source PRIVATE "/SUBSYSTEM:CONSOLE")
endif()

# --- Host unit test for the portable settings store (no LVGL/SDL/RTOS) ---
add_executable(test_settings
    ${HMI_ROOT_DIR}/src/settings/settings.c
    ${HMI_ROOT_DIR}/src/settings/test/fake_backend.c
    ${HMI_ROOT_DIR}/src/settings/test/test_settings.c
)
target_include_directories(test_settings PRIVATE
    ${HMI_ROOT_DIR}/src/settings
)
if (WIN32 AND MSVC)
    target_link_options(test_settings PRIVATE "/SUBSYSTEM:CONSOLE")
endif()

# --- Host unit test for the portable transport registry (no LVGL/SDL/RTOS) ---
add_executable(test_transport
    ${HMI_ROOT_DIR}/src/transport/transport.c
    ${HMI_ROOT_DIR}/src/transport/test/test_transport.c
)
target_include_directories(test_transport PRIVATE ${HMI_ROOT_DIR}/src/transport)
if (WIN32 AND MSVC)
    target_link_options(test_transport PRIVATE "/SUBSYSTEM:CONSOLE")
endif()

target_compile_definitions(main PRIVATE LV_CONF_INCLUDE_SIMPLE)

if(PRODUCER STREQUAL "socket")
    target_compile_definitions(main PRIVATE PRODUCER_SOCKET=1)
    message(STATUS "Sim producer: socket transport on :5555")
    if(WIN32)
        target_link_libraries(main ws2_32)
    endif()
else()
    message(STATUS "Sim producer: internal dummy wander task")
endif()

target_include_directories(main PRIVATE ${HMI_SHARED_UI_DIR} ${HMI_ROOT_DIR}/src/comms ${HMI_ROOT_DIR}/src/canbus ${HMI_ROOT_DIR}/src/demo ${HMI_ROOT_DIR}/src/settings)
target_include_directories(main PRIVATE
    ${PROJECT_SOURCE_DIR}/FreeRTOS/include
    ${PROJECT_SOURCE_DIR}/FreeRTOS/portable/MSVC-MingW
    ${PROJECT_SOURCE_DIR}/config
)
target_link_libraries(main ${MAIN_LIBS})

# On Windows, GUI applications do not show a console by default,
# which hides log output. This ensures a console is available for logging.
if (WIN32)
    if (MSVC)
        target_link_options(main PRIVATE "/SUBSYSTEM:CONSOLE")
    else()
        target_link_options(main PRIVATE "-mconsole")
    endif()
endif()

# Apply additional compile options if the build type is Debug
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(STATUS "Debug mode enabled")

    if(NOT MSVC)
        target_compile_options(lvgl PRIVATE
            -pedantic-errors
            -Wall
            -Wclobbered
            -Wdeprecated
            -Wdouble-promotion
            -Wempty-body
            -Wextra
            -Wformat-security
            -Wmaybe-uninitialized
            # -Wmissing-prototypes
            -Wpointer-arith
            -Wmultichar
            -Wno-pedantic # ignored for now, we convert functions to pointers for properties table.
            -Wreturn-type
            -Wshadow
            -Wshift-negative-value
            -Wsizeof-pointer-memaccess
            -Wtype-limits
            -Wundef
            -Wuninitialized
            -Wunreachable-code
            -Wfloat-conversion
            -Wstrict-aliasing
        )
    endif()

if (ASAN)
    message(STATUS "AddressSanitizer enabled")

    # Add AddressSanitizer flags
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address -fno-omit-frame-pointer")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address -fno-omit-frame-pointer")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fsanitize=address")
else()
    message(STATUS "AddressSanitizer disabled")
endif()
endif()
