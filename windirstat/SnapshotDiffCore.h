// WinDirStat-Diff
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// This module deliberately has no UI, filesystem-scanning, or PCH dependency.
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <istream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace diskdiff
{
    enum class Kind : std::uint8_t { Added, Removed, Grown, Shrunk };
    enum class Metric : std::uint8_t { Logical, Physical };
    enum class ErrorCode : std::uint8_t
    {
        InvalidFormat, MissingColumns, DuplicatePath, InvalidHierarchy,
        DifferentRoots, UnsupportedRoot, Overflow, Cancelled, CannotOpen
    };

    class Error : public std::runtime_error
    {
    public:
        Error(ErrorCode errorCode, const char* message) : std::runtime_error(message), code(errorCode) {}
        ErrorCode code;
    };

    struct Sizes
    {
        std::uint64_t logical = 0, physical = 0;
        std::uint64_t Get(Metric metric) const noexcept
        {
            return metric == Metric::Logical ? logical : physical;
        }
    };

    struct Totals
    {
        std::array<std::uint64_t, 4> bytes{}, files{};
        void Add(Kind kind, std::uint64_t magnitude);
        void Merge(const Totals& other);
        std::uint64_t Magnitude() const;
        std::uint64_t Increase() const;
        std::uint64_t Decrease() const;
    };

    struct Record
    {
        std::wstring path;
        Sizes sizes;
    };

    struct Snapshot
    {
        // Keys are normalized absolute paths. Display spelling is retained separately.
        std::unordered_map<std::wstring, Record> files, folders;
        std::vector<std::wstring> roots;
    };

    struct Change
    {
        std::wstring path;
        Sizes before, after;
        Kind kind = Kind::Added;
        std::uint64_t magnitude = 0;
    };

    struct Result
    {
        std::vector<Change> changes;
        Totals totals;
        Metric metric = Metric::Logical;
    };

    // Extra aliases, usually the application's currently selected translation.
    // Fields: path, logical size, physical size, WinDirStat attributes, optional file count.
    using Columns = std::array<std::vector<std::wstring>, 5>;
    using CancelCheck = std::function<bool()>;

    std::wstring FromUtf8(std::string_view text);
    std::string ToUtf8(std::wstring_view text);
    std::wstring NormalizePath(std::wstring_view path);
    std::wstring ParentPath(std::wstring_view normalizedPath);
    Snapshot ReadSnapshot(std::istream& input, const Columns& aliases = {},
        const CancelCheck& cancelled = {});
    Snapshot LoadSnapshot(const std::filesystem::path& path, const Columns& aliases = {},
        const CancelCheck& cancelled = {});
    Result Compare(const Snapshot& before, const Snapshot& after, Metric metric,
        const CancelCheck& cancelled = {});
}
