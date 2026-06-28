set(requires bluepad32 btstack)

idf_component_register(SRC_DIRS "./src"
                    INCLUDE_DIRS "./include"
                    REQUIRES "${requires}")
