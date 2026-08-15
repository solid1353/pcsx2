// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "FolderSettingsWidget.h"
#include "QtHost.h"
#include "QtUtils.h"
#include "SettingWidgetBinder.h"
#include "SettingsWindow.h"

#include <QtCore/QDir>
#include <QtCore/QUrl>
#include <QtWidgets/QFileDialog>

#include <algorithm>

FolderSettingsWidget::FolderSettingsWidget(SettingsWindow* settings_dialog, QWidget* parent)
	: SettingsWidget(settings_dialog, parent)
{
	SettingsInterface* sif = dialog()->getSettingsInterface();

	setupTab(m_ui);

	SettingWidgetBinder::BindWidgetToFolderSetting(sif, m_ui.cache, m_ui.cacheBrowse, m_ui.cacheOpen, m_ui.cacheReset, "Folders", "Cache", Path::Combine(EmuFolders::DataRoot, "cache"));
	SettingWidgetBinder::BindWidgetToFolderSetting(sif, m_ui.cheats, m_ui.cheatsBrowse, m_ui.cheatsOpen, m_ui.cheatsReset, "Folders", "Cheats", Path::Combine(EmuFolders::DataRoot, "cheats"));
	SettingWidgetBinder::BindWidgetToFolderSetting(sif, m_ui.covers, m_ui.coversBrowse, m_ui.coversOpen, m_ui.coversReset, "Folders", "Covers", Path::Combine(EmuFolders::DataRoot, "covers"));
	SettingWidgetBinder::BindWidgetToFolderSetting(sif, m_ui.snapshots, m_ui.snapshotsBrowse, m_ui.snapshotsOpen, m_ui.snapshotsReset, "Folders", "Snapshots", Path::Combine(EmuFolders::DataRoot, "snaps"));
	SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.organizeSnapshotsByGame, "EmuCore/GS", "OrganizeScreenshotsByGame", false);
	SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.organizeVideoDumpByGame, "EmuCore/GS", "OrganizeVideoCaptureByGame", false);
	SettingWidgetBinder::BindWidgetToFolderSetting(sif, m_ui.saveStates, m_ui.saveStatesBrowse, m_ui.saveStatesOpen, m_ui.saveStatesReset,
		"Folders", "Savestates", Path::Combine(EmuFolders::DataRoot, "sstates"));
	SettingWidgetBinder::BindWidgetToFolderSetting(sif, m_ui.videoDumpingDirectory, m_ui.videoDumpingDirectoryBrowse, m_ui.videoDumpingDirectoryOpen, m_ui.videoDumpingDirectoryReset,
		"Folders", "Videos", Path::Combine(EmuFolders::DataRoot, "videos"));
	for (const std::string& path : EmuFolders::AdditionalContentFolders)
		m_ui.additionalContentFolders->addItem(QDir::toNativeSeparators(QString::fromStdString(path)));
	connect(m_ui.additionalContentFolders, &QListWidget::currentRowChanged, this, &FolderSettingsWidget::updateAdditionalContentFolderButtons);
	connect(m_ui.additionalContentFolderAdd, &QPushButton::clicked, this, &FolderSettingsWidget::addAdditionalContentFolder);
	connect(m_ui.additionalContentFolderRemove, &QPushButton::clicked, this, &FolderSettingsWidget::removeAdditionalContentFolder);
	connect(m_ui.additionalContentFolderOpen, &QPushButton::clicked, this, &FolderSettingsWidget::openAdditionalContentFolder);
	connect(m_ui.additionalContentFolderUp, &QPushButton::clicked, this, [this]() { moveAdditionalContentFolder(-1); });
	connect(m_ui.additionalContentFolderDown, &QPushButton::clicked, this, [this]() { moveAdditionalContentFolder(1); });
	if (sif)
		m_ui.additionalContentFoldersGroup->setEnabled(false);
	updateAdditionalContentFolderButtons();
	dialog()->registerWidgetHelp(m_ui.organizeSnapshotsByGame, tr("Save Snapshots in Game-Specific Folders"), tr("Unchecked"),
		tr("Saves snapshots to per-game subfolders instead of a shared folder."));
	dialog()->registerWidgetHelp(m_ui.organizeVideoDumpByGame, tr("Save Video Recordings in Game-Specific Folders"), tr("Unchecked"),
		tr("Saves video recordings to per-game subfolders instead of a shared folder."));
}

FolderSettingsWidget::~FolderSettingsWidget() = default;

void FolderSettingsWidget::addAdditionalContentFolder()
{
	const QString path = QDir::toNativeSeparators(QFileDialog::getExistingDirectory(this, tr("Select Additional Content Folder")));
	if (path.isEmpty())
		return;

	for (int i = 0; i < m_ui.additionalContentFolders->count(); i++)
	{
		if (QString::compare(m_ui.additionalContentFolders->item(i)->text(), path, Qt::CaseInsensitive) == 0)
		{
			m_ui.additionalContentFolders->setCurrentRow(i);
			return;
		}
	}

	m_ui.additionalContentFolders->addItem(path);
	m_ui.additionalContentFolders->setCurrentRow(m_ui.additionalContentFolders->count() - 1);
	saveAdditionalContentFolders();
}

void FolderSettingsWidget::removeAdditionalContentFolder()
{
	const int row = m_ui.additionalContentFolders->currentRow();
	if (row < 0)
		return;
	delete m_ui.additionalContentFolders->takeItem(row);
	m_ui.additionalContentFolders->setCurrentRow(std::min(row, m_ui.additionalContentFolders->count() - 1));
	saveAdditionalContentFolders();
}

void FolderSettingsWidget::openAdditionalContentFolder()
{
	const QListWidgetItem* item = m_ui.additionalContentFolders->currentItem();
	if (item)
		QtUtils::OpenURL(this, QUrl::fromLocalFile(item->text()));
}

void FolderSettingsWidget::moveAdditionalContentFolder(const int direction)
{
	const int row = m_ui.additionalContentFolders->currentRow();
	const int new_row = row + direction;
	if (row < 0 || new_row < 0 || new_row >= m_ui.additionalContentFolders->count())
		return;
	m_ui.additionalContentFolders->insertItem(new_row, m_ui.additionalContentFolders->takeItem(row));
	m_ui.additionalContentFolders->setCurrentRow(new_row);
	saveAdditionalContentFolders();
}

void FolderSettingsWidget::saveAdditionalContentFolders()
{
	std::vector<std::string> paths;
	paths.reserve(m_ui.additionalContentFolders->count());
	for (int i = 0; i < m_ui.additionalContentFolders->count(); i++)
		paths.push_back(Path::MakeRelative(m_ui.additionalContentFolders->item(i)->text().toStdString(), EmuFolders::DataRoot));
	Host::SetBaseStringListSettingValue("Folders", "AdditionalContentFolders", paths);
	Host::CommitBaseSettingChanges();
	g_emu_thread->updateEmuFolders();
	updateAdditionalContentFolderButtons();
}

void FolderSettingsWidget::updateAdditionalContentFolderButtons()
{
	const int row = m_ui.additionalContentFolders->currentRow();
	const bool selected = row >= 0;
	m_ui.additionalContentFolderRemove->setEnabled(selected);
	m_ui.additionalContentFolderOpen->setEnabled(selected);
	m_ui.additionalContentFolderUp->setEnabled(selected && row > 0);
	m_ui.additionalContentFolderDown->setEnabled(selected && row + 1 < m_ui.additionalContentFolders->count());
}

#include "moc_FolderSettingsWidget.cpp"
