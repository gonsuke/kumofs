#include "gateway/framework.h"
#include <assert.h>

namespace kumo {
namespace gateway {


scope_store::scope_store() : m_error_count(0)
{ }

scope_store::~scope_store() { }


template <scope_store::hash_space_type Hs>
framework::shared_session scope_store::server_for(uint64_t h, unsigned int offset)
{
#if NUM_REPLICATION != 2
#error fix following code
#endif
	assert(offset == 0 || offset == 1 || offset == 2);

	pthread_scoped_rdlock hslk(share->hs_rwlock());

	if((Hs == HS_WRITE ? share->whs() : share->rhs()).empty()) {
		net->scope_proto_network().renew_hash_space();  // FIXME may burst
		throw std::runtime_error("No server");
	}
	HashSpace::iterator it =
		(Hs == HS_WRITE ? share->whs() : share->rhs()).find(h);

	{
		if(offset == 0) {
			if(it->is_active()) { goto node_found; }
		} else { --offset; }
	
		HashSpace::iterator origin(it);
		++it;
		for(; it != origin; ++it) {
			if(*it == *origin) { continue; }
	
			if(offset == 0) {
				if(it->is_active()) { goto node_found; }
			} else { --offset; }
	
			HashSpace::node rep1 = *it;
			++it;
			for(; it != origin; ++it) {
				if(*it == *origin || *it == rep1) { continue; }
				HashSpace::node _rep2_ = *it;
	
				if(offset == 0) {
					if(it->is_active()) { goto node_found; }
				} else { --offset; }
	
				break;
			}
			break;
		}
	}

node_found:
	address addr = it->addr();
	hslk.unlock();
	return net->get_server(addr);
}


// FIXME submit callback?
#define GATEWAY_CATCH(NAME, response_type) \
catch (msgpack::type_error& e) { \
	LOG_WARN(#NAME " FAILED: type error"); \
	response_type res; \
	res.life = life; \
	res.error = 1; \
	wavy::submit(*callback, user, res); \
} catch (std::exception& e) { \
	LOG_WARN(#NAME " FAILED: ",e.what()); \
	response_type res; \
	res.life = life; \
	res.error = 1; \
	wavy::submit(*callback, user, res); \
} catch (...) { \
	LOG_WARN(#NAME " FAILED: unknown error"); \
	response_type res; \
	res.life = life; \
	res.error = 1; \
	wavy::submit(*callback, user, res); \
}


void scope_store::Get(void (*callback)(void*, get_response&), void* user,
		shared_zone life,
		const char* key, uint32_t keylen, uint64_t hash)
try {
	if(!life) { life.reset(new msgpack::zone()); }
	rpc::retry<server::proto_store::Get_1>* retry =
		life->allocate< rpc::retry<server::proto_store::Get_1> >(
				server::proto_store::Get_1(
					msgtype::DBKey(key, keylen, hash)
					));

	retry->set_callback( BIND_RESPONSE(scope_store, Get_1, retry, callback, user) );
	retry->call(server_for<HS_READ>(hash), life, 10);
}
GATEWAY_CATCH(Get, get_response)


void scope_store::Set(void (*callback)(void*, set_response&), void* user,
		shared_zone life,
		const char* key, uint32_t keylen, uint64_t hash,
		const char* val, uint32_t vallen)
try {
	uint64_t meta = 0;
	if(!life) { life.reset(new msgpack::zone()); }
	rpc::retry<server::proto_store::Set_1>* retry =
		life->allocate< rpc::retry<server::proto_store::Set_1> >(
				server::proto_store::Set_1(
					( share->cfg_async_replicate_set() ?
					  static_cast<server::store_flags>(server::store_flags_async()) :
					  static_cast<server::store_flags>(server::store_flags_none() ) ),
					msgtype::DBKey(key, keylen, hash),
					msgtype::DBValue(val, vallen, meta))
				);

	retry->set_callback( BIND_RESPONSE(scope_store, Set_1, retry, callback, user) );
	retry->call(server_for<HS_WRITE>(hash), life, 10);
}
GATEWAY_CATCH(Set, set_response)


void scope_store::Delete(void (*callback)(void*, delete_response&), void* user,
		shared_zone life,
		const char* key, uint32_t keylen, uint64_t hash)
try {
	if(!life) { life.reset(new msgpack::zone()); }
	rpc::retry<server::proto_store::Delete_1>* retry =
		life->allocate< rpc::retry<server::proto_store::Delete_1> >(
				server::proto_store::Delete_1(
					(share->cfg_async_replicate_delete() ?
					 static_cast<server::store_flags>(server::store_flags_async()) :
					 static_cast<server::store_flags>(server::store_flags_none() ) ),
					msgtype::DBKey(key, keylen, hash))
				);

	retry->set_callback( BIND_RESPONSE(scope_store, Delete_1, retry, callback, user) );
	retry->call(server_for<HS_WRITE>(hash), life, 10);
}
GATEWAY_CATCH(Delete, delete_response)


template <typename Parameter>
struct scope_store::retry_after_callback {
	retry_after_callback(scope_store* self,
			rpc::retry<Parameter>* retry,
			uint64_t for_hash, shared_zone life) :
		m_self(self), m_retry(retry), m_for_hash(for_hash), m_life(life) { }

	void operator() ()
	{
		m_retry->call(
				m_self->server_for<scope_store::HS_WRITE>(m_for_hash),
				m_life, 10);
	}

private:
	scope_store* m_self;
	rpc::retry<Parameter>* m_retry;
	uint64_t m_for_hash;
	shared_zone m_life;
};

template <typename Parameter>
void scope_store::retry_after(unsigned int steps, rpc::retry<Parameter>* retry,
		uint64_t for_hash, shared_zone life)
{
	net->do_after(steps,
			retry_after_callback<Parameter>(this, retry, for_hash, life));
}


RPC_REPLY_IMPL(scope_store, Get_1, from, res, err, life,
		rpc::retry<server::proto_store::Get_1>* retry,
		void (*callback)(void*, get_response&), void* user)
try {
	msgtype::DBKey key(retry->param().dbkey);
	LOG_TRACE("ResGet ",err);

	if(err.is_nil()) {
		get_response ret;
		ret.error     = 0;
		ret.life      = life;
		ret.key       = key.data();
		ret.keylen    = key.size();
		ret.hash      = key.hash();
		if(res.is_nil()) {
			ret.val       = NULL;
			ret.vallen    = 0;
			ret.clocktime = 0;
		} else {
			msgtype::DBValue st(res.convert());
			ret.val       = (char*)st.data();
			ret.vallen    = st.size();
			ret.clocktime = st.clocktime().get();
		}
		try { (*callback)(user, ret); } catch (...) { }

	} else if( retry->retry_incr((NUM_REPLICATION+1) * share->cfg_get_retry_num() - 1) ) {
		incr_error_count();
		unsigned short offset = retry->num_retried() % (NUM_REPLICATION+1);
		retry->call(server_for<HS_READ>(key.hash(), offset), life, 10);
		LOG_INFO("Get error: ",err,", fallback to offset +",offset," node");

	} else {
		if(err.via.u64 == (uint64_t)rpc::protocol::TRANSPORT_LOST_ERROR ||
				err.via.u64 == (uint64_t)rpc::protocol::SERVER_ERROR) {
			net->scope_proto_network().renew_hash_space();   // FIXME
		}
		get_response ret;
		ret.error     = 1;  // ERROR
		ret.life      = life;
		ret.key       = key.data();
		ret.keylen    = key.size();
		ret.hash      = key.hash();
		ret.val       = NULL;
		ret.vallen    = 0;
		ret.clocktime = 0;
		try { (*callback)(user, ret); } catch (...) { }
		LOGPACK("eg",2,
				"key",msgtype::raw_ref(key.data(),key.size()),
				"err",err.via.u64);
		LOG_ERROR("Get error: ", err);
	}
}
GATEWAY_CATCH(ResGet, get_response)


RPC_REPLY_IMPL(scope_store, Set_1, from, res, err, life,
		rpc::retry<server::proto_store::Set_1>* retry,
		void (*callback)(void*, set_response&), void* user)
try {
	msgtype::DBKey key(retry->param().dbkey);
	msgtype::DBValue val(retry->param().dbval);
	LOG_TRACE("ResSet ",err);

	if(!res.is_nil()) {
		msgpack::type::tuple<uint64_t> st(res);
		set_response ret;
		ret.error     = 0;
		ret.life      = life;
		ret.key       = key.data();
		ret.keylen    = key.size();
		ret.hash      = key.hash();
		ret.val       = val.data();
		ret.vallen    = val.size();
		ret.clocktime = st.get<0>();
		try { (*callback)(user, ret); } catch (...) { }

	} else if( retry->retry_incr(share->cfg_set_retry_num()) ) {
		incr_error_count();
		retry_after(1*framework::DO_AFTER_BY_SECONDS, retry, key.hash(), life);
		LOG_WARN("Set error: ",err,", retry ",retry->num_retried());

	} else {
		if(err.via.u64 == (uint64_t)rpc::protocol::TRANSPORT_LOST_ERROR ||
				err.via.u64 == (uint64_t)rpc::protocol::SERVER_ERROR) {
			net->scope_proto_network().renew_hash_space();   // FIXME
		}
		set_response ret;
		ret.error     = 1;  // ERROR
		ret.life      = life;
		ret.key       = key.data();
		ret.keylen    = key.size();
		ret.hash      = key.hash();
		ret.val       = val.data();
		ret.vallen    = val.size();
		ret.clocktime = 0;
		try { (*callback)(user, ret); } catch (...) { }
		LOGPACK("es",2,
				"key",msgtype::raw_ref(key.data(),key.size()),
				"val",msgtype::raw_ref(val.data(),val.size()),
				"err",err.via.u64);
		LOG_ERROR("Set error: ",err);
	}
}
GATEWAY_CATCH(ResSet, set_response)


RPC_REPLY_IMPL(scope_store, Delete_1, from, res, err, life,
		rpc::retry<server::proto_store::Delete_1>* retry,
		void (*callback)(void*, delete_response&), void* user)
try {
	msgtype::DBKey key(retry->param().dbkey);
	LOG_TRACE("ResDelete ",err);

	if(!res.is_nil()) {
		bool st(res.convert());
		delete_response ret;
		ret.error     = 0;
		ret.life      = life;
		ret.key       = key.data();
		ret.keylen    = key.size();
		ret.hash      = key.hash();
		ret.deleted   = st;
		try { (*callback)(user, ret); } catch (...) { }

	} else if( retry->retry_incr(share->cfg_delete_retry_num()) ) {
		incr_error_count();
		retry_after(1*framework::DO_AFTER_BY_SECONDS, retry, key.hash(), life);
		LOG_WARN("Delete error: ",err,", retry ",retry->num_retried());

	} else {
		if(err.via.u64 == (uint64_t)rpc::protocol::TRANSPORT_LOST_ERROR ||
				err.via.u64 == (uint64_t)rpc::protocol::SERVER_ERROR) {
			net->scope_proto_network().renew_hash_space();   // FIXME
		}
		delete_response ret;
		ret.error     = 1;  // ERROR
		ret.life      = life;
		ret.key       = key.data();
		ret.keylen    = key.size();
		ret.hash      = key.hash();
		ret.deleted   = false;
		try { (*callback)(user, ret); } catch (...) { }
		LOGPACK("ed",2,
				"key",msgtype::raw_ref(key.data(),key.size()),
				"err",err.via.u64);
		LOG_ERROR("Delete error: ",err);
	}
}
GATEWAY_CATCH(ResDelete, delete_response)


void scope_store::incr_error_count()
{
	LOG_DEBUG("increment error count ",m_error_count);
	if(m_error_count >= share->cfg_renew_threshold()) {
		m_error_count = 0;
		net->scope_proto_network().renew_hash_space();
		sleep(1);   // FIXME ad-hoc delay
	} else {
		++m_error_count;
	}
}


}  // namespace gateway
}  // namespace kumo

