cmake_minimum_required(VERSION 3.16)

# config.toml at the repository root is the single place to configure the
# device. Generate the Kconfig fragment from it before project() reads
# SDKCONFIG_DEFAULTS, so a change to the TOML is picked up by a plain rebuild.
set(GLASS_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
set(GLASS_CONFIG_TOML "${GLASS_REPO_ROOT}/config.toml")
set(GLASS_GEN_CONFIG "${GLASS_REPO_ROOT}/tools/gen_config.py")

set(GLASS_PYTHON "$ENV{IDF_PYTHON_ENV_PATH}/bin/python")
if(NOT EXISTS "${GLASS_PYTHON}")
    find_program(GLASS_PYTHON NAMES python3 python)
endif()

if(EXISTS "${GLASS_CONFIG_TOML}")
    execute_process(
        COMMAND "${GLASS_PYTHON}" "${GLASS_GEN_CONFIG}" --toml "${GLASS_CONFIG_TOML}"
        RESULT_VARIABLE GLASS_GEN_RC
        OUTPUT_VARIABLE GLASS_GEN_OUT
        ERROR_VARIABLE GLASS_GEN_ERR)
    if(NOT GLASS_GEN_RC EQUAL 0)
        message(FATAL_ERROR
                "config.toml is not usable:\n${GLASS_GEN_ERR}${GLASS_GEN_OUT}")
    endif()
    message(STATUS "firmware/sdkconfig.defaults.local: from config.toml")
else()
    message(STATUS
            "No config.toml - building with sdkconfig.defaults only "
            "(no Wi-Fi credential, so the device will draw the offline panel). "
            "cp config.example.toml config.toml to configure it.")
endif()

set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.local")
    list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.defaults.local")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(weather_glass)
