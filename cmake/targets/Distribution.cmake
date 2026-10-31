# The Steam bridge is a separate MIT-licensed executable. It deliberately does
# not link SnowDesktop or any other GPL target from this build. Configure it
# after snowwidget so its Agent Skill bundle can embed the CLI.
add_subdirectory(steam_bridge)
target_link_libraries(SnowDesktop PRIVATE
    SnowDesktopWorkshopManagerSupport)

add_dependencies(SnowDesktop snowwidget SnowDesktopSteamBridge)
snowdesktop_add_product_build(SnowDesktopReleaseBuild SnowDesktop SnowDesktopLauncher
    SnowDesktopSteamBridge SnowDesktopWorkshopManager snowwidget)
snowdesktop_add_product_build(SnowDesktopDebugBuild SnowDesktop
    SnowDesktopSteamBridge SnowDesktopWorkshopManager)
add_custom_command(TARGET SnowDesktop POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:SnowDesktop>/widgets/snowdesktop-lua-widget/bin"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:snowwidget>"
        "$<TARGET_FILE_DIR:SnowDesktop>/widgets/snowdesktop-lua-widget/bin/snowwidget.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:SnowDesktopSteamBridge>"
        "$<TARGET_FILE_DIR:SnowDesktop>/widgets/snowdesktop-lua-widget/bin/SnowDesktopSteamBridge.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/steam_bridge/LICENSE"
        "$<TARGET_FILE_DIR:SnowDesktop>/widgets/snowdesktop-lua-widget/bin/SnowDesktopSteamBridge-LICENSE.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/steam_bridge/THIRD_PARTY_NOTICES.md"
        "$<TARGET_FILE_DIR:SnowDesktop>/widgets/snowdesktop-lua-widget/bin/SnowDesktopSteamBridge-THIRD-PARTY-NOTICES.md"
    COMMENT "Embedding component authoring and Workshop CLIs in the Agent Skill...")
if (SNOWDESKTOP_STEAMWORKS_SDK_ROOT)
    add_custom_command(TARGET SnowDesktop POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE_DIR:SnowDesktopSteamBridge>/steam_api64.dll"
            "$<TARGET_FILE_DIR:SnowDesktop>/widgets/snowdesktop-lua-widget/bin/steam_api64.dll"
        COMMENT "Embedding the permitted Steamworks runtime for the Agent Skill Workshop CLI...")
endif()
