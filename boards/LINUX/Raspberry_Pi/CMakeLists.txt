##
# @file CMakeLists.txt
# @brief Raspberry Pi board configuration
#/

# MODULE_PATH
set(MODULE_PATH ${CMAKE_CURRENT_LIST_DIR})
get_filename_component(COMMON_PATH "${MODULE_PATH}/../common" ABSOLUTE)

# MODULE_NAME
# Derive from the directory name: a CMakeLists copied to a new board can not
# silently keep the source board's target name.
get_filename_component(MODULE_NAME ${MODULE_PATH} NAME)

# LIB_SRCS
file(GLOB_RECURSE LIB_SRCS "${MODULE_PATH}/*.c")

# Shared V4L2 camera TDD (boards/LINUX/common/camera), one copy for every board
if (CONFIG_ENABLE_CAMERA STREQUAL "y" AND CONFIG_ENABLE_CAMERA_V4L2 STREQUAL "y")
    list(APPEND LIB_SRCS "${COMMON_PATH}/camera/tdd_camera_v4l2.c")
    message(STATUS "${MODULE_NAME} board: Enabling V4L2 camera driver")

    # Same condition the platform tree uses; its add_definitions() runs in a
    # separate build and never reaches this target.
    set(MPP_PREBUILT_DIR
        "${TOP_SOURCE_DIR}/platform/${TOS_PROJECT_PLATFORM}/tuyaos_adapter/src/tkl_venc/libs/${PLATFORM_CHIP}/mpp")
    if (EXISTS "${MPP_PREBUILT_DIR}/lib/librockchip_mpp.so")
        set(BOARD_HAS_MPP_VENC ON)
        message(STATUS "${MODULE_NAME} board: H264 via MPP hardware encoder")
    endif()
endif()

# LIB_PUBLIC_INC
set(LIB_PUBLIC_INC ${MODULE_PATH} ${COMMON_PATH}/camera)


########################################
# Target Configure
########################################
add_library(${MODULE_NAME})

target_sources(${MODULE_NAME}
    PRIVATE
        ${LIB_SRCS}
    )

target_include_directories(${MODULE_NAME}
    PRIVATE
        ${LIB_PRIVATE_INC}

    PUBLIC
        ${LIB_PUBLIC_INC}
    )

if (BOARD_HAS_MPP_VENC)
    target_compile_definitions(${MODULE_NAME} PRIVATE ENABLE_TKL_VENC_MPP=1)
    target_include_directories(${MODULE_NAME} PRIVATE
        "${TOP_SOURCE_DIR}/platform/${TOS_PROJECT_PLATFORM}/tuyaos_adapter/include"
        "${MPP_PREBUILT_DIR}/include")
endif()

# Link ALSA library if ALSA support is enabled
if (CONFIG_ENABLE_AUDIO_ALSA STREQUAL "y")
    target_link_libraries(${MODULE_NAME} PUBLIC asound)
    message(STATUS "Raspberry Pi board: Linking ALSA library")
endif()

# Link SDL2 library if SDL display support is enabled
if (CONFIG_ENABLE_SDL_DISPLAY STREQUAL "y")
    set(SDL2_PREBUILT_DIR "${TOP_SOURCE_DIR}/platform/LINUX/tuyaos_adapter/src/tkl_display/libs/Raspberry_Pi/SDL2")
    target_include_directories(${MODULE_NAME} PUBLIC "${SDL2_PREBUILT_DIR}/include/SDL2")
    target_link_libraries(${MODULE_NAME} PUBLIC "${SDL2_PREBUILT_DIR}/libSDL2.a" pthread dl)
    message(STATUS "Raspberry Pi board: Linking prebuilt ARM64 SDL2 static library (X11 via dlopen)")
endif()

########################################
# Layer Configure
########################################
list(APPEND COMPONENT_LIBS ${MODULE_NAME})
set(COMPONENT_LIBS "${COMPONENT_LIBS}" PARENT_SCOPE)
list(APPEND COMPONENT_PUBINC ${LIB_PUBLIC_INC})
set(COMPONENT_PUBINC "${COMPONENT_PUBINC}" PARENT_SCOPE)

