/*
* All or portions of this file Copyright (c) Amazon.com, Inc. or its affiliates or
* its licensors.
*
* For complete copyright and license terms please see the LICENSE at the root of this
* distribution (the "License"). All use of this software is governed by the License,
* or, if provided, by the license below or the license accompanying this file. Do not
* remove or modify any license notices. This file is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
*
*/
#include "StdAfx.h"
#include "Resource.h"
#include "LevelEditorMenuHandler.h"
#include "MainWindow.h"
#include "QtViewPaneManager.h"
#include "ToolBox.h"
#include "Viewport.h"
#include "Controls/RollupBar.h"
#include "Controls/ConsoleSCB.h"

#include <NetPromoterScore/NetPromoterScoreDialog.h>
#include <AzFramework/API/ApplicationAPI.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <AzCore/Component/ComponentApplicationBus.h>
#include <AzToolsFramework/Metrics/LyEditorMetricsBus.h>
#include <AzToolsFramework/AssetEditor/AssetEditorBus.h>
#include <AzToolsFramework/Slice/SliceUtilities.h>

#include <LyMetricsProducer/LyMetricsAPI.h>
#include <AzQtComponents/Components/SearchLineEdit.h>
#include <QMap>
#include <QSettings>
#include <QMenuBar>
#include <QTimer>
#include <QUrlQuery>
#include <QDesktopServices>

using namespace AZ;
using namespace AzToolsFramework;

static const char* const s_LUAEditorName = "Lua Editor";
static const char* const s_netPromoterScore = "NetPromoterScore";
static const char* const s_shortTimeInterval = "debug";
static const char* const s_assetImporterMetricsIdentifier = "AssetImporter";

// top level menu ids
static const char* const s_fileMenuId = "FileMenu";
static const char* const s_editMenuId = "EditMenu";
static const char* const s_gameMenuId = "GameMenu";
static const char* const s_toolMenuId = "ToolMenu";
static const char* const s_viewMenuId = "ViewMenu";
static const char* const s_awsMenuId = "AwsMenu";
static const char* const s_helpMenuId = "HelpMenu";

static bool CompareLayoutNames(const QString& name1, const QString& name2)
{
    return name1.compare(name2, Qt::CaseInsensitive) < 0;
}

static void AddOpenViewPaneAction(ActionManager::MenuWrapper& menu, const char* viewPaneName, const char* menuActionText = nullptr)
{
    QAction* action = menu->addAction(QObject::tr(menuActionText ? menuActionText : viewPaneName));
    QObject::connect(action, &QAction::triggered, action, [viewPaneName]()
        {
            QtViewPaneManager::instance()->OpenPane(viewPaneName);
        });
}

namespace
{
    // This helper allows us to watch editor notifications to control action enable states
    class EditorListener
        : public QObject
        , public IEditorNotifyListener
    {
    public:
        explicit EditorListener(QObject* parent, AZStd::function<void(EEditorNotifyEvent)> trigger)
            : QObject(parent)
            , m_trigger(trigger)
        {}

        virtual ~EditorListener()
        {
            GetIEditor()->UnregisterNotifyListener(this);
        }

        void OnEditorNotifyEvent(EEditorNotifyEvent event)
        {
            m_trigger(event);
        }

    private:
        AZStd::function<void(EEditorNotifyEvent)> m_trigger;
    };

    void DisableActionWhileLevelChanges(QAction* action, EEditorNotifyEvent e)
    {
        if (e == eNotify_OnBeginNewScene || e == eNotify_OnBeginLoad)
        {
            action->setDisabled(true);
        }
        else if (e == eNotify_OnEndNewScene || e == eNotify_OnEndLoad)
        {
            action->setDisabled(false);
        }
    }

    void HideActionWhileEntitiesDeselected(QAction* action, EEditorNotifyEvent editorNotifyEvent)
    {
        if (action == nullptr)
        {
            return;
        }

        switch (editorNotifyEvent)
        {
        case eNotify_OnEntitiesDeselected:
        {
            action->setVisible(false);
            break;
        }
        case eNotify_OnEntitiesSelected:
        {
            action->setVisible(true);
            break;
        }
        default:
            break;
        }
    }

}

LevelEditorMenuHandler::LevelEditorMenuHandler(
    MainWindow* mainWindow, QtViewPaneManager* const viewPaneManager, QSettings& settings)
    : QObject(mainWindow)
    , m_mainWindow(mainWindow)
    , m_viewPaneManager(viewPaneManager)
    , m_actionManager(mainWindow->GetActionManager())
    , m_settings(settings)
    , m_enableLegacyCryEntities(false)
{
#if defined(AZ_PLATFORM_MAC)
    // Hide the non-native toolbar, then setNativeMenuBar to ensure it is always visible on macOS.
    m_mainWindow->menuBar()->hide();
    m_mainWindow->menuBar()->setNativeMenuBar(true);
#endif

    ComponentModeFramework::EditorComponentModeNotificationBus::Handler::BusConnect(
        AzToolsFramework::GetEntityContextId());
    EditorMenuRequestBus::Handler::BusConnect();
}

LevelEditorMenuHandler::~LevelEditorMenuHandler()
{
    EditorMenuRequestBus::Handler::BusDisconnect();
    ComponentModeFramework::EditorComponentModeNotificationBus::Handler::BusDisconnect();
}

void LevelEditorMenuHandler::Initialize()
{
    m_enableLegacyCryEntities = GetIEditor()->IsLegacyUIEnabled();

    // make sure we can fix the view menus
    connect(
        m_viewPaneManager, &QtViewPaneManager::registeredPanesChanged,
        this, &LevelEditorMenuHandler::ResetToolsMenus);

    m_topLevelMenus << CreateFileMenu();

    auto editMenu = CreateEditMenu();
    auto editMenuWrapper = ActionManager::MenuWrapper(editMenu, m_actionManager);
    PopulateEditMenu(editMenuWrapper);

    m_editmenu = editMenu;

    m_topLevelMenus << editMenu;
    m_topLevelMenus << CreateGameMenu();
    m_topLevelMenus << CreateToolsMenu();
    m_topLevelMenus << CreateViewMenu();
    m_topLevelMenus << CreateAWSMenu();
    m_topLevelMenus << CreateHelpMenu();

    // have to do this after creating the AWS Menu for the first time
    ResetToolsMenus();

    // Add our menus to the main window menu bar
    QMenuBar* menuBar = m_mainWindow->menuBar();
    menuBar->clear();
    for (QMenu* menu : m_topLevelMenus)
    {
        menuBar->addMenu(menu);
    }
}

bool LevelEditorMenuHandler::MRUEntryIsValid(const QString& entry, const QString& gameFolderPath)
{
    if (entry.isEmpty())
    {
        return false;
    }

    QFileInfo info(entry);
    if (!info.exists())
    {
        return false;
    }

    const QDir gameDir(gameFolderPath);
    QDir dir(entry); // actually pointing at file, first cdUp() gets us the parent dir
    while (dir.cdUp())
    {
        if (dir == gameDir)
        {
            return true;
        }
    }

    return false;
}

void LevelEditorMenuHandler::IncrementViewPaneVersion()
{
    m_viewPaneVersion++;
}

int LevelEditorMenuHandler::GetViewPaneVersion() const
{
    return m_viewPaneVersion;
}

void LevelEditorMenuHandler::UpdateViewLayoutsMenu(ActionManager::MenuWrapper& layoutsMenu)
{
    if (layoutsMenu.isNull())
    {
        return;
    }

    QStringList layoutNames = m_viewPaneManager->LayoutNames();
    qSort(layoutNames.begin(), layoutNames.end(), CompareLayoutNames);
    layoutsMenu->clear();
    const int MAX_LAYOUTS = ID_VIEW_LAYOUT_LAST - ID_VIEW_LAYOUT_FIRST + 1;

    // Component entity layout is just the default if we are in Cry-Free mode
    QString componentLayoutLabel = m_enableLegacyCryEntities ? tr("Component Entity Layout") : tr("Default Layout");
    QAction* componentLayoutAction = layoutsMenu->addAction(componentLayoutLabel);
    connect(componentLayoutAction, &QAction::triggered, this, &LevelEditorMenuHandler::LoadComponentLayout);

    // Only show legacy layout if we're in legacy mode
    if (m_enableLegacyCryEntities)
    {
        // Load Legacy Layout
        QAction* legacyLayoutAction = layoutsMenu->addAction(tr("Legacy Layout"));
        connect(legacyLayoutAction, &QAction::triggered, this, &LevelEditorMenuHandler::LoadLegacyLayout);
    }

    layoutsMenu.AddSeparator();

    for (int i = 0; i < layoutNames.size() && i <= MAX_LAYOUTS; i++)
    {
        const QString layoutName = layoutNames[i];
        QAction* action = layoutsMenu->addAction(layoutName);
        QMenu* subSubMenu = new QMenu();

        QAction* subSubAction = subSubMenu->addAction(tr("Load"));
        connect(subSubAction, &QAction::triggered, this, [layoutName, this]() { m_mainWindow->ViewLoadPaneLayout(layoutName); });

        subSubAction = subSubMenu->addAction(tr("Save"));
        connect(subSubAction, &QAction::triggered, this, [layoutName, this]() { m_mainWindow->ViewSavePaneLayout(layoutName); });

        subSubAction = subSubMenu->addAction(tr("Rename..."));
        connect(subSubAction, &QAction::triggered, this, [layoutName, this]() { m_mainWindow->ViewRenamePaneLayout(layoutName); });

        subSubAction = subSubMenu->addAction(tr("Delete"));
        connect(subSubAction, &QAction::triggered, this, [layoutName, this]() { m_mainWindow->ViewDeletePaneLayout(layoutName); });

        action->setMenu(subSubMenu);
    }

    layoutsMenu.AddAction(ID_VIEW_SAVELAYOUT);

    layoutsMenu.AddAction(ID_VIEW_LAYOUT_LOAD_DEFAULT);
}

void LevelEditorMenuHandler::ResetToolsMenus()
{
    if (!m_toolsMenu->isEmpty())
    {
        // Clear everything from the Tools menu
        m_toolsMenu->clear();
    }

    if (!m_cloudMenu->isEmpty())
    {
        m_cloudMenu->clear();
    }

    QtViewPanes allRegisteredViewPanes = QtViewPaneManager::instance()->GetRegisteredPanes();

    QMap<QString, QList<QtViewPane*> > menuMap;

    CreateMenuMap(menuMap, allRegisteredViewPanes);

    CreateMenuOptions(&menuMap, m_toolsMenu, LyViewPane::CategoryTools);

    m_toolsMenu.AddSeparator();

    // Other
    auto otherSubMenu = m_toolsMenu.AddMenu(QObject::tr("Other"));

    CreateMenuOptions(&menuMap, otherSubMenu, LyViewPane::CategoryOther);

    // Plug-Ins
    auto plugInsMenu = m_toolsMenu.AddMenu(QObject::tr("Plug-Ins"));

    CreateMenuOptions(&menuMap, plugInsMenu, LyViewPane::CategoryPlugIns);

    m_toolsMenu.AddSeparator();

    // set up the cloud canvas menu, which is slightly different than the other menus because it
    // goes somewhere else
    m_cloudMenu.AddAction(ID_AWS_ACTIVE_DEPLOYMENT);
    CreateMenuOptions(&menuMap, m_cloudMenu, LyViewPane::CategoryCloudCanvas);

    // Optional Sub Menus
    if (!menuMap.isEmpty())
    {
        QMap<QString, QList<QtViewPane*> >::iterator it = menuMap.begin();

        while (it != menuMap.end())
        {
            auto currentSubMenu = m_toolsMenu.AddMenu(it.key());
            CreateMenuOptions(&menuMap, currentSubMenu, it.key().toStdString().c_str());

            it = menuMap.begin();
        }
    }
}

QMenu* LevelEditorMenuHandler::CreateFileMenu()
{
    auto fileMenu = m_actionManager->AddMenu(tr("&File"), s_fileMenuId);
    connect(fileMenu, &QMenu::aboutToShow, this, &LevelEditorMenuHandler::OnUpdateOpenRecent);

    // New level
    auto fileNew = fileMenu.AddAction(ID_FILE_NEW);
    GetIEditor()->RegisterNotifyListener(new EditorListener(fileNew, [fileNew](EEditorNotifyEvent e)
    {
        DisableActionWhileLevelChanges(fileNew, e);
    }));

    // Open level...
    auto fileOpenLevel = fileMenu.AddAction(ID_FILE_OPEN_LEVEL);
    GetIEditor()->RegisterNotifyListener(new EditorListener(fileOpenLevel, [fileOpenLevel](EEditorNotifyEvent e)
    {
        DisableActionWhileLevelChanges(fileOpenLevel, e);
    }));

#ifdef ENABLE_SLICE_EDITOR
    // New slice
    auto fileNewSlice = fileMenu.AddAction(ID_FILE_NEW_SLICE);
    GetIEditor()->RegisterNotifyListener(new EditorListener(fileNewSlice, [fileNewSlice](EEditorNotifyEvent e)
    {
        DisableActionWhileLevelChanges(fileNewSlice, e);
    }));

    // Open slice...
    auto fileOpenSlice = fileMenu.AddAction(ID_FILE_OPEN_SLICE);
    GetIEditor()->RegisterNotifyListener(new EditorListener(fileOpenSlice, [fileOpenSlice](EEditorNotifyEvent e)
    {
        DisableActionWhileLevelChanges(fileOpenSlice, e);
    }));
#endif

    // Save Selected Slice
    auto saveSelectedSlice = fileMenu.AddAction(ID_FILE_SAVE_SELECTED_SLICE);
    saveSelectedSlice->setVisible(false);
    GetIEditor()->RegisterNotifyListener(new EditorListener(saveSelectedSlice, [saveSelectedSlice](EEditorNotifyEvent e)
    {
        HideActionWhileEntitiesDeselected(saveSelectedSlice, e);
    }));

    // Save Slice to Root
    auto saveSliceToRoot = fileMenu.AddAction(ID_FILE_SAVE_SLICE_TO_ROOT);
    saveSliceToRoot->setVisible(false);
    GetIEditor()->RegisterNotifyListener(new EditorListener(saveSliceToRoot, [saveSliceToRoot](EEditorNotifyEvent e)
    {
        HideActionWhileEntitiesDeselected(saveSliceToRoot, e);
    }));

    // Open Recent
    m_mostRecentLevelsMenu = fileMenu.AddMenu(tr("Open Recent"));
    connect(m_mostRecentLevelsMenu, &QMenu::aboutToShow, this, &LevelEditorMenuHandler::UpdateMRUFiles);

    OnUpdateOpenRecent();

    // Import...
    auto assetImporterMenu = fileMenu.Get()->addAction(tr("Import..."));
    connect(assetImporterMenu, &QAction::triggered, this, &LevelEditorMenuHandler::ActivateAssetImporter);
    EditorMetricsEventsBus::Broadcast(
        &EditorMetricsEventsBus::Events::RegisterAction, assetImporterMenu, s_assetImporterMetricsIdentifier);

    fileMenu.AddSeparator();

    // Save
    fileMenu.AddAction(ID_FILE_SAVE_LEVEL);

    // Save As...
    fileMenu.AddAction(ID_FILE_SAVE_AS);

    // Save Level Resources...
    if (m_enableLegacyCryEntities)
    {
        fileMenu.AddAction(ID_FILE_SAVELEVELRESOURCES);
    }

    // Save Level Statistics
    fileMenu.AddAction(ID_TOOLS_LOGMEMORYUSAGE);

    // Save Modified External Layers
    if (m_enableLegacyCryEntities)
    {
        fileMenu.AddAction(ID_PANEL_LAYERS_SAVE_EXTERNAL_LAYERS);
    }

    fileMenu.AddSeparator();

    // Project Settings
    auto projectSettingMenu = fileMenu.AddMenu(tr("Project Settings"));

    // If the current project is external to the engine folder, then do not add options to launch AP or Project Configurator
    // because this feature is in preview mode.
    bool isProjectExternal = false;
    AzFramework::ApplicationRequests::Bus::BroadcastResult(
        isProjectExternal, &AzFramework::ApplicationRequests::IsEngineExternal);

    if (!isProjectExternal)
    {
        // Switch Projects
        projectSettingMenu.AddAction(ID_PROJECT_CONFIGURATOR_PROJECTSELECTION);

        if (m_enableLegacyCryEntities)
        {
            // Input Mapping
            auto inputMappingMenu = projectSettingMenu.Get()->addAction(tr("Input Mapping"));
            connect(inputMappingMenu, &QAction::triggered, this, &LevelEditorMenuHandler::OnOpenAssetEditor);
        }
        // Configure Gems
        auto configureGemSubMenu = projectSettingMenu.Get()->addAction(tr("Configure Gems"));
        connect(configureGemSubMenu, &QAction::triggered, this, &LevelEditorMenuHandler::ActivateGemConfiguration);
    }

    // Project Settings Tool
    // Shortcut must be set while adding the action otherwise it doesn't work
    projectSettingMenu.Get()->addAction(
        tr(LyViewPane::ProjectSettingsTool),
        []() { QtViewPaneManager::instance()->OpenPane(LyViewPane::ProjectSettingsTool); },
        tr("Ctrl+Shift+P"));

    projectSettingMenu.AddSeparator();

    // Deployment Tool
    AddOpenViewPaneAction(projectSettingMenu, LyViewPane::DeploymentTool, "Deploy to device");

    fileMenu.AddSeparator();

    // NEWMENUS: NEEDS IMPLEMENTATION
    // should have the equivalent of a Close here; it should be close the current slice, but the editor isn't slice based right now
    // so that won't work.
    // instead, it should be Close of the level, but we don't have that either. I'm leaving it here so that it's obvious where UX intended it
    // to go
    //fileMenu.AddAction(ID_FILE_CLOSE);

    // Show Log File
    fileMenu.AddAction(ID_FILE_EDITLOGFILE);

    fileMenu.AddSeparator();

    fileMenu.AddAction(ID_FILE_RESAVESLICES);

    fileMenu.AddSeparator();

    fileMenu.AddAction(ID_CONVERT_LEGACY_ENTITIES);

    fileMenu.AddAction(ID_APP_EXIT);

    return fileMenu;
}

void LevelEditorMenuHandler::PopulateEditMenu(ActionManager::MenuWrapper& editMenu)
{
    const bool newViewportInteractionModelEnabled = GetIEditor()->IsNewViewportInteractionModelEnabled();

    // Undo
    editMenu.AddAction(ID_UNDO);

    // Redo
    editMenu.AddAction(ID_REDO);

    editMenu.AddSeparator();

    // NEWMENUS: NEEDS IMPLEMENTATION
    // Not quite ready for these yet. Have to register them with the ActionManager in MainWindow.cpp when we're ready
    // editMenu->addAction(ID_EDIT_CUT);
    // editMenu->addAction(ID_EDIT_COPY);
    // editMenu->addAction(ID_EDIT_PASTE);
    // editMenu.AddSeparator();

    // Duplicate
    editMenu.AddAction(ID_EDIT_CLONE);

    // Delete
    editMenu.AddAction(ID_EDIT_DELETE);

    editMenu.AddSeparator();

    // Select All
    editMenu.AddAction(ID_EDIT_SELECTALL);

    // Deselect All
    if (!newViewportInteractionModelEnabled)
    {
        editMenu.AddAction(ID_EDIT_SELECTNONE);
    }

    // Next Selection Mask
    if (m_enableLegacyCryEntities && !newViewportInteractionModelEnabled)
    {
        editMenu.AddAction(ID_EDIT_NEXTSELECTIONMASK);
    }

    // Invert Selection
    editMenu.AddAction(ID_EDIT_INVERTSELECTION);

    // Select Slice Root
    if (!m_selectSliceRootMenu)
    {
        m_selectSliceRootMenu = editMenu.AddMenu(QObject::tr("Select root"));
        m_selectSliceRootMenu->setEnabled(false);
    }

    editMenu.AddSeparator();

    // New Viewport Interaction Model actions/shortcuts
    if (newViewportInteractionModelEnabled)
    {
        editMenu.AddAction(ID_EDIT_PIVOT);
        editMenu.AddAction(ID_EDIT_RESET);
        editMenu.AddAction(ID_EDIT_RESET_MANIPULATOR);
        editMenu.AddAction(ID_EDIT_RESET_LOCAL);
        editMenu.AddAction(ID_EDIT_RESET_WORLD);
    }

    // Hide Selection
    editMenu.AddAction(ID_EDIT_HIDE);

    if (!newViewportInteractionModelEnabled)
    {
        // Show Selection
        auto showSelectionMenu = editMenu.Get()->addAction(tr("Show Selection"));
        connect(showSelectionMenu, &QAction::triggered, this, [this]() { ToggleSelection(false); });

        // Show Last Hidden
        editMenu.AddAction(ID_EDIT_SHOW_LAST_HIDDEN);
    }

    // Unhide All
    editMenu.AddAction(ID_EDIT_UNHIDEALL);

    /*
     * The following block of code is part of the feature "Isolation Mode" and is temporarily
     * disabled for 1.10 release.
     * Jira: https://jira.agscollab.com/browse/LY-49532

    // Isolate Selected
    QAction* isolateSelectedAction = editMenu->addAction(tr("Isolate Selected"));
    connect(isolateSelectedAction, &QAction::triggered, this, []() {
        AzToolsFramework::ToolsApplicationRequestBus::Broadcast(&AzToolsFramework::ToolsApplicationRequestBus::Events::EnterEditorIsolationMode);
    });

    // Exit Isolation
    QAction* exitIsolationAction = editMenu->addAction(tr("Exit Isolation"));
    connect(exitIsolationAction, &QAction::triggered, this, []() {
        AzToolsFramework::ToolsApplicationRequestBus::Broadcast(&AzToolsFramework::ToolsApplicationRequestBus::Events::ExitEditorIsolationMode);
    });

    connect(editMenu, &QMenu::aboutToShow, this, [isolateSelectedAction, exitIsolationAction]() {
        bool isInIsolationMode = false;
        AzToolsFramework::ToolsApplicationRequestBus::BroadcastResult(isInIsolationMode, &AzToolsFramework::ToolsApplicationRequestBus::Events::IsEditorInIsolationMode);
        if (isInIsolationMode)
        {
            isolateSelectedAction->setDisabled(true);
            exitIsolationAction->setDisabled(false);
        }
        else
        {
            isolateSelectedAction->setDisabled(false);
            exitIsolationAction->setDisabled(true);
        }
    });

    */

    editMenu.AddSeparator();

    // Group Sub Menu
    if (m_enableLegacyCryEntities && !newViewportInteractionModelEnabled)
    {
        auto groupSubMenu = editMenu.AddMenu(tr("Group"));

        // Group
        groupSubMenu.AddAction(ID_GROUP_MAKE);

        // Ungroup
        groupSubMenu.AddAction(ID_GROUP_UNGROUP);

        // Open Group
        groupSubMenu.AddAction(ID_GROUP_OPEN);

        // Close Group
        groupSubMenu.AddAction(ID_GROUP_CLOSE);

        // Attach to Group
        groupSubMenu.AddAction(ID_GROUP_ATTACH);

        // Detach from Group
        groupSubMenu.AddAction(ID_GROUP_DETACH);

        groupSubMenu.AddSeparator();

        // Hold
        groupSubMenu.AddAction(ID_EDIT_HOLD);

        // Fetch
        groupSubMenu.AddAction(ID_EDIT_FETCH);
    }

    // Modify Menu
    auto modifyMenu = editMenu.AddMenu(tr("&Modify"));

    if (!newViewportInteractionModelEnabled)
    {
        modifyMenu.AddAction(ID_MODIFY_LINK);
        modifyMenu.AddAction(ID_MODIFY_UNLINK);
        modifyMenu.AddSeparator();

        auto alignMenu = modifyMenu.AddMenu(tr("Align"));
        alignMenu.AddAction(ID_OBJECTMODIFY_ALIGNTOGRID);
        alignMenu.AddAction(ID_OBJECTMODIFY_ALIGN);
        alignMenu.AddAction(ID_MODIFY_ALIGNOBJTOSURF);

        auto constrainMenu = modifyMenu.AddMenu(tr("Constrain"));
        constrainMenu.AddAction(ID_SELECT_AXIS_X);
        constrainMenu.AddAction(ID_SELECT_AXIS_Y);
        constrainMenu.AddAction(ID_SELECT_AXIS_Z);
        constrainMenu.AddAction(ID_SELECT_AXIS_XY);
        constrainMenu.AddAction(ID_SELECT_AXIS_TERRAIN);
    }

    auto snapMenu = modifyMenu.AddMenu(tr("Snap"));

    if (!newViewportInteractionModelEnabled)
    {
        snapMenu.AddAction(ID_SNAP_TO_GRID);
    }

    snapMenu.AddAction(ID_SNAPANGLE);

    if (!newViewportInteractionModelEnabled)
    {
        auto fastRotateMenu = modifyMenu.AddMenu(tr("Fast Rotate"));
        fastRotateMenu.AddAction(ID_ROTATESELECTION_XAXIS);
        fastRotateMenu.AddAction(ID_ROTATESELECTION_YAXIS);
        fastRotateMenu.AddAction(ID_ROTATESELECTION_ZAXIS);
        fastRotateMenu.AddAction(ID_ROTATESELECTION_ROTATEANGLE);
    }

    auto transformModeMenu = modifyMenu.AddMenu(tr("Transform Mode"));
    if (!newViewportInteractionModelEnabled)
    {
        transformModeMenu.AddAction(ID_EDITMODE_SELECT);
    }

    transformModeMenu.AddAction(ID_EDITMODE_MOVE);
    transformModeMenu.AddAction(ID_EDITMODE_ROTATE);
    transformModeMenu.AddAction(ID_EDITMODE_SCALE);

    if (!newViewportInteractionModelEnabled)
    {
        transformModeMenu.AddAction(ID_EDITMODE_SELECTAREA);
    }

    if (m_enableLegacyCryEntities && !newViewportInteractionModelEnabled)
    {
        auto convertToMenu = modifyMenu.AddMenu(tr("Convert to"));
        convertToMenu.AddAction(ID_CONVERTSELECTION_TOBRUSHES);
        convertToMenu.AddAction(ID_CONVERTSELECTION_TOSIMPLEENTITY);
        convertToMenu.AddAction(ID_CONVERTSELECTION_TODESIGNEROBJECT);
        convertToMenu.AddAction(ID_CONVERTSELECTION_TOSTATICENTITY);
        convertToMenu.AddAction(ID_CONVERTSELECTION_TOGAMEVOLUME);
        convertToMenu.AddAction(ID_CONVERTSELECTION_TOCOMPONENTENTITY);

        auto subObjectModeMenu = modifyMenu.AddMenu(tr("Sub Object Mode"));
        subObjectModeMenu.AddAction(ID_SUBOBJECTMODE_EDGE);
        subObjectModeMenu.AddAction(ID_SUBOBJECTMODE_FACE);
        subObjectModeMenu.AddAction(ID_SUBOBJECTMODE_PIVOT);
        subObjectModeMenu.AddAction(ID_SUBOBJECTMODE_VERTEX);

        modifyMenu.AddSeparator();

        modifyMenu.AddAction(ID_SELECTION_SAVE);
        modifyMenu.AddAction(ID_SELECTION_LOAD);
        modifyMenu.AddSeparator();

        modifyMenu.AddAction(ID_TOOLS_UPDATEPROCEDURALVEGETATION);
    }

    editMenu.AddSeparator();

    // Lock Selection
    editMenu.AddAction(ID_EDIT_FREEZE);

    // NEWMENUS: NEEDS IMPLEMENTATION
    //// Unlock Selection
    //auto unlockSelectionMenu = editMenu.Get()->addAction(tr("Unlock Selection"));

    //// Unlock Last Locked
    //auto unlockLastLockedMenu = editMenu.Get()->addAction(tr("Unlock Last Locked"));

    // Unlock All
    editMenu.AddAction(ID_EDIT_UNFREEZEALL);

    if (m_enableLegacyCryEntities && !newViewportInteractionModelEnabled)
    {
        // Rename Object(s)...
        editMenu.AddAction(ID_EDIT_RENAMEOBJECT);

        // Set Object(s) Height...
        editMenu.AddAction(ID_MODIFY_OBJECT_HEIGHT);
    }

    editMenu.AddSeparator();

    // Editor Settings
    auto editorSettingsMenu = editMenu.AddMenu(tr("Editor Settings"));

    // Global Preferences...
    editorSettingsMenu.AddAction(ID_TOOLS_PREFERENCES);

    // Graphics Settings...
    editorSettingsMenu.AddAction(ID_GRAPHICS_SETTINGS);

    // Editor Settings Manager (only shown here in Cry-Free mode)
    if (!m_enableLegacyCryEntities)
    {
        AddOpenViewPaneAction(editorSettingsMenu, LyViewPane::EditorSettingsManager);
    }

    // Graphics Performance
    auto graphicPerformanceSubMenu = editorSettingsMenu.AddMenu(QObject::tr("Graphics Performance"));

    auto pcMenu = graphicPerformanceSubMenu.AddMenu(tr("PC"));
    pcMenu.AddAction(ID_GAME_PC_ENABLEVERYHIGHSPEC);
    pcMenu.AddAction(ID_GAME_PC_ENABLEHIGHSPEC);
    pcMenu.AddAction(ID_GAME_PC_ENABLEMEDIUMSPEC);
    pcMenu.AddAction(ID_GAME_PC_ENABLELOWSPEC);

    auto osxmetalMenu = graphicPerformanceSubMenu.AddMenu(tr("OSX Metal"));
    osxmetalMenu.AddAction(ID_GAME_OSXMETAL_ENABLEVERYHIGHSPEC);
    osxmetalMenu.AddAction(ID_GAME_OSXMETAL_ENABLEHIGHSPEC);
    osxmetalMenu.AddAction(ID_GAME_OSXMETAL_ENABLEMEDIUMSPEC);
    osxmetalMenu.AddAction(ID_GAME_OSXMETAL_ENABLELOWSPEC);

    auto androidMenu = graphicPerformanceSubMenu.AddMenu(tr("Android"));
    androidMenu.AddAction(ID_GAME_ANDROID_ENABLEVERYHIGHSPEC);
    androidMenu.AddAction(ID_GAME_ANDROID_ENABLEHIGHSPEC);
    androidMenu.AddAction(ID_GAME_ANDROID_ENABLEMEDIUMSPEC);
    androidMenu.AddAction(ID_GAME_ANDROID_ENABLELOWSPEC);

    auto iosMenu = graphicPerformanceSubMenu.AddMenu(tr("iOS"));
    iosMenu.AddAction(ID_GAME_IOS_ENABLEVERYHIGHSPEC);
    iosMenu.AddAction(ID_GAME_IOS_ENABLEHIGHSPEC);
    iosMenu.AddAction(ID_GAME_IOS_ENABLEMEDIUMSPEC);
    iosMenu.AddAction(ID_GAME_IOS_ENABLELOWSPEC);

#if defined(AZ_TOOLS_EXPAND_FOR_RESTRICTED_PLATFORMS)
#define AZ_RESTRICTED_PLATFORM_EXPANSION(CodeName, CODENAME, codename, PrivateName, PRIVATENAME, privatename, PublicName, PUBLICNAME, publicname, PublicAuxName1, PublicAuxName2, PublicAuxName3)\
    auto publicname##Menu = graphicPerformanceSubMenu.AddMenu(tr(PublicAuxName2));\
    publicname##Menu.AddAction(ID_GAME_##PUBLICNAME##_ENABLEHIGHSPEC);\
    publicname##Menu.AddAction(ID_GAME_##PUBLICNAME##_ENABLEMEDIUMSPEC);\
    publicname##Menu.AddAction(ID_GAME_##PUBLICNAME##_ENABLELOWSPEC);
    AZ_TOOLS_EXPAND_FOR_RESTRICTED_PLATFORMS
#undef AZ_RESTRICTED_PLATFORM_EXPANSION
#endif

    graphicPerformanceSubMenu.AddAction(ID_GAME_APPLETV_ENABLESPEC);

    // Keyboard Customization
    auto keyboardCustomizationMenu = editorSettingsMenu.AddMenu(tr("Keyboard Customization"));
    keyboardCustomizationMenu.AddAction(ID_TOOLS_CUSTOMIZEKEYBOARD);
    keyboardCustomizationMenu.AddAction(ID_TOOLS_EXPORT_SHORTCUTS);
    keyboardCustomizationMenu.AddAction(ID_TOOLS_IMPORT_SHORTCUTS);
}

QMenu* LevelEditorMenuHandler::CreateEditMenu()
{
    return m_actionManager->AddMenu(tr("&Edit"), s_editMenuId);
}

QMenu* LevelEditorMenuHandler::CreateGameMenu()
{
    auto gameMenu = m_actionManager->AddMenu(tr("&Game"), s_gameMenuId);

    // Play Game
    gameMenu.AddAction(ID_VIEW_SWITCHTOGAME);

    // Enable Physics/AI
    gameMenu.AddAction(ID_SWITCH_PHYSICS);
    gameMenu.AddSeparator();

    // Export to Engine
    gameMenu.AddAction(ID_FILE_EXPORTTOGAMENOSURFACETEXTURE);

    // Export Selected Objects
    gameMenu.AddAction(ID_FILE_EXPORT_SELECTEDOBJECTS);

    // Export Occlusion Mesh
    gameMenu.AddAction(ID_FILE_EXPORTOCCLUSIONMESH);

    gameMenu.AddSeparator();

    // Terrain Collision
    gameMenu.AddAction(ID_TERRAIN_COLLISION);

    if (m_enableLegacyCryEntities)
    {
        // Edit Equipment-Packs...
        gameMenu.AddAction(ID_TOOLS_EQUIPPACKSEDIT);

        // Toggle SP/MP GameRules
        gameMenu.AddAction(ID_TOGGLE_MULTIPLAYER);
    }

    // Synchronize Player with Camera
    gameMenu.AddAction(ID_GAME_SYNCPLAYER);

    //AI
    auto aiMenu = gameMenu.AddMenu(tr("AI"));

    if (m_enableLegacyCryEntities)
    {
        // Generate All AI
        aiMenu.AddAction(ID_AI_GENERATEALL);

        // Generate Triangulation
        aiMenu.AddAction(ID_AI_GENERATETRIANGULATION);

        // Generate 3D Navigation Volumes
        aiMenu.AddAction(ID_AI_GENERATE3DVOLUMES);

        // Generate Flight Navigation
        aiMenu.AddAction(ID_AI_GENERATEFLIGHTNAVIGATION);

        // Generate Waypoints
        aiMenu.AddAction(ID_AI_GENERATEWAYPOINT);

        // Validate Navigation
        aiMenu.AddAction(ID_AI_VALIDATENAVIGATION);

        // Clear All Navigation
        aiMenu.AddAction(ID_AI_CLEARALLNAVIGATION);

        // Generate Spawner Entity Code
        aiMenu.AddAction(ID_AI_GENERATESPAWNERS);

        // Generate 3D Debug Voxels
        aiMenu.AddAction(ID_AI_GENERATE3DDEBUGVOXELS);

        // Create New Navigation Area
        aiMenu.AddAction(ID_AI_NAVIGATION_NEW_AREA);
    }

    // Request a Full MNM Rebuild
    aiMenu.AddAction(ID_AI_NAVIGATION_TRIGGER_FULL_REBUILD);

    // Show Navigation Areas
    aiMenu.AddAction(ID_AI_NAVIGATION_SHOW_AREAS);

    if (m_enableLegacyCryEntities)
    {
        // Add Navigation Seed
        aiMenu.AddAction(ID_AI_NAVIGATION_ADD_SEED);
    }

    // Continuous Update
    aiMenu.AddAction(ID_AI_NAVIGATION_ENABLE_CONTINUOUS_UPDATE);

    // Visualize Navigation Accessibility
    aiMenu.AddAction(ID_AI_NAVIGATION_VISUALIZE_ACCESSIBILITY);

    // Medium Sized Characters (Use the old version for now)
    aiMenu.AddAction(ID_AI_NAVIGATION_DISPLAY_AGENT);

    if (m_enableLegacyCryEntities)
    {
        // Generate Cover Surfaces
        aiMenu.AddAction(ID_AI_GENERATECOVERSURFACES);

        // AIPoint Pick Link
        aiMenu.AddAction(ID_MODIFY_AIPOINT_PICKLINK);

        // AIPoint Pick Impass Link
        aiMenu.AddAction(ID_MODIFY_AIPOINT_PICKIMPASSLINK);
    }

    gameMenu.AddSeparator();

    // Audio
    auto audioMenu = gameMenu.AddMenu(tr("Audio"));

    // Stop All Sounds
    audioMenu.AddAction(ID_SOUND_STOPALLSOUNDS);

    // Refresh Audio
    audioMenu.AddAction(ID_AUDIO_REFRESH_AUDIO_SYSTEM);

    gameMenu.AddSeparator();

    if (m_enableLegacyCryEntities)
    {
        // Clouds
        auto cloudsMenu = gameMenu.AddMenu(tr("Clouds"));

        // Create
        cloudsMenu.AddAction(ID_CLOUDS_CREATE);

        // Destroy
        cloudsMenu.AddAction(ID_CLOUDS_DESTROY);
        cloudsMenu.AddSeparator();

        // Open
        cloudsMenu.AddAction(ID_CLOUDS_OPEN);

        // Close
        cloudsMenu.AddAction(ID_CLOUDS_CLOSE);

        gameMenu.AddSeparator();

        // Prefabs
        auto prefabsMenu = gameMenu.AddMenu(tr("Prefabs"));

        prefabsMenu.AddAction(ID_PREFABS_MAKEFROMSELECTION);
        prefabsMenu.AddAction(ID_PREFABS_ADDSELECTIONTOPREFAB);
        prefabsMenu.AddSeparator();
        prefabsMenu.AddAction(ID_PREFABS_CLONESELECTIONFROMPREFAB);
        prefabsMenu.AddAction(ID_PREFABS_EXTRACTSELECTIONFROMPREFAB);
        prefabsMenu.AddSeparator();
        prefabsMenu.AddAction(ID_PREFABS_OPENALL);
        prefabsMenu.AddAction(ID_PREFABS_CLOSEALL);
        prefabsMenu.AddSeparator();
        prefabsMenu.AddAction(ID_PREFABS_REFRESHALL);

        gameMenu.AddSeparator();
    }

    if (!GetIEditor()->IsNewViewportInteractionModelEnabled())
    {
        // Terrain
        auto terrainMenu = gameMenu.AddMenu(tr("&Terrain"));

        terrainMenu.AddAction(ID_FILE_GENERATETERRAINTEXTURE);
        terrainMenu.AddAction(ID_FILE_GENERATETERRAIN);
        terrainMenu.AddSeparator();

        terrainMenu.AddAction(ID_TERRAIN);

        terrainMenu.AddAction(ID_TERRAIN_TEXTURE_EXPORT);

        terrainMenu.AddSeparator();

        terrainMenu.AddAction(ID_TERRAIN_EXPORTBLOCK);
        terrainMenu.AddAction(ID_TERRAIN_IMPORTBLOCK);
        terrainMenu.AddAction(ID_TERRAIN_RESIZE);
        terrainMenu.AddSeparator();

        auto terrainModifyMenu = terrainMenu.AddMenu(tr("Terrain Modify"));
        terrainModifyMenu.AddAction(ID_TOOLTERRAINMODIFY_SMOOTH);
        terrainModifyMenu.AddAction(ID_TERRAINMODIFY_SMOOTH);

        terrainMenu.AddAction(ID_TERRAIN_VEGETATION);
        terrainMenu.AddAction(ID_TERRAIN_PAINTLAYERS);
        terrainMenu.AddAction(ID_TERRAIN_REFINETERRAINTEXTURETILES);
        terrainMenu.AddSeparator();
        terrainMenu.AddAction(ID_FILE_EXPORT_TERRAINAREA);
        terrainMenu.AddAction(ID_FILE_EXPORT_TERRAINAREAWITHOBJECTS);

        gameMenu.AddSeparator();
    }

    CreateDebuggingSubMenu(gameMenu);

    return gameMenu;
}

QMenu* LevelEditorMenuHandler::CreateToolsMenu()
{
    // Tools
    m_toolsMenu = m_actionManager->AddMenu(tr("&Tools"), s_toolMenuId);
    return m_toolsMenu;
}

QMenu* LevelEditorMenuHandler::CreateAWSMenu()
{
    // AWS
    auto awsMenu = m_actionManager->AddMenu(tr("&AWS"), s_awsMenuId);
    connect(awsMenu, &QMenu::aboutToShow,
        this, &LevelEditorMenuHandler::AWSMenuClicked);
    awsMenu.AddAction(ID_AWS_CREDENTIAL_MGR);

    // Cloud Canvas
    m_cloudMenu = awsMenu.AddMenu(tr("Cloud Canvas"));

    // Commerce
    auto commerceMenu = awsMenu.AddMenu(tr("Commerce"));
    commerceMenu.AddAction(ID_COMMERCE_MERCH);
    commerceMenu.AddAction(ID_COMMERCE_PUBLISH);

    // GameLift
    auto awsGameLiftMenu = awsMenu.AddMenu(tr("GameLift"));
    awsGameLiftMenu.AddAction(ID_AWS_GAMELIFT_LEARN);
    awsGameLiftMenu.AddAction(ID_AWS_GAMELIFT_CONSOLE);
    awsGameLiftMenu.AddAction(ID_AWS_GAMELIFT_GETSTARTED);
    awsGameLiftMenu.AddAction(ID_AWS_GAMELIFT_TRIALWIZARD);

    // Open AWS Console
    auto awsConsoleMenu = awsMenu.AddMenu(tr("Open AWS Console"));
    awsConsoleMenu.AddAction(ID_AWS_LAUNCH);
    awsConsoleMenu.AddAction(ID_AWS_COGNITO_CONSOLE);
    awsConsoleMenu.AddAction(ID_AWS_DEVICEFARM_CONSOLE);
    awsConsoleMenu.AddAction(ID_AWS_DYNAMODB_CONSOLE);
    awsConsoleMenu.AddAction(ID_AWS_LAMBDA_CONSOLE);
    awsConsoleMenu.AddAction(ID_AWS_S3_CONSOLE);

    // Cloud Gem Portal
    awsMenu.AddSeparator();
    awsMenu.AddAction(ID_CGP_CONSOLE);

    return awsMenu;
}

QMenu* LevelEditorMenuHandler::CreateViewMenu()
{
    auto viewMenu = m_actionManager->AddMenu(tr("&View"), s_viewMenuId);

    // NEWMENUS: NEEDS IMPLEMENTATION
    // minimize window - Ctrl+M
    // Zoom - Ctrl+Plus(+) -> Need the inverse too?

#ifdef FEATURE_ORTHOGRAPHIC_VIEW
    // Cycle Viewports
    viewMenu.AddAction(ID_VIEW_CYCLE2DVIEWPORT);
#endif

    // Center on Selection
    viewMenu.AddAction(ID_MODIFY_GOTO_SELECTION);

    // Show Quick Access Bar
    viewMenu.AddAction(ID_OPEN_QUICK_ACCESS_BAR);

    // Error Report (In legacy mode, this is in the View menu)
    if (m_enableLegacyCryEntities)
    {
        AddOpenViewPaneAction(viewMenu, LyViewPane::ErrorReport);
    }

    // Layouts
    m_layoutsMenu = viewMenu.AddMenu(tr("Layouts"));
    connect(m_viewPaneManager, &QtViewPaneManager::savedLayoutsChanged, this, [this]()
        {
            UpdateViewLayoutsMenu(m_layoutsMenu);
        });

    UpdateViewLayoutsMenu(m_layoutsMenu);

    // Viewport
    auto viewportViewsMenuWrapper = viewMenu.AddMenu(tr("Viewport"));

#ifdef FEATURE_ORTHOGRAPHIC_VIEW
    auto viewportTypesMenuWrapper = viewportViewsMenuWrapper.AddMenu(tr("Viewport Type"));

    m_viewportViewsMenu = viewportViewsMenuWrapper;
    connect(viewportTypesMenuWrapper, &QMenu::aboutToShow, this, &LevelEditorMenuHandler::UpdateOpenViewPaneMenu);

    InitializeViewPaneMenu(m_actionManager, viewportTypesMenuWrapper, [](const QtViewPane& view)
        {
            return view.IsViewportPane();
        });
#endif

    viewportViewsMenuWrapper.AddAction(ID_WIREFRAME);
    viewportViewsMenuWrapper.AddSeparator();

    if (!GetIEditor()->IsNewViewportInteractionModelEnabled())
    {
        // Ruler
        viewportViewsMenuWrapper.AddAction(ID_RULER);
    }

    viewportViewsMenuWrapper.AddAction(ID_VIEW_GRIDSETTINGS);
    viewportViewsMenuWrapper.AddSeparator();

    viewportViewsMenuWrapper.AddAction(ID_VIEW_CONFIGURELAYOUT);
    viewportViewsMenuWrapper.AddSeparator();

    viewportViewsMenuWrapper.AddAction(ID_DISPLAY_GOTOPOSITION);
    viewportViewsMenuWrapper.AddAction(ID_MODIFY_GOTO_SELECTION);

    auto gotoLocationMenu = viewportViewsMenuWrapper.AddMenu(tr("Goto Location"));
    gotoLocationMenu.AddAction(ID_GOTO_LOC1);
    gotoLocationMenu.AddAction(ID_GOTO_LOC2);
    gotoLocationMenu.AddAction(ID_GOTO_LOC3);
    gotoLocationMenu.AddAction(ID_GOTO_LOC4);
    gotoLocationMenu.AddAction(ID_GOTO_LOC5);
    gotoLocationMenu.AddAction(ID_GOTO_LOC6);
    gotoLocationMenu.AddAction(ID_GOTO_LOC7);
    gotoLocationMenu.AddAction(ID_GOTO_LOC8);
    gotoLocationMenu.AddAction(ID_GOTO_LOC9);
    gotoLocationMenu.AddAction(ID_GOTO_LOC10);
    gotoLocationMenu.AddAction(ID_GOTO_LOC11);
    gotoLocationMenu.AddAction(ID_GOTO_LOC12);

    auto rememberLocationMenu = viewportViewsMenuWrapper.AddMenu(tr("Remember Location"));
    rememberLocationMenu.AddAction(ID_TAG_LOC1);
    rememberLocationMenu.AddAction(ID_TAG_LOC2);
    rememberLocationMenu.AddAction(ID_TAG_LOC3);
    rememberLocationMenu.AddAction(ID_TAG_LOC4);
    rememberLocationMenu.AddAction(ID_TAG_LOC5);
    rememberLocationMenu.AddAction(ID_TAG_LOC6);
    rememberLocationMenu.AddAction(ID_TAG_LOC7);
    rememberLocationMenu.AddAction(ID_TAG_LOC8);
    rememberLocationMenu.AddAction(ID_TAG_LOC9);
    rememberLocationMenu.AddAction(ID_TAG_LOC10);
    rememberLocationMenu.AddAction(ID_TAG_LOC11);
    rememberLocationMenu.AddAction(ID_TAG_LOC12);

    viewportViewsMenuWrapper.AddSeparator();

    auto changeMoveSpeedMenu = viewportViewsMenuWrapper.AddMenu(tr("Change Move Speed"));
    changeMoveSpeedMenu.AddAction(ID_CHANGEMOVESPEED_INCREASE);
    changeMoveSpeedMenu.AddAction(ID_CHANGEMOVESPEED_DECREASE);
    changeMoveSpeedMenu.AddAction(ID_CHANGEMOVESPEED_CHANGESTEP);

    auto switchCameraMenu = viewportViewsMenuWrapper.AddMenu(tr("Switch Camera"));
    switchCameraMenu.AddAction(ID_SWITCHCAMERA_DEFAULTCAMERA);
    switchCameraMenu.AddAction(ID_SWITCHCAMERA_SEQUENCECAMERA);
    switchCameraMenu.AddAction(ID_SWITCHCAMERA_SELECTEDCAMERA);
    switchCameraMenu.AddAction(ID_SWITCHCAMERA_NEXT);

    // NEWMENUS:
    // MISSING AVIRECORDER

    viewportViewsMenuWrapper.AddSeparator();
    viewportViewsMenuWrapper.AddAction(ID_DISPLAY_SHOWHELPERS);

    // Refresh Style
    viewMenu.AddAction(ID_SKINS_REFRESH);

    return viewMenu;
}

QMenu* LevelEditorMenuHandler::CreateHelpMenu()
{
    // Help
    auto helpMenu = m_actionManager->AddMenu(tr("&Help"), s_helpMenuId);

    auto lineEditSearchAction = new QWidgetAction(m_mainWindow);
    auto lineEdit = new QLineEdit(m_mainWindow);
    lineEdit->setPlaceholderText(tr("Search documentation"));
    lineEditSearchAction->setDefaultWidget(lineEdit);

    auto searchButtonAction = new QWidgetAction(m_mainWindow);
    auto pushButton = new QPushButton(QIcon(":/stylesheet/img/16x16/Search.png"), "", m_mainWindow);
    searchButtonAction->setDefaultWidget(pushButton);
    lineEdit->addAction(searchButtonAction, QLineEdit::TrailingPosition);

    auto searchAction = [lineEdit]()
        {
            auto text = lineEdit->text();
            if (text.isEmpty())
            {
                QDesktopServices::openUrl(QUrl("https://aws.amazon.com/documentation/lumberyard/"));
            }
            else
            {
                static const int versionStringSize = 128;
                char productVersionString[versionStringSize];
                const SFileVersion& productVersion = gEnv->pSystem->GetProductVersion();
                productVersion.ToString(productVersionString, versionStringSize);

                QUrl docSearchUrl("https://docs.aws.amazon.com/search/doc-search.html");
                QUrlQuery docSearchQuery;
                // The order of these QueryItems matters. wiki Search URL Formatting
                docSearchQuery.addQueryItem("searchPath", "documentation-product");
                docSearchQuery.addQueryItem("searchQuery", text);
                docSearchQuery.addQueryItem("this_doc_product", "Lumberyard");
                docSearchQuery.addQueryItem("ref", "lye");
                docSearchQuery.addQueryItem("ev", productVersionString);
                docSearchUrl.setQuery(docSearchQuery);
                docSearchUrl.setFragment("facet_doc_product=Lumberyard");
                QDesktopServices::openUrl(docSearchUrl);
            }
            lineEdit->clear();
        };
    connect(lineEdit, &QLineEdit::returnPressed, this, searchAction);
    connect(pushButton, &QAbstractButton::clicked, this, searchAction);
    connect(helpMenu.Get(), &QMenu::aboutToHide, lineEdit, &QLineEdit::clear);
    connect(helpMenu.Get(), &QMenu::aboutToShow, lineEdit, &QLineEdit::clearFocus);
    helpMenu->addAction(lineEditSearchAction);

    // Getting Started
    helpMenu.AddAction(ID_DOCUMENTATION_GETTINGSTARTEDGUIDE);

    // Tutorials
    helpMenu.AddAction(ID_DOCUMENTATION_TUTORIALS);

    // Documentation
    auto documentationMenu = helpMenu.AddMenu(tr("Documentation"));

    // Glossary
    documentationMenu.AddAction(ID_DOCUMENTATION_GLOSSARY);

    // Lumberyard Documentation
    documentationMenu.AddAction(ID_DOCUMENTATION_LUMBERYARD);

    // GameLift Documentation
    documentationMenu.AddAction(ID_DOCUMENTATION_GAMELIFT);

    // Release Notes
    documentationMenu.AddAction(ID_DOCUMENTATION_RELEASENOTES);

    // GameDev Resources
    auto gameDevResourceMenu = helpMenu.AddMenu(tr("GameDev Resources"));

    // Game Dev Blog
    gameDevResourceMenu.AddAction(ID_DOCUMENTATION_GAMEDEVBLOG);

    // GameDev Twitch Channel
    gameDevResourceMenu.AddAction(ID_DOCUMENTATION_TWITCHCHANNEL);

    // Forums
    gameDevResourceMenu.AddAction(ID_DOCUMENTATION_FORUMS);

    // AWS Support
    gameDevResourceMenu.AddAction(ID_DOCUMENTATION_AWSSUPPORT);

    helpMenu.AddSeparator();

    // Give Us Feedback
    helpMenu.AddAction(ID_DOCUMENTATION_FEEDBACK);

    // Report a Bug???
    // auto reportBugMenu = helpMenu.Get()->addAction(tr("Report a Bug"));

    // About Lumberyard
    helpMenu.AddAction(ID_APP_ABOUT);

    LoadNetPromoterScoreDialog(helpMenu);

    return helpMenu;
}

QAction* LevelEditorMenuHandler::CreateViewPaneAction(const QtViewPane* view)
{
    QAction* action = m_actionManager->HasAction(view->m_id) ? m_actionManager->GetAction(view->m_id) : nullptr;

    if (!action)
    {
        const QString menuText = view->m_options.optionalMenuText.length() > 0
            ? view->m_options.optionalMenuText
            : view->m_name;

        action = new QAction(menuText, this);
        action->setObjectName(view->m_name);
        action->setCheckable(view->IsViewportPane());
        m_actionManager->AddAction(view->m_id, action);

        if (!view->m_options.shortcut.isEmpty())
        {
            action->setShortcut(view->m_options.shortcut);
        }

        QObject::connect(
            action, &QAction::triggered, this, &LevelEditorMenuHandler::checkOrOpenView, Qt::UniqueConnection);

        if (view->m_options.sendViewPaneNameBackToAmazonAnalyticsServers)
        {
            AzToolsFramework::EditorMetricsEventsBus::Broadcast(
                &AzToolsFramework::EditorMetricsEventsBus::Events::RegisterAction,
                action, QString("ViewPaneMenu %1").arg(view->m_name));
        }
    }

    return action;
}

// Function used to show menu options without its icon and be able to toggle shortcut visibility in the new menu layout
// This is a work around for the fact that setting the shortcut on the original action isn't working for some reasons
// and we need to investigate it further in the future.
QAction* LevelEditorMenuHandler::CreateViewPaneMenuItem(
    ActionManager* actionManager, ActionManager::MenuWrapper& menu, const QtViewPane* view)
{
    QAction* action = CreateViewPaneAction(view);

    menu->addAction(action);

    return action;
}

void LevelEditorMenuHandler::InitializeViewPaneMenu(
    ActionManager* actionManager, ActionManager::MenuWrapper& menu,
    AZStd::function <bool(const QtViewPane& view)> functor)
{
    QtViewPanes views = QtViewPaneManager::instance()->GetRegisteredPanes();

    for (auto it = views.cbegin(), end = views.cend(); it != end; ++it)
    {
        const QtViewPane& view = *it;
        if (!functor(view))
        {
            continue;
        }

        CreateViewPaneMenuItem(actionManager, menu, it);
    }
}

void LevelEditorMenuHandler::LoadComponentLayout()
{
    m_viewPaneManager->RestoreDefaultLayout();
}

void LevelEditorMenuHandler::LoadLegacyLayout()
{
    m_viewPaneManager->RestoreLegacyLayout();
}

void LevelEditorMenuHandler::LoadNetPromoterScoreDialog(ActionManager::MenuWrapper& menu)
{
    m_settings.beginGroup(s_netPromoterScore);

    if (!m_settings.value(s_shortTimeInterval).isNull())
    {
        auto showNetPromoterDialog = menu.Get()->addAction(tr("Show Net Promoter Score Dialog"));
        connect(showNetPromoterDialog, &QAction::triggered, this, [this]()
            {
                NetPromoterScoreDialog p(m_mainWindow);
                p.exec();
            });
    }
    m_settings.endGroup();
}

QMap<QString, QList<QtViewPane*>> LevelEditorMenuHandler::CreateMenuMap(
    QMap<QString, QList<QtViewPane*> >& menuMap, QtViewPanes& allRegisteredViewPanes)
{
    // set up view panes to each category
    for (QtViewPane& viewpane : allRegisteredViewPanes)
    {
        // only store the view panes that should be shown in the menu
        if (!viewpane.IsViewportPane())
        {
            menuMap[viewpane.m_category].push_back(&viewpane);
        }
    }

    return menuMap;
}

// sort the menu option name in alphabetically order
struct CaseInsensitiveStringCompare
{
    bool operator() (const QString& left, const QString& right) const
    {
        return left.toLower() < right.toLower();
    }
};

static void LaunchLuaEditor()
{
    AzToolsFramework::EditorRequestBus::Broadcast(
        &AzToolsFramework::EditorRequests::LaunchLuaEditor, nullptr);
}

void LevelEditorMenuHandler::CreateMenuOptions(
    QMap<QString, QList<QtViewPane*> >* menuMap, ActionManager::MenuWrapper& menu, const char* category)
{
    // list in the menu and remove this menu category from the menuMap
    QList<QtViewPane*> menuList = menuMap->take(category);

    std::map<QString, std::function<void()>, CaseInsensitiveStringCompare> sortMenuMap;

    // store menu options into the map
    // name as a key, functionality as a value
    for (QtViewPane* viewpane : menuList)
    {
        if (viewpane->m_options.builtInActionId != LyViewPane::NO_BUILTIN_ACTION)
        {
            sortMenuMap[viewpane->m_name] = [&menu, viewpane]()
            {
                // Handle shortcuts for actions with a built-in ID since they
                // bypass our CreateViewPaneMenuItem method
                QAction* action = menu.AddAction(viewpane->m_options.builtInActionId);
                if (action && !viewpane->m_options.shortcut.isEmpty())
                {
                    action->setShortcut(viewpane->m_options.shortcut);
                }
            };
        }
        else
        {
            const QString menuText = viewpane->m_options.optionalMenuText.length() > 0
                ? viewpane->m_options.optionalMenuText
                : viewpane->m_name;

            sortMenuMap[menuText] = [this, viewpane, &menu]()
            {
                CreateViewPaneMenuItem(m_actionManager, menu, viewpane);
            };
        }
    }

    if (category == LyViewPane::CategoryTools)
    {
        // Add LUA Editor into the Tools map
        sortMenuMap[s_LUAEditorName] = [this, &menu]()
        {
            auto luaEditormenu = menu.AddAction(ID_TOOLS_LUA_EDITOR);
            connect(luaEditormenu, &QAction::triggered, this, &LaunchLuaEditor, Qt::UniqueConnection);
        };
    }

    // add each menu option into the menu
    std::map<QString, std::function<void()>, CaseInsensitiveStringCompare>::iterator iter;
    for (iter = sortMenuMap.begin(); iter != sortMenuMap.end(); ++iter)
    {
        iter->second();
    }
}

void LevelEditorMenuHandler::CreateDebuggingSubMenu(ActionManager::MenuWrapper gameMenu)
{
    // DebuggingSubMenu
    auto debuggingSubMenu = gameMenu.AddMenu(QObject::tr("Debugging"));

    if (m_enableLegacyCryEntities)
    {
        // Reload Script
        auto reloadScriptsMenu = debuggingSubMenu.AddMenu(tr("Reload Scripts (LEGACY)"));
        reloadScriptsMenu.AddAction(ID_RELOAD_ALL_SCRIPTS);
        reloadScriptsMenu.AddSeparator();
        reloadScriptsMenu.AddAction(ID_RELOAD_ACTOR_SCRIPTS);
        reloadScriptsMenu.AddAction(ID_RELOAD_AI_SCRIPTS);
        reloadScriptsMenu.AddAction(ID_RELOAD_ENTITY_SCRIPTS);
        reloadScriptsMenu.AddAction(ID_RELOAD_ITEM_SCRIPTS);
        reloadScriptsMenu.AddAction(ID_RELOAD_UI_SCRIPTS);

        // Reload Textures/Shaders
        debuggingSubMenu.AddAction(ID_RELOAD_TEXTURES);

        // Reload Geometry
        debuggingSubMenu.AddAction(ID_RELOAD_GEOMETRY);

        // Reload Terrain
        debuggingSubMenu.AddAction(ID_RELOAD_TERRAIN);

        // Resolve Missing Objects/Materials
        debuggingSubMenu.AddAction(ID_TOOLS_RESOLVEMISSINGOBJECTS);

        // Enable File Change Monitoring
        debuggingSubMenu.AddAction(ID_TOOLS_ENABLEFILECHANGEMONITORING);

        // Check Object Positions
        debuggingSubMenu.AddAction(ID_TOOLS_VALIDATEOBJECTPOSITIONS);

        // Clear Registry Data
        debuggingSubMenu.AddAction(ID_CLEAR_REGISTRY);

        // Check Level for Errors
        debuggingSubMenu.AddAction(ID_VALIDATELEVEL);

        // Save Level Statistics
        debuggingSubMenu.AddAction(ID_TOOLS_LOGMEMORYUSAGE);

        // Compile Scripts
        debuggingSubMenu.AddAction(ID_SCRIPT_COMPILESCRIPT);

        // Reduce Working Set
        debuggingSubMenu.AddAction(ID_RESOURCES_REDUCEWORKINGSET);

        // Update Procedural Vegetation
        debuggingSubMenu.AddAction(ID_TOOLS_UPDATEPROCEDURALVEGETATION);
    }
    else
    {
        // AI Debugger
        AddOpenViewPaneAction(debuggingSubMenu, LyViewPane::AIDebugger);

        // Error Report
        AddOpenViewPaneAction(debuggingSubMenu, LyViewPane::ErrorReport);

        debuggingSubMenu.AddSeparator();
    }

    // Configure Toolbox Macros
    debuggingSubMenu.AddAction(ID_TOOLS_CONFIGURETOOLS);

    // Toolbox Macros
    m_macrosMenu = debuggingSubMenu.AddMenu(tr("ToolBox Macros"));
    connect(m_macrosMenu, &QMenu::aboutToShow, this, &LevelEditorMenuHandler::UpdateMacrosMenu, Qt::UniqueConnection);

    if (m_enableLegacyCryEntities)
    {
        // Script Help
        debuggingSubMenu.AddAction(ID_TOOLS_SCRIPTHELP);
    }

    UpdateMacrosMenu();
}

void LevelEditorMenuHandler::UpdateMRUFiles()
{
    auto cryEdit = CCryEditApp::instance();
    RecentFileList* mruList = cryEdit->GetRecentFileList();
    const int numMru = mruList->GetSize();

    if (!m_mostRecentLevelsMenu)
    {
        return;
    }

    // Remove most recent items
    m_mostRecentLevelsMenu->clear();

    // Insert mrus
    QString sCurDir = QString(Path::GetEditingGameDataFolder().c_str()) + QDir::separator();

    QFileInfo gameDir(sCurDir); // Pass it through QFileInfo so it comes out normalized
    const QString gameDirPath = gameDir.absolutePath();

    for (int i = 0; i < numMru; ++i)
    {
        if (!MRUEntryIsValid((*mruList)[i], gameDirPath))
        {
            continue;
        }

        QString displayName;
        mruList->GetDisplayName(displayName, i, sCurDir);

        QString entry = QString("%1 %2").arg(i + 1).arg(displayName);
        QAction* action = m_actionManager->GetAction(ID_FILE_MRU_FILE1 + i);
        action->setText(entry);

        m_actionManager->RegisterActionHandler(ID_FILE_MRU_FILE1 + i, [i]()
            {
                auto cryEdit = CCryEditApp::instance();
                RecentFileList* mruList = cryEdit->GetRecentFileList();
                cryEdit->OpenDocumentFile((*mruList)[i].toUtf8().data());
            });
        m_actionManager->RegisterUpdateCallback(ID_FILE_MRU_FILE1 + i, cryEdit, &CCryEditApp::OnUpdateFileOpen);

        GetIEditor()->RegisterNotifyListener(new EditorListener(action, [action](EEditorNotifyEvent e)
            {
                DisableActionWhileLevelChanges(action, e);
            }));

        m_mostRecentLevelsMenu->addAction(action);
    }

    // Used when disabling the "Open Recent" menu options
    OnUpdateOpenRecent();

    m_mostRecentLevelsMenu->addSeparator();

    // Clear All
    auto clearAllMenu = m_mostRecentLevelsMenu->addAction(tr("Clear All"));
    connect(clearAllMenu, &QAction::triggered, this, &LevelEditorMenuHandler::ClearAll);
}

void LevelEditorMenuHandler::ActivateGemConfiguration()
{
    CCryEditApp::instance()->OnOpenProjectConfiguratorGems();
}

void LevelEditorMenuHandler::ClearAll()
{
    RecentFileList* mruList = CCryEditApp::instance()->GetRecentFileList();

    // remove everything from the mru list
    for (int i = mruList->GetSize(); i > 0; i--)
    {
        mruList->Remove(i - 1);
    }

    // save the settings immediately to the registry
    mruList->WriteList();

    // re-update the menus
    UpdateMRUFiles();
}

void LevelEditorMenuHandler::ToggleSelection(bool hide)
{
    CCryEditApp::instance()->OnToggleSelection(hide);
}

// Used for showing last hidden objects
void LevelEditorMenuHandler::ShowLastHidden()
{
    CSelectionGroup* sel = GetIEditor()->GetSelection();
    if (!sel->IsEmpty())
    {
        CUndo undo("Show Last Hidden");
        GetIEditor()->GetObjectManager()->ShowLastHiddenObject();
    }
}

// Used for disabling "Open Recent" menu option
void LevelEditorMenuHandler::OnUpdateOpenRecent()
{
    RecentFileList* mruList = CCryEditApp::instance()->GetRecentFileList();
    const int numMru = mruList->GetSize();
    QString currentMru = numMru > 0 ? (*mruList)[0] : QString();

    if (!currentMru.isEmpty())
    {
        m_mostRecentLevelsMenu->setEnabled(true);
    }
    else
    {
        m_mostRecentLevelsMenu->setEnabled(false);
    }
}

void LevelEditorMenuHandler::OnOpenAssetEditor()
{
    AssetEditor::AssetEditorRequestsBus::Broadcast(
        &AssetEditor::AssetEditorRequests::OpenAssetEditor, Data::Asset<Data::AssetData>());
}

void LevelEditorMenuHandler::OnUpdateMacrosMenu()
{
    auto tools = GetIEditor()->GetToolBoxManager();
    const int macroCount = tools->GetMacroCount(true);

    if (macroCount <= 0)
    {
        m_macrosMenu->setEnabled(false);
    }
    else
    {
        m_macrosMenu->setEnabled(true);
    }
}

void LevelEditorMenuHandler::SetupSliceSelectMenu(AZ::EntityId id)
{
    if (id.IsValid())
    {
        if (SliceUtilities::PopulateSliceSelectSubMenu(id, m_selectSliceRootMenu))
        {
            m_selectSliceRootMenu->setEnabled(true);
        }
        else
        {
            m_selectSliceRootMenu->setEnabled(false);
        }
    }
    else
    {
        m_selectSliceRootMenu->clear();
        m_selectSliceRootMenu->setEnabled(false);
    }
}

void LevelEditorMenuHandler::UpdateMacrosMenu()
{
    m_macrosMenu->clear();

    auto tools = GetIEditor()->GetToolBoxManager();
    const int macroCount = tools->GetMacroCount(true);

    for (int i = 0; i < macroCount; ++i)
    {
        auto macro = tools->GetMacro(i, true);
        const int toolbarId = macro->GetToolbarId();
        if (toolbarId == -1 || toolbarId == ID_TOOLS_TOOL1)
        {
            m_macrosMenu->addAction(macro->action());
        }
    }
}

void LevelEditorMenuHandler::UpdateOpenViewPaneMenu()
{
    // This function goes through all the viewport menu actions (top, left, perspective...)
    // and adds a check mark on the viewport that has focus

    QtViewport* viewport = m_mainWindow->GetActiveViewport();
    QString activeViewportName = viewport ? viewport->GetName() : QString();

    for (QAction* action : m_viewportViewsMenu->actions())
    {
        action->setChecked(action->objectName() == activeViewportName);
    }
}

void LevelEditorMenuHandler::AWSMenuClicked()
{
    auto metricId = LyMetrics_CreateEvent("AWSMenuClickedEvent");
    LyMetrics_SubmitEvent(metricId);
}

void LevelEditorMenuHandler::checkOrOpenView()
{
    QAction* action = qobject_cast<QAction*>(sender());
    const QString viewPaneName = action->objectName();
    // If this action is checkable and was just unchecked, then we
    // should close the view pane
    if (action->isCheckable() && !action->isChecked())
    {
        QtViewPaneManager::instance()->ClosePane(viewPaneName);
    }
    // Otherwise, this action should open the view pane
    else
    {
        const QtViewPane* pane = QtViewPaneManager::instance()->OpenPane(viewPaneName);

        QObject::connect(pane->Widget(), &QWidget::destroyed, action, [action]
        {
            action->setChecked(false);
        });
    }
}

void LevelEditorMenuHandler::EnteredComponentMode(const AZStd::vector<AZ::Uuid>& /*componentModeTypes*/)
{
    auto menuWrapper = m_actionManager->FindMenu(s_editMenuId);
    if (!menuWrapper.isNull())
    {
        // copy of menu actions
        auto actions = menuWrapper.Get()->actions();
        // remove all non-reserved edit menu options
        actions.erase(
            std::remove_if(actions.begin(), actions.end(), [](QAction* action)
            {
                return !action->property("Reserved").toBool();
            }),
            actions.end());

        // clear and update the menu with new actions
        menuWrapper.Get()->clear();
        menuWrapper.Get()->addActions(actions);
    }
}

void LevelEditorMenuHandler::LeftComponentMode(const AZStd::vector<AZ::Uuid>& /*componentModeTypes*/)
{
    RestoreEditMenuToDefault();
}

void LevelEditorMenuHandler::AddEditMenuAction(QAction* action)
{
    auto menuWrapper = m_actionManager->FindMenu(s_editMenuId);
    if (!menuWrapper.isNull())
    {
        menuWrapper.Get()->addAction(action);
    }
}

void LevelEditorMenuHandler::RestoreEditMenuToDefault()
{
    auto menuWrapper = m_actionManager->FindMenu(s_editMenuId);
    if (!menuWrapper.isNull())
    {
        menuWrapper.Get()->clear();
        PopulateEditMenu(menuWrapper);
    }
}

#include <Core/LevelEditorMenuHandler.moc>
