set(requires bluepad32 btstack hal esp_driver_rmt)

idf_component_register(SRC_DIRS "./src"
                    INCLUDE_DIRS "./include"
                    REQUIRES "${requires}")

idf_component_get_property(rmt_dir esp_driver_rmt COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} PRIVATE "${rmt_dir}/src")

