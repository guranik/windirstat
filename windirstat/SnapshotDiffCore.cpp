// WinDirStat-Diff
// SPDX-License-Identifier: GPL-3.0-or-later
#include "SnapshotDiffCore.h"

#include <algorithm>
#include <charconv>
#include <climits>
#include <fstream>
#include <limits>
#include <optional>
#include <unordered_set>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace diskdiff
{
    namespace
    {
        constexpr std::uint32_t Computer = 1, Drive = 2, Directory = 4, File = 8;
        constexpr std::uint32_t FreeSpace = 16, Unknown = 32, Root = 0x10000000;
        constexpr std::uint32_t Done = 0x20000000, Mtp = 0x80000000;

        [[noreturn]] void Invalid(const char* message)
        {
            throw Error(ErrorCode::InvalidFormat, message);
        }

        std::uint64_t CheckedAdd(std::uint64_t a, std::uint64_t b)
        {
            if (b > std::numeric_limits<std::uint64_t>::max() - a)
                throw Error(ErrorCode::Overflow, "Snapshot totals exceed 64-bit capacity.");
            return a + b;
        }

        void CheckCancellation(const CancelCheck& cancelled)
        {
            if (cancelled && cancelled()) throw Error(ErrorCode::Cancelled, "Comparison cancelled.");
        }

        void AppendUtf8(std::string& out, std::uint32_t cp)
        {
            if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) Invalid("Invalid Unicode character.");
            if (cp < 0x80) out.push_back(static_cast<char>(cp));
            else if (cp < 0x800)
            {
                out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                out.push_back(static_cast<char>(0x80 | (cp & 63)));
            }
            else if (cp < 0x10000)
            {
                out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 63)));
                out.push_back(static_cast<char>(0x80 | (cp & 63)));
            }
            else
            {
                out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 63)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 63)));
                out.push_back(static_cast<char>(0x80 | (cp & 63)));
            }
        }

        std::uint64_t Number(std::wstring_view wide, bool hex = false)
        {
            const std::string text = ToUtf8(wide);
            std::string_view value(text);
            int base = 10;
            if (hex)
            {
                base = 16;
                if (value.size() >= 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X'))
                    value.remove_prefix(2);
            }
            std::uint64_t number = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number, base);
            if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
                Invalid("A snapshot contains an invalid or overflowing size/attribute value.");
            return number;
        }

        bool IsAbsolute(std::wstring_view path)
        {
            if (path.size() >= 3 && path[1] == L':' && path[2] == L'\\' &&
                ((path[0] >= L'a' && path[0] <= L'z') || (path[0] >= L'A' && path[0] <= L'Z'))) return true;
            if (path.size() < 5 || path.substr(0, 2) != L"\\\\") return false;
            const auto serverEnd = path.find(L'\\', 2);
            return serverEnd != std::wstring_view::npos && serverEnd > 2 && serverEnd + 1 < path.size();
        }

        bool IsWithin(std::wstring_view path, std::wstring_view root)
        {
            if (path == root) return true;
            if (path.size() <= root.size() || path.substr(0, root.size()) != root) return false;
            return root.back() == L'\\' || path[root.size()] == L'\\';
        }

        int Field(std::wstring_view key, const Columns& aliases)
        {
            static const Columns known{{
                {L"Name", L"Имя"}, {L"Logical Size", L"Логический размер"},
                {L"Physical Size", L"Физический размер"},
                {L"WinDirStat Attributes", L"WinDirStat Атрибуты"}, {L"Files", L"Файлы"}
            }};
            for (std::size_t i = 0; i < known.size(); ++i)
            {
                if (std::find(known[i].begin(), known[i].end(), key) != known[i].end() ||
                    std::find(aliases[i].begin(), aliases[i].end(), key) != aliases[i].end())
                    return static_cast<int>(i);
            }
            return -1;
        }

        class Builder
        {
            Snapshot snapshot;
            std::optional<std::wstring> singleRoot;
            std::optional<std::uint64_t> declaredFiles;
            std::unordered_map<std::wstring, Sizes> spaceItems;
            bool rootSeen = false, multiRoot = false;
            std::size_t rows = 0;
            const CancelCheck& cancelled;

        public:
            explicit Builder(const CancelCheck& check) : cancelled(check) {}

            void Row(const std::array<std::wstring, 4>& fields, const std::array<bool, 4>& present,
                const std::optional<std::uint64_t> fileCount = {})
            {
                if ((++rows & 4095) == 0) CheckCancellation(cancelled);
                if (std::find(present.begin(), present.end(), false) != present.end())
                    throw Error(ErrorCode::MissingColumns, "Missing required WinDirStat export columns.");
                const auto rawType = Number(fields[3], true);
                if (rawType > UINT32_MAX) Invalid("Invalid WinDirStat item type.");
                const auto type = static_cast<std::uint32_t>(rawType);
                const auto base = type & 0xFFFF;
                if (type & Mtp) throw Error(ErrorCode::UnsupportedRoot, "MTP snapshots are not supported.");
                if (base == FreeSpace || base == Unknown)
                {
                    const auto path = NormalizePath(fields[0]);
                    auto& space = spaceItems[diskdiff::ParentPath(path)];
                    space.logical = CheckedAdd(space.logical, Number(fields[1]));
                    space.physical = CheckedAdd(space.physical, Number(fields[2]));
                    return;
                }
                if (base != Computer && base != Drive && base != Directory && base != File)
                    throw Error(ErrorCode::UnsupportedRoot, "Unsupported item type in snapshot.");
                if (type & Root)
                {
                    if (rootSeen || !(type & Done) || base == File) Invalid("Invalid or unfinished snapshot root.");
                    rootSeen = true;
                    declaredFiles = fileCount;
                    multiRoot = base == Computer;
                    if (!multiRoot) singleRoot = NormalizePath(fields[0]);
                }
                if (base == Computer)
                {
                    if (!(type & Root)) Invalid("Unexpected computer container.");
                    return;
                }
                if (!rootSeen) Invalid("Snapshot root must precede its entries.");
                const std::wstring key = NormalizePath(fields[0]);
                if (!IsAbsolute(key)) throw Error(ErrorCode::UnsupportedRoot, "Snapshot paths must be absolute Windows paths.");
                Record record{ fields[0], {Number(fields[1]), Number(fields[2])} };
                auto& entries = base == File ? snapshot.files : snapshot.folders;
                if (snapshot.files.count(key) || snapshot.folders.count(key))
                    throw Error(ErrorCode::DuplicatePath, "Duplicate paths (including case-only collisions) in a snapshot.");
                entries.emplace(key, std::move(record));
            }

            Snapshot Finish()
            {
                CheckCancellation(cancelled);
                if (!rootSeen || snapshot.folders.empty()) Invalid("Empty or invalid WinDirStat snapshot.");
                if (declaredFiles && *declaredFiles != snapshot.files.size())
                    throw Error(ErrorCode::InvalidHierarchy, "Snapshot file count does not match its entries.");
                if (singleRoot) snapshot.roots.push_back(*singleRoot);
                else if (multiRoot)
                {
                    for (const auto& entry : snapshot.folders)
                        if (!snapshot.folders.count(ParentPath(entry.first))) snapshot.roots.push_back(entry.first);
                }
                if (snapshot.roots.empty()) Invalid("No scan roots in snapshot.");
                std::sort(snapshot.roots.begin(), snapshot.roots.end());
                const std::unordered_set<std::wstring> roots(snapshot.roots.begin(), snapshot.roots.end());
                for (const auto& rootPath : snapshot.roots)
                    for (auto parent = ParentPath(rootPath); !parent.empty(); parent = ParentPath(parent))
                        if (roots.count(parent)) throw Error(ErrorCode::InvalidHierarchy, "Overlapping scan roots.");
                for (const auto& entries : {&snapshot.files, &snapshot.folders})
                    for (const auto& entry : *entries)
                    {
                        if ((++rows & 4095) == 0) CheckCancellation(cancelled);
                        if (std::binary_search(snapshot.roots.begin(), snapshot.roots.end(), entry.first)) continue;
                        if (!snapshot.folders.count(ParentPath(entry.first)))
                            throw Error(ErrorCode::InvalidHierarchy, "A snapshot entry has no parent folder.");
                    }
                if (singleRoot)
                    for (const auto& entry : snapshot.folders)
                        if (!IsWithin(entry.first, *singleRoot))
                            throw Error(ErrorCode::InvalidHierarchy, "Entries outside the selected scan root.");
                // Drive exports can include free/unknown space in folder totals.
                // Remove those pseudo-items from the A/B size columns too.
                for (const auto& space : spaceItems)
                {
                    auto path = space.first;
                    bool foundParent = false;
                    while (!path.empty())
                    {
                        const auto folder = snapshot.folders.find(path);
                        if (folder == snapshot.folders.end()) break; // Above an individual scan root.
                        foundParent = true;
                        auto& size = folder->second.sizes;
                        if (size.logical < space.second.logical || size.physical < space.second.physical)
                            throw Error(ErrorCode::InvalidHierarchy, "Space pseudo-items exceed their folder size.");
                        size.logical -= space.second.logical;
                        size.physical -= space.second.physical;
                        path = ParentPath(path);
                    }
                    if (!foundParent) throw Error(ErrorCode::InvalidHierarchy, "Space pseudo-item has no parent folder.");
                }
                return std::move(snapshot);
            }
        };

        // Streaming parser for the flat JSON array exported by WinDirStat. It accepts
        // compact JSON too, handles escapes, and rejects truncation instead of silently
        // turning unread entries into 'removed' files.
        class JsonReader
        {
            std::streambuf* input;
            const Columns& aliases;
            Builder& builder;

            int Peek() const { return input->sgetc(); }
            int Take() const { return input->sbumpc(); }
            void Whitespace() const
            {
                while (Peek() == ' ' || Peek() == '\t' || Peek() == '\r' || Peek() == '\n') Take();
            }
            void Expect(int ch) const
            {
                Whitespace();
                if (Take() != ch) Invalid("Malformed or truncated JSON snapshot.");
            }
            std::uint32_t Hex4() const
            {
                std::uint32_t cp = 0;
                for (int i = 0; i < 4; ++i)
                {
                    const int ch = Take();
                    const int digit = ch >= '0' && ch <= '9' ? ch - '0' :
                        ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
                    if (digit < 0) Invalid("Invalid JSON Unicode escape.");
                    cp = cp * 16 + static_cast<std::uint32_t>(digit);
                }
                return cp;
            }
            std::wstring String() const
            {
                Expect('"');
                std::string out;
                for (;;)
                {
                    int ch = Take();
                    if (ch == '"') return FromUtf8(out);
                    if (ch < 0x20) Invalid("Invalid character or EOF in JSON string.");
                    if (ch != '\\') { out.push_back(static_cast<char>(ch)); continue; }
                    ch = Take();
                    switch (ch)
                    {
                    case '"': case '\\': case '/': out.push_back(static_cast<char>(ch)); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u':
                    {
                        auto cp = Hex4();
                        if (cp >= 0xD800 && cp <= 0xDBFF)
                        {
                            if (Take() != '\\' || Take() != 'u') Invalid("Missing JSON low surrogate.");
                            const auto low = Hex4();
                            if (low < 0xDC00 || low > 0xDFFF) Invalid("Invalid JSON low surrogate.");
                            cp = 0x10000 + ((cp - 0xD800) << 10) + low - 0xDC00;
                        }
                        AppendUtf8(out, cp);
                        break;
                    }
                    default: Invalid("Invalid JSON escape.");
                    }
                }
            }
            std::wstring Value() const
            {
                Whitespace();
                if (Peek() == '"') return String();
                std::string out;
                while (Peek() != ',' && Peek() != '}' && Peek() != ' ' && Peek() != '\t' && Peek() != '\r' && Peek() != '\n')
                {
                    const int ch = Take();
                    if (ch == EOF || ch == '[' || ch == '{' || ch == ']') Invalid("Invalid JSON scalar.");
                    out.push_back(static_cast<char>(ch));
                }
                if (out.empty()) Invalid("Missing JSON value.");
                if (out != "null" && out != "true" && out != "false")
                {
                    // Export numbers are unsigned integers, including unknown numeric columns.
                    std::uint64_t ignored = 0;
                    const auto parsed = std::from_chars(out.data(), out.data() + out.size(), ignored);
                    if (parsed.ec != std::errc{} || parsed.ptr != out.data() + out.size()) Invalid("Invalid JSON number.");
                    if (out.size() > 1 && out[0] == '0') Invalid("Leading zero in JSON number.");
                }
                return FromUtf8(out);
            }

        public:
            JsonReader(std::istream& in, const Columns& names, Builder& dest)
                : input(in.rdbuf()), aliases(names), builder(dest) {}
            void Read()
            {
                Expect('[');
                Whitespace();
                if (Peek() == ']') { Take(); return; }
                for (;;)
                {
                    Expect('{');
                    std::array<std::wstring, 4> fields;
                    std::array<bool, 4> present{};
                    std::optional<std::uint64_t> fileCount;
                    std::unordered_set<std::wstring> keys;
                    Whitespace();
                    if (Peek() != '}') for (;;)
                    {
                        const auto key = String();
                        if (!keys.insert(key).second) Invalid("Duplicate JSON object key.");
                        Expect(':');
                        auto value = Value();
                        if (const int index = Field(key, aliases); index >= 0)
                        {
                            if (index == 4)
                            {
                                if (fileCount) Invalid("Duplicate file count column.");
                                fileCount = Number(value);
                            }
                            else
                            {
                                if (present[index]) Invalid("Two columns map to the same required field.");
                                present[index] = true;
                                fields[index] = std::move(value);
                            }
                        }
                        Whitespace();
                        if (Peek() != ',') break;
                        Take();
                    }
                    Expect('}');
                    builder.Row(fields, present, fileCount);
                    Whitespace();
                    if (Peek() == ']') { Take(); break; }
                    Expect(',');
                }
                Whitespace();
                if (Peek() != EOF) Invalid("Unexpected data after JSON snapshot.");
            }
        };

        bool CsvRow(std::istream& in, std::vector<std::wstring>& row)
        {
            row.clear();
            auto* input = in.rdbuf();
            if (input->sgetc() == EOF) return false;
            for (;;)
            {
                std::string cell;
                const bool quoted = input->sgetc() == '"';
                if (quoted)
                {
                    input->sbumpc();
                    for (;;)
                    {
                        const int ch = input->sbumpc();
                        if (ch == EOF) Invalid("Unclosed CSV quoted field.");
                        if (ch != '"') { cell.push_back(static_cast<char>(ch)); continue; }
                        if (input->sgetc() != '"') break;
                        input->sbumpc();
                        cell.push_back('"');
                    }
                }
                else
                {
                    while (input->sgetc() != ',' && input->sgetc() != '\r' && input->sgetc() != '\n' && input->sgetc() != EOF)
                    {
                        const int ch = input->sbumpc();
                        if (ch == '"') Invalid("Quote inside unquoted CSV field.");
                        cell.push_back(static_cast<char>(ch));
                    }
                }
                row.push_back(FromUtf8(cell));
                const int separator = input->sbumpc();
                if (separator == ',') continue;
                if (separator == '\r')
                {
                    if (input->sgetc() == '\n') input->sbumpc();
                    return true;
                }
                if (separator == '\n' || separator == EOF) return true;
                Invalid("Unexpected data after CSV quoted field.");
            }
        }
    }

    std::wstring FromUtf8(std::string_view text)
    {
        std::wstring out;
        out.reserve(text.size());
        for (std::size_t pos = 0; pos < text.size();)
        {
            const auto first = static_cast<unsigned char>(text[pos++]);
            std::uint32_t cp = first;
            unsigned remaining = 0;
            std::uint32_t minimum = 0;
            if (first >= 0xC2 && first <= 0xDF) { remaining = 1; cp &= 31; minimum = 0x80; }
            else if (first >= 0xE0 && first <= 0xEF) { remaining = 2; cp &= 15; minimum = 0x800; }
            else if (first >= 0xF0 && first <= 0xF4) { remaining = 3; cp &= 7; minimum = 0x10000; }
            else if (first >= 0x80) Invalid("Invalid UTF-8 in snapshot.");
            for (unsigned i = 0; i < remaining; ++i)
            {
                if (pos == text.size()) Invalid("Truncated UTF-8 in snapshot.");
                const auto next = static_cast<unsigned char>(text[pos++]);
                if ((next & 0xC0) != 0x80) Invalid("Invalid UTF-8 continuation.");
                cp = (cp << 6) | (next & 63);
            }
            if (cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) Invalid("Invalid UTF-8 codepoint.");
            if constexpr (sizeof(wchar_t) == 2)
            {
                if (cp >= 0x10000)
                {
                    cp -= 0x10000;
                    out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
                    out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 1023)));
                    continue;
                }
            }
            out.push_back(static_cast<wchar_t>(cp));
        }
        return out;
    }

    std::string ToUtf8(std::wstring_view text)
    {
        std::string out;
        out.reserve(text.size());
        for (std::size_t pos = 0; pos < text.size(); ++pos)
        {
            auto cp = static_cast<std::uint32_t>(text[pos]);
            if constexpr (sizeof(wchar_t) == 2)
            {
                if (cp >= 0xD800 && cp <= 0xDBFF)
                {
                    if (++pos == text.size()) Invalid("Missing UTF-16 low surrogate.");
                    const auto low = static_cast<std::uint32_t>(text[pos]);
                    if (low < 0xDC00 || low > 0xDFFF) Invalid("Invalid UTF-16 low surrogate.");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + low - 0xDC00;
                }
            }
            AppendUtf8(out, cp);
        }
        return out;
    }

    std::wstring NormalizePath(std::wstring_view path)
    {
        if (path.empty() || path.find(L'\0') != std::wstring_view::npos) Invalid("Empty path or NUL in path.");
        std::wstring out(path);
        std::replace(out.begin(), out.end(), L'/', L'\\');
        if (out.compare(0, 8, L"\\\\?\\UNC\\") == 0 || out.compare(0, 8, L"\\\\?\\unc\\") == 0) out = L"\\\\" + out.substr(8);
        else if (out.compare(0, 4, L"\\\\?\\") == 0) out.erase(0, 4);
        while (out.size() > 3 && out.back() == L'\\') out.pop_back();
        if (out.size() == 2 && out[1] == L':') out += L'\\';
        for (std::size_t pos = 0; pos < out.size();)
        {
            const auto end = out.find(L'\\', pos);
            const auto component = std::wstring_view(out).substr(pos, end == std::wstring::npos ? out.size() - pos : end - pos);
            if (component == L"." || component == L"..") Invalid("Relative path components in snapshot.");
            if (end == std::wstring::npos) break;
            if (end == pos && pos > 1) Invalid("Empty path component in snapshot.");
            pos = end + 1;
        }
#ifdef _WIN32
        if (out.size() > INT_MAX) Invalid("Snapshot path too long.");
        const auto length = static_cast<int>(out.size());
        const int needed = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE,
            out.data(), length, nullptr, 0, nullptr, nullptr, 0);
        if (needed <= 0) Invalid("Cannot normalize snapshot path.");
        std::wstring folded(static_cast<std::size_t>(needed), L'\0');
        if (!LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE,
            out.data(), length, folded.data(), needed, nullptr, nullptr, 0)) Invalid("Cannot normalize snapshot path.");
        return folded;
#else
        // Deterministic portable test fallback. Production uses Windows invariant Unicode folding.
        for (auto& ch : out)
        {
            if (ch >= L'A' && ch <= L'Z') ch += 32;
            else if (ch >= 0x0410 && ch <= 0x042F) ch += 32;
            else if (ch == 0x0401) ch = 0x0451;
        }
        return out;
#endif
    }

    std::wstring ParentPath(std::wstring_view path)
    {
        if (path.size() == 3 && path[1] == L':') return {};
        const auto pos = path.rfind(L'\\');
        if (pos == std::wstring_view::npos) return {};
        if (pos == 2 && path[1] == L':') return std::wstring(path.substr(0, 3));
        if (path.substr(0, 2) == L"\\\\" && path.find(L'\\', 2) == pos) return {};
        return std::wstring(path.substr(0, pos));
    }

    void Totals::Add(Kind kind, std::uint64_t magnitude)
    {
        const auto index = static_cast<std::size_t>(kind);
        bytes[index] = CheckedAdd(bytes[index], magnitude);
        files[index] = CheckedAdd(files[index], 1);
    }
    void Totals::Merge(const Totals& other)
    {
        for (std::size_t i = 0; i < bytes.size(); ++i)
        {
            bytes[i] = CheckedAdd(bytes[i], other.bytes[i]);
            files[i] = CheckedAdd(files[i], other.files[i]);
        }
    }
    std::uint64_t Totals::Magnitude() const { return CheckedAdd(Increase(), Decrease()); }
    std::uint64_t Totals::Increase() const { return CheckedAdd(bytes[0], bytes[2]); }
    std::uint64_t Totals::Decrease() const { return CheckedAdd(bytes[1], bytes[3]); }

    Snapshot ReadSnapshot(std::istream& input, const Columns& aliases, const CancelCheck& cancelled)
    {
        CheckCancellation(cancelled);
        if (input.peek() == 0xEF)
        {
            if (input.get() != 0xEF || input.get() != 0xBB || input.get() != 0xBF) Invalid("Invalid UTF-8 BOM.");
        }
        Builder builder(cancelled);
        if (input.peek() == '[' || input.peek() == ' ' || input.peek() == '\t' || input.peek() == '\r' || input.peek() == '\n')
        {
            // Whitespace before either format is permitted; export headers never begin with it.
            while (input.peek() == ' ' || input.peek() == '\t' || input.peek() == '\r' || input.peek() == '\n') input.get();
        }
        if (input.peek() == '[') JsonReader(input, aliases, builder).Read();
        else
        {
            std::vector<std::wstring> header, row;
            if (!CsvRow(input, header)) Invalid("Empty snapshot.");
            std::array<std::size_t, 5> order;
            order.fill(std::numeric_limits<std::size_t>::max());
            for (std::size_t i = 0; i < header.size(); ++i)
                if (const int field = Field(header[i], aliases); field >= 0)
                {
                    if (order[field] != std::numeric_limits<std::size_t>::max()) Invalid("Duplicate required CSV column.");
                    order[field] = i;
                }
            if (std::find(order.begin(), order.begin() + 4, std::numeric_limits<std::size_t>::max()) != order.begin() + 4)
                throw Error(ErrorCode::MissingColumns, "Missing required WinDirStat export columns.");
            while (CsvRow(input, row))
            {
                if (row.size() == 1 && row[0].empty()) continue;
                if (row.size() != header.size()) Invalid("Truncated or inconsistent CSV row.");
                std::array<std::wstring, 4> fields;
                for (std::size_t i = 0; i < fields.size(); ++i) fields[i] = std::move(row[order[i]]);
                const auto fileCount = order[4] == std::numeric_limits<std::size_t>::max() ?
                    std::optional<std::uint64_t>{} : std::optional<std::uint64_t>{Number(row[order[4]])};
                builder.Row(fields, {true, true, true, true}, fileCount);
            }
        }
        if (input.bad()) Invalid("Failed while reading snapshot.");
        return builder.Finish();
    }

    Snapshot LoadSnapshot(const std::filesystem::path& path, const Columns& aliases, const CancelCheck& cancelled)
    {
        std::vector<char> buffer(1024 * 1024);
        std::ifstream input;
        input.rdbuf()->pubsetbuf(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        input.open(path, std::ios::binary);
        if (!input) throw Error(ErrorCode::CannotOpen, "Cannot open snapshot file.");
        return ReadSnapshot(input, aliases, cancelled);
    }

    Result Compare(const Snapshot& before, const Snapshot& after, Metric metric, const CancelCheck& cancelled)
    {
        if (before.roots != after.roots)
            throw Error(ErrorCode::DifferentRoots, "The snapshots must cover the same scan roots.");
        Result result;
        result.metric = metric;
        std::size_t visited = 0;
        const auto add = [&](std::wstring path, Sizes oldSize, Sizes newSize, Kind kind)
        {
            const auto a = oldSize.Get(metric), b = newSize.Get(metric);
            const auto magnitude = a > b ? a - b : b - a;
            result.totals.Add(kind, magnitude);
            result.changes.push_back({std::move(path), oldSize, newSize, kind, magnitude});
        };
        for (const auto& entry : after.files)
        {
            if ((++visited & 4095) == 0) CheckCancellation(cancelled);
            const auto old = before.files.find(entry.first);
            if (old == before.files.end()) add(entry.second.path, {}, entry.second.sizes, Kind::Added);
            else
            {
                const auto a = old->second.sizes.Get(metric), b = entry.second.sizes.Get(metric);
                if (a != b) add(entry.second.path, old->second.sizes, entry.second.sizes, b > a ? Kind::Grown : Kind::Shrunk);
            }
        }
        for (const auto& entry : before.files)
        {
            if ((++visited & 4095) == 0) CheckCancellation(cancelled);
            if (!after.files.count(entry.first)) add(entry.second.path, entry.second.sizes, {}, Kind::Removed);
        }
        // Both signs are retained even when a directory's net change is zero.
        (void)result.totals.Magnitude();
        CheckCancellation(cancelled);
        return result;
    }
}
