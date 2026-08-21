idf_component_register(SRCS "gnuboy.c" "cpu.c" "hw.c" "lcd.c" "sound.c"
                       INCLUDE_DIRS ".")
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -fno-strict-aliasing
                       -Wno-error=maybe-uninitialized -Wno-error=unused-variable)
