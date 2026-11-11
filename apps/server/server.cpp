#include "apps/server/server.h"

#include <asio.hpp>

#include <iostream>
#include <set>

namespace glamdb::NServer {
using namespace NTransport;
namespace {
using TTcp = asio::ip::tcp;
class TSession;
struct TState
{
    asio::io_context IO;
    asio::thread_pool Workers{4};
    TRoutes& Routes;
    std::set<std::shared_ptr<TSession>> Sessions;
    std::chrono::milliseconds Timeout;
};

class TSession : public std::enable_shared_from_this<TSession>
{
public:
    TSession(TTcp::socket socket, TState& state)
        : Socket_(std::move(socket))
        , Timer_(state.IO)
        , State_(state)
    { }
    void Start()
    {
        Deadline(State_.Timeout);
        asio::async_read_until(Socket_,
            asio::dynamic_buffer(Input_, 16384),
            "\r\n\r\n",
            [self = shared_from_this()](const std::error_code& error, size_t size) {
                if (error) {
                    self->Finish();
                    return;
                }
                self->Headers(size);
            });
    }
    void StopIdle()
    {
        if (!Processing_) {
            Finish();
        }
    }

private:
    void Deadline(std::chrono::milliseconds timeout)
    {
        Timer_.expires_after(timeout);
        Timer_.async_wait([self = shared_from_this()](const std::error_code& error) {
            if (!error) {
                self->Finish();
            }
        });
    }
    void Finish()
    {
        if (Closed_) {
            return;
        }
        Closed_ = true;
        std::error_code ignored;
        Timer_.cancel();
        Socket_.close(ignored);
        State_.Sessions.erase(shared_from_this());
    }
    void Headers(size_t size)
    {
        try {
            Request_ = ParseHeaders(std::string_view(Input_).substr(0, size));
            if (!State_.Routes.IsAuthorized(Request_)) {
                Write(ErrorResponse(401, "UNAUTHORIZED", "bearer token required"));
                return;
            }
            Input_.erase(0, size);
            Require(Input_.size() <= Request_.ContentLength, "pipelining is unsupported");
            const auto remaining = Request_.ContentLength - Input_.size();
            if (remaining == 0) {
                Dispatch();
                return;
            }
            asio::async_read(Socket_,
                asio::dynamic_buffer(Input_, MaxRequestBytes),
                asio::transfer_exactly(remaining),
                [self = shared_from_this()](const std::error_code& error, size_t) {
                    if (error) {
                        self->Finish();
                        return;
                    }
                    self->Dispatch();
                });
        } catch (const TFailure& error) {
            Write(ErrorResponse(error.Detail.code == "PAYLOAD_TOO_LARGE" ? 413 : 400,
                error.Detail.code,
                error.Detail.message));
        } catch (const std::exception&) {
            Write(ErrorResponse(400, "INVALID_ARGUMENT", "invalid HTTP request"));
        }
    }
    void Dispatch()
    {
        if (Closed_) {
            return;
        }
        Timer_.cancel();
        Request_.Body = std::move(Input_);
        Processing_ = true;
        // Keep io_context alive while a worker owns the core call. The bounded
        // session set also bounds the worker queue and retained request bodies.
        asio::post(State_.Workers, [self = shared_from_this(), guard = asio::make_work_guard(State_.IO)]() mutable {
            auto response = self->State_.Routes.Handle(self->Request_);
            asio::post(self->State_.IO, [self, response = std::move(response), guard = std::move(guard)]() mutable {
                self->Write(response);
            });
        });
    }
    void Write(const TResponse& response)
    {
        if (Closed_) {
            return;
        }
        Processing_ = true;
        Output_ = SerializeResponse(response);
        Deadline(std::chrono::seconds(30));
        asio::async_write(Socket_, asio::buffer(Output_), [self = shared_from_this()](const std::error_code&, size_t) {
            self->Finish();
        });
    }
    TTcp::socket Socket_;
    asio::steady_timer Timer_;
    TState& State_;
    TRequest Request_;
    std::string Input_;
    std::string Output_;
    bool Processing_ = false;
    bool Closed_ = false;
};
} // namespace

void RunServer(TRoutes& routes, std::string_view address, uint16_t port, std::chrono::milliseconds timeout)
{
    TState state{{}, asio::thread_pool(4), routes, {}, timeout};
    TTcp::acceptor acceptor(state.IO, TTcp::endpoint(asio::ip::make_address(address), port));
    asio::signal_set signals(state.IO, SIGINT, SIGTERM);
    signals.async_wait([&](const std::error_code& error, int) {
        if (error) {
            return;
        }
        acceptor.close();
        const auto sessions = state.Sessions;
        for (const auto& session : sessions) {
            session->StopIdle();
        }
    });
    std::function<void()> accept;
    accept = [&] {
        acceptor.async_accept([&](const std::error_code& error, TTcp::socket socket) {
            if (!error) {
                if (state.Sessions.size() < 16) {
                    auto session = std::make_shared<TSession>(std::move(socket), state);
                    state.Sessions.insert(session);
                    session->Start();
                } else {
                    // No request has been admitted. Do not allocate another
                    // session or block the accept loop to write an overload body.
                    std::error_code ignored;
                    socket.close(ignored);
                }
                accept();
            }
        });
    };
    accept();
    std::cout << TJson({{"address", address}, {"port", acceptor.local_endpoint().port()}, {"backend", "local"}}).dump()
              << '\n'
              << std::flush;
    state.IO.run();
    state.Workers.join();
}
} // namespace glamdb::NServer
