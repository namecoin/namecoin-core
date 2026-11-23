# Copyright (c) 2023-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

include_guard(GLOBAL)

function(setup_split_debug_script)
  if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    set(OBJCOPY ${CMAKE_OBJCOPY})
    set(STRIP ${CMAKE_STRIP})
    configure_file(
      contrib/devtools/split-debug.sh.in split-debug.sh
      FILE_PERMISSIONS OWNER_READ OWNER_EXECUTE
                       GROUP_READ GROUP_EXECUTE
                       WORLD_READ
      @ONLY
    )
  endif()
endfunction()

function(add_maintenance_targets)
  if(NOT TARGET Python3::Interpreter)
    return()
  endif()

  foreach(target IN ITEMS namecoin namecoind namecoin-node namecoin-qt namecoin-gui namecoin-cli namecoin-tx namecoin-util namecoin-wallet test_namecoin bench_namecoin)
    if(TARGET ${target})
      list(APPEND executables $<TARGET_FILE:${target}>)
    endif()
  endforeach()

  add_custom_target(check-symbols
    COMMAND ${CMAKE_COMMAND} -E echo "Running symbol and dynamic library checks..."
    COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/contrib/guix/symbol-check.py ${executables}
    VERBATIM
  )

  add_custom_target(check-security
    COMMAND ${CMAKE_COMMAND} -E echo "Checking binary security..."
    COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/contrib/guix/security-check.py ${executables}
    VERBATIM
  )
endfunction()

function(add_windows_deploy_target)
  configure_file(${PROJECT_SOURCE_DIR}/cmake/script/GenerateWindowsInstaller.cmake.in ${PROJECT_BINARY_DIR}/GenerateWindowsInstaller.cmake USE_SOURCE_PERMISSIONS @ONLY)
  if(MINGW AND TARGET namecoin AND TARGET namecoin-qt AND TARGET namecoind AND TARGET namecoin-cli AND TARGET namecoin-tx AND TARGET namecoin-wallet AND TARGET namecoin-util AND TARGET test_namecoin)
    add_custom_command(
      OUTPUT ${PROJECT_BINARY_DIR}/namecoin-win64-setup.exe
      WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
      COMMAND ${CMAKE_COMMAND} -E make_directory release
      COMMAND ${CMAKE_STRIP} $<TARGET_FILE:namecoin> -o release/$<TARGET_FILE_NAME:namecoin>
      COMMAND ${CMAKE_STRIP} $<TARGET_FILE:namecoin-qt> -o release/$<TARGET_FILE_NAME:namecoin-qt>
      COMMAND ${CMAKE_STRIP} $<TARGET_FILE:namecoind> -o release/$<TARGET_FILE_NAME:namecoind>
      COMMAND ${CMAKE_STRIP} $<TARGET_FILE:namecoin-cli> -o release/$<TARGET_FILE_NAME:namecoin-cli>
      COMMAND ${CMAKE_STRIP} $<TARGET_FILE:namecoin-tx> -o release/$<TARGET_FILE_NAME:namecoin-tx>
      COMMAND ${CMAKE_STRIP} $<TARGET_FILE:namecoin-wallet> -o release/$<TARGET_FILE_NAME:namecoin-wallet>
      COMMAND ${CMAKE_STRIP} $<TARGET_FILE:namecoin-util> -o release/$<TARGET_FILE_NAME:namecoin-util>
      COMMAND ${CMAKE_STRIP} $<TARGET_FILE:test_namecoin> -o release/$<TARGET_FILE_NAME:test_namecoin>
      COMMAND ${CMAKE_COMMAND} -D BIN_DIR=release -D LIBEXEC_DIR=release -P GenerateWindowsInstaller.cmake
    )
    add_custom_target(deploy DEPENDS ${PROJECT_BINARY_DIR}/namecoin-win64-setup.exe)
  endif()
endfunction()

function(add_macos_deploy_target)
  if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND TARGET namecoin-qt)
    set(macos_app "Namecoin-Qt.app")
    # Populate Contents subdirectory.
    configure_file(${PROJECT_SOURCE_DIR}/share/qt/Info.plist.in ${macos_app}/Contents/Info.plist NO_SOURCE_PERMISSIONS)
    file(CONFIGURE OUTPUT ${macos_app}/Contents/PkgInfo CONTENT "APPL????")
    # Populate Contents/Resources subdirectory.
    file(CONFIGURE OUTPUT ${macos_app}/Contents/Resources/empty.lproj CONTENT "")
    configure_file(${PROJECT_SOURCE_DIR}/src/qt/res/icons/namecoin.icns ${macos_app}/Contents/Resources/namecoin.icns NO_SOURCE_PERMISSIONS COPYONLY)
    file(CONFIGURE OUTPUT ${macos_app}/Contents/Resources/Base.lproj/InfoPlist.strings
      CONTENT "{ CFBundleDisplayName = \"@CLIENT_NAME@\"; CFBundleName = \"@CLIENT_NAME@\"; }"
    )

    add_custom_command(
      OUTPUT ${PROJECT_BINARY_DIR}/${macos_app}/Contents/MacOS/Namecoin-Qt
      COMMAND ${CMAKE_COMMAND} --install ${PROJECT_BINARY_DIR} --config $<CONFIG> --component namecoin-qt --prefix ${macos_app}/Contents/MacOS --strip
      COMMAND ${CMAKE_COMMAND} -E rename ${macos_app}/Contents/MacOS/bin/$<TARGET_FILE_NAME:namecoin-qt> ${macos_app}/Contents/MacOS/Namecoin-Qt
      COMMAND ${CMAKE_COMMAND} -E rm -rf ${macos_app}/Contents/MacOS/bin
      COMMAND ${CMAKE_COMMAND} -E rm -rf ${macos_app}/Contents/MacOS/share
      VERBATIM
    )

    string(REPLACE " " "-" osx_volname ${CLIENT_NAME})
    if(CMAKE_HOST_APPLE)
      add_custom_command(
        OUTPUT ${PROJECT_BINARY_DIR}/${osx_volname}.zip
        COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/contrib/macdeploy/macdeployqtplus ${macos_app} -translations-dir=${QT_TRANSLATIONS_DIR} -zip=${osx_volname}
        DEPENDS ${PROJECT_BINARY_DIR}/${macos_app}/Contents/MacOS/Namecoin-Qt
        VERBATIM
      )
      add_custom_target(deploydir
        DEPENDS ${PROJECT_BINARY_DIR}/${osx_volname}.zip
      )
      add_custom_target(deploy
        DEPENDS ${PROJECT_BINARY_DIR}/${osx_volname}.zip
      )
    else()
      add_custom_command(
        OUTPUT ${PROJECT_BINARY_DIR}/dist/${macos_app}/Contents/MacOS/Namecoin-Qt
        COMMAND ${CMAKE_COMMAND} -E env OBJDUMP=${CMAKE_OBJDUMP} $<TARGET_FILE:Python3::Interpreter> ${PROJECT_SOURCE_DIR}/contrib/macdeploy/macdeployqtplus ${macos_app} -translations-dir=${QT_TRANSLATIONS_DIR}
        DEPENDS ${PROJECT_BINARY_DIR}/${macos_app}/Contents/MacOS/Namecoin-Qt
        VERBATIM
      )
      add_custom_target(deploydir
        DEPENDS ${PROJECT_BINARY_DIR}/dist/${macos_app}/Contents/MacOS/Namecoin-Qt
      )

      find_program(ZIP_EXECUTABLE zip)
      if(NOT ZIP_EXECUTABLE)
        add_custom_target(deploy
          COMMAND ${CMAKE_COMMAND} -E echo "Error: ZIP not found"
        )
      else()
        add_custom_command(
          OUTPUT ${PROJECT_BINARY_DIR}/dist/${osx_volname}.zip
          WORKING_DIRECTORY dist
          COMMAND ${PROJECT_SOURCE_DIR}/cmake/script/macos_zip.sh ${ZIP_EXECUTABLE} ${osx_volname}.zip
          VERBATIM
        )
        add_custom_target(deploy
          DEPENDS ${PROJECT_BINARY_DIR}/dist/${osx_volname}.zip
        )
      endif()
    endif()
    add_dependencies(deploydir namecoin-qt)
    add_dependencies(deploy deploydir)
  endif()
endfunction()
