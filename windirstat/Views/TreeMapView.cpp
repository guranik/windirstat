// WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Distributed WITHOUT ANY WARRANTY; see LICENSE.md for details.

#include "pch.h"
#include "TreeMapView.h"

void CTreeMapView::DrawEmptyPlaceholder(CDC* pDC, const CRect& rect)
{
    CTreeMap::Options options = COptions::TreeMapOptions;
    options.showExtensions = false;
    options.showFolderFrames = false;
    if (!DarkMode::IsDarkModeActive())
    {
        options.brightness = CColorSpace::GraphPaletteBrightness;
        options.ambientLight = 1.0;
    }

    struct Tile { int x, y, w, h, shade; };
    static constexpr std::array tiles = {
        Tile{  0,  0, 25, 58, 58 }, Tile{ 25,  0, 13, 34, 72 }, Tile{ 25, 34, 13, 24, 48 },
        Tile{  0, 58, 18, 42, 76 }, Tile{ 18, 58, 20, 25, 54 }, Tile{ 18, 83, 20, 17, 88 },
        Tile{ 38,  0, 28, 44, 66 }, Tile{ 38, 44, 14, 31, 82 }, Tile{ 52, 44, 14, 31, 52 },
        Tile{ 38, 75, 28, 25, 92 }, Tile{ 66,  0, 19, 62, 60 }, Tile{ 85,  0, 15, 38, 78 },
        Tile{ 66, 62, 16, 38, 50 }, Tile{ 82, 38, 18, 36, 86 }, Tile{ 82, 74, 18, 26, 68 },
    };

    for (const auto& [x, y, w, h, shade] : tiles)
    {
        CRect tile(
            rect.left + rect.Width()  * x / 100,
            rect.top  + rect.Height() * y / 100,
            rect.left + rect.Width()  * (x + w) / 100,
            rect.top  + rect.Height() * (y + h) / 100);

        const int tileShade = DarkMode::IsDarkModeActive() ? shade : 255 - shade / 2;
        if (tile.Width() > 0 && tile.Height() > 0)
            m_treeMap.DrawColorPreview(pDC->Handle(), tile, RGB(tileShade, tileShade, tileShade), &options);
    }
}

bool CTreeMapView::CreateRenderBitmap(CDC* pDC, const CSize size)
{
    // A top-down DIB lets the treemap renderer write directly into the cached frame.
    const BITMAPINFO bitmapInfo{ .bmiHeader = { .biSize = sizeof(BITMAPINFOHEADER),
        .biWidth = size.cx, .biHeight = -size.cy, .biPlanes = 1,
        .biBitCount = 32, .biCompression = BI_RGB } };
    void* bits = nullptr;
    if (CBitmap bitmap((CreateDIBSection(pDC->Handle(), &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0))); bitmap)
    {
        m_bitmap = std::move(bitmap);
        return true;
    }
    return CGraphView::CreateRenderBitmap(pDC, size);
}

void CTreeMapView::RenderVisualization(CDC* pDC, CRect rect)
{
    m_diffHeaderHeight = 0;
    if (CWinDirStatModel::Get()->IsDiffMode()) DrawDiffLegend(pDC, rect);
    if (CWinDirStatModel::Get()->IsZoomed()) DrawZoomFrame(pDC, rect);
    auto options = COptions::TreeMapOptions;
    if (CWinDirStatModel::Get()->IsDiffMode()) options.showFolderFrames = true;
    m_treeMap.DrawTreeMap(pDC->Handle(), rect,
        CWinDirStatModel::Get()->GetZoomItem(), &options);
    if (CWinDirStatModel::Get()->IsDiffMode() && CWinDirStatModel::Get()->GetZoomItem()->TmiGetSize() == 0)
    {
        pDC->SetTextColor(DarkMode::SystemColor(COLOR_WINDOWTEXT));
        pDC->SetBkMode(TRANSPARENT);
        pDC->DrawText(Localization::Lookup(IDS_DIFF_ZERO), &rect, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
    }
}

void CTreeMapView::DrawDiffLegend(CDC* pdc, CRect& rect)
{
    GdiObjectSelection font(pdc, GetAppFont());
    const auto* diff = CWinDirStatModel::Get()->GetSnapshotDiff();
    const auto* entry = diff->Find(CWinDirStatModel::Get()->GetZoomItem());
    if (!entry || rect.Width() <= 0) return;
    const int columns = rect.Width() >= ScaleForDpi(800) ? 4 : 2;
    const int rowHeight = std::max(ScaleForDpi(26), pdc->GetTextExtent(L"Ag").cy + ScaleForDpi(8));
    m_diffHeaderHeight = std::min<int>(rect.Height(), rowHeight * (4 / columns));
    CRect header = rect;
    header.bottom = header.top + m_diffHeaderHeight;
    pdc->FillSolidRect(header, DarkMode::SystemColor(COLOR_WINDOW));
    pdc->SetTextColor(DarkMode::SystemColor(COLOR_WINDOWTEXT));
    pdc->SetBkMode(TRANSPARENT);
    for (int i = 0; i < 4; ++i)
    {
        CRect label(header.left + header.Width() * (i % columns) / columns,
            header.top + rowHeight * (i / columns),
            header.left + header.Width() * (i % columns + 1) / columns,
            std::min(header.bottom, header.top + rowHeight * (i / columns + 1)));
        if (label.IsEmpty()) continue;
        label.Deflate(ScaleForDpi(5), ScaleForDpi(3));
        CRect swatch = label;
        swatch.right = swatch.left + ScaleForDpi(12);
        swatch.top += std::max(0, (swatch.Height() - ScaleForDpi(12)) / 2);
        swatch.bottom = std::min(swatch.bottom, swatch.top + ScaleForDpi(12));
        pdc->FillSolidRect(swatch, SnapshotDiffData::KindColor(static_cast<diskdiff::Kind>(i)));
        label.left = swatch.right + ScaleForDpi(5);
        const auto text = std::format(L"{}: {} ({})", SnapshotDiffData::KindName(static_cast<diskdiff::Kind>(i)),
            FormatBytes(entry->totals.bytes[i]), FormatCount(entry->totals.files[i]));
        pdc->DrawText(text, &label, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    rect.top += m_diffHeaderHeight;
}

void CTreeMapView::DrawZoomFrame(CDC* pdc, CRect& rc) const
{
    CRect r  = rc;
    r.bottom = r.top + ZoomFrameWidth;
    pdc->FillSolidRect(r, CWinDirStatModel::Get()->GetZoomColor());

    r = rc;
    r.top = r.bottom - ZoomFrameWidth;
    pdc->FillSolidRect(r, CWinDirStatModel::Get()->GetZoomColor());

    r = rc;
    r.right = r.left + ZoomFrameWidth;
    pdc->FillSolidRect(r, CWinDirStatModel::Get()->GetZoomColor());

    r = rc;
    r.left = r.right - ZoomFrameWidth;
    pdc->FillSolidRect(r, CWinDirStatModel::Get()->GetZoomColor());

    rc.Deflate(ZoomFrameWidth, ZoomFrameWidth);
}

void CTreeMapView::DrawHighlightExtension(CDC* pdc)
{
    CWaitCursor wc;

    const CWinDirStatModel* model = CWinDirStatModel::Get();
    const bool isZoomed = model->IsZoomed();

    const CPoint offset = isZoomed
        ? CPoint(ZoomFrameWidth, ZoomFrameWidth + m_diffHeaderHeight) : CPoint(0, m_diffHeaderHeight);
    for (const CTreeMap::VisibleItem& visible : m_treeMap.GetVisibleItems())
    {
        const CItem* item = visible.item;
        if (!item->TmiIsLeaf() || !IsExtensionHighlighted(item)) continue;

        CRect rc = visible.rectangle + offset;
        RenderHighlightRectangle(pdc, rc);
    }
}

void CTreeMapView::DrawHover(CDC* pdc)
{
    if (const auto* diff = CWinDirStatModel::Get()->GetSnapshotDiff(); diff)
    {
        const CItem* scope = diff->hoverItem ? diff->hoverItem : m_hoverItem;
        if (!scope) return;
        int kind = diff->highlightKind;
        // A tile identifies a class; highlight that class throughout its folder.
        if (!diff->hoverItem)
            if (const auto* entry = diff->Find(scope); entry && entry->kind)
            {
                if (kind < 0) kind = static_cast<int>(*entry->kind);
                scope = scope->GetParent();
            }
        HighlightDiffBranch(pdc, scope, kind, true);
        return;
    }
    HighlightSelectedItem(pdc, GetDisplayItem(m_hoverItem), false, true);
}

void CTreeMapView::DrawSelection(CDC* pdc)
{
    for (const auto& selectedItems = CWinDirStatModel::Get()->GetAllSelected(); const CItem* item : selectedItems)
    {
        // Ignore if not a child of the current zoomed item
        if (!CWinDirStatModel::Get()->GetZoomItem()->IsAncestorOf(item)) continue;

        if (const auto* diff = CWinDirStatModel::Get()->GetSnapshotDiff(); diff && diff->highlightKind >= 0)
            HighlightDiffBranch(pdc, item, diff->highlightKind, false);
        else HighlightSelectedItem(pdc, GetDisplayItem(item), selectedItems.size() == 1);
    }
}

void CTreeMapView::HighlightDiffBranch(CDC* pdc, const CItem* scope, const int kind, const bool hover) const
{
    const auto* diff = CWinDirStatModel::Get()->GetSnapshotDiff();
    if (!scope || !diff) return;
    const CPoint offset = CWinDirStatModel::Get()->IsZoomed() ?
        CPoint(ZoomFrameWidth, ZoomFrameWidth + m_diffHeaderHeight) : CPoint(0, m_diffHeaderHeight);
    for (const auto& visible : m_treeMap.GetVisibleItems())
    {
        if (!visible.item->TmiIsLeaf() || !scope->IsAncestorOf(visible.item)) continue;
        const auto* entry = diff->Find(visible.item);
        if (!entry || !entry->kind || (kind >= 0 && kind != static_cast<int>(*entry->kind))) continue;
        RenderHighlightRectangle(pdc, visible.rectangle + offset, hover);
    }
}

// Draws the highlight rectangle of item. If single, the rectangle is slightly
// bigger than the item rect; otherwise, it fits inside.
//
void CTreeMapView::HighlightSelectedItem(CDC* pdc, const CItem* item, const bool single,
    const bool hover) const
{
    CRect rc;
    if (!m_treeMap.TryGetItemRectangle(item, rc)) return;
    rc.Offset(0, m_diffHeaderHeight);

    // Offset the display rectangle if zoomed
    if (CWinDirStatModel::Get()->IsZoomed())
    {
        rc.Offset(ZoomFrameWidth, ZoomFrameWidth);
    }

    if (single)
    {
        const CRect rcClient = GetClientRect();
        if (m_treeMap.GetOptions().grid)
        {
            rc.right++;
            rc.bottom++;
        }

        if (rcClient.left < rc.left) rc.left--;
        if (rcClient.top < rc.top) rc.top--;
        if (rc.right < rcClient.right) rc.right++;
        if (rc.bottom < rcClient.bottom) rc.bottom++;
    }

    if (rc.Width() <= 0 || rc.Height() <= 0)
    {
        return;
    }

    RenderHighlightRectangle(pdc, rc, hover);
}

CItem* CTreeMapView::FindItemAtPoint(const CPoint point)
{
    // Offset the click point if zoomed
    CPoint pointClicked = point;
    pointClicked.Offset(0, -m_diffHeaderHeight);

    if (CWinDirStatModel::Get()->IsZoomed())
    {
        pointClicked.Offset(-ZoomFrameWidth, -ZoomFrameWidth);
    }

    return m_treeMap.FindItemByPoint(
        CWinDirStatModel::Get()->GetZoomItem(), pointClicked);
}

bool CTreeMapView::HasValidLayout() const
{
    return m_treeMap.HasValidLayout(CWinDirStatModel::Get()->GetZoomItem());
}

void CTreeMapView::ClearVisualizationLayout()
{
    m_treeMap.ClearLayout();
    m_diffHeaderHeight = 0;
}

void CTreeMapView::OnRenderCacheTrimmed()
{
    m_treeMap.TrimMemory();
}

void CTreeMapView::DrillDown(CItem* item)
{
    if (item != CWinDirStatModel::Get()->GetZoomItem())
    {
        NotifyOtherPanes(MODEL_CHANGE_SELECTION_ACTION, item);
        GetMainWindow()->SendMessage(WM_COMMAND, ID_TREEMAP_ZOOMIN);
    }
}

std::span<const UINT> CTreeMapView::GetPersistentContextCommands() const
{
    static constexpr std::array<UINT, 15> persistentCommands{
        ID_TREEMAP_ZOOMIN,
        ID_TREEMAP_ZOOMOUT,
        ID_TREEMAP_SELECT_PARENT,
        ID_TREEMAP_RESELECT_CHILD,
        ID_VIEW_GROUP_TYPES,
        ID_TREEMAP_SHOW_FOLDER_FRAMES,
        ID_TREEMAP_SHOW_EXTENSIONS,
        ID_TREEMAP_LOGICAL_SIZE,
        ID_TREEMAP_PHYSICAL_SIZE,
        ID_VIEW_GRAPH_PRESET_CLASSIC,
        ID_VIEW_GRAPH_PRESET_CALM,
        ID_VIEW_GRAPH_PRESET_FLAT,
        ID_VIEW_GRAPH_PRESET_PASTEL,
        ID_VIEW_GRAPH_PRESET_HIGH_CONTRAST,
        ID_VIEW_GRAPH_PRESET_CUSTOM,
    };
    return persistentCommands;
}
