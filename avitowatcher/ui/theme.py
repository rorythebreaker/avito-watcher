"""Тёмная тема оформления."""

DARK_QSS = """
QWidget {
    background: #171b21;
    color: #e8ecf1;
    font-size: 13px;
}
QMainWindow, QDialog { background: #131720; }

QToolBar {
    background: #1b2028;
    border: none;
    border-bottom: 1px solid #262c35;
    padding: 6px 8px;
    spacing: 4px;
}
QToolBar QToolButton {
    padding: 7px 14px;
    border-radius: 7px;
    color: #d7dee7;
}
QToolBar QToolButton:hover { background: #2a313b; }
QToolBar QToolButton:pressed { background: #00aaff; color: #08121c; }
QToolBar QToolButton:disabled { color: #5c646f; }

QTreeWidget, QListView, QPlainTextEdit {
    background: #12161c;
    border: 1px solid #262c35;
    border-radius: 10px;
    selection-background-color: #243040;
    outline: none;
}
QTreeWidget::item { padding: 7px 4px; }
QTreeWidget::item:selected { background: #243040; color: #ffffff; }
QTreeWidget::item:alternate { background: #151a21; }
QHeaderView::section {
    background: #1b2028;
    color: #96a0ad;
    border: none;
    border-bottom: 1px solid #262c35;
    padding: 7px 6px;
}

QPlainTextEdit {
    font-family: Consolas, "Cascadia Mono", monospace;
    font-size: 12px;
    color: #a9b4c0;
}

QLabel { color: #d7dee7; }
QLabel#hint { color: #8a93a0; font-size: 12px; }

QPushButton {
    background: #262d37;
    border: 1px solid #333c48;
    border-radius: 7px;
    padding: 7px 14px;
    color: #e8ecf1;
}
QPushButton:hover { background: #2f3844; }
QPushButton:pressed { background: #00aaff; color: #08121c; }
QPushButton:disabled { background: #1c2129; color: #5c646f; }
QPushButton:default { background: #00aaff; color: #08121c; border-color: #00aaff; }

QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox {
    background: #12161c;
    border: 1px solid #333c48;
    border-radius: 7px;
    padding: 6px 9px;
    selection-background-color: #00aaff;
    selection-color: #08121c;
}
QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus {
    border-color: #00aaff;
}
QComboBox::drop-down { border: none; width: 22px; }
QComboBox QAbstractItemView {
    background: #1b2028;
    border: 1px solid #333c48;
    selection-background-color: #243040;
}

QGroupBox {
    border: 1px solid #262c35;
    border-radius: 10px;
    margin-top: 14px;
    padding: 12px 10px 10px;
}
QGroupBox::title {
    subcontrol-origin: margin;
    left: 12px;
    padding: 0 6px;
    color: #96a0ad;
}

QCheckBox, QRadioButton { spacing: 8px; padding: 3px 0; }
QCheckBox::indicator, QRadioButton::indicator { width: 17px; height: 17px; }
QCheckBox::indicator {
    border: 1px solid #45505e;
    border-radius: 4px;
    background: #12161c;
}
QCheckBox::indicator:checked { background: #00aaff; border-color: #00aaff; }
QRadioButton::indicator {
    border: 1px solid #45505e;
    border-radius: 9px;
    background: #12161c;
}
QRadioButton::indicator:checked { background: #00aaff; border-color: #00aaff; }

QTabWidget::pane {
    border: 1px solid #262c35;
    border-radius: 10px;
    top: -1px;
}
QTabBar::tab {
    background: transparent;
    color: #8a93a0;
    padding: 9px 18px;
    border-bottom: 2px solid transparent;
}
QTabBar::tab:selected { color: #e8ecf1; border-bottom-color: #00aaff; }
QTabBar::tab:hover { color: #d7dee7; }

QSlider::groove:horizontal {
    height: 5px;
    background: #262d37;
    border-radius: 2px;
}
QSlider::sub-page:horizontal { background: #00aaff; border-radius: 2px; }
QSlider::handle:horizontal {
    background: #e8ecf1;
    width: 15px;
    margin: -6px 0;
    border-radius: 7px;
}

QScrollBar:vertical {
    background: transparent;
    width: 11px;
    margin: 2px;
}
QScrollBar::handle:vertical {
    background: #333c48;
    border-radius: 5px;
    min-height: 32px;
}
QScrollBar::handle:vertical:hover { background: #45505e; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

QSplitter::handle { background: #1b2028; }
QSplitter::handle:horizontal { width: 4px; }
QSplitter::handle:vertical { height: 4px; }

QStatusBar { background: #1b2028; color: #8a93a0; }
QStatusBar::item { border: none; }

QMenu {
    background: #1b2028;
    border: 1px solid #333c48;
    border-radius: 8px;
    padding: 5px;
}
QMenu::item { padding: 7px 22px; border-radius: 5px; }
QMenu::item:selected { background: #243040; }

QToolTip {
    background: #1b2028;
    color: #e8ecf1;
    border: 1px solid #333c48;
    padding: 6px;
}
"""
