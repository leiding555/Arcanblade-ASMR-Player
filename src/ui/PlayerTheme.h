#pragma once

#include "ui/InputControlPolicy.h"

#include <QString>

namespace asmr {

// Scoped to MainWindow and its owned controls, never QApplication or the
// independent floating subtitle window. Dimensions are logical pixels.
inline QString playerStyleSheet()
{
    initializeInputControlResources();
    // Keep each literal below MSVC's single-string size limit.
    return QStringLiteral(R"QSS(
QWidget { color: #F0F2F5; font-size: 13px; }
QWidget#asmrMainWindow, QWidget#contentWorkspace { background: #14171B; }
QWidget#librarySidebar { background: #14171B; }
QWidget#libraryTracksPane, QWidget#subtitlePanel { background: #1F232A; border-radius: 12px; }
QWidget#libraryWelcomePage, QWidget#libraryWelcomeContent, QWidget#libraryBreadcrumbBar { background: transparent; }
QWidget#playbackControls { background: #1C2127; border: 1px solid #242A32; border-radius: 12px; }
QLabel { background: transparent; border: none; }
QLabel#applicationTitle { font-size: 22px; font-weight: 600; color: #F0F2F5; }
QLabel[uiRole="heading"], QLabel[sectionTitle="true"] { font-size: 16px; font-weight: 600; }
QLabel#libraryBrowseTitleLabel { font-size: 28px; font-weight: 600; }
QLabel[uiRole="muted"], QLabel[subtleLabel="true"], QLabel[secondaryText="true"], QLabel#timeLabel,
QLabel#playbackVolumeLabel { color: #ADB5C1; }
QLabel[errorLabel="true"] { color: #F3B4A9; }
QLabel#libraryWelcomeTitle { font-size: 23px; font-weight: 600; }
QLabel#audioNameLabel { font-size: 14px; font-weight: 600; }
QPushButton, QToolButton { background: #2A3039; border: 1px solid #39414D;
    border-radius: 7px; padding: 6px 12px; min-height: 22px; outline: none; }
QPushButton:hover, QToolButton:hover { background: #343D47; border-color: #687A80; }
QPushButton:pressed, QToolButton:pressed { background: #3B4A4C; }
QPushButton:checked, QToolButton:checked { background: #224D40; border-color: #45B38C; color: #E8FFF4; }
QPushButton[buttonKeyboardFocus="true"]:focus:enabled,
QToolButton[buttonKeyboardFocus="true"]:focus:enabled { border-color: #69C8A5; }
QPushButton[primary="true"], QPushButton[primaryAction="true"] {
    background: #39A17D; color: #F4FFF9; border-color: #39A17D; font-weight: 600; }
QPushButton[primary="true"]:hover, QPushButton[primaryAction="true"]:hover { background: #45B38C; }
QPushButton[primary="true"]:pressed, QPushButton[primaryAction="true"]:pressed { background: #2D896A; }
QPushButton:disabled, QToolButton:disabled { color: #69717D; background: #20262D; border-color: #2C333D; }
QPushButton[primary="true"]:disabled, QPushButton[primaryAction="true"]:disabled {
    color: #788B84; background: #294038; border-color: #344D42; }
QToolButton#libraryCoverMenuButton {
    background: transparent; border: 1px solid transparent; border-radius: 5px;
    padding: 4px 6px; color: #F0F2F5; font-size: 14px; }
QToolButton#libraryCoverMenuButton:hover:enabled { background: #2A3039; border-color: transparent; }
QToolButton#libraryCoverMenuButton:pressed:enabled { background: #343D47; border-color: transparent; }
QToolButton#libraryCoverMenuButton[buttonKeyboardFocus="true"]:focus:enabled { border-color: #69C8A5; }
QToolButton#libraryCoverMenuButton:disabled {
    background: transparent; border-color: transparent; color: #69717D; }
QToolButton#libraryCoverMenuButton::menu-indicator {
    image: none; width: 0px; height: 0px; }
QWidget#librarySidebar QPushButton#libraryImportMenuButton,
QWidget#librarySidebar QToolButton#libraryImportMenuButton,
QWidget#librarySidebar QPushButton#libraryAiSubtitlesButton,
QWidget#librarySidebar QPushButton#librarySettingsButton,
QWidget#librarySidebar QToolButton#librarySettingsButton {
    background: transparent; border: 1px solid transparent; text-align: left; padding: 9px 12px; }
QWidget#librarySidebar QPushButton#libraryImportMenuButton::menu-indicator,
QWidget#librarySidebar QPushButton#librarySettingsButton::menu-indicator {
    image: none; width: 0px; height: 0px; }
QWidget#librarySidebar QPushButton#libraryImportMenuButton:hover,
QWidget#librarySidebar QToolButton#libraryImportMenuButton:hover,
QWidget#librarySidebar QPushButton#libraryAiSubtitlesButton:hover,
QWidget#librarySidebar QPushButton#librarySettingsButton:hover,
QWidget#librarySidebar QToolButton#librarySettingsButton:hover { background: #252C33; }
QWidget#librarySidebar QPushButton#libraryImportMenuButton[buttonKeyboardFocus="true"]:focus:enabled,
QWidget#librarySidebar QToolButton#libraryImportMenuButton[buttonKeyboardFocus="true"]:focus:enabled,
QWidget#librarySidebar QPushButton#libraryAiSubtitlesButton[buttonKeyboardFocus="true"]:focus:enabled,
QWidget#librarySidebar QPushButton#librarySettingsButton[buttonKeyboardFocus="true"]:focus:enabled,
QWidget#librarySidebar QToolButton#librarySettingsButton[buttonKeyboardFocus="true"]:focus:enabled { border-color: #69C8A5; }
QWidget#librarySidebar QPushButton#libraryRefreshScanRootsButton,
QWidget#librarySidebar QToolButton#libraryRefreshScanRootsButton {
    background: transparent; border-color: transparent; padding: 3px 5px; }
QWidget#librarySidebar QPushButton#libraryRefreshScanRootsButton:hover,
)QSS")
        + QStringLiteral(R"QSS(QWidget#librarySidebar QToolButton#libraryRefreshScanRootsButton:hover { background: #29333A; }
QWidget#librarySidebar QPushButton#libraryRefreshScanRootsButton[buttonKeyboardFocus="true"]:focus:enabled,
QWidget#librarySidebar QToolButton#libraryRefreshScanRootsButton[buttonKeyboardFocus="true"]:focus:enabled { border-color: #69C8A5; }
QWidget#playbackControls QPushButton#playPauseButton {
    min-width: 46px; max-width: 46px; min-height: 46px; max-height: 46px;
    padding: 0; border-radius: 24px; }
QWidget#playbackControls QPushButton[playbackTransportAction="true"],
QWidget#playbackControls QPushButton[playbackAuxiliary="true"] {
    min-width: 30px; max-width: 30px; min-height: 30px; max-height: 30px;
    padding: 0; background: transparent; border-color: transparent; }
QWidget#playbackControls QPushButton[playbackTransportAction="true"]:hover,
QWidget#playbackControls QPushButton[playbackAuxiliary="true"]:hover { background: #303A42; }
QWidget#playbackControls QPushButton[playbackAuxiliary="true"]:checked {
    background: #224D40; border-color: #3C8266; }
QWidget#playbackControls QPushButton[playbackQueueAction="true"] {
    min-width: 50px; max-width: 50px; min-height: 30px; max-height: 30px;
    padding: 0; background: transparent; border-color: transparent; font-size: 11px; }
QWidget#playbackControls QPushButton[playbackQueueAction="true"]:hover { background: #303A42; }
QWidget#playbackControls QPushButton[playbackQueueAction="true"]:checked {
    background: #224D40; border-color: #3C8266; }
QFrame#playbackQueuePanel { background: #1F232A; border: 1px solid #46515D; border-radius: 10px; }
QLabel#playbackQueueTitle { font-size: 16px; font-weight: 600; }
QWidget#playbackControls QPushButton[buttonKeyboardFocus="true"]:focus:enabled { border-color: #69C8A5; }
QMenu#playbackModeMenu, QMenu#playbackVolumeMenu {
    background: transparent; border: none; border-radius: 0; padding: 4px; }
QMenu#playbackModeMenu { font-size: 14px; }
QMenu#playbackModeMenu::item { padding: 10px 8px; min-width: 136px; }
QMenu#playbackModeMenu::separator { height: 1px; background: #414141; margin: 4px 4px; }
QWidget#playbackVolumeContent { background: #282828; border: none; }
QLabel#playbackVolumePercentLabel { color: #FFFFFF; font-size: 16px; min-width: 42px; }
QFrame#playbackVolumeSeparator { color: #414141; background: #414141; border: none; max-height: 1px; }
QPushButton[playbackPopupAction="true"] { padding: 0; min-height: 30px;
    background: transparent; border-color: transparent; }
QPushButton[playbackPopupAction="true"]:hover { background: #363636; }
QPushButton[playbackPopupAction="true"]:pressed { background: #414141; }
QPushButton[playbackPopupAction="true"][buttonKeyboardFocus="true"]:focus:enabled { border-color: #69C8A5; }
QMenu#playbackVolumeMenu QSlider::groove:vertical {
    width: 3px; background: #929292; border-radius: 1px; }
QMenu#playbackVolumeMenu QSlider::add-page:vertical { background: #1ED760; border-radius: 1px; }
QMenu#playbackVolumeMenu QSlider::sub-page:vertical { background: #929292; border-radius: 1px; }
QMenu#playbackVolumeMenu QSlider::handle:vertical { background: #1ED760; border: none;
    height: 13px; margin: 0 -5px; border-radius: 6px; }
QMenu#playbackVolumeMenu QSlider::groove:vertical:focus { background: #B0B0B0; }
QWidget#playbackControls QSlider::groove:horizontal { height: 3px; }
QWidget#playbackControls QSlider::handle:horizontal { width: 8px; margin: -4px 0; border-radius: 4px; }
QListView, QTableView, QTreeView { background: transparent; alternate-background-color: #22272F;
    border: none; outline: none; selection-background-color: #244B40; selection-color: #F2FFF8;
    gridline-color: #303640; }
QListView::item { padding: 10px 8px; margin: 2px 0; border-radius: 8px; }
QTreeView::item { min-height: 28px; padding: 8px 6px; border: none; }
QTableView::item { padding: 8px 6px; border: none; }
QListView::item:hover, QTableView::item:hover, QTreeView::item:hover { background: #2C363B; }
QListView::item:selected, QTableView::item:selected, QTreeView::item:selected { background: #244B40; color: #F2FFF8; }
QListView::item:disabled, QTableView::item:disabled, QTreeView::item:disabled { color: #69717D; }
QHeaderView, QHeaderView::section { background: #272D36; color: #ADB5C1; border: none; }
QHeaderView::section { padding: 10px 6px; border-bottom: 1px solid #343B45; }
QTableCornerButton::section { background: #272D36; border: none; }
QTableView#libraryTracksView QHeaderView,
QHeaderView#libraryTracksHeader,
QHeaderView#libraryTracksHeader::section {
    background: #1F232A; color: #ADB5C1; border: none; }
)QSS")
        + QStringLiteral(R"QSS(QWidget#libraryBreadcrumbBar QToolButton { background: transparent; border-color: transparent; padding: 4px 7px; }
QWidget#libraryBreadcrumbBar QToolButton:hover { background: #303A42; }
QWidget#libraryBreadcrumbBar QToolButton[buttonKeyboardFocus="true"]:focus:enabled { border-color: #69C8A5; }
QTextEdit, QPlainTextEdit { background: #181D23; border: 1px solid #343B45; border-radius: 6px;
    padding: 8px; selection-background-color: #244B40; selection-color: #F2FFF8; }
QTextEdit#subtitleTextDisplay { background: transparent; border: none; padding: 2px; font-size: 17px; }
QSlider::groove:horizontal { height: 4px; background: #3B424D; border-radius: 2px; }
QSlider::sub-page:horizontal { background: #45B38C; border-radius: 2px; }
QSlider::handle:horizontal { background: #F0F4F7; border: none; width: 12px; margin: -5px 0; border-radius: 6px; }
QSlider::groove:horizontal:disabled { background: #2B323C; }
QSlider::handle:horizontal:disabled { background: #5B6572; }
QSlider::sub-page:horizontal:disabled { background: #44574F; }
QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
QScrollBar::handle:vertical { background: #4D5865; border-radius: 3px; min-height: 24px; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; }
QScrollBar::handle:horizontal { background: #4D5865; border-radius: 3px; min-width: 24px; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
QSplitter::handle { background: #14171B; }
QSplitter::handle:hover { background: #345C4C; }
QDialog, QMessageBox, QMenu { background: #1F232A; }
QMenu { border: 1px solid #46515D; border-radius: 6px; padding: 5px; }
QMenu::item { padding: 8px 20px; }
QMenu::item:selected { background: #244B40; }
QMenu::item:disabled { color: #69717D; }
QMenu::separator { height: 1px; background: #39414B; margin: 5px 9px; }
QComboBox, QSpinBox, QDoubleSpinBox, QLineEdit { background: #181D23; border: 1px solid #46515D;
    border-radius: 4px; padding: 5px; selection-background-color: #244B40; }
QComboBox, QSpinBox, QDoubleSpinBox { padding-right: 31px; min-height: 20px; }
QComboBox:hover:enabled, QSpinBox:hover:enabled, QDoubleSpinBox:hover:enabled { border-color: #687A80; }
QComboBox:focus:enabled, QSpinBox:focus:enabled, QDoubleSpinBox:focus:enabled { border-color: #69C8A5; }
QComboBox:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled, QLineEdit:disabled {
    color: #697581; background: #20262D; border-color: #303843; }
QComboBox::drop-down {
    subcontrol-origin: padding; subcontrol-position: top right; width: 27px;
    border-left: 1px solid #39434F; border-top-right-radius: 3px; border-bottom-right-radius: 3px;
    background: #252D36; }
QComboBox::drop-down:hover:enabled { background: #343F49; }
QComboBox::drop-down:pressed:enabled, QComboBox::drop-down:on:enabled { background: #2B4B40; }
QComboBox::drop-down:disabled { background: #252B32; border-color: #303843; }
QComboBox::down-arrow { image: url(:/input-controls/chevron-down.png); width: 12px; height: 8px; }
QComboBox::down-arrow:on { image: url(:/input-controls/chevron-up.png); }
QComboBox::down-arrow:disabled { image: url(:/input-controls/chevron-down-disabled.png); }
QSpinBox::up-button, QDoubleSpinBox::up-button {
    subcontrol-origin: border; subcontrol-position: top right; width: 27px;
    border-left: 1px solid #39434F; border-bottom: 1px solid #39434F;
    border-top-right-radius: 4px; background: #252D36; }
QSpinBox::down-button, QDoubleSpinBox::down-button {
    subcontrol-origin: border; subcontrol-position: bottom right; width: 27px;
    border-left: 1px solid #39434F; border-bottom-right-radius: 4px; background: #252D36; }
QSpinBox::up-button:hover:enabled, QDoubleSpinBox::up-button:hover:enabled,
QSpinBox::down-button:hover:enabled, QDoubleSpinBox::down-button:hover:enabled { background: #343F49; }
QSpinBox::up-button:pressed:enabled, QDoubleSpinBox::up-button:pressed:enabled,
QSpinBox::down-button:pressed:enabled, QDoubleSpinBox::down-button:pressed:enabled { background: #2B4B40; }
QSpinBox::up-button:disabled, QDoubleSpinBox::up-button:disabled,
QSpinBox::down-button:disabled, QDoubleSpinBox::down-button:disabled { background: #252B32; border-color: #303843; }
QSpinBox::up-arrow, QDoubleSpinBox::up-arrow {
    image: url(:/input-controls/chevron-up.png); width: 10px; height: 6px; }
QSpinBox::down-arrow, QDoubleSpinBox::down-arrow {
    image: url(:/input-controls/chevron-down.png); width: 10px; height: 6px; }
QSpinBox::up-arrow:disabled, QDoubleSpinBox::up-arrow:disabled,
)QSS")
        + QStringLiteral(R"QSS(QSpinBox::up-arrow:off, QDoubleSpinBox::up-arrow:off { image: url(:/input-controls/chevron-up-disabled.png); }
QSpinBox::down-arrow:disabled, QDoubleSpinBox::down-arrow:disabled,
QSpinBox::down-arrow:off, QDoubleSpinBox::down-arrow:off { image: url(:/input-controls/chevron-down-disabled.png); }
QComboBox QAbstractItemView { background: #1F232A; border: 1px solid #46515D;
    padding: 3px; selection-background-color: #244B40; outline: none; }
QToolTip { color: #F0F2F5; background: #29333A; border: 1px solid #60756D; padding: 6px; }
QWidget#mainTitleBar { background: #14171B; }
QLabel#mainWindowTitle { font-size: 22px; font-weight: 600; color: #F0F2F5; }
QLabel#mainWindowTitle[windowActive="false"] { color: #97A0AD; }
QWidget#mainTitleBar QToolButton[windowControl="true"] {
    background: transparent; border: 1px solid transparent; border-radius: 0;
    padding: 0; min-width: 46px; max-width: 46px; min-height: 46px; max-height: 46px; }
QWidget#mainTitleBar QToolButton[windowControl="true"]:hover,
QWidget#mainTitleBar QToolButton[nativeHovered="true"] { background: #2B323B; }
QWidget#mainTitleBar QToolButton[windowControl="true"]:pressed { background: #3B4551; }
QWidget#mainTitleBar QToolButton[windowControl="true"][buttonKeyboardFocus="true"]:focus:enabled { border-color: #69C8A5; }
QWidget#mainTitleBar QToolButton#windowCloseButton:hover { background: #C7424C; }
QWidget#mainTitleBar QToolButton#windowCloseButton:pressed { background: #A8323B; }
QWidget#mainTitleBar QToolButton[windowControl="true"]:disabled { background: transparent; }
QDialog#workEditDialog { background: #1F232A; border: 1px solid #343B45; }
QDialog[dialogFrame="true"] { background: #1F232A; border: 1px solid #343B45; }
QWidget#dialogFrameContent { background: transparent; }
QWidget#workEditContent, QWidget#workEditButtonBox { background: transparent; }
QWidget#dialogTitleBar { background: #1E1F22; border-bottom: 1px solid #343B45; }
QLabel#dialogWindowTitle { font-size: 16px; font-weight: 500; color: #F0F2F5; }
QLabel#dialogWindowTitle[windowActive="false"] { color: #ADB5C1; }
QWidget#dialogTitleBar QToolButton[windowControl="true"] {
    background: transparent; border: 1px solid transparent; border-radius: 0;
    padding: 0; min-width: 46px; max-width: 46px; min-height: 38px; max-height: 38px; }
QWidget#dialogTitleBar QToolButton[windowControl="true"]:hover:enabled { background: #C7424C; }
QWidget#dialogTitleBar QToolButton[windowControl="true"]:pressed:enabled { background: #A8323B; }
QWidget#dialogTitleBar QToolButton[windowControl="true"][buttonKeyboardFocus="true"]:focus:enabled { border-color: #69C8A5; }
QWidget#dialogTitleBar QToolButton[windowControl="true"]:disabled { background: transparent; }
)QSS");
}

} // namespace asmr
