# ─── UI Library (Qt 6) ────────────────────────────────────────────────────────

set(CMAKE_AUTOMOC ON)

add_library(roundtable_ui STATIC
    App.cpp
    MainWindow.cpp
    MainWindowUI.cpp
    MainWindowMenus.cpp
    MainWindowProject.cpp
    MainWindowRecovery.cpp
    ShortcutManager.cpp
    SplashScreen.cpp
    Theme.cpp
    ThemeStylesheet.cpp
    ThemePresets.cpp
    UiScale.cpp
    UpdateChecker.cpp
    QtHelpers.cpp

    # Panels
    panels/timeline/TrackHeader.cpp
    panels/timeline/DockBehavior.cpp
    panels/timeline/PlayheadLineWidget.cpp
    panels/timeline/TimelinePanel.cpp
    panels/timeline/TimelinePanelMenus.cpp
    panels/timeline/TimelinePanelMedia.cpp
    panels/timeline/TimelinePanelMouse.cpp
    panels/timeline/TimelinePanelMouseDrag.cpp
    panels/timeline/TimelinePanelDragDrop.cpp
    panels/timeline/TimelinePanelLayout.cpp
    panels/timeline/TimelinePanelTracks.cpp
    panels/timeline/TimelinePanelZoomScroll.cpp
    panels/timeline/TimelinePanelPaintEvents.cpp
    panels/timeline/TimelinePanelHitTest.cpp
    panels/timeline/TimelinePanelCommands.cpp
    panels/monitors/SourceMonitor.cpp
    panels/monitors/SourceMonitorUI.cpp
    panels/monitors/ProgramMonitor.cpp
    panels/monitors/ProgramMonitorUI.cpp
    panels/project/ProjectBin.cpp
    panels/project/ProjectBinConfig.cpp
    panels/project/ProjectBinUI.cpp
    panels/project/ProjectBinSync.cpp
    panels/project/ProjectBinDragDrop.cpp
    panels/project/ProjectBinImport.cpp
    panels/project/ProjectBinQueries.cpp
    panels/project/ProjectBinEvents.cpp
    panels/project/ProjectBinSlots.cpp
    panels/project/ProjectBinSequence.cpp
    panels/properties/PropertiesPanel.cpp
    panels/properties/PropertiesPanelUI.cpp
    panels/properties/PropertiesPanelApply.cpp
    panels/properties/PropertiesPanelSpine.cpp
    panels/properties/PropertiesPanelTransitions.cpp
    panels/properties/PropertiesPanelVideo.cpp
    panels/properties/PropertiesPanelAudio.cpp
    panels/properties/PropertiesPanelTitle.cpp
    panels/properties/PropertiesPanelGraphic.cpp
    panels/effects/EffectControlsPanel.cpp
    panels/effects/EffectControlsPanelUI.cpp
    panels/effects/EffectControlsPanelTree.cpp
    panels/effects/EffectControlsPanelEffects.cpp
    panels/effects/GraphicsEditorPanel.cpp
    panels/effects/GraphicsEditorControls.cpp
    panels/effects/GraphicsEditorProperties.cpp
    panels/effects/ColorGradingPanel.cpp
    panels/effects/EffectsPanel.cpp
    panels/audio/AudioMixer.cpp
    panels/audio/AudioMixerUI.cpp
    panels/characters/ShotComposer.cpp
    panels/characters/ShotComposerVideo.cpp
    panels/characters/ShotComposerUI.cpp
    panels/characters/ShotComposerProperties.cpp
    panels/characters/ShotComposerThumbnails.cpp
    panels/characters/ShotComposerThumbnailGen.cpp
    panels/characters/ShotComposerLibrary.cpp
    panels/audio/AudioSync.cpp
    panels/audio/AudioSyncUI.cpp
    panels/audio/AudioSyncCards.cpp
    panels/audio/AudioSyncData.cpp
    panels/audio/AudioSyncExport.cpp
    panels/audio/AudioSyncHandlers.cpp
    panels/audio/AudioSyncPersistence.cpp
    panels/audio/AudioSyncState.cpp
    panels/audio/AudioSyncFileList.cpp
    panels/audio/AudioSyncPlayback.cpp
    panels/audio/AudioSyncPopulateCards.cpp
    panels/audio/AudioSyncSession.cpp
    panels/audio/AudioSyncResegmentAndManual.cpp
    panels/audio/AudioSyncUIScriptPage.cpp
    panels/export/ExportPanel.cpp
    panels/export/ExportPanelUI.cpp
    panels/export/ExportMiniTimeline.cpp
    panels/effects/KeyframeEditor.cpp
    panels/effects/KeyframeEditorRender.cpp
    panels/project/HistoryPanel.cpp
    panels/monitors/ScopesPanel.cpp
    panels/characters/CharacterBrowser.cpp
    panels/characters/CharacterBrowserUI.cpp
    panels/characters/CharacterBrowserNetwork.cpp
    panels/characters/CharacterThumbnailCache.cpp
    panels/characters/CharacterShotPanel.cpp
    panels/characters/CharactersPanel.cpp
    panels/library/LibraryPanel.cpp
    panels/backgrounds/BackgroundDownloadPanel.cpp
    panels/backgrounds/BackgroundDownloadUI.cpp
    panels/project/ConversionPanel.cpp
    panels/project/ProjectPanel.cpp
    panels/project/ProjectPanelUI.cpp
    panels/timeline/TimelineWorkspace.cpp
    panels/timeline/TimelineWorkspacePanels.cpp
    panels/timeline/TimelineWorkspacePanelsWiring.cpp
    panels/timeline/TimelineWorkspacePanelsWiringClipSelection.cpp
    panels/timeline/TimelineWorkspaceComposite.cpp
    panels/timeline/TimelineWorkspaceOverlay.cpp
    panels/timeline/TimelineWorkspaceSpine.cpp
    panels/timeline/TimelineWorkspaceAudio.cpp
    panels/timeline/TimelineWorkspaceKeys.cpp
    panels/timeline/DockLayoutManager.cpp
    panels/timeline/ClipRenderers.cpp

    # Widgets
    widgets/TimelineTrackWidget.cpp
    widgets/TimelineClipWidget.cpp
    widgets/TimelineRuler.cpp
    widgets/NLEScrollBar.cpp
    widgets/TransportBar.cpp
    widgets/WaveformWidget.cpp
    widgets/MiniWaveformWidget.cpp
    widgets/VUMeter.cpp
    widgets/ColorWheelWidget.cpp
    widgets/CurveEditor.cpp
    widgets/ScrubbySpinBox.cpp
    widgets/ThumbnailGrid.cpp
    widgets/MiniTimeline.cpp
    widgets/SpinePreviewWidget.cpp
    widgets/SpinePreviewRendering.cpp
    widgets/SpinePreviewOverlay.cpp
    widgets/FullWaveformWidget.cpp
    widgets/ManualMatchDialog.cpp
    widgets/DockTitleBar.cpp
    widgets/MediaDragTreeWidget.cpp

    # Dialogs
    dialogs/ProjectSettingsDialog.cpp
    dialogs/SequenceDialog.cpp
    dialogs/KeyboardShortcutsDialog.cpp
    dialogs/AppPreferencesDialog.cpp
    dialogs/RelinkMediaDialog.cpp
    dialogs/InterpretFootageDialog.cpp

    # Viewport
    viewport/Viewport.cpp
    viewport/VulkanViewport.cpp
    viewport/VulkanViewportSetup.cpp
    viewport/OverlayMath.cpp
    viewport/TransformOverlayWidget.cpp
    viewport/TransformOverlayDraw.cpp
    viewport/TransformOverlayMask.cpp
    viewport/TransformOverlayInput.cpp
)

# Newly-extracted TUs — skip unity batch to avoid MSVC heap exhaustion
set_source_files_properties(
    panels/project/ProjectBinConfig.cpp
    panels/project/ProjectBinImport.cpp
    panels/project/ProjectBinQueries.cpp
    panels/project/ProjectBinEvents.cpp
    panels/project/ProjectBinSlots.cpp
    panels/project/ProjectBinSequence.cpp
    panels/timeline/TimelinePanelLayout.cpp
    panels/timeline/TimelinePanelTracks.cpp
    panels/timeline/TimelinePanelZoomScroll.cpp
    panels/timeline/TimelinePanelPaintEvents.cpp
    panels/timeline/TimelinePanelHitTest.cpp
    panels/timeline/TimelinePanelCommands.cpp
    panels/properties/PropertiesPanelVideo.cpp
    panels/properties/PropertiesPanelAudio.cpp
    panels/properties/PropertiesPanelTitle.cpp
    panels/properties/PropertiesPanelGraphic.cpp
    PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON
)

target_include_directories(roundtable_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# PCH — enabled May 3, 2026.
target_precompile_headers(roundtable_ui PRIVATE pch_ui.h)

target_link_libraries(roundtable_ui PUBLIC
    roundtable_core
    roundtable_gpu
    roundtable_export
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    spdlog::spdlog
)
