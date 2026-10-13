// WinDirStat-Diff
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "CompareSnapshotsDlg.h"

bool CCompareSnapshotsDlg::OnInitDialog()
{
    if (!CDialog::OnInitDialog()) return false;
    Localization::UpdateDialogs(*this);
    CheckRadioButton(Handle(), IDC_DIFF_LOGICAL, IDC_DIFF_PHYSICAL, IDC_DIFF_LOGICAL);
    OnPathsChanged();
    return true;
}

void CCompareSnapshotsDlg::Browse(const UINT id)
{
    const auto path = CDialog::PickFile(FilePickerMode::Open,
        std::format(L"{} (*.csv;*.json)|*.csv;*.json||", Localization::Lookup(IDS_FILE_FILTER)), this);
    if (path) GetDlgItem(id == IDC_DIFF_BROWSE_BEFORE ? IDC_DIFF_BEFORE : IDC_DIFF_AFTER).SetText(*path);
}

void CCompareSnapshotsDlg::OnSwap()
{
    const auto before = GetText(IDC_DIFF_BEFORE), after = GetText(IDC_DIFF_AFTER);
    GetDlgItem(IDC_DIFF_BEFORE).SetText(after);
    GetDlgItem(IDC_DIFF_AFTER).SetText(before);
}

void CCompareSnapshotsDlg::OnPathsChanged()
{
    GetDlgItem(IDOK).EnableWindow(!GetText(IDC_DIFF_BEFORE).empty() && !GetText(IDC_DIFF_AFTER).empty());
}

void CCompareSnapshotsDlg::OnOK()
{
    beforePath = GetText(IDC_DIFF_BEFORE);
    afterPath = GetText(IDC_DIFF_AFTER);
    if (beforePath.empty() || afterPath.empty()) return;
    metric = IsDlgButtonChecked(Handle(), IDC_DIFF_PHYSICAL) == BST_CHECKED ?
        diskdiff::Metric::Physical : diskdiff::Metric::Logical;
    CDialog::OnOK();
}
