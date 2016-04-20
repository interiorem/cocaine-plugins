#include "cocaine/detail/isolate/external.hpp"
#include "cocaine/idl/isolate.hpp"

#include <cocaine/context.hpp>
#include <cocaine/context/signal.hpp>
#include <cocaine/dynamic.hpp>
#include <cocaine/errors.hpp>
#include <cocaine/idl/context.hpp>
#include <cocaine/logging.hpp>
#include <cocaine/engine.hpp>

#include <cocaine/rpc/dispatch.hpp>
#include <cocaine/rpc/session.hpp>

#include <cocaine/traits/dynamic.hpp>
#include <cocaine/traits/endpoint.hpp>
#include <cocaine/traits/vector.hpp>

#include <blackhole/logger.hpp>
#include <asio/ip/tcp.hpp>

namespace cocaine { namespace isolate {

using asio::ip::tcp;

struct spool_dispatch_t :
    public dispatch<io::event_traits<io::isolate::spool>::upstream_type>
{
    typedef io::protocol<io::event_traits<io::isolate::spool>::upstream_type>::scope protocol;

    spool_dispatch_t(const std::string& name, std::shared_ptr<api::spool_handle_base_t> _spool_handle):
        dispatch(name),
        spool_handle(std::move(_spool_handle))
    {
        on<protocol::value>(std::bind(&spool_dispatch_t::on_done, this));
        on<protocol::error>(std::bind(&spool_dispatch_t::on_error, this, std::placeholders::_1, std::placeholders::_2));
    }

    void on_done() {
        spool_handle->on_ready();
    }

    void on_error(std::error_code ec, const std::string& msg) {
        spool_handle->on_abort(ec, msg);
    }

    std::shared_ptr<api::spool_handle_base_t> spool_handle;
};

struct spawn_dispatch_t :
    public dispatch<io::event_traits<io::isolate::spawn>::upstream_type>
{
    typedef io::protocol<io::event_traits<io::isolate::spawn>::upstream_type>::scope protocol;

    spawn_dispatch_t(const std::string& name, std::shared_ptr<api::spawn_handle_base_t> _spool_handle)
        : dispatch(name),
          spawn_handle(std::move(_spool_handle)),
          ready(false)
    {
        on<protocol::chunk>(std::bind(&spawn_dispatch_t::on_chunk, this, std::placeholders::_1));
        on<protocol::choke>(std::bind(&spawn_dispatch_t::on_choke, this));
        on<protocol::error>(std::bind(&spawn_dispatch_t::on_error, this, std::placeholders::_1, std::placeholders::_2));
    }

    void on_chunk(std::string chunk) {
        // If it's the first empty chunk - it signals that worker has been spawned,
        // if it's not empty - it's a chunk of output from worker.
        if(chunk.empty() && !ready) {
            spawn_handle->on_ready();
            ready = true;
        } else {
            spawn_handle->on_data(std::move(chunk));
        }

    }

    void on_choke() {
        spawn_handle->on_terminate(std::error_code(), "");
    }

    void on_error(std::error_code ec, const std::string& msg) {
        spawn_handle->on_terminate(ec, msg);
    }

    virtual void discard(const std::error_code& ec) const {
        spawn_handle->on_terminate(ec, "external isolation session was discarded");
    }

    std::shared_ptr<api::spawn_handle_base_t> spawn_handle;
    bool ready;
};

struct spool_load_t :
    public api::cancellation_t
{
    external_t::inner_t& inner;
    std::shared_ptr<api::spool_handle_base_t> handle;
    bool cancelled;
    synchronized<io::upstream_ptr_t> stream;

    spool_load_t(external_t::inner_t& _inner, std::shared_ptr<api::spool_handle_base_t> _handle) :
        inner(_inner),
        handle(std::move(_handle)),
        cancelled(false)
    {}

    ~spool_load_t() {
        cancel();
    }

    void apply(std::shared_ptr<session_t> session);

    virtual
    void
    cancel() noexcept ;
};


struct spawn_load_t :
    public api::cancellation_t
{
    external_t::inner_t& inner;
    std::shared_ptr<api::spawn_handle_base_t> handle;
    std::string path;
    api::args_t worker_args;
    api::env_t environment;
    bool cancelled;
    synchronized<io::upstream_ptr_t> stream;

    spawn_load_t(external_t::inner_t& _inner,
                 std::shared_ptr<api::spawn_handle_base_t> _handle,
                 const std::string& _path,
                 const api::args_t& _worker_args,
                 const api::env_t& _environment
    ) :
        inner(_inner),
        handle(std::move(_handle)),
        path(_path),
        worker_args(_worker_args),
        environment(_environment),
        cancelled(false)
    {}

    ~spawn_load_t() {
        cancel();
    }

    void
    apply(std::shared_ptr<session_t> session);

    virtual
    void
    cancel() noexcept;
};

struct external_t::inner_t :
    public std::enable_shared_from_this<inner_t>
{
    context_t& context;
    asio::io_service& io_context;
    std::unique_ptr<tcp::socket> socket;
    asio::deadline_timer retry_timer;
    std::shared_ptr<session_t> session;
    const std::string name;
    dynamic_t args;
    std::unique_ptr<logging::logger_t> log;
    std::atomic<id_t> cur_id;
    std::shared_ptr<dispatch<io::context_tag>> signal_dispatch;
    bool prepared;


    std::vector<std::shared_ptr<spool_load_t>> spool_queue;
    std::vector<std::shared_ptr<spawn_load_t>> spawn_queue;

    inner_t(context_t& _context, asio::io_service& _io_context, const std::string& _name, const std::string& type, const dynamic_t& _args) :
        context(_context),
        io_context(_io_context),
        socket(),
        retry_timer(io_context),
        name(_name),
        args(_args),
        log(context.log("universal_isolate/"+_name)),
        signal_dispatch(std::make_shared<dispatch<io::context_tag>>("universal_isolate_signal")),
        prepared(false)
    {
        signal_dispatch->on<io::context::prepared>([&](){
            prepared = true;
            on_ready();
        });
        context.signal_hub().listen(signal_dispatch, io_context);
        if(args.as_object().at("type", "").as_string().empty()) {
            args.as_object()["type"] = type;
        }
    }

    void connect() {
        socket.reset(new asio::ip::tcp::socket(io_context));
        auto ep = args.as_object().at("external_isolation_endpoint", dynamic_t::empty_object).as_object();
        tcp::endpoint endpoint(
            asio::ip::address::from_string(ep.at("host", "127.0.0.1").as_string()),
            static_cast<unsigned short>(ep.at("port", 29042u).as_uint())
        );

        auto retry_timeout = args.as_object().at("retry_timeout", 5u).as_uint();

        COCAINE_LOG_INFO(log, "connecting to external isolation daemon to {}", boost::lexical_cast<std::string>(endpoint));
        auto self_shared = shared_from_this();

        retry_timer.expires_from_now(boost::posix_time::seconds(retry_timeout));
        retry_timer.async_wait([=](const std::error_code& ec){
            if(!ec) {
                socket->cancel();
            }
        });
        socket->async_connect(endpoint, [=](const std::error_code& ec) {
            if (retry_timer.cancel() && !ec) {
                COCAINE_LOG_INFO(log, "connected to isolation daemon");
                session = context.engine().attach(std::move(socket), nullptr);
                self_shared->on_ready();
            } else {
                COCAINE_LOG_WARNING(log, "could not connect to external isolation daemon - {}, retrying", ec.message());
                retry_timer.expires_from_now(boost::posix_time::seconds(retry_timeout));
                retry_timer.async_wait([=](const std::error_code& ec){
                    if(!ec) {
                        connect();
                    } else {
                        COCAINE_LOG_ERROR(log, "retry timer was reset - {}", ec.message());
                    }
                });
            }
        });
    }

    void on_ready() {
        if(!prepared) {
            COCAINE_LOG_DEBUG(log, "external isolation daemon connection is ready, but context is still not prepared");
            return;
        }
        if(!session) {
            COCAINE_LOG_DEBUG(log, "context is ready, but external isolation daemon is not connected yet");
            return;
        }
        COCAINE_LOG_INFO(log, "successfully connected to external isolation daemon and prepared context, dequeing spool and spawn requests");
        COCAINE_LOG_INFO(log, "processing {} queued spool requests", spool_queue.size());
        for (auto& load: spool_queue) {
            load->apply(session);
        }
        spool_queue.clear();
        COCAINE_LOG_INFO(log, "processing {} queued spawn requests", spawn_queue.size());
        for(auto& load: spawn_queue) {
            load->apply(session);
        }
        spawn_queue.clear();
    }

};

void spool_load_t::apply(std::shared_ptr<session_t> session) {
    stream.apply([&](decltype(stream.unsafe())& stream){
        if(!cancelled) {
            try {
                stream = session->fork(std::make_shared<spool_dispatch_t>("external_spool/" + inner.name, handle));
                stream->send<io::isolate::spool>(inner.args, inner.name);
            } catch(const std::system_error& e) {
                COCAINE_LOG_ERROR(inner.log, "failed to process spool request - {}", error::to_string(e));
                handle->on_abort(e.code(), e.what());
            }
        } else {
            COCAINE_LOG_WARNING(inner.log, "can not process spool request - cancelled");
        }
    });
}

void
spool_load_t::cancel() noexcept {
    stream.apply([&](decltype(stream.unsafe())& stream){
        if(!cancelled) {
            cancelled = true;
            if(stream) {
                try {
                    stream->send<io::isolate_spooled::cancel>();
                } catch(const std::system_error& e) {
                    COCAINE_LOG_WARNING(inner.log, "could not cancel spool request: {}", error::to_string(e));
                }
            }
        }
    });
}

void
spawn_load_t::apply(std::shared_ptr<session_t> session) {
    stream.apply([&](decltype(stream.unsafe())& stream){
        if(!cancelled) {
            try {
                stream = session->fork(std::make_shared<spawn_dispatch_t>("external_spawn/" + inner.name, handle));
                stream->send<io::isolate::spawn>(inner.args, inner.name, path, worker_args, environment);
            } catch(const std::system_error& e) {
                handle->on_terminate(e.code(), e.what());
            }
        }
    });
}

void
spawn_load_t::cancel() noexcept {
    stream.apply([&](decltype(stream.unsafe())& stream){
        if(!cancelled) {
            cancelled = true;
            if(stream) {
                try {
                    stream->send<io::isolate_spawned::kill>();
                } catch(...) {
                    //swallow. TODO: log
                }
            }
        }
    });
}

external_t::external_t(context_t& context, asio::io_service& io_context, const std::string& name, const std::string& type, const dynamic_t& args) :
    isolate_t(context, io_context, name, type, args),
    inner(new inner_t(context, io_context, name, type, args))
{
    inner->connect();
}

std::unique_ptr<api::cancellation_t>
external_t::spool(std::shared_ptr<api::spool_handle_base_t> handler) {
    std::shared_ptr<spool_load_t> load(new spool_load_t(*inner, std::move(handler)));
    inner->io_context.post([=](){
        if(inner->session && inner->prepared) {
            COCAINE_LOG_DEBUG(inner->log, "processing spool request");
            load->apply(inner->session);
        } else {
            COCAINE_LOG_DEBUG(inner->log, "queuing spool request");
            inner->spool_queue.push_back(load);
        }
    });
    return std::unique_ptr<api::cancellation_t>(new api::cancellation_wrapper(load));
}

std::unique_ptr<api::cancellation_t>
external_t::spawn(const std::string& path,
            const api::args_t& worker_args,
            const api::env_t& environment,
            std::shared_ptr<api::spawn_handle_base_t> handler) {

    std::shared_ptr<spawn_load_t> load(new spawn_load_t(*inner, std::move(handler), path, worker_args, environment));
    inner->io_context.post([=](){
        if(inner->session && inner->prepared) {
            COCAINE_LOG_DEBUG(inner->log, "processing spawn request");
            load->apply(inner->session);
        } else {
            COCAINE_LOG_DEBUG(inner->log, "queuing spawn request");
            inner->spawn_queue.push_back(load);
        }
    });
    return std::unique_ptr<api::cancellation_t>(new api::cancellation_wrapper(load));
}

}} // namespace cocaine::isolate