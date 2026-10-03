cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(vga_project C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialize the Pico SDK
pico_sdk_init()

add_executable(vga_project
    picofw.c
    vga_rgb.pio
)

# Link libraries
target_link_libraries(vga_project
    pico_stdlib
    hardware_pio
    hardware_dma
    hardware_irq
    hardware_uart
)

# Enable USB output / Standard IO routing configurations
pico_enable_stdio_uart(vga_project 1)
pico_enable_stdio_usb(vga_project 0)

# Build step compiling the PIO assembly file into standard header configurations
pico_generate_pio_header(vga_project
    ${CMAKE_CURRENT_LIST_DIR}/vga_rgb.pio
)

# Create final flashable UF2 file
pico_add_extra_outputs(vga_project)