#include "filetransfer.hh"
#include "compression.hh"

#include <cstdint>
#include <exception>
#include <future>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <string>
#include <string_view>
#include <sys/poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

// local server tests don't work on darwin without some incantations
// the horrors do not want to look up. contributions welcome though!
#if __APPLE__
#define NOT_ON_DARWIN(n) DISABLED_##n
#else
#define NOT_ON_DARWIN(n) n
#endif

using namespace std::chrono_literals;

namespace {

struct Reply {
    std::string status, headers;
    std::function<std::optional<std::string>(int)> content;

    Reply(
        std::string_view status, std::string_view headers, std::function<std::string()> content
    )
        : Reply(status, headers, [content](int round) {
            return round == 0 ? std::optional(content()) : std::nullopt;
        })
    {
    }

    Reply(
        std::string_view status,
        std::string_view headers,
        std::function<std::optional<std::string>(int)> content
    )
        : status(status)
        , headers(headers)
        , content(content)
    {
    }
};
}

namespace nix {

static std::tuple<uint16_t, AutoCloseFD>
serveHTTP(std::vector<Reply> replies)
{
    AutoCloseFD listener(::socket(AF_INET6, SOCK_STREAM, 0));
    if (!listener) {
        throw SysError(errno, "socket() failed");
    }

    Pipe trigger;
    trigger.create();

    sockaddr_in6 addr = {
        .sin6_family = AF_INET6,
        .sin6_addr = IN6ADDR_LOOPBACK_INIT,
    };
    socklen_t len = sizeof(addr);
    if (::bind(listener.get(), reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) < 0) {
        throw SysError(errno, "bind() failed");
    }
    if (::getsockname(listener.get(), reinterpret_cast<sockaddr *>(&addr), &len) < 0) {
        throw SysError(errno, "getsockname() failed");
    }
    if (::listen(listener.get(), 1) < 0) {
        throw SysError(errno, "listen() failed");
    }

    std::thread(
        [replies, at{0}](AutoCloseFD socket, AutoCloseFD trigger) mutable {
            while (true) {
                pollfd pfds[2] = {
                    {
                        .fd = socket.get(),
                        .events = POLLIN,
                    },
                    {
                        .fd = trigger.get(),
                        .events = POLLHUP,
                    },
                };

                if (::poll(pfds, 2, -1) <= 0) {
                    throw SysError(errno, "poll() failed");
                }
                if (pfds[1].revents & POLLHUP) {
                    return;
                }
                if (!(pfds[0].revents & POLLIN)) {
                    continue;
                }

                AutoCloseFD conn(::accept(socket.get(), nullptr, nullptr));
                if (!conn) {
                    throw SysError(errno, "accept() failed");
                }

                const auto & reply = replies[at++ % replies.size()];

                std::thread([=, conn{std::move(conn)}] {
                    auto send = [&](std::string_view bit) {
                        while (!bit.empty()) {
                            auto written = ::write(conn.get(), bit.data(), bit.size());
                            if (written < 0) {
                                throw SysError(errno, "write() failed");
                            }
                            bit.remove_prefix(written);
                        }
                    };

                    send("HTTP/1.1 ");
                    send(reply.status);
                    send("\r\n");
                    send(reply.headers);
                    send("\r\n");
                    for (int round = 0; ; round++) {
                        if (auto content = reply.content(round); content.has_value()) {
                            send(*content);
                        } else {
                            break;
                        }
                    }
                    ::shutdown(conn.get(), SHUT_WR);
                    for (;;) {
                        char buf[1];
                        switch (read(conn.get(), buf, 1)) {
                        case 0:
                            return; // remote closed
                        case 1:
                            continue; // connection still held open by remote
                        default:
                            throw SysError(errno, "read() failed");
                        }
                    }
                }).detach();
            }
        },
        std::move(listener),
        std::move(trigger.readSide)
    )
        .detach();

    return {
        ntohs(addr.sin6_port),
        std::move(trigger.writeSide),
    };
}

static std::tuple<uint16_t, AutoCloseFD>
serveHTTP(std::string status, std::string headers, std::function<std::string()> content)
{
    return serveHTTP({{{status, headers, content}}});
}

TEST(FileTransfer, exceptionAbortsDownload)
{
    struct Done : std::exception
    {};

    auto ft = makeFileTransfer();

    LambdaSink broken([](auto block) { throw Done(); });

    ASSERT_THROW(ft->download("file:///dev/zero").second->drainInto(broken), Done);

    // makeFileTransfer returns a ref<>, which cannot be cleared. since we also
    // can't default-construct it we'll have to overwrite it instead, but we'll
    // take the raw pointer out first so we can destroy it in a detached thread
    // (otherwise a failure will stall the process and have it killed by meson)
    auto reset = std::async(std::launch::async, [&]() { ft = makeFileTransfer(); });
    EXPECT_EQ(reset.wait_for(10s), std::future_status::ready);
    // if this did time out we have to leak `reset`.
    if (reset.wait_for(0s) == std::future_status::timeout) {
        (void) new auto(std::move(reset));
    }
}

TEST(FileTransfer, exceptionAbortsRead)
{
    auto [port, srv] = serveHTTP("200 ok", "content-length: 0\r\n", [] { return ""; });
    auto ft = makeFileTransfer();
    char buf[10] = "";
    ASSERT_THROW(ft->download(fmt("http://[::1]:%d/index", port)).second->read(buf, 10), EndOfFile);
}

TEST(FileTransfer, NOT_ON_DARWIN(reportsSetupErrors))
{
    auto [port, srv] = serveHTTP("404 not found", "", [] { return ""; });
    auto ft = makeFileTransfer();
    ASSERT_THROW(
        ft->download(fmt("http://[::1]:%d/index", port)),
        FileTransferError
    );
}

TEST(FileTransfer, NOT_ON_DARWIN(defersFailures))
{
    auto [port, srv] = serveHTTP("200 ok", "content-length: 100000000\r\n", [] {
        std::this_thread::sleep_for(10ms);
        // just a bunch of data to fill the curl wrapper buffer, otherwise the
        // initial wait for header data will also wait for the the response to
        // complete (the source is only woken when curl returns data, and curl
        // might only do so once its internal buffer has already been filled.)
        return std::string(1024 * 1024, ' ');
    });
    auto ft = makeFileTransfer(0);
    auto src = ft->download(fmt("http://[::1]:%d/index", port)).second;
    ASSERT_THROW(src->drain(), FileTransferError);
}

TEST(FileTransfer, NOT_ON_DARWIN(handlesContentEncoding))
{
    std::string original = "Test data string";
    std::string compressed = compress("gzip", original);

    auto [port, srv] = serveHTTP("200 ok", "content-encoding: gzip\r\n", [&] { return compressed; });
    auto ft = makeFileTransfer();

    StringSink sink;
    ft->download(fmt("http://[::1]:%d/index", port)).second->drainInto(sink);
    EXPECT_EQ(sink.s, original);
}

TEST(FileTransfer, usesIntermediateLinkHeaders)
{
    auto [port, srv] = serveHTTP({
        {"301 ok",
         "location: /second\r\n"
         "content-length: 0\r\n",
         [] { return ""; }},
        {"307 ok",
         "location: /third\r\n"
         "content-length: 0\r\n",
         [] { return ""; }},
        {"307 ok",
         "location: /fourth\r\n"
         "link: <http://foo>; rel=\"immutable\"\r\n"
         "content-length: 0\r\n",
         [] { return ""; }},
        {"200 ok", "content-length: 1\r\n", [] { return "a"; }},
    });
    auto ft = makeFileTransfer(0);
    auto [result, _data] = ft->download(fmt("http://[::1]:%d/first", port));
    ASSERT_EQ(result.immutableUrl, "http://foo");
}

TEST(FileTransfer, stalledReaderDoesntBlockOthers)
{
    auto [port, srv] = serveHTTP({
        {"200 ok",
         "content-length: 100000000\r\n",
         [](int round) mutable {
             return round < 100 ? std::optional(std::string(1'000'000, ' ')) : std::nullopt;
         }},
    });
    auto ft = makeFileTransfer(0);
    auto [_result1, data1] = ft->download(fmt("http://[::1]:%d", port));
    auto [_result2, data2] = ft->download(fmt("http://[::1]:%d", port));
    auto drop = [](Source & source, size_t size) {
        char buf[1000];
        while (size > 0) {
            auto round = std::min(size, sizeof(buf));
            source(buf, round);
            size -= round;
        }
    };
    // read 10M of each of the 100M, then the rest. neither reader should
    // block the other, nor should it take that long to copy 200MB total.
    drop(*data1, 10'000'000);
    drop(*data2, 10'000'000);
    drop(*data1, 90'000'000);
    drop(*data2, 90'000'000);

    ASSERT_THROW(drop(*data1, 1), EndOfFile);
    ASSERT_THROW(drop(*data2, 1), EndOfFile);
}

}
