// WinDirStat-Diff
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "pch.h"
#include "SnapshotDiffCore.h"

class CCompareSnapshotsDlg final : public MessageTarget<CCompareSnapshotsDlg, CDialog>
{
public:
    explicit CCompareSnapshotsDlg(WindowRef parent = nullptr) : MessageTarget(IDD_COMPARE_SNAPSHOTS, parent) {}
    std::wstring beforePath, afterPath;
    diskdiff::Metric metric = diskdiff::Metric::Logical;
    bool OnInitDialog() override;
    void OnOK() override;
    static std::span<const RouteEntry> Routes();

private:
    void Browse(UINT id);
    void OnSwap();
    void OnPathsChanged();
};

inline std::span<const RouteEntry> CCompareSnapshotsDlg::Routes()
{
    static constexpr std::array entries{
        Route::Control<&Browse>(BN_CLICKED, IDC_DIFF_BROWSE_BEFORE, IDC_DIFF_BROWSE_AFTER),
        Route::Control<&OnSwap>(BN_CLICKED, IDC_DIFF_SWAP),
        Route::Control<&OnPathsChanged>(EN_CHANGE, IDC_DIFF_BEFORE),
        Route::Control<&OnPathsChanged>(EN_CHANGE, IDC_DIFF_AFTER)
    };
    return entries;
}
