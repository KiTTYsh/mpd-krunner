cmake_minimum_required(VERSION 3.16)
project(krunner-mpd VERSION 0.1)

find_package(ECM 6.0 REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core)
find_package(KF6 6.0 REQUIRED COMPONENTS CoreAddons Runner I18n)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MPDCLIENT REQUIRED IMPORTED_TARGET libmpdclient>=2.20)

add_definitions(-DTRANSLATION_DOMAIN=\"krunner_mpd\")

kcoreaddons_add_plugin(krunner_mpd SOURCES src/mpdrunner.cpp INSTALL_NAMESPACE "kf6/krunner")
target_link_libraries(krunner_mpd KF6::Runner KF6::I18n PkgConfig::MPDCLIENT)
