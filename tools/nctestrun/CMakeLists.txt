# Automated com0com test suite. Starts nccli and nctestpeer as separate
# processes and checks what they report; links no transfer code itself.
add_executable(nctestrun main.cpp)
target_link_libraries(nctestrun PRIVATE Qt6::Core Qt6::SerialPort)
