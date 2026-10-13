// WinDirStat-Diff
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "SnapshotDiffCore.h"
#include <memory>
#include <optional>

class CItem;
class CProgressDlg;

struct SnapshotDiffEntry
{
    diskdiff::Sizes before, after;
    diskdiff::Totals totals;
    std::optional<diskdiff::Kind> kind; // Folders retain each class in their totals.
};

struct SnapshotDiffData
{
    SnapshotDiffData();
    ~SnapshotDiffData();
    std::unique_ptr<CItem> root;
    std::unordered_map<const CItem*, SnapshotDiffEntry> entries;
    diskdiff::Metric metric = diskdiff::Metric::Logical;
    std::wstring beforePath, afterPath;
    const CItem* hoverItem = nullptr;
    int highlightKind = -1; // -1: all, 0..3: Added/Removed/Grown/Shrunk.

    const SnapshotDiffEntry* Find(const CItem* item) const;
    std::wstring Describe(const CItem* item) const;
    static std::wstring KindName(diskdiff::Kind kind);
    static std::uint32_t KindColor(diskdiff::Kind kind);
    static std::wstring NetText(const diskdiff::Totals& totals);
};

std::unique_ptr<SnapshotDiffData> BuildSnapshotDiff(const std::wstring& beforePath,
    const std::wstring& afterPath, diskdiff::Metric metric, CProgressDlg* progress);
