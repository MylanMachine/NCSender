# Stand-in for a CNC control on the far end of a virtual (com0com) or real
# null-modem link. It does NOT use nccore, so it checks the sender
# independently instead of agreeing with it by construction.
add_executable(nctestpeer main.cpp)
target_link_libraries(nctestpeer PRIVATE Qt6::Core Qt6::SerialPort)
