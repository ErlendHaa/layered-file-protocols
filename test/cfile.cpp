#include <ciso646>

#include <catch2/catch.hpp>

#include <lfp/tapeimage.h>
#include <lfp/lfp.h>

#include "utils.hpp"


using namespace Catch::Matchers;

namespace {

struct random_cfile : random_memfile {
    random_cfile() {
        REQUIRE(not expected.empty());

        std::FILE* fp = std::tmpfile();
        std::fwrite(expected.data(), 1, expected.size(), fp);
        std::rewind(fp);

        lfp_close(f);
        f = nullptr;

        f = lfp_cfile(fp);
        REQUIRE(f);
    }
};

}

TEST_CASE(
    "File closes correctly",
    "[cfile][close]") {

    std::FILE* fp = std::tmpfile();
    std::fputs("Very simple file", fp);
    std::rewind(fp);

    auto* cfile = lfp_cfile(fp);

    auto err = lfp_close(cfile);
    CHECK(err == LFP_OK);
}

TEST_CASE(
    "Layered cfile closes correctly",
    "[cfile][close]") {
    const auto contents = std::vector< unsigned char > {
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x18, 0x00, 0x00, 0x00,

        0x01, 0x02, 0x03, 0x04,
        0x54, 0x41, 0x50, 0x45,
        0x4D, 0x41, 0x52, 0x4B,

        0x01, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x24, 0x00, 0x00, 0x00,
    };
    std::FILE* fp = std::tmpfile();
    std::fwrite(contents.data(), 1, contents.size(), fp);
    std::rewind(fp);

    auto* cfile = lfp_cfile(fp);
    auto* outer = lfp_tapeimage_open(cfile);

    auto err = lfp_close(outer);
    CHECK(err == LFP_OK);
}

TEST_CASE(
    "Layering non-existing file is a no-op",
    "[cfile][filehandle]") {

    FILE* fp    = nullptr;
    auto* cfile = lfp_cfile(fp);
    auto* tif   = lfp_tapeimage_open(cfile);

    CHECK(!cfile);
    CHECK(!tif);
}

TEST_CASE(
    "Unsupported peel leaves the protocol intact",
    "[cfile][peel]") {
    std::FILE* fp = std::tmpfile();
    std::fputs("Very simple file" , fp);
    std::rewind(fp);

    auto* cfile = lfp_cfile(fp);

    lfp_protocol* protocol;
    auto err = lfp_peel(cfile, &protocol);

    CHECK(err == LFP_LEAF_PROTOCOL);

    auto buffer = std::vector< unsigned char >(17, 0xFF);
    std::int64_t nread;
    err = lfp_readinto(cfile, buffer.data(), 17, &nread);

    CHECK(err == LFP_EOF);
    CHECK(nread == 16);

    err = lfp_close(cfile);
    CHECK(err == LFP_OK);
}

TEST_CASE(
    "Unsupported peek leaves the protocol intact",
    "[cfile][peek]") {
    std::FILE* fp = std::tmpfile();
    std::fputs("Very simple file" , fp);
    std::rewind(fp);

    auto* cfile = lfp_cfile(fp);

    lfp_protocol* protocol;
    auto err = lfp_peek(cfile, &protocol);

    CHECK(err == LFP_LEAF_PROTOCOL);

    auto buffer = std::vector< unsigned char >(17, 0xFF);
    std::int64_t nread;
    err = lfp_readinto(cfile, buffer.data(), 17, &nread);

    CHECK(err == LFP_EOF);
    CHECK(nread == 16);

    err = lfp_close(cfile);
    CHECK(err == LFP_OK);
}

TEST_CASE_METHOD(
    random_cfile,
    "Cfile can be read",
    "[cfile][read]") {

    SECTION( "full read" ) {
        std::int64_t nread = -1;
        const auto err = lfp_readinto(f, out.data(), out.size(), &nread);

        CHECK(err == LFP_OK);
        CHECK(nread == expected.size());
        CHECK_THAT(out, Equals(expected));
    }

    SECTION( "incomplete read" ) {
        std::int64_t nread = -1;
        const auto err = lfp_readinto(f, out.data(), 2*out.size(), &nread);

        CHECK(err == LFP_EOF);
        CHECK(nread == expected.size());
        CHECK_THAT(out, Equals(expected));
    }

    SECTION( "A file can be read in multiple, smaller reads" ) {
        test_split_read(this);
    }

    SECTION( "negative read" ) {
        std::int64_t nread = -1;
        const auto err = lfp_readinto(f, out.data(), -1, &nread);

        CHECK(err == LFP_INVALID_ARGS);
        auto msg = std::string(lfp_errormsg(f));
        CHECK_THAT(msg, Contains(">= 0"));
    }

    SECTION( "zero read" ) {
        std::int64_t nread = -1;
        const auto err = lfp_readinto(f, out.data(), 0, &nread);

        CHECK(err == LFP_OK);
        CHECK(nread == 0);
    }
}

TEST_CASE_METHOD(
    random_cfile,
    "Cfile can be seeked",
    "[cfile][seek]") {

    SECTION( "correct seek" ) {
        test_random_seek(this);
    }

    SECTION( "seek beyond file end" ) {
        // untested. behavior is determined by the handle
    }

    SECTION( "negative seek" ) {
        const auto err = lfp_seek(f, -1);

        CHECK(err == LFP_INVALID_ARGS);
        auto msg = std::string(lfp_errormsg(f));
        CHECK_THAT(msg, Contains(">= 0"));
    }
}
