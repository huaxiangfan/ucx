/*
 * Copyright (C) Mellanox Technologies Ltd. 2020.  ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#include "ucx_wrapper.h"

#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <iostream>
#include <string.h>
#include <getopt.h>
#include <assert.h>
#include <unistd.h>
#include <cstdlib>
#include <ctime>
#include <csignal>
#include <cerrno>
#include <vector>
#include <map>
#include <queue>
#include <algorithm>
#include <limits>
#include <malloc.h>
#include <dlfcn.h>
#include <set>

#define ALIGNMENT           4096
#define BUSY_PROGRESS_COUNT 1000

/* IO operation type */
typedef enum {
    IO_READ,
    IO_WRITE,
    IO_OP_MAX,
    IO_COMP_MIN  = IO_OP_MAX,
    IO_READ_COMP = IO_COMP_MIN,
    IO_WRITE_COMP
} io_op_t;

static const char *io_op_names[] = {
    "read",
    "write",
    "read completion",
    "write completion"
};


#ifndef NDEBUG
const bool do_assert = true;
#else
const bool do_assert = false;
#endif


/* test options */
typedef struct {
    std::vector<const char*> servers;
    int                      port_num;
    double                   connect_timeout;
    double                   client_timeout;
    long                     retries;
    double                   retry_interval;
    double                   client_runtime_limit;
    double                   print_interval;
    size_t                   iomsg_size;
    size_t                   min_data_size;
    size_t                   max_data_size;
    size_t                   chunk_size;
    long                     iter_count;
    long                     window_size;
    long                     conn_window_size;
    std::vector<io_op_t>     operations;
    unsigned                 random_seed;
    size_t                   num_offcache_buffers;
    bool                     verbose;
    bool                     validate;
    bool                     debug_timeout;
    size_t                   rndv_thresh;
} options_t;

#define LOG_PREFIX  "[DEMO]"
#define LOG         UcxLog(LOG_PREFIX)
#define VERBOSE_LOG UcxLog(LOG_PREFIX, _test_opts.verbose)

#define ASSERTV_STR(_expression_str) \
        "Assertion \"" << _expression_str << "\" failed "
#define ASSERTV(_expression) \
        UcxLog(LOG_PREFIX, !(_expression), &std::cerr, do_assert) \
                << ASSERTV_STR(#_expression)


template<class T, bool use_offcache = false>
class MemoryPool {
public:
    MemoryPool(size_t buffer_size, const std::string& name, size_t offcache = 0) :
        _num_allocated(0), _buffer_size(buffer_size), _name(name) {

        for (size_t i = 0; i < offcache; ++i) {
            _offcache_queue.push(get_free());
        }
    }

    ~MemoryPool() {
        while (!_offcache_queue.empty()) {
            _free_stack.push_back(_offcache_queue.front());
            _offcache_queue.pop();
        }

        if (_num_allocated != _free_stack.size()) {
            LOG << (_num_allocated - _free_stack.size())
                << " buffers were not released from " << _name;
        }

        for (size_t i = 0; i < _free_stack.size(); i++) {
            delete _free_stack[i];
        }
    }

    inline T* get() {
        T* item = get_free();

        if (use_offcache && !_offcache_queue.empty()) {
            _offcache_queue.push(item);
            item = _offcache_queue.front();
            _offcache_queue.pop();
        }

        return item;
    }

    inline void put(T* item) {
        _free_stack.push_back(item);
    }

    inline size_t allocated() const {
        return _num_allocated;
    }

    const std::string& name() const {
        return _name;
    }

private:
    inline T* get_free() {
        T* item;

        if (_free_stack.empty()) {
            item = new T(_buffer_size, *this);
            _num_allocated++;
        } else {
            item = _free_stack.back();
            _free_stack.pop_back();
        }
        return item;
    }

private:
    std::vector<T*> _free_stack;
    std::queue<T*>  _offcache_queue;
    uint32_t        _num_allocated;
    size_t          _buffer_size;
    std::string     _name;
};

/**
 * Linear congruential generator (LCG):
 * n[i + 1] = (n[i] * A + C) % M
 * where A, C, M used as in glibc
 */
class IoDemoRandom {
public:
    static void srand(unsigned seed) {
        _seed = seed & _M;
    }

    template <typename T>
    static inline T rand(T min = std::numeric_limits<T>::min(),
                         T max = std::numeric_limits<T>::max() - 1) {
        return rand(_seed, min, max);
    }

    template <typename T>
    static inline T rand(unsigned &seed, T min = std::numeric_limits<T>::min(),
                         T max = std::numeric_limits<T>::max() - 1) {
        seed = (seed * _A + _C) & _M;
        /* To resolve that LCG returns alternating even/odd values */
        if (max - min == 1) {
            return (seed & 0x100) ? max : min;
        } else {
            return T(seed) % (max - min + 1) + min;
        }
    }

    template <typename unsigned_type>
    static inline unsigned_type urand(unsigned_type max)
    {
        assert(max < std::numeric_limits<unsigned_type>::max());
        assert(unsigned_type(0) == std::numeric_limits<unsigned_type>::min());

        return rand(_seed, unsigned_type(0), max - 1);
    }

    static inline void fill(unsigned &seed, void *buffer, size_t size) {
        size_t body_count = size / sizeof(uint64_t);
        size_t tail_count = size & (sizeof(uint64_t) - 1);
        uint64_t *body    = reinterpret_cast<uint64_t*>(buffer);
        uint8_t *tail     = reinterpret_cast<uint8_t*>(body + body_count);

        fill(seed, body, body_count);
        fill(seed, tail, tail_count);
    }

    static inline size_t validate(unsigned &seed, const void *buffer,
                                  size_t size) {
        size_t body_count    = size / sizeof(uint64_t);
        size_t tail_count    = size & (sizeof(uint64_t) - 1);
        const uint64_t *body = reinterpret_cast<const uint64_t*>(buffer);
        const uint8_t *tail  = reinterpret_cast<const uint8_t*>(body + body_count);

        size_t err_pos = validate(seed, body, body_count);
        if (err_pos < body_count) {
            return err_pos * sizeof(body[0]);
        }

        err_pos = validate(seed, tail, tail_count);
        if (err_pos < tail_count) {
            return (body_count * sizeof(body[0])) + (err_pos * sizeof(tail[0]));
        }

        return size;
    }

private:
    template <typename T>
    static inline void fill(unsigned &seed, T *buffer, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            buffer[i] = rand<T>(seed);
        }
    }

    template <typename T>
    static inline size_t validate(unsigned &seed, const T *buffer, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            if (buffer[i] != rand<T>(seed)) {
                return i;
            }
        }

        return count;
    }

    static       unsigned     _seed;
    static const unsigned     _A;
    static const unsigned     _C;
    static const unsigned     _M;
};
unsigned IoDemoRandom::_seed    = 0;
const unsigned IoDemoRandom::_A = 1103515245U;
const unsigned IoDemoRandom::_C = 12345U;
const unsigned IoDemoRandom::_M = 0x7fffffffU;

class P2pDemoCommon : public UcxContext {
public:
    /* IO header */
    typedef struct {
        uint32_t    sn;
        uint8_t     op;
        uint64_t    data_size;
    } iomsg_t;

    typedef enum {
        OK,
        CONN_RETRIES_EXCEEDED,
        RUNTIME_EXCEEDED,
        TERMINATE_SIGNALED
    } status_t;

protected:
    typedef enum {
        XFER_TYPE_SEND,
        XFER_TYPE_RECV
    } xfer_type_t;

    class Buffer {
    public:
        Buffer(size_t size, MemoryPool<Buffer, true>& pool) :
            _capacity(size),
            _buffer(UcxContext::memalign(ALIGNMENT, size, pool.name().c_str())),
            _size(0), _pool(pool) {
            if (_buffer == NULL) {
                throw std::bad_alloc();
            }
        }

        ~Buffer() {
            UcxContext::free(_buffer);
        }

        void release() {
            _pool.put(this);
        }

        inline void *buffer(size_t offset = 0) const {
            return (uint8_t*)_buffer + offset;
        }

        inline void resize(size_t size) {
            assert(size <= _capacity);
            _size = size;
        }

        inline size_t size() const {
            return _size;
        }

    public:
        const size_t         _capacity;

    private:
        void*                     _buffer;
        size_t                    _size;
        MemoryPool<Buffer, true>& _pool;
    };

    class BufferIov {
    public:
        BufferIov(size_t size, MemoryPool<BufferIov>& pool) :
                _data_size(0), _pool(pool) {
            _iov.reserve(size);
        }

        size_t size() const {
            return _iov.size();
        }

        size_t data_size() const {
            return _data_size;
        }

        void init(size_t data_size, MemoryPool<Buffer, true> &chunk_pool,
                  uint32_t sn, bool validate) {
            assert(_iov.empty());

            _data_size    = data_size;
            Buffer *chunk = chunk_pool.get();
            _iov.resize(get_chunk_cnt(data_size, chunk->_capacity));

            size_t remaining = init_chunk(0, chunk, data_size);
            for (size_t i = 1; i < _iov.size(); ++i) {
                remaining = init_chunk(i, chunk_pool.get(), remaining);
            }

            assert(remaining == 0);

            if (validate) {
                fill_data(sn);
            }
        }

        inline Buffer& operator[](size_t i) const {
            return *_iov[i];
        }

        void release() {
            while (!_iov.empty()) {
                _iov.back()->release();
                _iov.pop_back();
            }

            _pool.put(this);
        }

        inline size_t validate(unsigned seed) const {
            assert(!_iov.empty());

            for (size_t iov_err_pos = 0, i = 0; i < _iov.size(); ++i) {
                size_t buf_err_pos = IoDemoRandom::validate(seed,
                                                            _iov[i]->buffer(),
                                                            _iov[i]->size());
                iov_err_pos       += buf_err_pos;
                if (buf_err_pos < _iov[i]->size()) {
                    return iov_err_pos;
                }
            }

            return _npos;
        }

    private:
        size_t init_chunk(size_t i, Buffer *chunk, size_t remaining) {
            _iov[i] = chunk;
            _iov[i]->resize(std::min(_iov[i]->_capacity, remaining));
            return remaining - _iov[i]->size();
        }

        void fill_data(unsigned seed) {
            for (size_t i = 0; i < _iov.size(); ++i) {
                IoDemoRandom::fill(seed, _iov[i]->buffer(), _iov[i]->size());
            }
        }

    public:
        static const size_t    _npos = static_cast<size_t>(-1);

    private:
        size_t                 _data_size;
        std::vector<Buffer*>   _iov;
        MemoryPool<BufferIov>& _pool;
    };

    /* Asynchronous IO message */
    class IoMessage : public UcxCallback {
    public:
        IoMessage(size_t io_msg_size, MemoryPool<IoMessage>& pool) :
            _buffer(UcxContext::malloc(io_msg_size, pool.name().c_str())),
            _pool(pool), _io_msg_size(io_msg_size) {

            if (_buffer == NULL) {
                throw std::bad_alloc();
            }
        }

        void init(io_op_t op, uint32_t sn, size_t data_size, bool validate) {
            iomsg_t *m = reinterpret_cast<iomsg_t *>(_buffer);

            m->sn        = sn;
            m->op        = op;
            m->data_size = data_size;
            if (validate) {
                void *tail       = reinterpret_cast<void*>(m + 1);
                size_t tail_size = _io_msg_size - sizeof(*m);
                IoDemoRandom::fill(sn, tail, tail_size);
            }
        }

        ~IoMessage() {
            UcxContext::free(_buffer);
        }

        virtual void operator()(ucs_status_t status) {
            _pool.put(this);
        }

        void *buffer() const {
            return _buffer;
        }

        const iomsg_t* msg() const{
            return reinterpret_cast<iomsg_t*>(buffer());
        }

    private:
        void*                  _buffer;
        MemoryPool<IoMessage>& _pool;
        size_t                 _io_msg_size;
    };

    class SendCompleteCallback : public UcxCallback {
    public:
        SendCompleteCallback(size_t buffer_size,
                             MemoryPool<SendCompleteCallback>& pool) :
            _counter(0), _iov(NULL), _pool(pool) {
        }

        void init(BufferIov* iov, long* op_counter) {
            _op_counter = op_counter;
            _counter    = iov->size();
            _iov        = iov;
            assert(_counter > 0);
        }

        virtual void operator()(ucs_status_t status) {
            if (--_counter > 0) {
                return;
            }

            if (_op_counter != NULL) {
                ++(*_op_counter);
            }

            _iov->release();
            _pool.put(this);
        }

    private:
        long*                             _op_counter;
        size_t                            _counter;
        BufferIov*                        _iov;
        MemoryPool<SendCompleteCallback>& _pool;
    };

    static void signal_terminate_handler(int signo)
    {
        LOG << "Run-time signal handling: " << signo;

        _status = TERMINATE_SIGNALED;
    }

    P2pDemoCommon(const options_t& test_opts) :
        UcxContext(test_opts.iomsg_size, test_opts.connect_timeout,
                   test_opts.rndv_thresh),
        _test_opts(test_opts),
        _io_msg_pool(test_opts.iomsg_size, "io messages"),
        _send_callback_pool(0, "send callbacks"),
        _data_buffers_pool(get_chunk_cnt(test_opts.max_data_size,
                                         test_opts.chunk_size), "data iovs"),
        _data_chunks_pool(test_opts.chunk_size, "data chunks",
                          test_opts.num_offcache_buffers)
    {
        _status                  = OK;

        struct sigaction new_sigaction;
        new_sigaction.sa_handler = signal_terminate_handler;
        new_sigaction.sa_flags   = 0;
        sigemptyset(&new_sigaction.sa_mask);

        if (sigaction(SIGINT, &new_sigaction, NULL) != 0) {
            LOG << "ERROR: failed to set SIGINT signal handler: "
                << strerror(errno);
            abort();
        }
    }

    const options_t& opts() const {
        return _test_opts;
    }

    inline size_t get_data_size() {
        return IoDemoRandom::rand(opts().min_data_size,
                                  opts().max_data_size);
    }

    bool send_io_message(UcxConnection *conn, io_op_t op, uint32_t sn,
                         size_t data_size, bool validate) {
        IoMessage *m = _io_msg_pool.get();
        m->init(op, sn, data_size, validate);
        return send_io_message(conn, m);
    }

    void send_recv_data(UcxConnection* conn, const BufferIov &iov, uint32_t sn,
                        xfer_type_t send_recv_data,
                        UcxCallback* callback = EmptyCallback::get()) {
        for (size_t i = 0; i < iov.size(); ++i) {
            if (send_recv_data == XFER_TYPE_SEND) {
                conn->send_data(iov[i].buffer(), iov[i].size(), sn, callback);
            } else {
                conn->recv_data(iov[i].buffer(), iov[i].size(), sn, callback);
            }
        }
    }

    void send_data(UcxConnection* conn, const BufferIov &iov, uint32_t sn,
                   UcxCallback* callback = EmptyCallback::get()) {
        send_recv_data(conn, iov, sn, XFER_TYPE_SEND, callback);
    }

    void recv_data(UcxConnection* conn, const BufferIov &iov, uint32_t sn,
                   UcxCallback* callback = EmptyCallback::get()) {
        send_recv_data(conn, iov, sn, XFER_TYPE_RECV, callback);
    }

    void send_io_write_response(UcxConnection* conn, const BufferIov& iov,
                                uint32_t sn)
    {
        // send IO write response packet only if the connection status is OK
        send_io_message(conn, IO_WRITE_COMP, sn, iov.data_size(),
                        opts().validate);
    }

    static uint32_t get_chunk_cnt(size_t data_size, size_t chunk_size) {
        return (data_size + chunk_size - 1) / chunk_size;
    }

    static void validate(const BufferIov& iov, unsigned seed) {
        assert(iov.size() != 0);

        size_t err_pos = iov.validate(seed);
        if (err_pos != iov._npos) {
            LOG << "ERROR: iov data corruption at " << err_pos << " position";
            abort();
        }
    }

    static void validate(const iomsg_t *msg, size_t iomsg_size) {
        unsigned seed   = msg->sn;
        const void *buf = msg + 1;
        size_t buf_size = iomsg_size - sizeof(*msg);

        size_t err_pos  = IoDemoRandom::validate(seed, buf, buf_size);
        if (err_pos < buf_size) {
            LOG << "ERROR: io msg data corruption at " << err_pos << " position";
            abort();
        }
    }

    static void validate(const iomsg_t *msg, uint32_t sn, size_t iomsg_size) {
        if (sn != msg->sn) {
            LOG << "ERROR: io msg sn missmatch " << sn << " != " << msg->sn;
            abort();
        }

        validate(msg, iomsg_size);
    }

private:
    bool send_io_message(UcxConnection *conn, IoMessage *msg) {
        VERBOSE_LOG << "sending IO " << io_op_names[msg->msg()->op] << ", sn "
                    << msg->msg()->sn << " size " << sizeof(iomsg_t);

        /* send IO_READ_COMP as a data since the transaction must be matched
         * by sn on receiver side */
        if (msg->msg()->op == IO_READ_COMP) {
            return conn->send_data(msg->buffer(), opts().iomsg_size,
                                   msg->msg()->sn, msg);
        } else {
            return conn->send_io_message(msg->buffer(), opts().iomsg_size, msg);
        }
    }

protected:
    const options_t                  _test_opts;
    MemoryPool<IoMessage>            _io_msg_pool;
    MemoryPool<SendCompleteCallback> _send_callback_pool;
    MemoryPool<BufferIov>            _data_buffers_pool;
    MemoryPool<Buffer, true>         _data_chunks_pool;
    static status_t                  _status;
};


P2pDemoCommon::status_t P2pDemoCommon::_status = OK;


class DemoServer : public P2pDemoCommon {
public:
    // sends an IO response when done
    class IoWriteResponseCallback : public UcxCallback {
    public:
        IoWriteResponseCallback(size_t buffer_size,
            MemoryPool<IoWriteResponseCallback>& pool) :
            _server(NULL), _conn(NULL), _sn(0), _iov(NULL), _pool(pool) {
        }

        void init(DemoServer *server, UcxConnection* conn, uint32_t sn,
                  BufferIov *iov, long* op_cnt) {
            _server    = server;
            _conn      = conn;
            _op_cnt    = op_cnt;
            _sn        = sn;
            _iov       = iov;
            _chunk_cnt = iov->size();
        }

        virtual void operator()(ucs_status_t status) {
            if (--_chunk_cnt > 0) {
                return;
            }

            if (status == UCS_OK) {
                if (_conn->ucx_status() == UCS_OK) {
                    _server->send_io_write_response(_conn, *_iov, _sn);
                }

                if (_server->opts().validate) {
                    validate(*_iov, _sn);
                }
            }

            assert(_op_cnt != NULL);
            ++(*_op_cnt);

            _iov->release();
            _pool.put(this);
        }

    private:
        DemoServer*                          _server;
        UcxConnection*                       _conn;
        long*                                _op_cnt;
        uint32_t                             _chunk_cnt;
        uint32_t                             _sn;
        BufferIov*                           _iov;
        MemoryPool<IoWriteResponseCallback>& _pool;
    };

    class ConnectionStat {
    public:
        ConnectionStat() {
            reset();
        }

        void reset() {
            for (int i = 0; i < IO_OP_MAX; ++i) {
                _bytes_counters[i] = 0;
                _op_counters[i]    = 0;
            }
        }

        void operator+=(const ConnectionStat &other) {
            for (int i = 0; i < IO_OP_MAX; ++i) {
                _bytes_counters[i] += other._bytes_counters[i];
                _op_counters[i]    += other._op_counters[i];
            }
        }

        template<io_op_t op_type> long&
        completions() {
            UCS_STATIC_ASSERT(op_type < IO_OP_MAX);
            return _op_counters[op_type];
        }

        template<io_op_t op_type> long&
        bytes() {
            UCS_STATIC_ASSERT(op_type < IO_OP_MAX);
            return _bytes_counters[op_type];
        }

    private:
        long _bytes_counters[IO_OP_MAX];
        long _op_counters[IO_OP_MAX];
    };

    typedef std::map<UcxConnection*, ConnectionStat> conn_stat_map_t;

    class DisconnectCallback : public UcxDisconnectCallback {
    public:
        DisconnectCallback(conn_stat_map_t &stat_map,
                           conn_stat_map_t::key_type map_key) :
            _stat_map(stat_map), _map_key(map_key) {
        }

        virtual void operator()(ucs_status_t status) {
            conn_stat_map_t::iterator it = _stat_map.find(_map_key);
            assert(it != _stat_map.end());
            _stat_map.erase(it);
            delete this;
        }

    private:
        conn_stat_map_t           &_stat_map;
        conn_stat_map_t::key_type _map_key;
    };

    DemoServer(const options_t& test_opts) :
        P2pDemoCommon(test_opts), _callback_pool(0, "callbacks") {
    }

    ~DemoServer()
    {
        destroy_connections();
    }

    void run() {
        struct sockaddr_in listen_addr;
        memset(&listen_addr, 0, sizeof(listen_addr));
        listen_addr.sin_family      = AF_INET;
        listen_addr.sin_addr.s_addr = INADDR_ANY;
        listen_addr.sin_port        = htons(opts().port_num);

        for (long retry = 1; _status == OK; ++retry) {
            if (listen((const struct sockaddr*)&listen_addr,
                       sizeof(listen_addr))) {
                break;
            }

            if (retry > opts().retries) {
                return;
            }

            {
                UcxLog log(LOG_PREFIX);
                log << "restarting listener on "
                    << UcxContext::sockaddr_str((struct sockaddr*)&listen_addr,
                                                sizeof(listen_addr))
                    << " in " << opts().retry_interval << " seconds (retry "
                    << retry;

                if (opts().retries < std::numeric_limits<long>::max()) {
                    log << "/" << opts().retries;
                }

                log << ")";
            }

            sleep(opts().retry_interval);
        }

        double prev_time = get_time();
        while (_status == OK) {
            try {
                for (size_t i = 0; i < BUSY_PROGRESS_COUNT; ++i) {
                    progress();
                }

                double curr_time = get_time();
                if (curr_time >= (prev_time + opts().print_interval)) {
                    report_state(curr_time - prev_time);
                    prev_time = curr_time;
                }
            } catch (const std::exception &e) {
                std::cerr << e.what();
            }
        }

        destroy_listener();
    }

    void handle_io_read_request(UcxConnection* conn, const iomsg_t *msg) {
        // send data
        VERBOSE_LOG << "sending IO read data";
        assert(opts().max_data_size >= msg->data_size);

        BufferIov *iov            = _data_buffers_pool.get();
        SendCompleteCallback *cb  = _send_callback_pool.get();
        ConnectionStat &conn_stat = _conn_stat_map.find(conn)->second;

        iov->init(msg->data_size, _data_chunks_pool, msg->sn, opts().validate);
        cb->init(iov, &conn_stat.completions<IO_READ>());

        conn_stat.bytes<IO_READ>() += msg->data_size;
        send_data(conn, *iov, msg->sn, cb);

        // send response as data
        VERBOSE_LOG << "sending IO read response";
        send_io_message(conn, IO_READ_COMP, msg->sn, 0, opts().validate);
    }

    void handle_io_write_request(UcxConnection* conn, const iomsg_t *msg) {
        VERBOSE_LOG << "receiving IO write data";
        assert(msg->data_size != 0);

        BufferIov *iov             = _data_buffers_pool.get();
        IoWriteResponseCallback *w = _callback_pool.get();
        ConnectionStat &conn_stat  = _conn_stat_map.find(conn)->second;

        iov->init(msg->data_size, _data_chunks_pool, msg->sn, opts().validate);
        w->init(this, conn, msg->sn, iov, &conn_stat.completions<IO_WRITE>());

        conn_stat.bytes<IO_WRITE>() += msg->data_size;
        recv_data(conn, *iov, msg->sn, w);
    }

    virtual void dispatch_connection_accepted(UcxConnection* conn) {
        if (!_conn_stat_map.insert(std::make_pair(conn,
                                                  ConnectionStat())).second) {
            LOG << "connection duplicate in statistics map";
            abort();
        }
    }

    virtual void dispatch_connection_error(UcxConnection *conn) {
        LOG << "disconnecting connection " << conn->get_log_prefix()
            << " with status " << ucs_status_string(conn->ucx_status());
        conn->disconnect(new DisconnectCallback(_conn_stat_map, conn));
    }

    virtual void dispatch_io_message(UcxConnection* conn, const void *buffer,
                                     size_t length) {
        iomsg_t const *msg = reinterpret_cast<const iomsg_t*>(buffer);

        VERBOSE_LOG << "got io message " << io_op_names[msg->op] << " sn "
                    << msg->sn << " data size " << msg->data_size
                    << " conn " << conn;

        if (opts().validate) {
            assert(length == opts().iomsg_size);
            validate(msg, length);
        }

        if (msg->op == IO_READ) {
            handle_io_read_request(conn, msg);
        } else if (msg->op == IO_WRITE) {
            handle_io_write_request(conn, msg);
        } else {
            LOG << "Invalid opcode: " << msg->op;
        }
    }

private:
    template<io_op_t op_type> static void
    update_min_max(const conn_stat_map_t::iterator& i,
                   conn_stat_map_t::iterator& min,
                   conn_stat_map_t::iterator& max)
    {
        long i_completions = i->second.completions<op_type>();

        if (i_completions <= min->second.completions<op_type>()) {
            min = i;
        }

        if (i_completions >= max->second.completions<op_type>()) {
            max = i;
        }
    }

    void report_state(double time_interval) {
        ConnectionStat total_stat;
        conn_stat_map_t::iterator it_read_min  = _conn_stat_map.begin(),
                                  it_read_max  = _conn_stat_map.begin(),
                                  it_write_min = _conn_stat_map.begin(),
                                  it_write_max = _conn_stat_map.begin();
        conn_stat_map_t::iterator it;
        for (it = _conn_stat_map.begin(); it != _conn_stat_map.end(); ++it) {
            total_stat += it->second;
            update_min_max<IO_READ>(it, it_read_min, it_read_max);
            update_min_max<IO_WRITE>(it, it_write_min, it_write_max);
        }

        UcxLog log(LOG_PREFIX);
        if (!_conn_stat_map.empty()) {
            log << "read " << total_stat.bytes<IO_READ>() /
                              (time_interval * UCS_MBYTE) << " MBs "
                << "min:" << it_read_min->second.completions<IO_READ>()
                << "(" << it_read_min->first->get_peer_name() << ") "
                << "max:" << it_read_max->second.completions<IO_READ>()
                << " total:" << total_stat.completions<IO_READ>() << " | "
                << "write " << total_stat.bytes<IO_WRITE>() /
                               (time_interval * UCS_MBYTE) << " MBs "
                << "min:" << it_write_min->second.completions<IO_WRITE>()
                << "(" << it_write_min->first->get_peer_name() << ") "
                << "max:" << it_write_max->second.completions<IO_WRITE>()
                << " total:" << total_stat.completions<IO_WRITE>() << " | ";
        }

        log << "active: " << _conn_stat_map.size() << "/"
            << UcxConnection::get_num_instances()
            << " buffers:" << _data_buffers_pool.allocated() << " | ";

        memory_pin_stats_t pin_stats;
        memory_pin_stats(&pin_stats);
        log << "pin bytes:" << pin_stats.bytes
            << " regions:" << pin_stats.regions
            << " evict:" << pin_stats.evictions;

        for (it = _conn_stat_map.begin(); it != _conn_stat_map.end(); ++it) {
            it->second.reset();
        }
    }

private:
    MemoryPool<IoWriteResponseCallback> _callback_pool;
    conn_stat_map_t                     _conn_stat_map;
};


class DemoClient : public P2pDemoCommon {
public:
    typedef struct {
        UcxConnection* conn;
        long           retry_count;                /* Connect retry counter */
        double         prev_connect_time;          /* timestamp of last connect attempt */
        size_t         active_index;               /* Index in active vector */
        long           num_sent[IO_OP_MAX];        /* Number of sent operations */
        long           num_completed[IO_OP_MAX];   /* Number of completed operations */
        size_t         bytes_sent[IO_OP_MAX];      /* Number of bytes sent */
        size_t         bytes_completed[IO_OP_MAX]; /* Number of bytes completed */
    } server_info_t;

private:
    // Map of connection to server index
    typedef std::map<const UcxConnection*, size_t> server_map_t;

    class DisconnectCallback : public UcxCallback {
    public:
        DisconnectCallback(DemoClient &client, size_t _server_index) :
            _client(client), _server_index(_server_index) {
        }

        virtual void operator()(ucs_status_t status) {
            server_info_t &_server_info = _client._server_info[_server_index];

            assert(_server_info.active_index ==
                   std::numeric_limits<size_t>::max());

            _client._num_sent -= get_num_uncompleted(_server_info);
            // Remove connection pointer
            _client._server_index_lookup.erase(_server_info.conn);

            reset_server_info(_server_info);
            delete this;
        }

    private:
        DemoClient &_client;
        size_t     _server_index;
    };

public:
    class ConnectCallback : public UcxCallback {
    public:
        ConnectCallback(DemoClient &client, size_t server_idx) :
            _client(client), _server_idx(server_idx)
        {
        }

        virtual void operator()(ucs_status_t status)
        {
            _client._connecting_servers.erase(_server_idx);

            if (status == UCS_OK) {
                _client.connect_succeed(_server_idx);
            } else {
                _client.connect_failed(_server_idx, status);
            }

            delete this;
        }

    private:
        DemoClient   &_client;
        const size_t _server_idx;
    };

    class IoReadResponseCallback : public UcxCallback {
    public:
        IoReadResponseCallback(size_t buffer_size,
            MemoryPool<IoReadResponseCallback>& pool) :
            _comp_counter(0), _client(NULL),
            _server_index(std::numeric_limits<size_t>::max()),
            _sn(0), _iov(NULL),
            _buffer(UcxContext::malloc(buffer_size, pool.name().c_str())),
            _buffer_size(buffer_size), _pool(pool) {

            if (_buffer == NULL) {
                throw std::bad_alloc();
            }
        }

        void init(DemoClient *client, size_t server_index,
                  uint32_t sn, bool validate, BufferIov *iov) {
            /* wait for all data chunks and the read response completion */
            _comp_counter = iov->size() + 1;
            _client       = client;
            _server_index = server_index;
            _sn           = sn;
            _validate     = validate;
            _iov          = iov;
        }

        ~IoReadResponseCallback() {
            UcxContext::free(_buffer);
        }

        virtual void operator()(ucs_status_t status) {
            if (--_comp_counter > 0) {
                return;
            }

            assert(_server_index != std::numeric_limits<size_t>::max());
            _client->handle_operation_completion(_server_index, IO_READ,
                                                 _iov->data_size());

            if (_validate && (status == UCS_OK)) {
                iomsg_t *msg = reinterpret_cast<iomsg_t*>(_buffer);
                validate(msg, _sn, _buffer_size);
                validate(*_iov, _sn);
            }

            _iov->release();
            _pool.put(this);
        }

        void* buffer() {
            return _buffer;
        }

    private:
        long                                _comp_counter;
        DemoClient*                         _client;
        size_t                              _server_index;
        uint32_t                            _sn;
        bool                                _validate;
        BufferIov*                          _iov;
        void*                               _buffer;
        const size_t                        _buffer_size;
        MemoryPool<IoReadResponseCallback>& _pool;
    };

    DemoClient(const options_t& test_opts) :
        P2pDemoCommon(test_opts),
        _next_active_index(0),
        _num_sent(0), _num_completed(0),
        _start_time(get_time()),
        _read_callback_pool(opts().iomsg_size, "read callbacks") {
    }

    size_t get_active_server_index(const UcxConnection *conn) {
        std::map<const UcxConnection*, size_t>::const_iterator i =
                                                _server_index_lookup.find(conn);
        return (i == _server_index_lookup.end()) ? _server_info.size() :
               i->second;
    }

    void check_counters(const server_info_t& server_info, io_op_t op,
                        const char *type_str)
    {
        ASSERTV(server_info.num_completed[op] < server_info.num_sent[op])
                << type_str << ": op=" << io_op_names[op] << " num_completed="
                << server_info.num_completed[op] << " num_sent="
                << server_info.num_sent[op];
        ASSERTV(_num_completed < _num_sent) << type_str << ": num_completed="
                << _num_completed << " num_sent=" << _num_sent;
    }

    void commit_operation(size_t server_index, io_op_t op, size_t data_size) {
        server_info_t& server_info = _server_info[server_index];

        ASSERTV(get_num_uncompleted(server_info) < opts().conn_window_size)
                << "num_ucompleted=" << get_num_uncompleted(server_info)
                << " conn_window_size=" << opts().conn_window_size;

        ++server_info.num_sent[op];
        ++_num_sent;

        ASSERTV(server_info.bytes_completed[op] <= server_info.bytes_sent[op])
                << "op=" << io_op_names[op] << " bytes_completed="
                << server_info.bytes_completed[op] << " bytes_sent="
                << server_info.bytes_sent[op];
        server_info.bytes_sent[op] += data_size;

        if (get_num_uncompleted(server_info) == opts().conn_window_size) {
            active_servers_remove(server_index);
        }

        check_counters(server_info, op, "commit");
    }

    void handle_operation_completion(size_t server_index, io_op_t op,
                                     size_t data_size) {
        ASSERTV(server_index < _server_info.size()) << "server_index="
                << server_index << " server_info_size=" << _server_info.size();
        server_info_t& server_info = _server_info[server_index];

        ASSERTV(get_num_uncompleted(server_info) <= opts().conn_window_size)
                << "num_uncompleted=" << get_num_uncompleted(server_info)
                << " conn_window_size" << opts().conn_window_size;
        assert(_server_index_lookup.find(server_info.conn) !=
               _server_index_lookup.end());
        check_counters(server_info, op, "completion");

        if ((get_num_uncompleted(server_info) == opts().conn_window_size) &&
            !server_info.conn->is_disconnecting()) {
            active_servers_add(server_index);
        }

        server_info.bytes_completed[op] += data_size;
        ++_num_completed;
        ++server_info.num_completed[op];

        if (get_num_uncompleted(server_info, op) == 0) {
            ASSERTV(server_info.bytes_completed[op] ==
                    server_info.bytes_sent[op])
                    << "op=" << io_op_names[op] << " bytes_completed="
                    << server_info.bytes_completed[op] << " bytes_sent="
                    << server_info.bytes_sent[op];
        } else {
            ASSERTV(server_info.bytes_completed[op] <=
                    server_info.bytes_sent[op])
                    << "op=" << io_op_names[op] << " bytes_completed="
                    << server_info.bytes_completed[op] << " bytes_sent="
                    << server_info.bytes_sent[op];
        }
    }

    size_t do_io_read(size_t server_index, uint32_t sn) {
        server_info_t& server_info = _server_info[server_index];
        size_t data_size           = get_data_size();
        bool validate              = opts().validate;

        if (!send_io_message(server_info.conn, IO_READ, sn, data_size,
                             validate)) {
            return 0;
        }

        BufferIov *iov            = _data_buffers_pool.get();
        IoReadResponseCallback *r = _read_callback_pool.get();

        commit_operation(server_index, IO_READ, data_size);

        iov->init(data_size, _data_chunks_pool, sn, validate);
        r->init(this, server_index, sn, validate, iov);

        recv_data(server_info.conn, *iov, sn, r);
        server_info.conn->recv_data(r->buffer(), opts().iomsg_size, sn, r);

        return data_size;
    }

    size_t do_io_write(size_t server_index, uint32_t sn) {
        server_info_t& server_info = _server_info[server_index];
        size_t data_size           = get_data_size();
        bool validate              = opts().validate;

        if (!send_io_message(server_info.conn, IO_WRITE, sn, data_size,
                             validate)) {
            return 0;
        }

        BufferIov *iov           = _data_buffers_pool.get();
        SendCompleteCallback *cb = _send_callback_pool.get();

        commit_operation(server_index, IO_WRITE, data_size);

        iov->init(data_size, _data_chunks_pool, sn, validate);
        cb->init(iov, NULL);

        VERBOSE_LOG << "sending data " << iov << " size "
                    << data_size << " sn " << sn;
        send_data(server_info.conn, *iov, sn, cb);

        return data_size;
    }

    static void dump_server_info(const server_info_t& server_info,
                                 UcxLog &log)
    {
        log << server_info.conn->get_log_prefix()
            << " read " << server_info.num_completed[IO_READ] << "/"
            << server_info.num_sent[IO_READ] << " write "
            << server_info.num_completed[IO_WRITE] << "/"
            << server_info.num_sent[IO_WRITE];

        if (server_info.conn->is_disconnecting()) {
            log << " (disconnecting)";
        }
    }

    void dump_timeout_waiting_for_replies_info()
    {
        size_t total_uncompleted = 0;
        UcxLog log(LOG_PREFIX);

        log << "timeout waiting for " << (_num_sent - _num_completed)
            << " replies on the following connections:";

        for (server_map_t::const_iterator iter = _server_index_lookup.begin();
             iter != _server_index_lookup.end(); ++iter) {
            size_t server_index = iter->second;
            if (get_num_uncompleted(server_index) == 0) {
                continue;
            }

            log << "\n";
            dump_server_info(_server_info[server_index], log);
            total_uncompleted++;
        }

        log << "\ntotal: " << total_uncompleted;
    }

    void disconnect_uncompleted_servers(const char *reason) {
        std::vector<size_t> server_idxs;
        for (server_map_t::const_iterator iter = _server_index_lookup.begin();
             iter != _server_index_lookup.end(); ++iter) {
            size_t server_index = iter->second;
            if (get_num_uncompleted(server_index) > 0) {
                server_idxs.push_back(server_index);
            }
        }

        while (!server_idxs.empty()) {
            disconnect_server(server_idxs.back(), reason);
            server_idxs.pop_back();
        }
    }

    virtual void dispatch_io_message(UcxConnection* conn, const void *buffer,
                                     size_t length) {
        iomsg_t const *msg = reinterpret_cast<const iomsg_t*>(buffer);

        VERBOSE_LOG << "got io message " << io_op_names[msg->op] << " sn "
                    << msg->sn << " data size " << msg->data_size
                    << " conn " << conn;

        if (msg->op >= IO_COMP_MIN) {
            assert(msg->op == IO_WRITE_COMP);

            size_t server_index = get_active_server_index(conn);
            if (server_index < _server_info.size()) {
                handle_operation_completion(server_index, IO_WRITE,
                                            msg->data_size);
            } else {
                /* do not increment _num_completed here since we decremented
                 * _num_sent on connection termination */
                LOG << "got WRITE completion on failed connection";
            }
        }
    }

    static long get_num_uncompleted(const server_info_t& server_info,
                                    io_op_t op)
    {
        long num_uncompleted = server_info.num_sent[op] -
                               server_info.num_completed[op];

        assert(num_uncompleted >= 0);
        return num_uncompleted;
    }

    static long get_num_uncompleted(const server_info_t& server_info)
    {
        return get_num_uncompleted(server_info, IO_READ) +
               get_num_uncompleted(server_info, IO_WRITE);
    }

    long get_num_uncompleted(size_t server_index) const
    {
        assert(server_index < _server_info.size());
        return get_num_uncompleted(_server_info[server_index]);
    }

    static void reset_server_info(server_info_t& server_info) {
        server_info.conn                    = NULL;
        server_info.active_index            = std::numeric_limits<size_t>::max();
        for (int op = 0; op < IO_OP_MAX; ++op) {
            server_info.num_sent[op]        = 0;
            server_info.num_completed[op]   = 0;
            server_info.bytes_sent[op]      = 0;
            server_info.bytes_completed[op] = 0;
        }
    }

    virtual void dispatch_connection_error(UcxConnection *conn) {
        size_t server_index = get_active_server_index(conn);
        if (server_index < _server_info.size()) {
            disconnect_server(server_index,
                              ucs_status_string(conn->ucx_status()));
        }
    }

    void disconnect_server(size_t server_index, const char *reason) {
        server_info_t& server_info = _server_info[server_index];
        assert(server_info.conn != NULL);

        bool disconnecting = server_info.conn->is_disconnecting();

        {
            UcxLog log(LOG_PREFIX);
            if (disconnecting) {
                log << "not ";
            }

            log << "disconnecting ";
            dump_server_info(server_info, log);
            log << " due to \"" << reason << "\"";

            if (disconnecting) {
                log << " because disconnection is already in progress";
            }
        }

        if (!disconnecting) {
            // remove active servers entry
            if (server_info.active_index !=
                std::numeric_limits<size_t>::max()) {
                active_servers_remove(server_index);
            }

            /* Destroying the connection will complete its outstanding
             * operations */
            server_info.conn->disconnect(new DisconnectCallback(*this,
                                                                server_index));
        }

        // server must be removed from the list of active servers
        assert(server_info.active_index == std::numeric_limits<size_t>::max());
    }

    void wait_for_responses(long max_outstanding) {
        bool timer_started  = false;
        bool timer_finished = false;
        double start_time, curr_time, elapsed_time;
        long count = 0;

        while (((_num_sent - _num_completed) > max_outstanding) &&
               (_status == OK)) {
            if ((count++ < BUSY_PROGRESS_COUNT) || timer_finished) {
                progress();
                continue;
            }

            count     = 0;
            curr_time = get_time();

            if (!timer_started) {
                start_time    = curr_time;
                timer_started = true;
                continue;
            }

            elapsed_time = curr_time - start_time;
            if (elapsed_time > _test_opts.client_timeout) {
                dump_timeout_waiting_for_replies_info();
                if (!_test_opts.debug_timeout) {
                    // don't destroy connections, they will be debugged
                    disconnect_uncompleted_servers("timeout for replies");
                }
                timer_finished = true;
            }
            check_time_limit(curr_time);
        }
    }

    void connect(size_t server_index)
    {
        const char *server = opts().servers[server_index];
        struct sockaddr_in connect_addr;
        std::string server_addr;
        int port_num;

        memset(&connect_addr, 0, sizeof(connect_addr));
        connect_addr.sin_family = AF_INET;

        const char *port_separator = strchr(server, ':');
        if (port_separator == NULL) {
            /* take port number from -p argument */
            port_num    = opts().port_num;
            server_addr = server;
        } else {
            /* take port number from the server parameter */
            server_addr = std::string(server)
                            .substr(0, port_separator - server);
            port_num    = atoi(port_separator + 1);
        }

        connect_addr.sin_port = htons(port_num);
        int ret = inet_pton(AF_INET, server_addr.c_str(), &connect_addr.sin_addr);
        if (ret != 1) {
            LOG << "invalid address " << server_addr;
            abort();
        }

        if (!_connecting_servers.insert(server_index).second) {
            LOG << server_name(server_index) << " is already connecting";
            abort();
        }

        UcxConnection *conn = new UcxConnection(*this);
        _server_info[server_index].conn = conn;
        conn->connect((const struct sockaddr*)&connect_addr,
                      sizeof(connect_addr),
                      new ConnectCallback(*this, server_index));
    }

    const std::string server_name(size_t server_index) {
        std::stringstream ss;
        ss << "server [" << server_index << "] " << opts().servers[server_index];
        return ss.str();
    }

    void connect_succeed(size_t server_index)
    {
        server_info_t &server_info = _server_info[server_index];
        long attempts              = server_info.retry_count + 1;

        server_info.retry_count                = 0;
        server_info.prev_connect_time          = 0.;
        _server_index_lookup[server_info.conn] = server_index;
        active_servers_add(server_index);
        LOG << "Connected to " << server_name(server_index) << " after "
            << attempts << " attempts";
    }

    void connect_failed(size_t server_index, ucs_status_t status) {
        server_info_t &server_info = _server_info[server_index];

        if (++server_info.retry_count >= opts().retries) {
            /* If at least one server exceeded its retries, bail */
            _status = CONN_RETRIES_EXCEEDED;
        }

        {
            UcxLog log(LOG_PREFIX);
            log << "Connect to " << server_name(server_index) << " failed"
                << " (retry " << server_info.retry_count;
            if (opts().retries < std::numeric_limits<long>::max()) {
                log << "/" << opts().retries;
            }
            log << ")";
        }

        disconnect_server(server_index, ucs_status_string(status));
    }

    void connect_all(bool force) {
        if (_server_index_lookup.size() == _server_info.size()) {
            assert((_status == OK) || (_status == TERMINATE_SIGNALED));
            // All servers are connected
            return;
        }

        if (!force && !_server_index_lookup.empty()) {
            // The active list is not empty, and we don't have to check the
            // connect retry timeout
            return;
        }

        double curr_time = get_time();
        for (size_t server_index = 0; server_index < _server_info.size();
             ++server_index) {
            server_info_t& server_info = _server_info[server_index];
            if (server_info.conn != NULL) {
                // Already connecting to this server
                continue;
            }

            // If retry count exceeded for at least one server, we should have
            // exited already
            assert((_status == OK) || (_status == TERMINATE_SIGNALED));
            assert(server_info.retry_count < opts().retries);

            if (curr_time < (server_info.prev_connect_time +
                             opts().retry_interval)) {
                // Not enough time elapsed since previous connection attempt
                continue;
            }

            connect(server_index);
            server_info.prev_connect_time = curr_time;
            assert(server_info.conn != NULL);
            assert((_status == OK) || (_status == TERMINATE_SIGNALED));
        }
    }

    size_t pick_server_index() {
        assert(_next_active_index < _active_servers.size());
        size_t server_index = _active_servers[_next_active_index];
        assert(get_num_uncompleted(server_index) < opts().conn_window_size);
        assert(_server_info[server_index].conn != NULL);
        assert(_server_info[server_index].conn->ucx_status() == UCS_OK);

        if (++_next_active_index == _active_servers.size()) {
            _next_active_index = 0;
        }

        return server_index;
    }

    static inline bool is_control_iter(long iter) {
        return (iter % 10) == 0;
    }

    void destroy_servers()
    {
        for (size_t server_index = 0; server_index < _server_info.size();
             ++server_index) {
            server_info_t& server_info = _server_info[server_index];
            if (server_info.conn == NULL) {
                continue;
            }

            disconnect_server(server_index, "End of the Client run");
        }

        if (!_server_index_lookup.empty()) {
            LOG << "waiting for " << _server_index_lookup.size()
                << " disconnects to complete";
            do {
                progress();
            } while (!_server_index_lookup.empty());
        }

        wait_disconnected_connections();
    }

    status_t run() {
        _server_info.resize(opts().servers.size());
        std::for_each(_server_info.begin(), _server_info.end(),
                      reset_server_info);

        _status = OK;

        // TODO reset these values by canceling requests
        _num_sent      = 0;
        _num_completed = 0;

        uint32_t sn          = IoDemoRandom::rand<uint32_t>();
        double prev_time     = get_time();
        long total_iter      = 0;
        long total_prev_iter = 0;

        while ((total_iter < opts().iter_count) && (_status == OK)) {
            connect_all(is_control_iter(total_iter));
            if (_status != OK) {
                break;
            }

            if (_server_index_lookup.empty()) {
                if (_connecting_servers.empty()) {
                    LOG << "All remote servers are down, reconnecting in "
                        << opts().retry_interval << " seconds";
                    sleep(opts().retry_interval);
                    check_time_limit(get_time());
                } else {
                    progress();
                }
                continue;
            }

            VERBOSE_LOG << " <<<< iteration " << total_iter << " >>>>";
            long conns_window_size = opts().conn_window_size *
                                     _server_index_lookup.size();
            long max_outstanding   = std::min(opts().window_size,
                                              conns_window_size) - 1;

            progress();
            wait_for_responses(max_outstanding);
            if (_status != OK) {
                break;
            }

            if (_active_servers.empty()) {
                // It is possible that the number of active servers to use is 0
                // after wait_for_responces(), if some clients were closed in
                // UCP Worker progress during handling of remote disconnection
                // from servers
                continue;
            }

            size_t server_index = pick_server_index();
            io_op_t op          = get_op();
            switch (op) {
            case IO_READ:
                do_io_read(server_index, sn);
                break;
            case IO_WRITE:
                do_io_write(server_index, sn);
                break;
            default:
                abort();
            }

            ++total_iter;
            ++sn;

            if (is_control_iter(total_iter) &&
                ((total_iter - total_prev_iter) >= _server_index_lookup.size())) {
                // Print performance every <print_interval> seconds
                double curr_time = get_time();
                if (curr_time >= (prev_time + opts().print_interval)) {
                    wait_for_responses(0);
                    if (_status != OK) {
                        break;
                    }

                    report_performance(total_iter - total_prev_iter,
                                       curr_time - prev_time);

                    total_prev_iter = total_iter;
                    prev_time       = curr_time;

                    check_time_limit(curr_time);
                }
            }
        }

        wait_for_responses(0);
        if (_status == OK) {
            double curr_time = get_time();
            report_performance(total_iter - total_prev_iter,
                               curr_time - prev_time);
        }

        destroy_servers();

        return _status;
    }

    status_t get_status() const {
        return _status;
    }

    static const char* get_status_str(status_t status) {
        switch (status) {
        case OK:
            return "OK";
        case CONN_RETRIES_EXCEEDED:
            return "connection retries exceeded";
        case RUNTIME_EXCEEDED:
            return "run-time exceeded";
        case TERMINATE_SIGNALED:
            return "run-time terminated by signal";
        default:
            return "invalid status";
        }
    }

private:
    inline io_op_t get_op() {
        if (opts().operations.size() == 1) {
            return opts().operations[0];
        }

        return opts().operations[IoDemoRandom::urand<size_t>(
                                         opts().operations.size())];
    }

    struct io_op_perf_info_t {
        long   min; // Minimum number of completed operations on some server
        long   max; // Maximum number of completed operations on some server
        size_t min_index; // Server index with the smallest number of completed
                          // operations
        long   total; // Total number of completed operations
        size_t total_bytes; // Total number of bytes of completed operations
    };

    void report_performance(long num_iters, double elapsed) {
        if (num_iters == 0) {
            return;
        }

        double latency_usec = (elapsed / num_iters) * 1e6;
        std::vector<io_op_perf_info_t> io_op_perf_info;
        UcxLog log(LOG_PREFIX);

        io_op_perf_info.resize(IO_OP_MAX + 1);
        for (int op = 0; op <= IO_OP_MAX; ++op) {
            io_op_perf_info[op].min         = std::numeric_limits<long>::max();
            io_op_perf_info[op].max         = 0;
            io_op_perf_info[op].min_index   = _server_info.size();
            io_op_perf_info[op].total       = 0;
            io_op_perf_info[op].total_bytes = 0;
        }

        // Collect min/max among all connections
        for (size_t server_index = 0; server_index < _server_info.size();
             ++server_index) {
            server_info_t& server_info   = _server_info[server_index];
            long total_completed         = 0;
            size_t total_bytes_completed = 0;
            for (int op = 0; op <= IO_OP_MAX; ++op) {
                size_t bytes_completed;
                long num_completed;
                if (op != IO_OP_MAX) {
                    assert(server_info.bytes_sent[op] ==
                                   server_info.bytes_completed[op]);
                    bytes_completed = server_info.bytes_completed[op];
                    num_completed   = server_info.num_completed[op];

                    size_t min_index = io_op_perf_info[op].min_index;
                    if ((num_completed < io_op_perf_info[op].min) ||
                        ((num_completed == io_op_perf_info[op].min) &&
                         (server_info.retry_count >
                                  _server_info[min_index].retry_count))) {
                        io_op_perf_info[op].min_index = server_index;
                    }

                    total_bytes_completed          += bytes_completed;
                    total_completed                += num_completed;
                    server_info.num_sent[op]        = 0;
                    server_info.num_completed[op]   = 0;
                    server_info.bytes_sent[op]      = 0;
                    server_info.bytes_completed[op] = 0;
                } else {
                    bytes_completed = total_bytes_completed;
                    num_completed   = total_completed;
                }

                io_op_perf_info[op].min          =
                        std::min(num_completed, io_op_perf_info[op].min);
                io_op_perf_info[op].max          =
                        std::max(num_completed, io_op_perf_info[op].max);
                io_op_perf_info[op].total       += num_completed;
                io_op_perf_info[op].total_bytes += bytes_completed;
            }
        }

        log << "total min:" << io_op_perf_info[IO_OP_MAX].min
            << " max:" << io_op_perf_info[IO_OP_MAX].max
            << " total:" << io_op_perf_info[IO_OP_MAX].total;

        // Report bandwidth and min/max/total for every operation
        for (int op = 0; op < IO_OP_MAX; ++op) {
            log << " | ";

            double throughput_mbs = (io_op_perf_info[op].total_bytes /
                                     elapsed) / UCS_MBYTE;
            log << io_op_names[op] << " " << throughput_mbs << " MBs"
                << " min:" << io_op_perf_info[op].min << "("
                << opts().servers[io_op_perf_info[op].min_index]
                << ") max:" << io_op_perf_info[op].max
                << " total:" << io_op_perf_info[op].total;
        }

        log << " | active:" << _server_index_lookup.size() << "/"
            << UcxConnection::get_num_instances();

        if (opts().window_size == 1) {
            log << " latency:" << latency_usec << "usec";
        }

        log << " buffers:" << _data_buffers_pool.allocated();
    }

    inline void check_time_limit(double current_time) {
        if ((_status == OK) &&
            ((current_time - _start_time) >= opts().client_runtime_limit)) {
            _status = RUNTIME_EXCEEDED;
        }
    }

    void active_servers_swap(size_t index1, size_t index2) {
        assert(index1 < _active_servers.size());
        assert(index2 < _active_servers.size());
        size_t& active_server1 = _active_servers[index1];
        size_t& active_server2 = _active_servers[index2];

        std::swap(_server_info[active_server1].active_index,
                  _server_info[active_server2].active_index);
        std::swap(active_server1, active_server2);
    }

    void active_servers_add(size_t server_index)
    {
        server_info_t &server_info = _server_info[server_index];

        // First, add the new server at the end
        assert(server_info.active_index == std::numeric_limits<size_t>::max());
        _active_servers.push_back(server_index);
        server_info.active_index = _active_servers.size() - 1;

        // Swap this new server with a random index, and it could be sent in
        // either this round or the next round
        size_t active_index = IoDemoRandom::urand(_active_servers.size());
        active_servers_swap(active_index, _active_servers.size() - 1);
        assert(server_info.active_index == active_index);
    }

    void active_servers_remove(size_t server_index)
    {
        server_info_t &server_info = _server_info[server_index];

        // Swap active_index with the last element, and remove it
        size_t active_index = server_info.active_index;
        active_servers_swap(active_index, _active_servers.size() - 1);
        _active_servers.pop_back();
        server_info.active_index = std::numeric_limits<size_t>::max();

        if (_next_active_index == _active_servers.size()) {
            // If the next active index is the last one, then the next active
            // index should be 0
            _next_active_index = 0;
        } else if (active_index < _next_active_index) {
            // Swap the last element to use (which is saved in active_index now)
            // and the most recent elemenet which was used for IO.
            // It guarantees that we will not skip IO for the last element.
            --_next_active_index;
            active_servers_swap(active_index, _next_active_index);
        }
    }

private:
    std::vector<server_info_t>              _server_info;
    // Connection establishment is in progress
    std::set<size_t>                        _connecting_servers;
    // Active servers is the list of communicating servers
    std::vector<size_t>                     _active_servers;
    // Active server index to use for communications
    size_t                                  _next_active_index;
    // Number of active servers to use handles window size, server becomes
    // "unused" if its window is full
    server_map_t                            _server_index_lookup;
    long                                    _num_sent;
    long                                    _num_completed;
    double                                  _start_time;
    MemoryPool<IoReadResponseCallback>      _read_callback_pool;
};

static int set_data_size(char *str, options_t *test_opts)
{
    const static char token = ':';
    char *val1, *val2;

    if (strchr(str, token) == NULL) {
        test_opts->min_data_size =
            test_opts->max_data_size = strtol(str, NULL, 0);
        return 0;
    }

    val1 = strtok(str, ":");
    val2 = strtok(NULL, ":");

    if ((val1 != NULL) && (val2 != NULL)) {
        test_opts->min_data_size = strtol(val1, NULL, 0);
        test_opts->max_data_size = strtol(val2, NULL, 0);
    } else if (val1 != NULL) {
        if (str[0] == ':') {
            test_opts->min_data_size = 0;
            test_opts->max_data_size = strtol(val1, NULL, 0);
        } else {
            test_opts->min_data_size = strtol(val1, NULL, 0);
        }
    } else {
        return -1;
    }

    return 0;
}

static int set_time(char *str, double *dest_p)
{
    char units[3] = "";
    int num_fields;
    double value;
    double per_sec;

    if (!strcmp(str, "inf")) {
        *dest_p = std::numeric_limits<double>::max();
        return 0;
    }

    num_fields = sscanf(str, "%lf%c%c", &value, &units[0], &units[1]);
    if (num_fields == 1) {
        per_sec = 1;
    } else if ((num_fields == 2) || (num_fields == 3)) {
        if (!strcmp(units, "h")) {
            per_sec = 1.0 / 3600.0;
        } else if (!strcmp(units, "m")) {
            per_sec = 1.0 / 60.0;
        } else if (!strcmp(units, "s")) {
            per_sec = 1;
        } else if (!strcmp(units, "ms")) {
            per_sec = 1e3;
        } else if (!strcmp(units, "us")) {
            per_sec = 1e6;
        } else if (!strcmp(units, "ns")) {
            per_sec = 1e9;
        } else {
            return -1;
        }
    } else {
        return -1;
    }

    *(double*)dest_p = value / per_sec;
    return 0;
}

static void adjust_opts(options_t *test_opts) {
    if (test_opts->operations.size() == 0) {
        test_opts->operations.push_back(IO_WRITE);
    }

    test_opts->chunk_size = std::min(test_opts->chunk_size,
                                     test_opts->max_data_size);
}

static int parse_window_size(const char *optarg, long &window_size,
                             const std::string &window_size_str) {
    window_size = strtol(optarg, NULL, 0);
    if ((window_size <= 0) ||
        // If the converted value falls out of range of corresponding
        // return type, LONG_MAX is returned
        (window_size == std::numeric_limits<long>::max())) {
        std::cout << "invalid " << window_size_str << " size '" << optarg
                  << "'" << std::endl;
        return -1;
    }

    return 0;
}

static int parse_args(int argc, char **argv, options_t *test_opts)
{
    char *str;
    bool found;
    int c;

    test_opts->port_num              = 1337;
    test_opts->connect_timeout       = 20.0;
    test_opts->client_timeout        = 50.0;
    test_opts->retries               = std::numeric_limits<long>::max();
    test_opts->retry_interval        = 5.0;
    test_opts->client_runtime_limit  = std::numeric_limits<double>::max();
    test_opts->print_interval        = 1.0;
    test_opts->min_data_size         = 4096;
    test_opts->max_data_size         = 4096;
    test_opts->chunk_size            = std::numeric_limits<unsigned>::max();
    test_opts->num_offcache_buffers  = 0;
    test_opts->iomsg_size            = 256;
    test_opts->iter_count            = 1000;
    test_opts->window_size           = 1;
    test_opts->conn_window_size      = 1;
    test_opts->random_seed           = std::time(NULL) ^ getpid();
    test_opts->verbose               = false;
    test_opts->validate              = false;
    test_opts->debug_timeout         = false;
    test_opts->rndv_thresh           = UcxContext::rndv_thresh_auto;

    while ((c = getopt(argc, argv,
                       "p:c:r:d:b:i:w:a:k:o:t:n:l:s:y:vqDHP:L:R:")) != -1) {
        switch (c) {
        case 'p':
            test_opts->port_num = atoi(optarg);
            break;
        case 'c':
            if (strcmp(optarg, "inf")) {
                test_opts->retries = strtol(optarg, NULL, 0);
            }
            break;
        case 'y':
            if (set_time(optarg, &test_opts->retry_interval) != 0) {
                std::cout << "invalid '" << optarg
                          << "' value for retry interval" << std::endl;
                return -1;
            }
            break;
        case 'r':
            test_opts->iomsg_size = strtol(optarg, NULL, 0);
            if (test_opts->iomsg_size < sizeof(P2pDemoCommon::iomsg_t)) {
                std::cout << "io message size must be >= "
                          << sizeof(P2pDemoCommon::iomsg_t) << std::endl;
                return -1;
            }
            break;
        case 'd':
            if (set_data_size(optarg, test_opts) == -1) {
                std::cout << "invalid data size range '" << optarg << "'" << std::endl;
                return -1;
            }
            break;
        case 'b':
            test_opts->num_offcache_buffers = strtol(optarg, NULL, 0);
            break;
        case 'i':
            test_opts->iter_count = strtol(optarg, NULL, 0);
            if (test_opts->iter_count == 0) {
                test_opts->iter_count = std::numeric_limits<long int>::max();
            }
            break;
        case 'w':
            if (parse_window_size(optarg, test_opts->window_size,
                                  "window") != 0) {
                return -1;
            }
            break;
        case 'a':
            if (parse_window_size(optarg, test_opts->conn_window_size,
                                  "per connection window") != 0) {
                return -1;
            }
            break;
        case 'k':
            test_opts->chunk_size = strtol(optarg, NULL, 0);
            break;
        case 'o':
            str = strtok(optarg, ",");
            while (str != NULL) {
                found = false;

                for (int op_it = 0; op_it < IO_OP_MAX; ++op_it) {
                    if (!strcmp(io_op_names[op_it], str)) {
                        io_op_t op = static_cast<io_op_t>(op_it);
                        if (std::find(test_opts->operations.begin(),
                                      test_opts->operations.end(),
                                      op) == test_opts->operations.end()) {
                            test_opts->operations.push_back(op);
                        }
                        found = true;
                    }
                }

                if (!found) {
                    std::cout << "invalid operation name '" << str << "'" << std::endl;
                    return -1;
                }

                str = strtok(NULL, ",");
            }

            if (test_opts->operations.size() == 0) {
                std::cout << "no operation names were provided '" << optarg << "'" << std::endl;
                return -1;
            }
            break;
        case 'n':
            if (set_time(optarg, &test_opts->connect_timeout) != 0) {
                std::cout << "invalid '" << optarg << "' value for connect timeout" << std::endl;
                return -1;
            }
            break;
        case 't':
            if (set_time(optarg, &test_opts->client_timeout) != 0) {
                std::cout << "invalid '" << optarg << "' value for client timeout" << std::endl;
                return -1;
            }
            break;
        case 'l':
            if (set_time(optarg, &test_opts->client_runtime_limit) != 0) {
                std::cout << "invalid '" << optarg << "' value for client run-time limit" << std::endl;
                return -1;
            }
            break;
        case 's':
            test_opts->random_seed = strtoul(optarg, NULL, 0);
            break;
        case 'v':
            test_opts->verbose = true;
            break;
        case 'q':
            test_opts->validate = true;
            break;
        case 'D':
            test_opts->debug_timeout = true;
            break;
        case 'H':
            UcxLog::use_human_time = true;
            break;
        case 'L':
            UcxLog::timeout_sec = atof(optarg);
            break;
        case 'P':
            test_opts->print_interval = atof(optarg);
            break;
        case 'R':
            test_opts->rndv_thresh = strtol(optarg, NULL, 0);
            break;
        case 'h':
        default:
            std::cout << "Usage: io_demo [options] [server_address]" << std::endl;
            std::cout << "       or io_demo [options] [server_address0:port0] [server_address1:port1]..." << std::endl;
            std::cout << "" << std::endl;
            std::cout << "Supported options are:" << std::endl;
            std::cout << "  -p <port>                  TCP port number to use" << std::endl;
            std::cout << "  -n <connect timeout>       Timeout for connecting to the peer (or \"inf\")" << std::endl;
            std::cout << "  -o <op1,op2,...,opN>       Comma-separated string of IO operations [read|write]" << std::endl;
            std::cout << "                             NOTE: if using several IO operations, performance" << std::endl;
            std::cout << "                                   measurments may be inaccurate" << std::endl;
            std::cout << "  -d <min>:<max>             Range that should be used to get data" << std::endl;
            std::cout << "                             size of IO payload" << std::endl;
            std::cout << "  -b <number of buffers>     Number of offcache IO buffers" << std::endl;
            std::cout << "  -i <iterations-count>      Number of iterations to run communication" << std::endl;
            std::cout << "  -w <window-size>           Number of outstanding requests" << std::endl;
            std::cout << "  -a <conn-window-size>      Number of outstanding requests per connection" << std::endl;
            std::cout << "  -k <chunk-size>            Split the data transfer to chunks of this size" << std::endl;
            std::cout << "  -r <io-request-size>       Size of IO request packet" << std::endl;
            std::cout << "  -t <client timeout>        Client timeout (or \"inf\")" << std::endl;
            std::cout << "  -c <retries>               Number of connection retries on client or " << std::endl;
            std::cout << "                             listen retries on server" << std::endl;
            std::cout << "                             (or \"inf\") for failure" << std::endl;
            std::cout << "  -y <retry interval>        Retry interval" << std::endl;
            std::cout << "  -l <client run-time limit> Time limit to run the IO client (or \"inf\")" << std::endl;
            std::cout << "                             Examples: -l 17.5s; -l 10m; 15.5h" << std::endl;
            std::cout << "  -s <random seed>           Random seed to use for randomizing" << std::endl;
            std::cout << "  -v                         Set verbose mode" << std::endl;
            std::cout << "  -q                         Enable data integrity and transaction check" << std::endl;
            std::cout << "  -D                         Enable debugging mode for IO operation timeouts" << std::endl;
            std::cout << "  -H                         Use human-readable timestamps" << std::endl;
            std::cout << "  -L <logger life-time>      Set life time of logger object, if log message print takes longer, warning will be printed" << std::endl;
            std::cout << "  -P <interval>              Set report printing interval"  << std::endl;
            std::cout << "  -R <rndv-thresh>           Rendezvous threshold used to force eager or rendezvous protocol";
            std::cout << "" << std::endl;
            return -1;
        }
    }

    while (optind < argc) {
        test_opts->servers.push_back(argv[optind++]);
    }

    adjust_opts(test_opts);

    return 0;
}

static int do_server(const options_t& test_opts)
{
    DemoServer server(test_opts);
    if (!server.init()) {
        return -1;
    }

    server.run();
    return 0;
}

static int do_client(options_t& test_opts)
{
    IoDemoRandom::srand(test_opts.random_seed);
    LOG << "random seed: " << test_opts.random_seed;

    // randomize servers to optimize startup
    std::random_shuffle(test_opts.servers.begin(), test_opts.servers.end(),
                        IoDemoRandom::urand<size_t>);

    UcxLog vlog(LOG_PREFIX, test_opts.verbose);
    vlog << "List of servers:";
    for (size_t i = 0; i < test_opts.servers.size(); ++i) {
        vlog << " " << test_opts.servers[i];
    }

    DemoClient client(test_opts);
    if (!client.init()) {
        return -1;
    }

    DemoClient::status_t status = client.run();
    LOG << "Client exit with status '" << DemoClient::get_status_str(status) << "'";
    return ((status == DemoClient::OK) ||
            (status == DemoClient::RUNTIME_EXCEEDED)) ? 0 : -1;
}

static void print_info(int argc, char **argv)
{
    // Process ID and hostname
    char host[64];
    gethostname(host, sizeof(host));
    LOG << "Starting io_demo pid " << getpid() << " on " << host;

    // Command line
    std::stringstream cmdline;
    for (int i = 0; i < argc; ++i) {
        cmdline << argv[i] << " ";
    }
    LOG << "Command line: " << cmdline.str();

    // UCX library path
    Dl_info info;
    int ret = dladdr((void*)ucp_init_version, &info);
    if (ret) {
        LOG << "UCX library path: " << info.dli_fname;
    }
}

int main(int argc, char **argv)
{
    options_t test_opts;
    int ret;

    print_info(argc, argv);

    ret = parse_args(argc, argv, &test_opts);
    if (ret < 0) {
        return ret;
    }

    if (test_opts.servers.empty()) {
        return do_server(test_opts);
    } else {
        return do_client(test_opts);
    }
}
