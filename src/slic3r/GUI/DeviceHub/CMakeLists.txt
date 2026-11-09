
list(APPEND SLIC3R_GUI_SOURCES
    GUI/DeviceHub/BuiltinCloudProviders.hpp
    GUI/DeviceHub/BuiltinCloudProviders.cpp
    GUI/DeviceHub/CloudPrinterProvider.hpp
    GUI/DeviceHub/CloudPrinterProvider.cpp
    GUI/DeviceHub/CloudPrinterProviderList.cpp
    GUI/DeviceHub/DeviceHubPanel.hpp
    GUI/DeviceHub/DeviceHubPanel.cpp
    GUI/DeviceHub/LocalPrintersProvider.hpp
    GUI/DeviceHub/LocalPrintersProvider.cpp
)
set(SLIC3R_GUI_SOURCES ${SLIC3R_GUI_SOURCES} PARENT_SCOPE)
