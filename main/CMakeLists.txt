set(WEB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/web")
set(WEB_DIST_DIR "${WEB_DIR}/dist")

# ── Schema code generation paths ──────────────────────────────────────────────
set(JSON_GEN_C_SRC_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/../libs/json-gen-c")
set(JSON_GEN_C_HOST_DIR "${CMAKE_BINARY_DIR}/json-gen-c-host")
set(JSON_GEN_C_BIN      "${JSON_GEN_C_HOST_DIR}/json-gen-c")
set(SCHEMA_GEN_DIR      "${CMAKE_CURRENT_BINARY_DIR}/json_gen")
set(SCHEMA_GEN_SRCS
    "${SCHEMA_GEN_DIR}/json.gen.c"
    "${SCHEMA_GEN_DIR}/sstr.c"
)

# ── Component registration ────────────────────────────────────────────────────

idf_component_register(
    SRCS
        "uni-gtw.c"
        "webserver.c"
        "ethernet_manager.c"
        "config.c"
        "background_worker.c"
        "mqtt.c"
        "utils.c"
        "sdcard.c"
        "wiegand.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_eth
        esp_http_server
        nvs_flash
        esp_event
        esp_netif
        esp_driver_gpio
        esp_driver_spi
        esp_driver_sdspi
        esp_hw_support
        esp_timer
        littlefs
        lwip
        mdns
        mqtt
        mbedtls
        fatfs
        sdmmc
    EMBED_FILES
        ${WEB_DIST_DIR}/index.html
        ${WEB_DIST_DIR}/app.js
        ${WEB_DIST_DIR}/app.css
)

if(NOT CMAKE_BUILD_EARLY_EXPANSION)

  # ── pnpm web build ────────────────────────────────────────────────────────
  file(GLOB_RECURSE WEB_SRC_FILES
        "${WEB_DIR}/src/*.ts"
        "${WEB_DIR}/src/*.tsx"
        "${WEB_DIR}/src/*.css"
        "${WEB_DIR}/index.html"
    )
  set(WEB_CONFIG_FILES
        "${WEB_DIR}/package.json"
        "${WEB_DIR}/tsconfig.json"
        "${WEB_DIR}/vite.config.js"
    )

  set(PNPM_STAMP "${CMAKE_CURRENT_BINARY_DIR}/pnpm_build.stamp")

  add_custom_command(
        OUTPUT "${PNPM_STAMP}"
        COMMAND pnpm install --frozen-lockfile
        COMMAND pnpm run build
        COMMAND "${CMAKE_COMMAND}" -E touch "${PNPM_STAMP}"
        WORKING_DIRECTORY ${WEB_DIR}
        DEPENDS ${WEB_SRC_FILES} ${WEB_CONFIG_FILES}
        COMMENT "Building frontend (pnpm)..."
        VERBATIM
    )

  add_custom_target(pnpm_build DEPENDS "${PNPM_STAMP}")
  add_dependencies(${COMPONENT_LIB} pnpm_build)

  # ── Host build of json-gen-c ──────────────────────────────────────────────
  find_program(HOST_C_COMPILER NAMES gcc cc
        REQUIRED
        NO_CMAKE_FIND_ROOT_PATH
    )

  include(ExternalProject)
  ExternalProject_Add(json_gen_c_host
        SOURCE_DIR  "${JSON_GEN_C_SRC_DIR}"
        BINARY_DIR  "${JSON_GEN_C_HOST_DIR}"
        CMAKE_ARGS
            -DCMAKE_C_COMPILER=${HOST_C_COMPILER}
            -DCMAKE_BUILD_TYPE=Release
            -DBUILD_TESTING=OFF
        BUILD_BYPRODUCTS
            "${JSON_GEN_C_BIN}"
        INSTALL_COMMAND ""
    )

  # ── Generate C sources from schema ────────────────────────────────────────
  set(SCHEMA_INPUT_FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/schema.json-gen-c"
        "${CMAKE_CURRENT_SOURCE_DIR}/schema_config.json-gen-c"
        "${CMAKE_CURRENT_SOURCE_DIR}/schema_ws_messages.json-gen-c"
        "${CMAKE_CURRENT_SOURCE_DIR}/schema_mqtt_messages.json-gen-c"
    )

  add_custom_command(
        OUTPUT
            ${SCHEMA_GEN_SRCS}
            "${SCHEMA_GEN_DIR}/json.gen.h"
            "${SCHEMA_GEN_DIR}/sstr.h"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SCHEMA_GEN_DIR}"
        COMMAND "${JSON_GEN_C_BIN}"
            -in  "${CMAKE_CURRENT_SOURCE_DIR}/schema.json-gen-c"
            -out "${SCHEMA_GEN_DIR}"
        DEPENDS
            ${SCHEMA_INPUT_FILES}
            "${JSON_GEN_C_BIN}"
        COMMENT "Generating JSON serialisation code from schema.json-gen-c"
        VERBATIM
    )

  target_sources(${COMPONENT_LIB} PRIVATE ${SCHEMA_GEN_SRCS})
  target_include_directories(${COMPONENT_LIB} PRIVATE
        "${SCHEMA_GEN_DIR}"
        "${JSON_GEN_C_SRC_DIR}/src"
    )

  add_custom_target(schema_codegen DEPENDS ${SCHEMA_GEN_SRCS})
  add_dependencies(schema_codegen json_gen_c_host)
  add_dependencies(${COMPONENT_LIB} schema_codegen)

endif()
