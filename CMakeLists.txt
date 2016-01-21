#Standard stuff
cmake_minimum_required(VERSION 2.6)
project(portpilot-logger)

set(CMAKE_C_FLAGS "-O1 -Wall")
set(LIBS usb-1.0)

add_executable(portpilot-logger
               backend_event_loop.c
               portpilot_callbacks.c
               portpilot_helpers.c
               portpilot_logger.c)

target_link_libraries(portpilot-logger ${LIBS})
