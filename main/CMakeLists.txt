# The web page files are gzipped at configure time and embedded, so one firmware image (and one OTA update) has everything.
set(WEB_SRC "${PROJECT_DIR}/web")
set(WEB_OUT "${CMAKE_CURRENT_BINARY_DIR}/web")
execute_process(COMMAND ${PYTHON} "${PROJECT_DIR}/tools/pack_web.py" "${WEB_SRC}" "${WEB_OUT}"
                RESULT_VARIABLE PACK_RESULT OUTPUT_VARIABLE PACK_OUT)
if(NOT PACK_RESULT EQUAL 0)
    message(FATAL_ERROR "pack_web.py failed: ${PACK_OUT}")
endif()
file(GLOB WEB_GZ "${WEB_OUT}/*.gz")

idf_component_register(SRCS "main.c" "sensors.c" "calib.c" "display.c" "web.c" "font5x7.c"
                       INCLUDE_DIRS "." "${WEB_OUT}"
                       EMBED_FILES ${WEB_GZ}
                       REQUIRES closer_demo esp_http_server nvs_flash driver esp_driver_i2c esp_driver_gpio json esp_timer)
