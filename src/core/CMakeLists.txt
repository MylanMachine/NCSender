# nccore: serial transfer logic with no dependency on Qt Widgets.
# The GUI, nccli and any future tool (drip feed, service, etc.) link this.
add_library(nccore STATIC
    AppPaths.h            AppPaths.cpp
    SerialSettings.h      SerialSettings.cpp
    MachineProfile.h      MachineProfile.cpp
    ProfileStore.h        ProfileStore.cpp
    ProgramFormatter.h    ProgramFormatter.cpp
    DataSource.h          DataSource.cpp
    TransferTypes.h       TransferTypes.cpp
    TransferWorker.h      TransferWorker.cpp
    MachineChannel.h      MachineChannel.cpp
    TransferLog.h         TransferLog.cpp
)

target_include_directories(nccore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(nccore PUBLIC Qt6::Core Qt6::SerialPort)
