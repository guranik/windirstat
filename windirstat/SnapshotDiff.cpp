// WinDirStat-Diff
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "SnapshotDiff.h"
#include "CompareSnapshotsDlg.h"
#include "FileTopControl.h"
#include "ProgressDlg.h"
#include "VisualizationPane.h"

SnapshotDiffData::SnapshotDiffData() = default;
SnapshotDiffData::~SnapshotDiffData() = default;

const SnapshotDiffEntry* SnapshotDiffData::Find(const CItem* item) const
{
    const auto found = entries.find(item);
    return found == entries.end() ? nullptr : &found->second;
}

std::wstring SnapshotDiffData::KindName(const diskdiff::Kind kind)
{
    static constexpr std::array ids{ IDS_DIFF_ADDED, IDS_DIFF_REMOVED, IDS_DIFF_GROWN, IDS_DIFF_SHRUNK };
    return Localization::Lookup(ids[static_cast<std::size_t>(kind)]);
}

std::uint32_t SnapshotDiffData::KindColor(const diskdiff::Kind kind)
{
    static constexpr std::array<COLORREF, 4> colors{
        RGB(42, 196, 89), RGB(230, 65, 70), RGB(242, 163, 40), RGB(58, 132, 235)
    };
    return colors[static_cast<std::size_t>(kind)];
}

std::wstring SnapshotDiffData::NetText(const diskdiff::Totals& totals)
{
    const auto increase = totals.Increase(), decrease = totals.Decrease();
    return (increase >= decrease ? L"+" : L"-") +
        FormatBytes(increase >= decrease ? increase - decrease : decrease - increase);
}

std::wstring SnapshotDiffData::Describe(const CItem* item) const
{
    const auto* entry = Find(item);
    if (!entry) return {};
    std::wstring text = item->GetPath();
    if (entry->kind)
    {
        text += std::format(L" | {} | A: {} | B: {} | Δ: {}",
            KindName(*entry->kind), FormatBytes(entry->before.Get(metric)),
            FormatBytes(entry->after.Get(metric)), NetText(entry->totals));
    }
    else
    {
        text += L" | Δ: " + NetText(entry->totals);
        for (std::size_t i = 0; i < entry->totals.bytes.size(); ++i)
            text += std::format(L" | {}: {} ({})", KindName(static_cast<diskdiff::Kind>(i)),
                FormatBytes(entry->totals.bytes[i]), FormatCount(entry->totals.files[i]));
    }
    return text;
}

std::unique_ptr<SnapshotDiffData> BuildSnapshotDiff(const std::wstring& beforePath,
    const std::wstring& afterPath, const diskdiff::Metric metric, CProgressDlg* progress)
{
    const diskdiff::Columns aliases{{
        {Localization::Lookup(IDS_COL_NAME)}, {Localization::Lookup(IDS_COL_SIZE_LOGICAL)},
        {Localization::Lookup(IDS_COL_SIZE_PHYSICAL)},
        {Localization::LookupNeutral(IDS_APP_TITLE) + L" " + Localization::Lookup(IDS_COL_ATTRIBUTES)},
        {Localization::Lookup(IDS_COL_FILES)}
    }};
    const diskdiff::CancelCheck cancelled = [progress] { return progress && progress->IsCancelled(); };
    const auto before = diskdiff::LoadSnapshot(beforePath, aliases, cancelled);
    const auto after = diskdiff::LoadSnapshot(afterPath, aliases, cancelled);
    const auto result = diskdiff::Compare(before, after, metric, cancelled);

    auto data = std::make_unique<SnapshotDiffData>();
    data->metric = metric;
    data->beforePath = beforePath;
    data->afterPath = afterPath;
    data->entries.reserve(result.changes.size() + 1);
    const bool singleRoot = before.roots.size() == 1;
    const auto& rootPath = before.roots.front();
    const auto rootName = singleRoot ? after.folders.at(rootPath).path : L"Disk Diff";
    data->root.reset(CItem::Create((singleRoot ? IT_DIRECTORY : IT_MYCOMPUTER) | ITF_ROOTITEM,
        rootName, {}, 0, 0, 0, FILE_ATTRIBUTE_DIRECTORY, 0, 0));
    CItem* root = data->root.get();
    data->entries.emplace(root, SnapshotDiffEntry{});

    std::unordered_map<std::wstring, CItem*> folders;
    if (singleRoot) folders.emplace(rootPath, root);
    std::vector<CItem*> ordered{root}; // Parents always precede their children.
    const auto sizes = [&](const std::wstring& key)
    {
        SnapshotDiffEntry entry;
        if (const auto a = before.folders.find(key); a != before.folders.end()) entry.before = a->second.sizes;
        if (const auto b = after.folders.find(key); b != after.folders.end()) entry.after = b->second.sizes;
        return entry;
    };
    if (singleRoot) data->entries.at(root) = sizes(rootPath);
    else
    {
        const auto add = [](std::uint64_t& total, const std::uint64_t value)
        {
            if (value > UINT64_MAX - total) throw diskdiff::Error(diskdiff::ErrorCode::Overflow, "Root sizes overflow.");
            total += value;
        };
        auto& entry = data->entries.at(root);
        for (const auto& key : before.roots)
        {
            const auto a = before.folders.at(key).sizes, b = after.folders.at(key).sizes;
            add(entry.before.logical, a.logical); add(entry.before.physical, a.physical);
            add(entry.after.logical, b.logical); add(entry.after.physical, b.physical);
        }
    }

    // Iterative ancestor creation avoids recursion and stores only changed branches.
    const auto ensureFolder = [&](const std::wstring& key) -> CItem*
    {
        std::vector<std::wstring> missing;
        std::wstring current = key;
        CItem* parent = root;
        while (!current.empty())
        {
            if (const auto found = folders.find(current); found != folders.end())
            {
                parent = found->second;
                break;
            }
            missing.push_back(current);
            if (std::binary_search(before.roots.begin(), before.roots.end(), current)) break;
            current = diskdiff::ParentPath(current);
        }
        for (auto it = missing.rbegin(); it != missing.rend(); ++it)
        {
            const auto a = after.folders.find(*it), b = before.folders.find(*it);
            const auto& displayPath = a != after.folders.end() ? a->second.path : b->second.path;
            const bool branchRoot = parent == root && !singleRoot;
            const auto separator = displayPath.find_last_of(L"\\/");
            const auto name = branchRoot ? std::wstring_view(displayPath) : std::wstring_view(displayPath).substr(separator + 1);
            auto child = std::unique_ptr<CItem>(CItem::Create(IT_DIRECTORY, name, {}, 0, 0, 0,
                FILE_ATTRIBUTE_DIRECTORY, 0, 0));
            parent->AddChild(child.get(), true);
            parent->UpwardAddFolders(1);
            parent = child.release();
            folders.emplace(*it, parent);
            data->entries.emplace(parent, sizes(*it));
            ordered.push_back(parent);
        }
        return parent;
    };

    std::size_t visited = 0;
    for (const auto& change : result.changes)
    {
        if ((++visited & 4095) == 0 && cancelled()) throw diskdiff::Error(diskdiff::ErrorCode::Cancelled, "Comparison cancelled.");
        const auto key = diskdiff::NormalizePath(change.path);
        CItem* parent = ensureFolder(diskdiff::ParentPath(key));
        const auto separator = change.path.find_last_of(L"\\/");
        auto item = std::unique_ptr<CItem>(CItem::Create(IT_FILE,
            std::wstring_view(change.path).substr(separator + 1), {}, change.magnitude,
            change.magnitude, 0, FILE_ATTRIBUTE_NORMAL, 0, 0));
        SnapshotDiffEntry entry{change.before, change.after, {}, change.kind};
        entry.totals.Add(change.kind, change.magnitude);
        data->entries.emplace(item.get(), std::move(entry));
        ordered.push_back(item.get());
        parent->AddChild(item.get(), true);
        parent->UpwardAddFiles(1);
        item.release();
    }
    // Aggregate in postorder once, retaining additions and removals separately.
    for (auto it = ordered.rbegin(); it != ordered.rend(); ++it)
    {
        CItem* item = *it;
        const auto& totals = data->entries.at(item).totals;
        const auto magnitude = totals.Magnitude();
        item->SetSizeLogical(magnitude);
        item->SetSizePhysical(magnitude);
        if (item != root) data->entries.at(item->GetParent()).totals.Merge(totals);
        if (!item->IsLeaf()) item->SortItemsBySizeLogical();
        item->SetFlag(ITF_DONE); // No SetDone()/drive metadata queries for a comparison.
    }
    if (cancelled()) throw diskdiff::Error(diskdiff::ErrorCode::Cancelled, "Comparison cancelled.");
    return data;
}

bool CWinDirStatModel::IsDiffMode() const { return m_snapshotDiff != nullptr; }
const SnapshotDiffData* CWinDirStatModel::GetSnapshotDiff() const { return m_snapshotDiff.get(); }

void CWinDirStatModel::SetDiffHover(const CItem* item)
{
    if (!m_snapshotDiff || m_snapshotDiff->hoverItem == item) return;
    m_snapshotDiff->hoverItem = item;
    NotifyPanes(MODEL_CHANGE_DIFF_HOVER);
    CMainFrame::Get()->UpdatePaneText();
}

void CWinDirStatModel::OnDiffHighlight(const UINT id)
{
    if (!m_snapshotDiff) return;
    m_snapshotDiff->highlightKind = static_cast<int>(id - ID_DIFF_HIGHLIGHT_ALL) - 1;
    NotifyPanes(MODEL_CHANGE_DIFF_HOVER);
}

void CWinDirStatModel::OnUpdateDiffHighlight(CCmdUI* ui)
{
    ui->Enable(IsDiffMode());
    ui->SetRadio(m_snapshotDiff && m_snapshotDiff->highlightKind == static_cast<int>(ui->m_nID - ID_DIFF_HIGHLIGHT_ALL) - 1);
}

void CWinDirStatModel::OnCompareSnapshots()
{
    if (IsScanRunning()) return;
    CCompareSnapshotsDlg dialog(GetMainWindow());
    if (dialog.ShowModal() == IDOK) CompareSnapshotPaths(dialog.beforePath, dialog.afterPath, dialog.metric);
}

void CWinDirStatModel::OnUpdateCompareSnapshots(CCmdUI* ui) { ui->Enable(!IsScanRunning()); }

bool CWinDirStatModel::CompareSnapshotPaths(const std::wstring& beforePath,
    const std::wstring& afterPath, const diskdiff::Metric metric)
{
    if (IsScanRunning()) return false;
    std::unique_ptr<SnapshotDiffData> data;
    std::wstring error;
    CProgressDlg dialog(0, CProgressDlg::Flags::None, GetMainWindow(), [&](CProgressDlg* progress)
    {
        try { data = BuildSnapshotDiff(beforePath, afterPath, metric, progress); }
        catch (const diskdiff::Error& e)
        {
            if (e.code == diskdiff::ErrorCode::Cancelled) return;
            static constexpr std::array ids{
                IDS_DIFF_ERROR_FORMAT, IDS_DIFF_ERROR_COLUMNS, IDS_DIFF_ERROR_DUPLICATE,
                IDS_DIFF_ERROR_HIERARCHY, IDS_DIFF_ERROR_ROOTS, IDS_DIFF_ERROR_UNSUPPORTED,
                IDS_DIFF_ERROR_OVERFLOW, IDS_DIFF_ERROR_CANCELLED, IDS_DIFF_ERROR_OPEN
            };
            error = Localization::Lookup(ids[static_cast<std::size_t>(e.code)]);
        }
        catch (const std::exception&) { error = Localization::Lookup(IDS_DIFF_ERROR_MEMORY); }
    });
    dialog.ShowModal();
    if (dialog.WasCancelled()) return false;
    if (!error.empty()) CMessageBoxDlg::Show(error, MB_OK | MB_ICONERROR, GetMainWindow());
    if (!data) return false;

    // Install the fully built comparison atomically on the UI thread. Failed loads
    // and cancellation leave the previous document intact.
    ClearScanState();
    COptions::TreeMapUseLogical = metric == diskdiff::Metric::Logical;
    m_rootItem = data->root.release();
    m_zoomItem = m_rootItem;
    m_snapshotDiff = std::move(data);
    m_scanPathSpec.clear();
    SetScanTitle(std::format(L"Disk Diff | {} → {} | Δ {}",
        std::filesystem::path(beforePath).filename().wstring(),
        std::filesystem::path(afterPath).filename().wstring(), SnapshotDiffData::NetText(m_snapshotDiff->entries.at(m_rootItem).totals)));
    RebuildExtensionData();
    for (auto* item : CItem::GetItemsRecursive({m_rootItem})) CFileTopControl::Get()->ProcessTop(item);
    CFileTopControl::Get()->SortItems();
    NotifyPanes(MODEL_CHANGE_NEW_ROOT);
    CMainFrame::Get()->SetProgressComplete();
    auto* visualization = static_cast<CVisualizationPane*>(CMainFrame::Get()->GetVisualizationPane());
    COptions::GraphPaneStyle = EncodeGraphPane(GraphPane::TreeMap);
    visualization->SelectPane(GraphPane::TreeMap);
    visualization->ShowVisualization(true);
    CMainFrame::Get()->ApplyPaneVisibility(true);
    SetScanTitlePrefix(L"");
    CMainFrame::Get()->MoveFocus(LF_FILETREE);
    CMainFrame::Get()->UpdatePaneText();
    return true;
}
