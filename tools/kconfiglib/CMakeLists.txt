##
# @file kconfiglib/CMakeLists.txt
# @brief 
# @author Tuya
# @version 1.0.0
# @date 2023-04-08
#/

########################################
# First Run
########################################
# check dot-config
if(NOT EXISTS "${DOT_CONFIG_DIR}/${DOT_CONFIG}")  # copy pre-config to dot-config
    message(FATAL_ERROR "
    [KCONFIG] Don't found
    [${DOT_CONFIG_DIR}/${DOT_CONFIG}]
    Please use command: [tos.py config choice or tos.py config menu].
    ")
endif()

# use dot-config to generate cmake-config
if(NOT EXISTS "${DOT_CONFIG_DIR}/${CMAKE_CONFIG}")
    message(STATUS "[KCONFIG] Use dot-config to generate cmake-config.")
    execute_process(
        COMMAND
        python ${KCONFIG_TOOLS}/conf2cmake.py -c "${DOT_CONFIG_DIR}/${DOT_CONFIG}" -o "${DOT_CONFIG_DIR}/${CMAKE_CONFIG}"
        RESULT_VARIABLE conf2cmake_rc
        )
    if(NOT conf2cmake_rc EQUAL 0)
        message(FATAL_ERROR
            "[KCONFIG] conf2cmake.py failed (${conf2cmake_rc}); "
            "${CMAKE_CONFIG} was not generated.")
    endif()
endif()

# use dot-config to generate header-file
if(NOT EXISTS "${HEADER_DIR}/${HEADER_FILE}")
    message(STATUS "[KCONFIG] Use dot-config to generate header-file.")
    # file(MAKE_DIRECTORY), not a second COMMAND in the execute_process below:
    # CMake runs the COMMANDs of one execute_process as a *pipeline*, so they
    # start together, and conf2h.py opens its -o path without creating the
    # directory first (open(header, 'w') raises FileNotFoundError). Making the
    # directory a concurrent step therefore left whether the header appears up
    # to which process won.
    file(MAKE_DIRECTORY "${HEADER_DIR}")
    execute_process(
        COMMAND
        python ${KCONFIG_TOOLS}/conf2h.py -c "${DOT_CONFIG_DIR}/${DOT_CONFIG}" -i "${HEADER_IN_PATH}" -p "${HEADER_PARAM_LIST}" -o "${HEADER_DIR}/${HEADER_FILE}"
        RESULT_VARIABLE conf2h_rc
        )
    # Stop here rather than at the first compile of every source file: without
    # RESULT_VARIABLE the failure was silent, configure reported success, and
    # the build then said only "tuya_kconfig.h: No such file or directory" a
    # few hundred times, naming neither the generator nor its error.
    if(NOT conf2h_rc EQUAL 0)
        message(FATAL_ERROR
            "[KCONFIG] conf2h.py failed (${conf2h_rc}); "
            "${HEADER_DIR}/${HEADER_FILE} was not generated.")
    endif()
    if(NOT EXISTS "${HEADER_DIR}/${HEADER_FILE}")
        message(FATAL_ERROR
            "[KCONFIG] conf2h.py reported success but "
            "${HEADER_DIR}/${HEADER_FILE} is not there.")
    endif()
endif()
