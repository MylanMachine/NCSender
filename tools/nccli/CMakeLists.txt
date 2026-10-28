# Headless front end to nccore. Uses exactly the same MachineChannel /
# TransferWorker code as the GUI, so the automated tests exercise the real
# transfer logic, including several channels running at once.
add_executable(nccli main.cpp)
target_link_libraries(nccli PRIVATE nccore)
