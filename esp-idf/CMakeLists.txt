set(NOMAD_SRCS
    "src/nomad.cpp"
    # LCD slice — see reticulous-lxmf/CMakeLists.txt for the same pattern.
    "lcd/src/nomad_lcd.cpp"
)

idf_component_register(
    SRCS ${NOMAD_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES
        spangap-core
        spangap-lcd
        reticulous-core
        json
)
