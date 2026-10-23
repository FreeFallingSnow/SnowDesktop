#include "app/app.h"

void DesktopApp::ApplyRuntimeModulePolicy()
{
    namespace modules = snowdesktop::runtime_modules;
    modules::Apply(generalSettings_.runtimeModules);
    if (!modules::Allowed(modules::Module::StartMenu) || !modules::Allowed(modules::Module::Taskbar))
    {
        if (dockStartMenu_) dockStartMenu_->Cancel();
        UpdateDockStartMenu();
    }
    if (!modules::Allowed(modules::Module::WindowMinimize))
    {
        if (dockWindowTransition_) dockWindowTransition_->Cancel();
        dockExternalMinimize_.reset();
        modules::SetActive(modules::Module::WindowMinimize, 0);
    }
    if (!modules::Allowed(modules::Module::Tray))
    {
        if (dockTrayPopup_) CloseCollectionPopup(false);
        if (statusBar_) statusBar_->CancelTrayMenuSession();
        if (systemPanel_) systemPanel_->HideTray();
        dockTrayService_.reset();
    }
    // Refresh also executes the reverse operations when a module was disabled.
    RefreshSystemTaskbarAppearance(false);
    UpdateDockExternalMinimizeTargets();
}
