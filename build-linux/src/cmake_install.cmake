# Install script for directory: /home/hannes/Documents/zelda-vr/BotW-BetterVR/src

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/home/hannes/Documents/zelda-vr/BotW-BetterVR/Cemu")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Release")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Install shared libraries without execute permission?
if(NOT DEFINED CMAKE_INSTALL_SO_NO_EXE)
  set(CMAKE_INSTALL_SO_NO_EXE "0")
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

# Set path to fallback-tool for dependency-resolution.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/usr/bin/llvm-objdump")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
          file(REMOVE_RECURSE "${CMAKE_INSTALL_PREFIX}/graphicPacks/BreathOfTheWild_BetterVR")
    
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}/home/hannes/Documents/zelda-vr/BotW-BetterVR/Cemu/libVkLayer_BetterVR.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}/home/hannes/Documents/zelda-vr/BotW-BetterVR/Cemu/libVkLayer_BetterVR.so")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}/home/hannes/Documents/zelda-vr/BotW-BetterVR/Cemu/libVkLayer_BetterVR.so"
         RPATH "")
  endif()
  list(APPEND CMAKE_ABSOLUTE_DESTINATION_FILES
   "/home/hannes/Documents/zelda-vr/BotW-BetterVR/Cemu/libVkLayer_BetterVR.so")
  if(CMAKE_WARN_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(WARNING "ABSOLUTE path INSTALL DESTINATION : ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  if(CMAKE_ERROR_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(FATAL_ERROR "ABSOLUTE path INSTALL DESTINATION forbidden (by caller): ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  file(INSTALL DESTINATION "/home/hannes/Documents/zelda-vr/BotW-BetterVR/Cemu" TYPE SHARED_LIBRARY FILES "/home/hannes/Documents/zelda-vr/BotW-BetterVR/build-linux/lib/libVkLayer_BetterVR.so")
  if(EXISTS "$ENV{DESTDIR}/home/hannes/Documents/zelda-vr/BotW-BetterVR/Cemu/libVkLayer_BetterVR.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}/home/hannes/Documents/zelda-vr/BotW-BetterVR/Cemu/libVkLayer_BetterVR.so")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/llvm-strip" "$ENV{DESTDIR}/home/hannes/Documents/zelda-vr/BotW-BetterVR/Cemu/libVkLayer_BetterVR.so")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
    list(APPEND CMAKE_ABSOLUTE_DESTINATION_FILES
     "/home/hannes/Documents/zelda-vr/BotW-BetterVR/Cemu/graphicPacks/BreathOfTheWild_BetterVR/")
    if(CMAKE_WARN_ON_ABSOLUTE_INSTALL_DESTINATION)
      message(WARNING "ABSOLUTE path INSTALL DESTINATION : ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
    endif()
    if(CMAKE_ERROR_ON_ABSOLUTE_INSTALL_DESTINATION)
      message(FATAL_ERROR "ABSOLUTE path INSTALL DESTINATION forbidden (by caller): ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
    endif()
    file(INSTALL DESTINATION "/home/hannes/Documents/zelda-vr/BotW-BetterVR/Cemu/graphicPacks/BreathOfTheWild_BetterVR" TYPE DIRECTORY FILES "/home/hannes/Documents/zelda-vr/BotW-BetterVR/resources/BreathOfTheWild_BetterVR/")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee]|[Rr][Ee][Ll][Ww][Ii][Tt][Hh][Dd][Ee][Bb][Ii][Nn][Ff][Oo]|[Mm][Ii][Nn][Ss][Ii][Zz][Ee][Rr][Ee][Ll])$")
    list(APPEND CMAKE_ABSOLUTE_DESTINATION_FILES
     "/home/hannes/Documents/zelda-vr/BotW-BetterVR/Cemu/graphicPacks/BreathOfTheWild_BetterVR/")
    if(CMAKE_WARN_ON_ABSOLUTE_INSTALL_DESTINATION)
      message(WARNING "ABSOLUTE path INSTALL DESTINATION : ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
    endif()
    if(CMAKE_ERROR_ON_ABSOLUTE_INSTALL_DESTINATION)
      message(FATAL_ERROR "ABSOLUTE path INSTALL DESTINATION forbidden (by caller): ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
    endif()
    file(INSTALL DESTINATION "/home/hannes/Documents/zelda-vr/BotW-BetterVR/Cemu/graphicPacks/BreathOfTheWild_BetterVR" TYPE DIRECTORY FILES "/home/hannes/Documents/zelda-vr/BotW-BetterVR/resources/BreathOfTheWild_BetterVR/" REGEX "/patch\\_debug\\_[^/]*\\.asm$" EXCLUDE)
  endif()
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/home/hannes/Documents/zelda-vr/BotW-BetterVR/build-linux/src/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
