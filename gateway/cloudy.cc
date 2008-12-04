#include "gateway/cloudy.h"
#include "memproto/memproto.h"
#include <stdexcept>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#include <algorithm>
#include <memory>

namespace kumo {


static const size_t CLOUDY_INITIAL_ALLOCATION_SIZE = 2048;
static const size_t CLOUDY_RESERVE_SIZE = 1024;

Cloudy::Cloudy(int lsock) :
	m_lsock(lsock) { }

Cloudy::~Cloudy() {}


void Cloudy::accepted(void* data, int fd)
{
	Gateway* gw = reinterpret_cast<Gateway*>(data);
	if(fd < 0) {
		LOG_FATAL("accept failed: ",strerror(-fd));
		gw->signal_end(SIGTERM);
		return;
	}
	mp::set_nonblock(fd);
	mp::iothreads::add<Connection>(fd, gw);
}

void Cloudy::listen(Gateway* gw)
{
	mp::iothreads::listen(m_lsock,
			&Cloudy::accepted,
			reinterpret_cast<void*>(gw));
}


class Cloudy::Connection : public iothreads::handler {
public:
	Connection(int fd, Gateway* gw);
	~Connection();

public:
	void read_event();

private:
	// get, getq, getk, getkq
	inline void memproto_getx(memproto_header* h, const char* key, uint16_t keylen);

	// set
	inline void memproto_set(memproto_header* h, const char* key, uint16_t keylen,
			const char* val, uint16_t vallen,
			uint32_t flags, uint32_t expiration);

	// delete
	inline void memproto_delete(memproto_header* h, const char* key, uint16_t keylen,
			uint32_t expiration);

private:
	memproto_parser m_memproto;
	Gateway* m_gw;

	typedef Gateway::get_request get_request;
	typedef Gateway::set_request set_request;
	typedef Gateway::delete_request delete_request;

	typedef Gateway::get_response get_response;
	typedef Gateway::set_response set_response;
	typedef Gateway::delete_response delete_response;

	typedef rpc::shared_zone shared_zone;

	shared_zone m_zone;

	struct buffer_t {
		struct counter {
			counter(size_t sz)
			{
				ptr = (char*)::malloc(sz);
				if(!ptr) { throw std::bad_alloc(); }
			}
			counter(char* p) : ptr(p) { }
			~counter()
			{
				::free(ptr);
			}
			void realloc(size_t sz)
			{
				void* tmp = ::realloc(ptr, sz);
				if(!tmp) { throw std::bad_alloc(); }
				ptr = (char*)tmp;
			}
			char* ptr;
		};
		typedef mp::shared_ptr<counter> shared_counter;

		struct counter_keeper {
			counter_keeper(shared_counter c) : m(c) { }
			counter_keeper() { }
			~counter_keeper() { }
		private:
			shared_counter m;
		};

		buffer_t() :
			m_counter(new counter(CLOUDY_INITIAL_ALLOCATION_SIZE)),
			m_used(0),
			m_free(CLOUDY_INITIAL_ALLOCATION_SIZE),
			m_off(0),
			m_lot(0) { }

		~buffer_t() { }

		void reserve()
		{
			if(m_free >= CLOUDY_RESERVE_SIZE) { return; }
			if(m_lot == 0) {
				size_t nsize = (m_used + m_free)*2;
				while(nsize < CLOUDY_RESERVE_SIZE) { nsize *= 2; }
				m_counter->realloc(nsize);
				m_free = nsize - m_used;

			} else {
				size_t nused = m_used - m_off;
				size_t nsize = std::max(CLOUDY_INITIAL_ALLOCATION_SIZE, nused);
				void* p = malloc(nsize);
				if(!p) { throw std::bad_alloc(); }
				memcpy(p, m_counter->ptr + m_off, nused);
				m_counter.reset(new counter(CLOUDY_INITIAL_ALLOCATION_SIZE));
				m_lot = 0;
				m_off = 0;
				m_used = nused;
				m_free = nsize - nused;
			}
		}

		void consumed(size_t len)
		{
			m_used += len;
			m_free -= len;
		}

		char* used_last()
		{
			return m_counter->ptr + m_used;
		}

		char* offset_first()
		{
			return m_counter->ptr;
		}

		size_t* offset_ptr()
		{
			return &m_off;
		}

		shared_counter incr_lot()
		{
			++m_lot;
			return m_counter;
		}

		size_t unused_size() const
		{
			return m_free;
		}

		size_t used_size() const
		{
			return m_used;
		}

	private:
		shared_counter m_counter;
		size_t m_used;
		size_t m_free;
		size_t m_off;
		size_t m_lot;
	};
	buffer_t m_buffer;

	struct Queue {
		Queue() : m_valid(true) { }
		~Queue() { }
		int is_valid() const { return m_valid; }
		void invalidate() { m_valid = false; }
	private:
		bool m_valid;
	};

	typedef mp::shared_ptr<Queue> SharedQueue;
	SharedQueue m_queue;


	struct LifeKeeper {
		LifeKeeper(shared_zone& z) : m(z) { }
		~LifeKeeper() { }
	private:
		shared_zone m;
		LifeKeeper();
	};


	struct Responder {
		Responder(int fd, SharedQueue& queue) :
			m_fd(fd), m_queue(queue) { }
		~Responder() { }

		bool is_valid() const { return m_queue->is_valid(); }

		int fd() const { return m_fd; }

		void send_data(const char* buf, size_t buflen);
		void send_datav(struct iovec* vb, size_t count, shared_zone& life);

	private:
		int m_fd;
		SharedQueue m_queue;
	};

	struct ResGet : Responder {
		ResGet(int fd, SharedQueue& queue) :
			Responder(fd, queue) { }
		~ResGet() { }
		void response(get_response& res);
	public:
		void set_req_key() { m_req_key = true; }
		void set_req_quiet() { m_req_quiet = true; }
	private:
		bool m_req_key;
		bool m_req_quiet;
	};

	struct ResSet : Responder {
		ResSet(int fd, SharedQueue& queue) :
			Responder(fd, queue) { }
		~ResSet() { }
		void response(set_response& res);
		void no_response(set_response& res);
	};

	struct ResDelete : Responder {
		ResDelete(int fd, SharedQueue& queue) :
			Responder(fd, queue) { }
		~ResDelete() { }
		void response(delete_response& res);
		void no_response(delete_response& res);
	};

private:
	Connection();
	Connection(const Connection&);
};

Cloudy::Connection::Connection(int fd, Gateway* gw) :
	mp::iothreads::handler(fd),
	m_gw(gw),
	m_zone(new mp::zone()),
	m_queue(new Queue())
{
	void (*cmd_getx)(void*, memproto_header*,
			const char*, uint16_t) = &mp::object_callback<void (memproto_header*,
				const char*, uint16_t)>
				::mem_fun<Connection, &Connection::memproto_getx>;

	void (*cmd_set)(void*, memproto_header*,
			const char*, uint16_t,
			const char*, uint16_t,
			uint32_t, uint32_t) = &mp::object_callback<void (memproto_header*,
				const char*, uint16_t,
				const char*, uint16_t,
				uint32_t, uint32_t)>
				::mem_fun<Connection, &Connection::memproto_set>;

	void (*cmd_delete)(void*, memproto_header*,
			const char*, uint16_t,
			uint32_t) = &mp::object_callback<void (memproto_header*,
				const char*, uint16_t,
				uint32_t)>
				::mem_fun<Connection, &Connection::memproto_delete>;

	//void (*cmd_noop)(void*, memproto_header*) =
	//		&mp::object_callback<void (memproto_header*)>
	//			::mem_fun<Connection, &Connection::memproto_noop>;

	memproto_callback cb = {
		cmd_getx,    // get
		cmd_set,     // set
		NULL,        // add
		NULL,        // replace
		cmd_delete,  // delete
		NULL,        // increment
		NULL,        // decrement
		NULL,        // quit
		NULL,        // flush
		cmd_getx,    // getq
		NULL,//cmd_noop,    // noop
		NULL,        // version
		cmd_getx,    // getk
		cmd_getx,    // getkq
		NULL,        // append
		NULL,        // prepend
	};

	memproto_parser_init(&m_memproto, &cb, this);
}

Cloudy::Connection::~Connection()
{
	m_queue->invalidate();
}


void Cloudy::Connection::read_event()
try {
	m_buffer.reserve();

	ssize_t rl = ::read(fd(), m_buffer.used_last(), m_buffer.unused_size());
	if(rl < 0) {
		if(errno == EAGAIN || errno == EINTR) {
			return;
		} else {
			throw std::runtime_error("read error");
		}
	} else if(rl == 0) {
		throw std::runtime_error("connection closed");
	}

	int ret;
	while( (ret = memproto_parser_execute(&m_memproto, m_buffer.offset_first(), m_buffer.used_size(), m_buffer.offset_ptr())) > 0) {
		m_zone->allocate<buffer_t::counter_keeper>(m_buffer.incr_lot());
		if( (ret = memproto_dispatch(&m_memproto)) <= 0) {
			LOG_WARN("unknown command ",(-ret));
			throw std::runtime_error("unknown command");
		}
		m_zone.reset(new mp::zone());
	}

	if(ret < 0) { throw std::runtime_error("parse error"); }

} catch (std::runtime_error& e) {
	LOG_DEBUG("memcached binary protocol error: ",e.what());
	throw;
} catch (...) {
	LOG_DEBUG("memcached binary protocol error: unknown error");
	throw;
}


void Cloudy::Connection::memproto_getx(memproto_header* h, const char* key, uint16_t keylen)
{
	LOG_TRACE("getx");

	bool cmd_k = (h->opcode == MEMPROTO_CMD_GETK || h->opcode == MEMPROTO_CMD_GETKQ);
	bool cmd_q = (h->opcode == MEMPROTO_CMD_GETQ || h->opcode == MEMPROTO_CMD_GETKQ);

	ResGet* ctx = m_zone->allocate<ResGet>(fd(), m_queue);
	if(cmd_k) { ctx->set_req_key(); }
	if(cmd_q) { ctx->set_req_quiet(); }

	get_request req;
	req.keylen = keylen;
	req.key = key;
	req.user = (void*)ctx;
	req.callback = &mp::object_callback<void (get_response&)>
		::mem_fun<ResGet, &ResGet::response>;
	req.life = m_zone;

	m_gw->submit(req);
}

void Cloudy::Connection::memproto_set(memproto_header* h, const char* key, uint16_t keylen,
		const char* val, uint16_t vallen,
		uint32_t flags, uint32_t expiration)
{
	LOG_TRACE("set");

	if(h->cas || flags || expiration) {
		// FIXME error response
		throw std::runtime_error("memcached binary protocol: invalid argument");
	}

	ResSet* ctx = m_zone->allocate<ResSet>(fd(), m_queue);
	set_request req;
	req.keylen = keylen;
	req.key = key;
	req.vallen = vallen;
	req.val = val;
	req.user = (void*)ctx;
	req.callback = &mp::object_callback<void (set_response&)>
		::mem_fun<ResSet, &ResSet::response>;
	req.life = m_zone;

	m_gw->submit(req);
}

void Cloudy::Connection::memproto_delete(memproto_header* h, const char* key, uint16_t keylen,
		uint32_t expiration)
{
	LOG_TRACE("delete");

	if(expiration) {
		// FIXME error response
		throw std::runtime_error("memcached binary protocol: invalid argument");
	}

	ResDelete* ctx = m_zone->allocate<ResDelete>(fd(), m_queue);
	delete_request req;
	req.key = key;
	req.keylen = keylen;
	req.user = (void*)ctx;
	req.callback = &mp::object_callback<void (delete_response&)>
		::mem_fun<ResDelete, &ResDelete::response>;
	req.life = m_zone;

	m_gw->submit(req);
}


void Cloudy::Connection::ResGet::response(get_response& res)
{
	if(!is_valid()) { return; }
	LOG_TRACE("get response");

	if(res.error) {
		// error response
		return;
	}

	if(!res.val) {
		// not found
		if(m_req_quiet) { return; }
		return;
	}

	// found
}

void Cloudy::Connection::ResSet::response(set_response& res)
{
	if(!is_valid()) { return; }
	LOG_TRACE("set response");

	if(res.error) {
		// error response
		return;
	}

	// success response
}

void Cloudy::Connection::ResDelete::response(delete_response& res)
{
	if(!is_valid()) { return; }
	LOG_TRACE("delete response");

	if(res.error) {
		// error response
		return;
	}

	if(res.deleted) {
		//send_data("DELETED\r\n", 9);
	} else {
		//send_data("NOT FOUND\r\n", 11);
	}
}


}  // namespace kumo


