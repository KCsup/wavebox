set(srcs "main.c")

set(requires "bluepad32" "btstack")

idf_component_register(SRCS "wavebox.c"
                    INCLUDE_DIRS ".")
