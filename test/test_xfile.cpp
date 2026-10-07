#if ENABLE_TEST_XFILE

#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#include "gtest/gtest.h"
#include "file/xfile.h"
#include "log/xerror.h"

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory()
    {
        std::error_code ec;
        const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
        if (ec) return;

        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            mPath = base / ("aura_xfile_" + std::to_string(nonce) + "_" + std::to_string(attempt));
            if (std::filesystem::create_directory(mPath, ec)) return;
            if (ec) break;
        }
        mPath.clear();
    }

    ~TemporaryDirectory() { cleanup(); }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    TemporaryDirectory(TemporaryDirectory&& other) noexcept : mPath(std::move(other.mPath))
    {
        other.mPath.clear();
    }

    TemporaryDirectory& operator=(TemporaryDirectory&& other) noexcept
    {
        if (this != &other) {
            cleanup();
            mPath = std::move(other.mPath);
            other.mPath.clear();
        }
        return *this;
    }

    bool valid() const { return !mPath.empty(); }
    std::string path() const { return mPath.u8string(); }
    std::string child(const char* name) const { return (mPath / name).u8string(); }

private:
    void cleanup() noexcept
    {
        if (mPath.empty()) return;
        std::error_code ec;
        std::filesystem::remove_all(mPath, ec);
    }

    std::filesystem::path mPath;
};

}  // namespace

TEST(XFile, ExistenceAndCreation)
{
    TemporaryDirectory dir;
    ASSERT_TRUE(dir.valid());

    EXPECT_TRUE(au::file::exists(dir.path()));
    EXPECT_TRUE(au::file::existDir(dir.path()));
    EXPECT_FALSE(au::file::existFile(dir.path()));
    EXPECT_FALSE(au::file::exists(dir.child("missing")));
    EXPECT_EQ(au::file::sizeOf(dir.child("missing")), 0u);

    ASSERT_EQ(au::file::createDir(dir.child("single")), au::err::kSuccess);
    EXPECT_EQ(au::file::createDir(dir.child("single")), au::err::kSuccess);
    ASSERT_EQ(au::file::createDirs(dir.child("nested/deep")), au::err::kSuccess);
    EXPECT_EQ(au::file::createDirs(dir.child("nested/deep")), au::err::kSuccess);
    EXPECT_TRUE(au::file::existDir(dir.child("nested/deep")));
    EXPECT_NE(au::file::createDir(dir.child("absent/child")), au::err::kSuccess);

    const std::string file = dir.child("created.txt");
    ASSERT_EQ(au::file::createFile(file), au::err::kSuccess);
    EXPECT_TRUE(au::file::exists(file));
    EXPECT_TRUE(au::file::existFile(file));
    EXPECT_FALSE(au::file::existDir(file));
    EXPECT_EQ(au::file::sizeOf(file), 0u);
    ASSERT_EQ(au::file::write(std::string("data"), file), au::err::kSuccess);
    EXPECT_EQ(au::file::sizeOf(file), 4u);
    ASSERT_EQ(au::file::createFile(file), au::err::kSuccess);
    EXPECT_EQ(au::file::sizeOf(file), 0u);
    EXPECT_EQ(au::file::removeFile(file), au::err::kSuccess);
    EXPECT_FALSE(au::file::exists(file));
    EXPECT_EQ(au::file::removeFile(file), au::err::kErrorFileNotFound);
}

TEST(XFile, ReadWriteAndAppend)
{
    TemporaryDirectory dir;
    ASSERT_TRUE(dir.valid());
    const std::string file = dir.child("content.bin");
    const std::string content("A\0B", 3);

    ASSERT_EQ(au::file::write(content, file), au::err::kSuccess);
    EXPECT_EQ(au::file::sizeOf(file), content.size());

    std::string text = "stale";
    ASSERT_EQ(au::file::read(file, text), au::err::kSuccess);
    EXPECT_EQ(text, content);

    char buffer[3] = {};
    ASSERT_EQ(au::file::read(file, buffer, sizeof(buffer)), au::err::kSuccess);
    EXPECT_EQ(std::string(buffer, sizeof(buffer)), content);

    char partial[2] = {};
    ASSERT_EQ(au::file::readAt(file, 1, partial, sizeof(partial)), au::err::kSuccess);
    EXPECT_EQ(std::string(partial, sizeof(partial)), content.substr(1));

    ASSERT_EQ(au::file::append(std::string("C"), file), au::err::kSuccess);
    const char last = 'D';
    ASSERT_EQ(au::file::append(&last, 1, file), au::err::kSuccess);
    ASSERT_EQ(au::file::read(file, text), au::err::kSuccess);
    EXPECT_EQ(text, content + "CD");

    const char replacement[] = {'x', '\0', 'y'};
    ASSERT_EQ(au::file::write(replacement, sizeof(replacement), file), au::err::kSuccess);
    ASSERT_EQ(au::file::read(file, text), au::err::kSuccess);
    EXPECT_EQ(text, std::string(replacement, sizeof(replacement)));

    ASSERT_EQ(au::file::createFile(file), au::err::kSuccess);
    ASSERT_EQ(au::file::read(file, text), au::err::kSuccess);
    EXPECT_TRUE(text.empty());
}

TEST(XFile, ReadWriteErrors)
{
    TemporaryDirectory dir;
    ASSERT_TRUE(dir.valid());
    const std::string file = dir.child("content.txt");
    const std::string missing = dir.child("missing.txt");
    ASSERT_EQ(au::file::write(std::string("abc"), file), au::err::kSuccess);

    char buffer[3] = {};
    std::string text;
    EXPECT_EQ(au::file::read(missing, text), au::err::kErrorFileNotFound);
    EXPECT_EQ(au::file::read(file, buffer, 2), au::err::kErrorFileSizeMismatch);
    EXPECT_EQ(au::file::readAt(file, 2, buffer, 2), au::err::kErrorOutOfRange);
    EXPECT_EQ(au::file::read(file, nullptr, 3), au::err::kErrorNullPointer);
    EXPECT_EQ(au::file::readAt(file, 0, nullptr, 1), au::err::kErrorNullPointer);
    EXPECT_EQ(au::file::write(nullptr, 1, file), au::err::kErrorNullPointer);
    EXPECT_EQ(au::file::append(nullptr, 1, file), au::err::kErrorNullPointer);
    EXPECT_EQ(au::file::createDir(std::string()), au::err::kErrorInvalidParam);
    EXPECT_EQ(au::file::createDirs(std::string()), au::err::kErrorInvalidParam);
}

TEST(XFile, DirectoryListing)
{
    TemporaryDirectory dir;
    ASSERT_TRUE(dir.valid());
    ASSERT_EQ(au::file::write(std::string("a"), dir.child("a.txt")), au::err::kSuccess);
    ASSERT_EQ(au::file::write(std::string("b"), dir.child("b.log")), au::err::kSuccess);
    ASSERT_EQ(au::file::createDirs(dir.child("sub/nested")), au::err::kSuccess);

    const auto files = au::file::listFiles(dir.path(), "", false);
    ASSERT_EQ(files.size(), 2u);
    bool foundText = false;
    bool foundLog = false;
    for (const au::file::XPath& entry : files) {
        foundText |= entry.string() == "a.txt";
        foundLog |= entry.string() == "b.log";
    }
    EXPECT_TRUE(foundText);
    EXPECT_TRUE(foundLog);

    const auto filtered = au::file::listFiles(dir.path(), R"(\.txt$)", true);
    ASSERT_EQ(filtered.size(), 1u);
    EXPECT_EQ(filtered[0].string(), (au::file::XPath(dir.path()) / "a.txt").string());

    const auto subdirs = au::file::listDirs(dir.path(), "", false);
    ASSERT_EQ(subdirs.size(), 1u);
    EXPECT_EQ(subdirs[0].string(), "sub");
    EXPECT_EQ(au::file::listDirs(dir.path(), "^sub$", false).size(), 1u);
    EXPECT_TRUE(au::file::listFiles(dir.path(), "missing").empty());
    EXPECT_TRUE(au::file::listFiles(dir.child("absent")).empty());
}

#endif  // ENABLE_TEST_XFILE
