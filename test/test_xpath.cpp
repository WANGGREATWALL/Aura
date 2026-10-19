#if ENABLE_TEST_XPATH

#include <string>

#include "gtest/gtest.h"
#include "file/xpath.h"

namespace {

#ifdef _WIN32
const char kSeparator = '\\';
const char* const kRoot = "C:\\";
#else
const char kSeparator = '/';
const char* const kRoot = "/";
#endif

const std::string kSep(1, kSeparator);

}  // namespace

TEST(XPath, ConstructionAndDecomposition) {
    const au::file::XPath path("dir//sub///file.tar.gz");
    EXPECT_EQ(path.string(), "dir" + kSep + "sub" + kSep + "file.tar.gz");
    EXPECT_STREQ(path.c_str(), path.string().c_str());
    EXPECT_EQ(static_cast<std::string>(path), path.string());
    EXPECT_EQ(path.filename().string(), "file.tar.gz");
    EXPECT_EQ(path.stem().string(), "file.tar");
    EXPECT_EQ(path.extension().string(), ".gz");
    EXPECT_EQ(path.parent().string(), "dir" + kSep + "sub" + kSep);

    EXPECT_EQ(au::file::XPath("file").parent().string(), "." + kSep);
    EXPECT_EQ(au::file::XPath("dir/sub/").parent().string(), "dir" + kSep + "sub" + kSep);
    EXPECT_EQ(au::file::XPath("dir/sub/").filename().string(), "sub");
    EXPECT_TRUE(au::file::XPath(".hidden").extension().isEmpty());
    EXPECT_EQ(au::file::XPath(".hidden").stem().string(), ".hidden");
    EXPECT_TRUE(au::file::XPath(nullptr).isEmpty());
}

TEST(XPath, PredicatesAndTransformations) {
    EXPECT_TRUE(au::file::XPath().isEmpty());
    EXPECT_FALSE(au::file::XPath("folder").isDirectory());
    EXPECT_TRUE(au::file::XPath("folder/").isDirectory());
    EXPECT_FALSE(au::file::XPath("folder").isAbsolute());
    EXPECT_TRUE(au::file::XPath(kRoot).isAbsolute());
    EXPECT_TRUE(au::file::XPath(kRoot).isRoot());
    EXPECT_FALSE(au::file::XPath("folder/").isRoot());

    const au::file::XPath folder("folder");
    EXPECT_EQ(folder.withTrailingSeparator().string(), "folder" + kSep);
    EXPECT_EQ(au::file::XPath("folder/").withoutTrailingSeparator().string(), "folder");
    EXPECT_EQ(folder.string(), "folder");

    const au::file::XPath file("archive.tar.GZ");
    EXPECT_EQ(file.withoutExtension("gz").string(), "archive.tar");
    EXPECT_EQ(file.withoutExtension(".GZ").string(), "archive.tar");
    EXPECT_EQ(file.withoutExtension("zip").string(), "archive.tar.GZ");
    EXPECT_EQ(file.replaceExtension("zip").string(), "archive.tar.zip");
    EXPECT_EQ(au::file::XPath("file").replaceExtension(".txt").string(), "file.txt");
    EXPECT_EQ(file.string(), "archive.tar.GZ");
}

TEST(XPath, JoinAndMakeFilename) {
    const au::file::XPath base("base");
    const au::file::XPath path = base / "sub" / "file.txt";
    EXPECT_EQ(path.string(), "base" + kSep + "sub" + kSep + "file.txt");
    EXPECT_EQ(base.string(), "base");
    EXPECT_EQ(au::file::XPath::join(base, au::file::XPath("leaf")).string(), "base" + kSep + "leaf");
    EXPECT_EQ((base / std::string("leaf")).string(), "base" + kSep + "leaf");
    EXPECT_EQ(au::file::XPath::join(au::file::XPath(), au::file::XPath("leaf")).string(), "leaf");
    EXPECT_TRUE(path == au::file::XPath(path.string()));
    EXPECT_TRUE(path != base);

    EXPECT_EQ(au::file::XPath::makeFilename(au::file::XPath("out"), au::file::XPath("image"), 0, "png").string(),
              "out" + kSep + "image.png");
    EXPECT_EQ(au::file::XPath::makeFilename(au::file::XPath("out"), au::file::XPath("image"), 3, "png").string(),
              "out" + kSep + "image_3.png");
    EXPECT_EQ(au::file::XPath::makeFilename("out", "image", "png").string(),
              "out" + kSep + "image.png");
}

TEST(XPath, RegexAndImageSize) {
    const au::file::XPath path("dir/img_128x256_640x480.jpg");
    EXPECT_EQ(path.firstMatch(R"([0-9]+x[0-9]+)"), "128x256");
    EXPECT_EQ(path.lastMatch(R"([0-9]+x[0-9]+)"), "640x480");
    EXPECT_EQ(path.firstMatch("missing"), "");
    EXPECT_EQ(path.lastMatch("missing"), "");

    const au::file::XPath::ImageSize first = path.firstImageSize();
    const au::file::XPath::ImageSize last = path.lastImageSize();
    EXPECT_EQ(first.width, 128u);
    EXPECT_EQ(first.height, 256u);
    EXPECT_EQ(last.width, 640u);
    EXPECT_EQ(last.height, 480u);
    EXPECT_EQ(path.stemBeforeFirstSize(), "img");
    EXPECT_EQ(path.stemBeforeLastSize(), "img_128x256");

    const au::file::XPath noSize("dir_999x111/plain.jpg");
    EXPECT_EQ(noSize.firstMatch(R"([0-9]+x[0-9]+)"), "999x111");
    EXPECT_EQ(noSize.firstImageSize().width, 0u);
    EXPECT_EQ(noSize.lastImageSize().height, 0u);
    EXPECT_EQ(noSize.stemBeforeFirstSize(), "plain");
    EXPECT_EQ(noSize.stemBeforeLastSize(), "plain");
}

#endif  // ENABLE_TEST_XPATH
