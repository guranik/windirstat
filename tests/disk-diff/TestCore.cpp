// SPDX-License-Identifier: GPL-3.0-or-later
#include "SnapshotDiffCore.h"
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>

using namespace diskdiff;
namespace
{
    int checks = 0;
    void Require(bool condition, const char* message)
    {
        ++checks;
        if (!condition) throw std::runtime_error(message);
    }
    template<class F> void Fails(ErrorCode code, F&& operation)
    {
        try { operation(); }
        catch (const Error& e) { Require(e.code == code, "Unexpected error code"); return; }
        throw std::runtime_error("Invalid snapshot/comparison was accepted");
    }
    Snapshot Parse(const std::string& text)
    {
        std::istringstream input(text);
        return ReadSnapshot(input);
    }
    std::string Row(const std::string& path, std::uint64_t logical, std::uint64_t physical, const std::string& type)
    {
        std::ostringstream out;
        out << "{\"Name\":" << std::quoted(path) << ",\"Logical Size\":" << logical
            << ",\"Physical Size\":" << physical << ",\"WinDirStat Attributes\":" << std::quoted(type) << "}";
        return out.str();
    }
    const std::string root = Row("C:\\Test", 0, 0, "0x30000004");
    std::string OneFile(const std::string& name, std::uint64_t logical = 100, std::uint64_t physical = 4096)
    {
        return "[" + root + "," + Row("C:\\Test\\" + name, logical, physical, "0x20000008") + "]";
    }

    void TestFixtures(const std::filesystem::path& fixtures)
    {
        const auto a = LoadSnapshot(fixtures / "before-en.json");
        const auto b = LoadSnapshot(fixtures / "after-ru.json");
        const auto csvA = LoadSnapshot(fixtures / "before-en.csv");
        const auto csvB = LoadSnapshot(fixtures / "after-ru.csv");
        const auto diff = Compare(a, b, Metric::Logical);
        Require(diff.changes.size() == 5, "Expected four sized changes plus one zero-byte addition");
        Require(diff.totals.bytes == std::array<std::uint64_t, 4>{200, 1000, 800, 500}, "Incorrect fixture deltas");
        Require(diff.totals.files == std::array<std::uint64_t, 4>{2, 1, 1, 1}, "Incorrect fixture class counts");
        Require(diff.totals.Magnitude() == 2500, "Map area must represent gross change, not net change");
        Require(diff.totals.Increase() == 1000 && diff.totals.Decrease() == 1500, "Incorrect growth/contraction");
        const auto csvDiff = Compare(csvA, csvB, Metric::Logical);
        Require(csvDiff.totals.bytes == diff.totals.bytes && csvDiff.totals.files == diff.totals.files, "CSV/JSON disagree");
        const auto reverse = Compare(b, a, Metric::Logical);
        Require(reverse.totals.bytes == std::array<std::uint64_t, 4>{1000, 200, 500, 800}, "Swapping A/B did not invert signs");
        Require(Compare(a, a, Metric::Logical).changes.empty(), "Identical snapshots should be empty");
        const auto physical = Compare(a, b, Metric::Physical);
        Require(physical.totals.bytes == std::array<std::uint64_t, 4>{4096, 4096, 4096, 4096}, "Incorrect allocated-size deltas");
        Require(physical.totals.Increase() == physical.totals.Decrease() && physical.totals.Magnitude() > 0,
            "Opposing changes must remain visible when net delta is zero");
        std::cout << "Fixture comparison (EN/RU, CSV/JSON, four classes, zero-byte files): passed\n";
    }

    void TestParsing()
    {
        Require(NormalizePath(L"\\\\?\\C:\\Test\\FILE.txt") == L"c:\\test\\file.txt", "Long path/case normalization");
        Require(NormalizePath(L"\\\\?\\UNC\\SERVER\\Share\\X") == L"\\\\server\\share\\x", "UNC normalization");
        Require(NormalizePath(L"C:/Test/") == L"c:\\test", "Slash normalization");
        Require(ParentPath(L"c:\\a") == L"c:\\" && ParentPath(L"c:\\").empty(), "Drive parent paths");
        Require(ParentPath(L"\\\\server\\share").empty(), "UNC share has no filesystem parent");
        const auto unicode = FromUtf8("Кэш/😀.txt");
        Require(ToUtf8(unicode) == "Кэш/😀.txt", "UTF-8/UTF-16 roundtrip");
        auto escaped = OneFile("plain.txt");
        const auto pos = escaped.find("plain.txt");
        escaped.replace(pos, 9, "\\u041A\\u044D\\u0448\\uD83D\\uDE00.txt");
        Require(Parse(escaped).files.begin()->second.path == L"C:\\Test\\" + FromUtf8("Кэш😀.txt"), "JSON Unicode escapes");
        Require(Compare(Parse(OneFile("FILE.bin")), Parse(OneFile("file.bin")), Metric::Logical).changes.empty(), "Case-only path changes");
        Require(Compare(Parse(OneFile("a", 100, 4096)), Parse(OneFile("a", 100, 8192)), Metric::Logical).changes.empty(), "Wrong metric used");
        Require(Compare(Parse(OneFile("a", 100, 4096)), Parse(OneFile("a", 100, 8192)), Metric::Physical).totals.bytes[2] == 4096,
            "Allocated-only growth missed");
        const auto quoted = Parse("\xEF\xBB\xBF\"Name\",\"Logical Size\",\"Physical Size\",\"WinDirStat Attributes\"\r\n"
            "\"C:\\Test\",0,0,0x30000004\r\n\"C:\\Test\\a,b.txt\",12,16,0x20000008\r\n");
        Require(quoted.files.size() == 1 && quoted.files.begin()->second.path == L"C:\\Test\\a,b.txt", "Quoted CSV comma");
        const auto csvQuote = Parse("Name,Logical Size,Physical Size,WinDirStat Attributes\n"
            "C:\\Test,0,0,0x30000004\n\"C:\\Test\\a\"\"b.txt\",12,16,0x20000008\n");
        Require(csvQuote.files.begin()->second.path == L"C:\\Test\\a\"b.txt", "CSV escaped quote");
        const auto pseudo = Parse("[" + Row("C:\\Test", 1100, 1100, "0x30000004") + "," + Row("C:\\Test\\<Free Space>", 1000, 1000, "0x20000010") +
            "," + Row("C:\\Test\\<Unknown>", 100, 100, "0x20000020") + "]");
        Require(pseudo.files.empty(), "Free/unknown pseudo-files must not appear as file changes");
        Require(pseudo.folders.at(L"c:\\test").sizes.physical == 0, "Free/unknown space leaked into folder size");
        const auto multi = Parse("[" + Row("This PC", 0, 0, "0x30000001") + "," +
            Row("D:\\Second", 0, 0, "0x20000004") + "," + Row("C:\\Test", 0, 0, "0x20000004") + "]");
        Require(multi.roots == std::vector<std::wstring>{L"c:\\test", L"d:\\second"}, "Multiple roots/order normalization");
        const auto fileToFolder = Parse("[" + root + "," + Row("C:\\Test\\a", 0, 0, "0x20000004") + "," +
            Row("C:\\Test\\a\\child", 10, 16, "0x20000008") + "]");
        const auto replacement = Compare(Parse(OneFile("a")), fileToFolder, Metric::Logical);
        Require(replacement.totals.files[0] == 1 && replacement.totals.files[1] == 1, "File-to-directory replacement");
        std::cout << "Paths, Unicode, quoting, pseudo-files, roots and metric selection: passed\n";
    }

    void TestFailures()
    {
        Fails(ErrorCode::InvalidFormat, [] { Parse(OneFile("a").substr(0, OneFile("a").size() - 1)); });
        Fails(ErrorCode::InvalidFormat, [] { Parse(OneFile("a") + "junk"); });
        Fails(ErrorCode::InvalidFormat, [] { Parse("[" + root + ",]"); });
        Fails(ErrorCode::InvalidFormat, [] { Parse("[" + root + "," + Row("C:\\Test\\a", 0, 0, "not-hex") + "]"); });
        Fails(ErrorCode::MissingColumns, [] { Parse("[{\"Name\":\"C:\\\\Test\"}]"); });
        Fails(ErrorCode::InvalidFormat, [] { Parse("Name,Logical Size,Physical Size,WinDirStat Attributes\nC:\\Test,0,0,0x30000004\nC:\\Test\\a,12\n"); });
        Fails(ErrorCode::InvalidFormat, [] { Parse("Name,Logical Size,Physical Size,WinDirStat Attributes\n\"C:\\Test,0,0,0x30000004"); });
        Fails(ErrorCode::DuplicatePath, [] { Parse("[" + root + "," + Row("C:\\Test\\A", 1, 1, "0x20000008") +
            "," + Row("c:\\test\\a", 1, 1, "0x20000008") + "]"); });
        Fails(ErrorCode::InvalidHierarchy, [] { Parse(OneFile("missing\\a")); });
        Fails(ErrorCode::InvalidHierarchy, [] { Parse("Name,Files,Logical Size,Physical Size,WinDirStat Attributes\n"
            "C:\\Test,2,0,0,0x30000004\nC:\\Test\\a,0,12,16,0x20000008\n"); });
        Fails(ErrorCode::InvalidHierarchy, [] { Parse("[" + Row("This PC", 0, 0, "0x30000001") + "," +
            Row("C:\\a", 0, 0, "0x20000004") + "," + Row("C:\\a-", 0, 0, "0x20000004") + "," +
            Row("C:\\a\\missing\\sub", 0, 0, "0x20000004") + "]"); });
        Fails(ErrorCode::InvalidFormat, [] { Parse("[" + Row("C:\\Test", 0, 0, "0x10000004") + "]"); });
        Fails(ErrorCode::UnsupportedRoot, [] { Parse("[" + Row("C:\\Test", 0, 0, "0xB0000004") + "]"); });
        Fails(ErrorCode::InvalidFormat, [] { (void)NormalizePath(L"C:\\Test\\..\\a"); });
        Fails(ErrorCode::InvalidFormat, [] { (void)FromUtf8("\xC0\x80"); });
        Fails(ErrorCode::InvalidFormat, [] { (void)FromUtf8("\xF4\x90\x80\x80"); });
        Fails(ErrorCode::InvalidFormat, [] { auto text = OneFile("a"); text.replace(text.find("a\""), 1, "\\uD800"); Parse(text); });
        Fails(ErrorCode::InvalidFormat, [] { auto text = OneFile("a"); text.replace(text.find("a\""), 1, "\\u0000"); Parse(text); });
        Fails(ErrorCode::InvalidFormat, [] { auto text = OneFile("a"); text.replace(text.find("100"), 3, "-100"); Parse(text); });
        Fails(ErrorCode::InvalidFormat, [] { auto text = OneFile("a"); text.replace(text.find("100"), 3, "18446744073709551616"); Parse(text); });
        Fails(ErrorCode::DifferentRoots, [] { Compare(Parse("[" + root + "]"), Parse("[" + Row("D:\\Test", 0, 0, "0x30000004") + "]"), Metric::Logical); });
        Fails(ErrorCode::Cancelled, [] { std::istringstream input(OneFile("a")); ReadSnapshot(input, {}, [] { return true; }); });
        Fails(ErrorCode::Cancelled, [] { const auto a = Parse(OneFile("a")); Compare(a, a, Metric::Logical, [] { return true; }); });
        Fails(ErrorCode::Overflow, [] { Totals totals; totals.Add(Kind::Added, UINT64_MAX); totals.Add(Kind::Added, 1); });
        Fails(ErrorCode::Overflow, [] { Totals totals; totals.Add(Kind::Added, UINT64_MAX); totals.Add(Kind::Removed, 1); (void)totals.Magnitude(); });
        // Every incomplete prefix of a genuine export must be rejected.
        const auto valid = OneFile("prefix-test");
        for (std::size_t i = 0; i < valid.size(); ++i)
        {
            try { (void)Parse(valid.substr(0, i)); throw std::runtime_error("Truncated JSON prefix was accepted"); }
            catch (const Error&) { ++checks; }
        }
        std::cout << "Malformed/truncated input, duplicates, cancellation and overflow: passed\n";
    }

    void TestConservation()
    {
        std::mt19937 random(20261013);
        for (int trial = 0; trial < 20; ++trial)
        {
            Snapshot a, b;
            a.roots = b.roots = {L"c:\\test"};
            std::uint64_t totalA = 0, totalB = 0;
            std::array<std::uint64_t, 4> expected{};
            for (int i = 0; i < 2000; ++i)
            {
                const auto path = L"C:\\Test\\f" + std::to_wstring(i);
                const auto key = NormalizePath(path);
                const bool existsA = random() % 5 != 0, existsB = random() % 5 != 0;
                const std::uint64_t sizeA = random() % 100000, sizeB = random() % 100000;
                if (existsA) { a.files.emplace(key, Record{path, {sizeA, sizeA}}); totalA += sizeA; }
                if (existsB) { b.files.emplace(key, Record{path, {sizeB, sizeB}}); totalB += sizeB; }
                if (!existsA && existsB) expected[0] += sizeB;
                else if (existsA && !existsB) expected[1] += sizeA;
                else if (existsA && existsB && sizeB > sizeA) expected[2] += sizeB - sizeA;
                else if (existsA && existsB) expected[3] += sizeA - sizeB;
            }
            const auto result = Compare(a, b, Metric::Logical);
            Require(result.totals.bytes == expected, "Randomized join disagrees with independent reference");
            Require(totalB + result.totals.Decrease() == totalA + result.totals.Increase(), "Net byte conservation failed");
        }
        std::cout << "40,000 randomized file cases and byte conservation: passed\n";
    }

    void Benchmark(std::size_t count)
    {
        if (count < 20 || count > 2000000) throw std::runtime_error("Benchmark count must be 20..2000000");
        using Clock = std::chrono::steady_clock;
        const auto folder = std::filesystem::temp_directory_path() /
            ("windirstat-diff-bench-" + std::to_string(Clock::now().time_since_epoch().count()));
        std::filesystem::create_directory(folder);
        struct Cleanup { std::filesystem::path folder; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(folder, ec); } } cleanup{folder};
        const auto fileA = folder / "before.csv", fileB = folder / "after.csv";
        std::uint64_t totalB = 0, countB = 0;
        for (std::size_t i = 0; i < count; ++i)
            if (i % 20 != 0) { totalB += i % 20 == 1 ? 1088 : i % 20 == 2 ? 512 : 1024; ++countB; }
        totalB += (count / 20) * 1024;
        countB += count / 20;
        const std::string header = "Name,Files,Folders,Logical Size,Physical Size,Attributes,Last Change,WinDirStat Attributes,Index\n";
        {
            std::ofstream a(fileA), b(fileB);
            a << header << "C:\\Bench," << count << ",0," << count * 1024 << ',' << count * 4096 << ",D,2026-10-13T08:00:00Z,0x30000004,0x0\n";
            b << header << "C:\\Bench," << countB << ",0," << totalB << ',' << countB * 4096 << ",D,2026-10-13T08:00:00Z,0x30000004,0x0\n";
            for (std::size_t i = 0; i < count; ++i)
            {
                a << "C:\\Bench\\f" << i << ",0,0,1024,4096,A,2026-10-13T08:00:00Z,0x20000008,0x"
                    << std::hex << i + 1 << std::dec << '\n';
                if (i % 20 != 0) b << "C:\\Bench\\f" << i << ",0,0," << (i % 20 == 1 ? 1088 : i % 20 == 2 ? 512 : 1024)
                    << ",4096,A,2026-10-13T08:00:00Z,0x20000008,0x" << std::hex << i + 1 << std::dec << '\n';
            }
            for (std::size_t i = count; i < count + count / 20; ++i)
                b << "C:\\Bench\\f" << i << ",0,0,1024,4096,A,2026-10-13T08:00:00Z,0x20000008,0x"
                    << std::hex << i + 1 << std::dec << '\n';
        }
        const auto start = Clock::now();
        const auto a = LoadSnapshot(fileA);
        const auto loadedA = Clock::now();
        const auto b = LoadSnapshot(fileB);
        const auto loadedB = Clock::now();
        const auto diff = Compare(a, b, Metric::Logical);
        const auto end = Clock::now();
        Require(count * 1024 + diff.totals.Increase() == totalB + diff.totals.Decrease(), "Benchmark conservation failed");
        std::cout << std::fixed << std::setprecision(3) << "Benchmark " << count << " files per snapshot: load A "
            << std::chrono::duration<double>(loadedA - start).count() << " s; load B "
            << std::chrono::duration<double>(loadedB - loadedA).count() << " s; compare "
            << std::chrono::duration<double>(end - loadedB).count() << " s; changed " << diff.changes.size() << '\n';
    }
}

int main(int argc, char** argv)
{
    try
    {
        if (argc > 2 && std::string_view(argv[1]) == "--benchmark") Benchmark(std::stoull(argv[2]));
        else
        {
            TestFixtures(argc > 1 ? argv[1] : "tests/disk-diff/fixtures");
            TestParsing();
            TestFailures();
            TestConservation();
            std::cout << "All " << checks << " checks passed.\n";
        }
        return 0;
    }
    catch (const std::exception& error) { std::cerr << "FAILED: " << error.what() << '\n'; return 1; }
}
