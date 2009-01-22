#include "logic/mgr_impl.h"

namespace kumo {


Manager::~Manager()
{
}

void Manager::cluster_dispatch(
		shared_node from, weak_responder response,
		method_id method, msgobj param, auto_zone z)
{
	role_type role = from->role();
	if(role == protocol::MANAGER) {
		switch(method) {
		RPC_DISPATCH(KeepAlive);
		RPC_DISPATCH(HashSpaceSync);
		RPC_DISPATCH(ReplaceElection);
		default:
			throw std::runtime_error("unknown method");
		}

	} else if(role == protocol::SERVER) {
		switch(method) {
		RPC_DISPATCH(KeepAlive);
		RPC_DISPATCH(WHashSpaceRequest);
		RPC_DISPATCH(RHashSpaceRequest);
		RPC_DISPATCH(ReplaceCopyEnd);
		RPC_DISPATCH(ReplaceDeleteEnd);
		default:
			throw std::runtime_error("unknown method");
		}

	} else {
		throw std::runtime_error("unknown role");
	}
}


void Manager::subsystem_dispatch(
		shared_peer from, weak_responder response,
		method_id method, msgobj param, auto_zone z)
{
	switch(method) {
	RPC_DISPATCH(HashSpaceRequest);
	default:
		throw std::runtime_error("unknown method");
	}
}


void Manager::step_timeout()
{
	rpc::cluster::step_timeout();
	if(m_delayed_replace_clock > 0) {
		--m_delayed_replace_clock;
		if(m_delayed_replace_clock == 0) {
			replace_election();
		}
	}
}


void Manager::new_node(address addr, role_type id, shared_node n)
{
	LOG_WARN("new node ",id," ",addr);
	if(id == protocol::MANAGER) {
		if(addr != m_partner) {
			MLOGPACK("EPner",1, "unknown partner",
					"addr",addr);
			LOG_ERROR("unknown partner node");
			// FIXME
			return;
		}
		LOG_INFO("partner connected ",addr);
		pthread_scoped_lock hslk(m_hs_mutex);
		sync_hash_space_partner(hslk);
		return;

	} else if(id == protocol::SERVER) {
		// FIXME delayed change
		add_server(addr, n);
		return;

	} else {
		LOG_ERROR("unkown node id ",(uint16_t)id);
	}
}

void Manager::lost_node(address addr, role_type id)
{
	LOG_WARN("lost node ",id," ",addr);
	if(id == protocol::MANAGER) {
		return;

	} else if(id == protocol::SERVER) {
		// FIXME delayed change
		remove_server(addr);
		return;

	}
}


}  // namespace kumo

