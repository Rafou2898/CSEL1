cmake_minimum_required(VERSION 3.28)
project(ex7-app)

include(../../nanopi.cmake)
add_executable(silly_led_control_poll silly_led_control_poll.c)
#add_executable(silly_led_control_select silly_led_control_select.c)
add_executable(silly_led_control_original silly_led_control_original.c)