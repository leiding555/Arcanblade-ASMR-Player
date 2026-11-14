# Generated source export; tests are maintained in the development project only.
cmake_minimum_required(VERSION 3.21)

# Localized MSVC /showIncludes output can leave NMake/JOM dependencies empty.
# Use CMake's scanner so header changes cannot leave objects with stale layouts.
if(CMAKE_GENERATOR STREQUAL "NMake Makefiles"
        OR CMAKE_GENERATOR STREQUAL "NMake Makefiles JOM")
    set(CMAKE_DEPENDS_USE_COMPILER FALSE)
endif()
# One release version drives the program, Windows resources and package names.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" ARCANBLADE_VERSION)
string(STRIP "${ARCANBLADE_VERSION}" ARCANBLADE_VERSION)
if(NOT ARCANBLADE_VERSION MATCHES "^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)(-[0-9A-Za-z-]+(\\.[0-9A-Za-z-]+)*)?(\\+[0-9A-Za-z-]+(\\.[0-9A-Za-z-]+)*)?$")
    message(FATAL_ERROR "VERSION must contain one valid semantic version, such as 0.1.0-beta.2")
endif()
set(ARCANBLADE_VERSION_MAJOR "${CMAKE_MATCH_1}")
set(ARCANBLADE_VERSION_MINOR "${CMAKE_MATCH_2}")
set(ARCANBLADE_VERSION_PATCH "${CMAKE_MATCH_3}")
set(_arcanblade_prerelease "${CMAKE_MATCH_4}")
foreach(_part MAJOR MINOR PATCH)
    string(LENGTH "${ARCANBLADE_VERSION_${_part}}" _part_length)
    if(_part_length GREATER 5 OR ARCANBLADE_VERSION_${_part} GREATER 65535)
        message(FATAL_ERROR "VERSION numeric components must fit the Windows file version range 0..65535")
    endif()
endforeach()
if(_arcanblade_prerelease)
    string(SUBSTRING "${_arcanblade_prerelease}" 1 -1 _arcanblade_prerelease)
    string(REPLACE "." ";" _arcanblade_prerelease "${_arcanblade_prerelease}")
    foreach(_identifier IN LISTS _arcanblade_prerelease)
        if(_identifier MATCHES "^0[0-9]+$")
            message(FATAL_ERROR "VERSION numeric prerelease identifiers must not contain leading zeroes")
        endif()
    endforeach()
endif()
project(AsmrPlayer VERSION "${ARCANBLADE_VERSION_MAJOR}.${ARCANBLADE_VERSION_MINOR}.${ARCANBLADE_VERSION_PATCH}" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets Multimedia Sql Network Pdf)
qt_standard_project_setup()

add_library(AsmrVersion STATIC src/update/ApplicationVersion.h src/update/ApplicationVersion.cpp)
target_include_directories(AsmrVersion PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(AsmrVersion PUBLIC Qt6::Core)
target_compile_definitions(AsmrVersion PUBLIC
    ARCANBLADE_VERSION="${ARCANBLADE_VERSION}"
    ARCANBLADE_VERSION_MAJOR=${ARCANBLADE_VERSION_MAJOR}
    ARCANBLADE_VERSION_MINOR=${ARCANBLADE_VERSION_MINOR}
    ARCANBLADE_VERSION_PATCH=${ARCANBLADE_VERSION_PATCH})

add_library(AsmrZipArchive STATIC
    src/ai/ChickenRiceArchive.h src/ai/ChickenRiceArchive.cpp)
target_include_directories(AsmrZipArchive PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(AsmrZipArchive PUBLIC Qt6::Core)

add_library(AsmrDownloadNetwork STATIC
    src/network/DownloadProxyResolver.h src/network/DownloadProxyResolver.cpp)
target_include_directories(AsmrDownloadNetwork PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(AsmrDownloadNetwork PUBLIC Qt6::Network)

add_library(AsmrUpdates STATIC
    src/update/SemanticVersion.h src/update/SemanticVersion.cpp
    src/update/UpdateCheckService.h src/update/UpdateCheckService.cpp
    src/update/UpdateDownloadService.h src/update/UpdateDownloadService.cpp
    src/update/UpdatePackage.h src/update/UpdatePackage.cpp
    src/update/UpdateInstaller.h src/update/UpdateInstaller.cpp)
target_link_libraries(AsmrUpdates PUBLIC AsmrVersion AsmrZipArchive AsmrDownloadNetwork Qt6::Network)
if(WIN32)
    target_link_libraries(AsmrUpdates PRIVATE bcrypt user32)
    # This helper runs from the update cache after the player exits. It must
    # not depend on Qt or DLLs in the installation it is about to replace.
    add_executable(ArcanbladeUpdater WIN32
        src/update/UpdaterMain.cpp src/update/UpdateInstaller.h src/update/UpdateInstaller.cpp)
    set_target_properties(ArcanbladeUpdater PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    target_include_directories(ArcanbladeUpdater PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_definitions(ArcanbladeUpdater PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(ArcanbladeUpdater PRIVATE bcrypt shell32 user32)
    if(MSVC)
        set_property(TARGET ArcanbladeUpdater PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_options(ArcanbladeUpdater PRIVATE /W4 /utf-8)
    endif()
endif()
foreach(_update_target AsmrVersion AsmrUpdates AsmrZipArchive AsmrDownloadNetwork)
    if(MSVC)
        target_compile_options(${_update_target} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${_update_target} PRIVATE -Wall -Wextra)
    endif()
endforeach()

add_library(AsmrSubtitle STATIC
    src/subtitle/GeneratedSubtitleInfo.h
    src/subtitle/GeneratedSubtitleInfo.cpp
    src/subtitle/SubtitleEntry.h
    src/subtitle/SrtParser.h
    src/subtitle/SrtParser.cpp
    src/subtitle/VttParser.h
    src/subtitle/VttParser.cpp
    src/subtitle/LrcParser.h
    src/subtitle/LrcParser.cpp
    src/subtitle/SubtitleSynchronizer.h
    src/subtitle/SubtitleSynchronizer.cpp
    src/subtitle/SubtitleFileLoader.h
    src/subtitle/SubtitleFileLoader.cpp
)
target_include_directories(AsmrSubtitle PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(AsmrSubtitle PUBLIC Qt6::Core)
if(MSVC)
    target_compile_options(AsmrSubtitle PRIVATE /W4 /utf-8)
else()
    target_compile_options(AsmrSubtitle PRIVATE -Wall -Wextra)
endif()

add_library(AsmrLibrary STATIC
    src/playback/PlaybackQueue.h
    src/playback/PlaybackQueue.cpp
    src/library/DirectoryScanner.h
    src/library/DirectoryScanner.cpp
    src/library/WorkReferenceTypes.h
    src/library/WorkReferenceSelection.h src/library/WorkReferenceSelection.cpp
    src/library/WorkReferenceExtractor.h src/library/WorkReferenceExtractor.cpp
    src/library/WorkReferenceRepository.h src/library/WorkReferenceRepository.cpp
    src/library/NaturalSort.h
    src/library/NaturalSort.cpp
    src/library/WorkDirectoryDiscovery.h
    src/library/WorkDirectoryDiscovery.cpp
    src/library/ScanRootStore.h
    src/library/ScanRootStore.cpp
    src/library/LibraryRootRefreshService.h
    src/library/LibraryRootRefreshService.cpp
    src/library/LibraryBatchImportService.h
    src/library/LibraryBatchImportService.cpp
    src/library/SubtitleMatcher.h
    src/library/SubtitleMatcher.cpp
    src/library/LibraryRepository.h
    src/library/SqliteWriteTransaction.h
    src/library/SqliteWriteTransaction.cpp
    src/library/BusinessDatabase.h src/library/BusinessDatabase.cpp
    src/library/SubtitleTranslationCleanup.h src/library/SubtitleTranslationCleanup.cpp
    src/library/LibraryRepositoryWorkSettings.cpp
    src/library/WorkCategoryTypes.h
    src/library/LibraryRepositoryCategories.cpp
    src/library/LibraryRepository.cpp
    src/library/LibraryRepositorySubtitles.cpp
    src/library/LibraryRepositoryProgress.cpp
    src/library/LibraryImportService.h
    src/library/LibraryImportService.cpp
    src/playback/PlaybackProgressCoordinator.h
    src/playback/PlaybackProgressCoordinator.cpp
)
target_include_directories(AsmrLibrary PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(AsmrLibrary PUBLIC AsmrSubtitle Qt6::Core Qt6::Sql Qt6::Pdf)
if(MSVC)
    target_compile_options(AsmrLibrary PRIVATE /W4 /utf-8)
else()
    target_compile_options(AsmrLibrary PRIVATE -Wall -Wextra)
endif()

add_library(AsmrGpu STATIC
    src/ai/GpuCompatibility.h src/ai/GpuCompatibility.cpp
    src/ai/GpuDetector.h src/ai/GpuDetector.cpp
)
target_include_directories(AsmrGpu PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(AsmrGpu PUBLIC Qt6::Core)
if(MSVC)
    target_compile_options(AsmrGpu PRIVATE /W4 /utf-8)
else()
    target_compile_options(AsmrGpu PRIVATE -Wall -Wextra)
endif()

add_library(AsmrAiSubtitle STATIC
    src/ai/AiSubtitleTaskTypes.h
    src/ai/AiSubtitleProcess.h src/ai/AiSubtitleProcess.cpp
    src/ai/WindowsConsoleHostCache.h
    src/ai/AiSubtitleOutput.h src/ai/AiSubtitleOutput.cpp
)
target_include_directories(AsmrAiSubtitle PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(AsmrAiSubtitle PUBLIC AsmrSubtitle Qt6::Core)
if(MSVC)
    target_compile_options(AsmrAiSubtitle PRIVATE /W4 /utf-8)
else()
    target_compile_options(AsmrAiSubtitle PRIVATE -Wall -Wextra)
endif()

add_library(AsmrChickenRice STATIC
    src/ai/ChickenRicePackage.h src/ai/ChickenRicePackage.cpp
    src/ai/ChickenRiceInstallService.h src/ai/ChickenRiceInstallService.cpp
    src/ai/ChickenRiceTranslateService.h src/ai/ChickenRiceTranslateService.cpp
    src/ai/ChickenRiceGenerationOptions.h src/ai/ChickenRiceGenerationOptions.cpp
)
target_include_directories(AsmrChickenRice PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(AsmrChickenRice PUBLIC AsmrZipArchive AsmrGpu AsmrAiSubtitle AsmrSubtitle AsmrDownloadNetwork Qt6::Core Qt6::Network)
if(MSVC)
    target_compile_options(AsmrChickenRice PRIVATE /W4 /utf-8)
else()
    target_compile_options(AsmrChickenRice PRIVATE -Wall -Wextra)
endif()

add_library(AsmrFasterWhisper STATIC
    src/ai/FasterWhisperPackage.h src/ai/FasterWhisperPackage.cpp
    src/ai/FasterWhisperArchive.h src/ai/FasterWhisperArchive.cpp
    src/ai/FasterWhisperInstallService.h src/ai/FasterWhisperInstallService.cpp
    src/ai/FasterWhisperGenerationOptions.h src/ai/FasterWhisperGenerationOptions.cpp
    src/ai/FasterWhisperTranscribeService.h src/ai/FasterWhisperTranscribeService.cpp
)
target_include_directories(AsmrFasterWhisper PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(AsmrFasterWhisper PUBLIC AsmrAiSubtitle AsmrDownloadNetwork Qt6::Core Qt6::Network)
# An unmodified standalone binary, matching source and licenses are shipped together.
set(_sevenzip_root "${CMAKE_CURRENT_SOURCE_DIR}/resources/7zip")
file(READ "${_sevenzip_root}/SOURCE.json" _sevenzip_manifest)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_sevenzip_root}/SOURCE.json")
string(JSON _sevenzip_version GET "${_sevenzip_manifest}" Version)
string(JSON _sevenzip_count LENGTH "${_sevenzip_manifest}" Files)
math(EXPR _sevenzip_last "${_sevenzip_count} - 1")
set(_sevenzip_notices SOURCE.json)
foreach(_i RANGE 0 ${_sevenzip_last})
    string(JSON _sevenzip_file GET "${_sevenzip_manifest}" Files ${_i} File)
    string(JSON _sevenzip_hash GET "${_sevenzip_manifest}" Files ${_i} Sha256)
    file(SHA256 "${_sevenzip_root}/${_sevenzip_file}" _sevenzip_actual)
    if(NOT _sevenzip_actual STREQUAL _sevenzip_hash)
        message(FATAL_ERROR "Bundled 7-Zip hash mismatch: ${_sevenzip_file}")
    endif()
    if(_sevenzip_file STREQUAL "7za.exe")
        set(_sevenzip_binary_hash "${_sevenzip_hash}")
    else()
        list(APPEND _sevenzip_notices "${_sevenzip_file}")
    endif()
endforeach()
target_compile_definitions(AsmrFasterWhisper PRIVATE
    ARCANBLADE_SEVENZIP_VERSION="${_sevenzip_version}"
    ARCANBLADE_SEVENZIP_SHA256="${_sevenzip_binary_hash}")
function(arcanblade_deploy_sevenzip target)
    if(WIN32)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/tools/7zip"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_sevenzip_root}/7za.exe"
                "$<TARGET_FILE_DIR:${target}>/tools/7zip/7za.exe"
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/licenses/7zip"
            VERBATIM)
        foreach(_notice IN LISTS _sevenzip_notices)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_sevenzip_root}/${_notice}"
                    "$<TARGET_FILE_DIR:${target}>/licenses/7zip/${_notice}" VERBATIM)
        endforeach()
    endif()
endfunction()
if(MSVC)
    target_compile_options(AsmrFasterWhisper PRIVATE /W4 /utf-8)
else()
    target_compile_options(AsmrFasterWhisper PRIVATE -Wall -Wextra)
endif()

# Filesystem-only validation is also built directly into the isolated tests.
set(_audio_file_validation_sources
    src/playback/AudioFileValidation.h
    src/playback/AudioFileValidation.cpp
)
target_sources(AsmrAiSubtitle PRIVATE ${_audio_file_validation_sources})
add_library(AsmrPlaybackDiagnostics STATIC
    src/playback/PlaybackDiagnosticLog.h src/playback/PlaybackDiagnosticLog.cpp
    src/playback/QtAudioLogCapture.h src/playback/QtAudioLogCapture.cpp
    src/playback/DirectSoundOutputTest.h src/playback/DirectSoundOutputTest.cpp
    src/playback/PcmDiagnosticSummary.h src/playback/PcmDiagnosticSummary.cpp)
target_include_directories(AsmrPlaybackDiagnostics PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(AsmrPlaybackDiagnostics PUBLIC Qt6::Core Qt6::Multimedia)
if(WIN32)
    target_link_libraries(AsmrPlaybackDiagnostics PRIVATE dsound ole32)
endif()
if(MSVC)
    target_compile_options(AsmrPlaybackDiagnostics PRIVATE /W4 /utf-8)
else()
    target_compile_options(AsmrPlaybackDiagnostics PRIVATE -Wall -Wextra)
endif()
add_library(AsmrDuration STATIC
    src/playback/TrackDurationService.h
    src/playback/TrackDurationService.cpp
    ${_audio_file_validation_sources}
)
target_include_directories(AsmrDuration PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(AsmrDuration PUBLIC Qt6::Core Qt6::Multimedia)
if(MSVC)
    target_compile_options(AsmrDuration PRIVATE /W4 /utf-8)
else()
    target_compile_options(AsmrDuration PRIVATE -Wall -Wextra)
endif()
set(_cover_widget_sources src/ui/CoverArtWidget.h src/ui/CoverArtWidget.cpp)
set(_gpu_recommendation_dialog_sources
    src/ui/GpuRecommendationDialog.h src/ui/GpuRecommendationDialog.cpp
)
set(_chickenrice_translate_dialog_sources
    src/ui/ChickenRiceTranslateDialog.h src/ui/ChickenRiceTranslateDialog.cpp
    src/ui/FasterWhisperInstallWidget.h src/ui/FasterWhisperInstallWidget.cpp
)
set(_library_subtitle_playback_sources
    ${_gpu_recommendation_dialog_sources}
    ${_chickenrice_translate_dialog_sources}
    src/ui/MainWindowChickenRice.cpp
    src/ui/PlaybackDetailWidget.h src/ui/PlaybackDetailWidget.cpp
    src/ui/PlaybackDetailTransition.h src/ui/PlaybackDetailTransition.cpp
    src/ui/MainWindowPlaybackDetail.cpp
    ${_cover_widget_sources}
    src/ui/MainWindowArtwork.cpp
    src/ui/MainWindowWorkEditor.cpp
    src/ui/MainWindowCategories.cpp
    src/ui/CategoryEditDialog.h src/ui/CategoryEditDialog.cpp
    src/ui/MainWindowWorkReferences.cpp
    src/ui/WorkReferenceDialog.h src/ui/WorkReferenceDialog.cpp
    src/ui/WorkEditDialog.h src/ui/WorkEditDialog.cpp
    src/ui/CoverSelectionDialog.h src/ui/CoverSelectionDialog.cpp
    src/ui/MainWindowPlaybackQueue.cpp
    src/ui/MainWindowTrackDurations.cpp
    src/ui/PlaybackQueuePanel.h
    src/ui/PlaybackQueuePanel.cpp
    src/playback/LibrarySubtitleResolver.h
    src/playback/LibrarySubtitleResolver.cpp
    src/ui/MainWindowLibrarySubtitles.cpp
    src/ui/MainWindowPlaybackProgress.cpp
    src/ui/MainWindowSubtitlePreferences.cpp
    src/ui/MainWindowScanRoots.cpp
    src/ui/ScanRootManagerDialog.h
    src/ui/ScanRootManagerDialog.cpp
    src/ui/MainWindowBatchImports.cpp
    src/ui/BatchImportPreviewDialog.h
    src/ui/BatchImportPreviewDialog.cpp
    src/ui/BoundSubtitleDialog.h
    src/ui/BoundSubtitleDialog.cpp
)

add_library(AsmrArtwork STATIC
    src/artwork/CoverArtLoader.h src/artwork/CoverArtLoader.cpp
    src/artwork/CoverArtService.h src/artwork/CoverArtService.cpp
    src/artwork/WorkCoverService.h src/artwork/WorkCoverService.cpp
    src/artwork/ManualCoverStore.h src/artwork/ManualCoverStore.cpp
    src/artwork/ManagedCoverFile.h src/artwork/ManagedCoverFile.cpp
    src/artwork/CoverDirectoryBrowser.h src/artwork/CoverDirectoryBrowser.cpp
)
target_include_directories(AsmrArtwork PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(AsmrArtwork PUBLIC Qt6::Gui PRIVATE AsmrLibrary)
if(MSVC)
    target_compile_options(AsmrArtwork PRIVATE /W4 /utf-8)
else()
    target_compile_options(AsmrArtwork PRIVATE -Wall -Wextra)
endif()

add_library(AsmrWindowFrame STATIC
    src/ui/ApplicationBrand.h
    src/ui/ApplicationBrand.cpp
    src/ui/ButtonFocusController.h
    src/ui/ButtonFocusController.cpp
    src/ui/InputControlPolicy.h
    src/ui/InputControlPolicy.cpp
    src/ui/TitleBarWidget.h
    src/ui/TitleBarWidget.cpp
    src/ui/WindowFrameController.h
    src/ui/WindowFrameController.cpp
    src/ui/DialogFrame.h
    src/ui/DialogFrame.cpp
    src/ui/PlayerMessageBox.h
    src/ui/PlayerMessageBox.cpp
)
target_include_directories(AsmrWindowFrame PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(AsmrWindowFrame PUBLIC Qt6::Widgets AsmrVersion)
qt_add_resources(AsmrWindowFrame input_controls
    PREFIX "/input-controls"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources/input-controls"
    FILES
        resources/input-controls/chevron-down.png
        resources/input-controls/chevron-up.png
        resources/input-controls/chevron-down-disabled.png
        resources/input-controls/chevron-up-disabled.png
        resources/input-controls/chevron-down@2x.png
        resources/input-controls/chevron-up@2x.png
        resources/input-controls/chevron-down-disabled@2x.png
        resources/input-controls/chevron-up-disabled@2x.png
)
qt_add_resources(AsmrWindowFrame application_branding
    PREFIX "/branding"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources/branding"
    FILES
        resources/branding/arcanblade-16.png
        resources/branding/arcanblade-20.png
        resources/branding/arcanblade-24.png
        resources/branding/arcanblade-32.png
        resources/branding/arcanblade-48.png
        resources/branding/arcanblade-64.png
        resources/branding/arcanblade-128.png
        resources/branding/arcanblade-256.png
        resources/branding/arcanblade-mark-16.png
        resources/branding/arcanblade-mark-20.png
        resources/branding/arcanblade-mark-24.png
        resources/branding/arcanblade-mark-32.png
        resources/branding/arcanblade-mark-48.png
        resources/branding/arcanblade-mark-64.png
        resources/branding/arcanblade-mark-128.png
        resources/branding/arcanblade-mark-256.png
        resources/branding/arcanblade.ico
)
if(MSVC)
    target_compile_options(AsmrWindowFrame PRIVATE /W4 /utf-8)
else()
    target_compile_options(AsmrWindowFrame PRIVATE -Wall -Wextra)
endif()

add_library(AsmrUpdateUi STATIC
    src/ui/UpdateNotificationController.h src/ui/UpdateNotificationController.cpp)
target_link_libraries(AsmrUpdateUi PUBLIC AsmrUpdates AsmrWindowFrame Qt6::Widgets)
if(MSVC)
    target_compile_options(AsmrUpdateUi PRIVATE /W4 /utf-8)
else()
    target_compile_options(AsmrUpdateUi PRIVATE -Wall -Wextra)
endif()

add_library(AsmrMobilePlayback STATIC
    src/mobile/MobileOfflineZip.h
    src/mobile/MobileOfflineZip.cpp
    src/mobile/MobilePlaybackService.h
    src/mobile/MobilePlaybackService.cpp
    src/ui/MobilePlaybackDialog.h
    src/ui/MobilePlaybackDialog.cpp
    src/third_party/qrcodegen/qrcodegen.cpp
    src/playback/AudioFileValidation.cpp
    src/playback/LibrarySubtitleResolver.cpp
)
target_include_directories(AsmrMobilePlayback PUBLIC src)
target_include_directories(AsmrMobilePlayback SYSTEM PRIVATE src/third_party/cpp-httplib src/third_party/qrcodegen)
target_link_libraries(AsmrMobilePlayback PUBLIC Qt6::Network
    PRIVATE AsmrLibrary AsmrArtwork AsmrWindowFrame)
qt_add_resources(AsmrMobilePlayback mobile_playback PREFIX "/mobile-playback"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources/mobile-playback"
    FILES resources/mobile-playback/index.html resources/mobile-playback/player.css
          resources/mobile-playback/player.js resources/mobile-playback/offline-transfer-sender.js)
qt_add_resources(AsmrMobilePlayback mobile_playback_branding PREFIX "/mobile-playback"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources/branding"
    FILES resources/branding/arcanblade-mark-256.png)
if(MSVC)
    target_compile_options(AsmrMobilePlayback PRIVATE /W4 /utf-8)
else()
    target_compile_options(AsmrMobilePlayback PRIVATE -Wall -Wextra)
endif()

add_library(AsmrTranslationApi STATIC
    src/ai/TranslationApiSettings.h src/ai/TranslationApiSettings.cpp
    src/ai/ApiKeyStore.h src/ai/ApiKeyStore.cpp
    src/ai/TranslationApiProbe.h src/ai/TranslationApiProbe.cpp
    src/ai/AiTaskRepository.h src/ai/AiTaskRepository.cpp
    src/ai/SubtitleTranslationDocument.h src/ai/SubtitleTranslationDocument.cpp
    src/ai/SubtitleTranslationService.h src/ai/SubtitleTranslationService.cpp
    src/ai/SubtitleTranslationRepository.h src/ai/SubtitleTranslationRepository.cpp
    src/ai/SubtitleTranslationTools.h src/ai/SubtitleTranslationTools.cpp
    src/ai/SubtitleWorkContext.h src/ai/SubtitleWorkContext.cpp
    src/ai/SubtitleTranslationPairPublication.h src/ai/SubtitleTranslationPairPublication.cpp
)
target_include_directories(AsmrTranslationApi PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(AsmrTranslationApi PUBLIC AsmrLibrary AsmrAiSubtitle AsmrSubtitle Qt6::Core Qt6::Network)
if(WIN32)
    target_link_libraries(AsmrTranslationApi PRIVATE Advapi32)
endif()
add_library(AsmrTranslationSettingsUi STATIC
    src/ui/TranslationApiSettingsWidget.h src/ui/TranslationApiSettingsWidget.cpp
    src/ui/TranslationApiSettingsDialog.h src/ui/TranslationApiSettingsDialog.cpp
    src/ui/SubtitleTranslationWidget.h src/ui/SubtitleTranslationWidget.cpp)
target_link_libraries(AsmrTranslationSettingsUi PUBLIC AsmrTranslationApi AsmrWindowFrame Qt6::Widgets)
foreach(_translation_target AsmrTranslationApi AsmrTranslationSettingsUi)
    if(MSVC)
        target_compile_options(${_translation_target} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${_translation_target} PRIVATE -Wall -Wextra)
    endif()
endforeach()

qt_add_executable(AsmrPlayer
    WIN32
    src/main.cpp
    src/ui/MainWindow.h
    src/ui/PlayerTheme.h
    src/ui/MainWindow.cpp
    src/ui/LibraryPanel.h
    src/ui/LibraryPanel.cpp
    src/ui/FloatingSubtitleAppearanceDialog.h
    src/ui/FloatingSubtitleAppearanceDialog.cpp
    src/ui/SubtitlePanel.h
    src/ui/SubtitlePanel.cpp
    src/ui/PlaybackControlsWidget.h
    src/ui/PlaybackControlsWidget.cpp
    src/ui/FloatingSubtitleWindow.h
    src/ui/FloatingSubtitleWindow.cpp
    src/ui/FloatingSubtitleSettings.h
    src/ui/FloatingSubtitleSettings.cpp
    src/ui/TimeFormat.h
    src/ui/TimeFormat.cpp
    src/playback/AudioPlayer.h
    src/playback/AudioPlayer.cpp
)

target_sources(AsmrPlayer PRIVATE ${_audio_file_validation_sources} ${_library_subtitle_playback_sources})
set_target_properties(AsmrPlayer PROPERTIES OUTPUT_NAME Arcanblade)
if(WIN32)
    add_dependencies(AsmrPlayer ArcanbladeUpdater)
endif()
target_include_directories(AsmrPlayer PRIVATE src)
if(WIN32)
    enable_language(RC)
    target_sources(AsmrPlayer PRIVATE resources/branding/arcanblade.rc)
    target_include_directories(AsmrPlayer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/resources/branding")
    # NMake/JOM's RC dependency scanner may not discover referenced ICO files.
    set_property(SOURCE resources/branding/arcanblade.rc APPEND PROPERTY OBJECT_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/branding/arcanblade.ico")
endif()

target_link_libraries(AsmrPlayer PRIVATE
    AsmrPlaybackDiagnostics
    AsmrMobilePlayback
    AsmrUpdateUi
    AsmrTranslationSettingsUi
    AsmrChickenRice
    AsmrFasterWhisper
    AsmrGpu
    AsmrArtwork
    AsmrDuration
    AsmrWindowFrame
    AsmrSubtitle
    AsmrLibrary
    Qt6::Widgets
    Qt6::Multimedia
)

if(MSVC)
    target_compile_options(AsmrPlayer PRIVATE /W4 /utf-8)
else()
    target_compile_options(AsmrPlayer PRIVATE -Wall -Wextra)
endif()

target_sources(AsmrPlayer PRIVATE src/ui/LibraryTrackTreeModel.h src/ui/LibraryTrackTreeModel.cpp)
arcanblade_deploy_sevenzip(AsmrPlayer)


# Explicit development-only reset tool: backups precede any database replacement.
add_executable(DevelopmentDatabaseTool tools/DevelopmentDatabaseTool.cpp)
target_link_libraries(DevelopmentDatabaseTool PRIVATE AsmrLibrary)
if(MSVC)
    target_compile_options(DevelopmentDatabaseTool PRIVATE /W4 /utf-8)
endif()
