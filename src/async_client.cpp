// async_client.cpp

/*******************************************************************************
 * Copyright (c) 2013-2022 Frank Pagliughi <fpagliughi@mindspring.com>
 *
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v1.0
 * and Eclipse Distribution License v1.0 which accompany this distribution.
 *
 * The Eclipse Public License is available at
 *    http://www.eclipse.org/legal/epl-v10.html
 * and the Eclipse Distribution License is available at
 *   http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * Contributors:
 *    Frank Pagliughi - initial implementation and documentation
 *    Frank Pagliughi - MQTT v5 support
 *******************************************************************************/

#include "mqtt/async_client.h"
#include "mqtt/token.h"
#include "mqtt/message.h"
#include "mqtt/response_options.h"
#include "mqtt/disconnect_options.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <cstring>
#include <cstdio>

namespace mqtt {

/////////////////////////////////////////////////////////////////////////////
// Constructors

async_client::async_client(const string& serverURI, const string& clientId,
						   const string& persistDir)
				: async_client(serverURI, clientId, 0, persistDir)
{
}

async_client::async_client(const string& serverURI, const string& clientId,
						   iclient_persistence* persistence /*=nullptr*/)
				: async_client(serverURI, clientId, 0, persistence)
{
}

async_client::async_client(const string& serverURI, const string& clientId,
						   int maxBufferedMessages, const string& persistDir)
				: serverURI_(serverURI), clientId_(clientId),
					mqttVersion_(MQTTVERSION_DEFAULT), userCallback_(nullptr)
{
	create_options opts(MQTTVERSION_DEFAULT, maxBufferedMessages);

	int rc = MQTTAsync_createWithOptions(&cli_, serverURI.c_str(), clientId.c_str(),
										 MQTTCLIENT_PERSISTENCE_DEFAULT,
										 const_cast<char*>(persistDir.c_str()),
										 &opts.opts_);
	if (rc != 0)
		throw exception(rc);
}

async_client::async_client(const string& serverURI, const string& clientId,
						   int maxBufferedMessages,
						   iclient_persistence* persistence /*=nullptr*/)
				: async_client(serverURI, clientId,
							   create_options(MQTTVERSION_DEFAULT, maxBufferedMessages),
							   persistence)
{
}

async_client::async_client(const string& serverURI, const string& clientId,
						   const create_options& opts,
						   const string& persistDir)
				: serverURI_(serverURI), clientId_(clientId),
					mqttVersion_(opts.opts_.MQTTVersion), userCallback_(nullptr)
{
	int rc = MQTTAsync_createWithOptions(&cli_, serverURI.c_str(), clientId.c_str(),
										 MQTTCLIENT_PERSISTENCE_DEFAULT,
										 const_cast<char*>(persistDir.c_str()),
										 const_cast<MQTTAsync_createOptions*>(&opts.opts_));
	if (rc != 0)
		throw exception(rc);
}

async_client::async_client(const string& serverURI, const string& clientId,
						   const create_options& opts,
						   iclient_persistence* persistence /*=nullptr*/)
				: serverURI_(serverURI), clientId_(clientId),
					mqttVersion_(opts.opts_.MQTTVersion), userCallback_(nullptr)
{
	int rc = MQTTASYNC_SUCCESS;

	if (!persistence) {
		rc = MQTTAsync_createWithOptions(&cli_, serverURI.c_str(), clientId.c_str(),
										 MQTTCLIENT_PERSISTENCE_NONE, nullptr,
										 const_cast<MQTTAsync_createOptions*>(&opts.opts_));
	}
	else {
		persist_.reset(new MQTTClient_persistence {
			persistence,
			&iclient_persistence::persistence_open,
			&iclient_persistence::persistence_close,
			&iclient_persistence::persistence_put,
			&iclient_persistence::persistence_get,
			&iclient_persistence::persistence_remove,
			&iclient_persistence::persistence_keys,
			&iclient_persistence::persistence_clear,
			&iclient_persistence::persistence_containskey
		});

		rc = MQTTAsync_createWithOptions(&cli_, serverURI.c_str(), clientId.c_str(),
										 MQTTCLIENT_PERSISTENCE_USER, persist_.get(),
										 const_cast<MQTTAsync_createOptions*>(&opts.opts_));
	}
	if (rc != 0)
		throw exception(rc);
}

async_client::~async_client()
{
	MQTTAsync_destroy(&cli_);
}

// --------------------------------------------------------------------------
// Class static callbacks.
// These are the callbacks directly from the C-lib. In each case the
// 'context' should be the address of the async_client object that
// registered the callback. 


// Callback for MQTTAsync_setConnected()
// This is installed with the normal callbacks and with a call to 
// reconnect() to indicate that it succeeded. It signals the client's
// connect token then calls any registered callbacks.
void async_client::on_connected(void* context, char* cause)
{
	if (context) {
		async_client* cli = static_cast<async_client*>(context);
		string cause_str = cause ? string(cause) : string();

		auto tok = cli->connTok_;
		if (tok)
			tok->on_success(nullptr);

		callback* cb = cli->userCallback_;
		if (cb)
			cb->connected(cause_str);

		auto& connHandler = cli->connHandler_;
		if (connHandler)
			connHandler(cause_str);
	}
}

// Callback for when the connection is lost.
// This is called from the MQTTAsync_connectionLost registered via 
// MQTTAsync_setCallbacks().
// It calls the registered handlers then, if there's a consumer queue, it
// places a null pointer in the queue to alert the consumer to a closed
// connection.
void async_client::on_connection_lost(void *context, char *cause)
{
	if (context) {
		async_client* cli = static_cast<async_client*>(context);
		string cause_str = cause ? string(cause) : string();

		callback* cb = cli->userCallback_;
		if (cb)
			cb->connection_lost(cause_str);

		auto& connLostHandler = cli->connLostHandler_;
		if (connLostHandler)
			connLostHandler(cause_str);

		consumer_queue_type& que = cli->que_;
		if (que)
			que->put(const_message_ptr{});
	}
}

// Callback from the C lib for when a disconnect packet is received from
// the server.
void async_client::on_disconnected(void* context, MQTTProperties* cprops,
								   MQTTReasonCodes reasonCode)
{
	if (context) {
		async_client* cli = static_cast<async_client*>(context);

		auto& disconnectedHandler = cli->disconnectedHandler_;
		if (disconnectedHandler) {
			properties props(*cprops);
			disconnectedHandler(props, ReasonCode(reasonCode));
		}
	}
}

// Callback for when a subscribed message arrives.
// This is called from the MQTTAsync_messageArrived registered via 
// MQTTAsync_setCallbacks(). 
int async_client::on_message_arrived(void* context, char* topicName, int topicLen,
									 MQTTAsync_message* msg)
{
	if (context) {
		async_client* cli = static_cast<async_client*>(context);
		callback* cb = cli->userCallback_;
		consumer_queue_type& que = cli->que_;
		message_handler& msgHandler = cli->msgHandler_;

		if (cb || que || msgHandler) {
			size_t len = (topicLen == 0) ? strlen(topicName) : size_t(topicLen);

			string topic { topicName, len };
			auto m = message::create(std::move(topic), *msg);

			if (msgHandler)
				msgHandler(m);

			if (cb)
				cb->message_arrived(m);

			if (que)
				que->put(m);
		}
	}

	MQTTAsync_freeMessage(&msg);
	MQTTAsync_free(topicName);

	// TODO: Should the user code determine the return value?
	// The Java version does doesn't seem to...
	return to_int(true);
}

// Callback to indicate that a message was delivered to the server.
// It is called for a message with a QOS >= 1, but it happens before the
// on_success() call for the token. Thus we don't have the underlying
// MQTTAsync_token of the outgoing message at the time of this callback.
//
// *** So using the Async C library we have no way to match this msgID with
//     a delivery_token object. So this is useless to us.
//
// So, all in all, this callback in it's current implementation seems rather
// redundant.
//
#if 0
void async_client::on_delivery_complete(void* context, MQTTAsync_token msgID)
{
	if (context) {
		async_client* m = static_cast<async_client*>(context);
		callback* cb = m->get_callback();
		if (cb) {
			delivery_token_ptr tok = m->get_pending_delivery_token(msgID);
			cb->delivery_complete(tok);
		}
	}
}
#endif


// Callback from the C lib for when a registered updateConnectOptions
// needs to be called.
int async_client::on_update_connection(void* context,
									   MQTTAsync_connectData* cdata)
{
	if (context) {
		async_client* cli = static_cast<async_client*>(context);
		auto& updateConnection = cli->updateConnectionHandler_;

		if (updateConnection) {
			connect_data data(*cdata);
			if (updateConnection(data)) {
				// Copy username and password into newly allocated buffers.
				// The C lib will take ownership of the memory
				auto n = data.get_user_name().length();
				if (n > 0) {
					char* username = static_cast<char*>(MQTTAsync_malloc(n+1));
					strncpy(username, data.get_user_name().c_str(), n+1);
					username[n] = '\0';
					cdata->username = username;
				}
				else
					cdata->username = nullptr;

				n = data.get_password().length();
				if (n > 0) {
					char* passwd = static_cast<char*>(MQTTAsync_malloc(n));
					memcpy(passwd, data.get_password().data(), n);
					cdata->binarypwd.data = passwd;
				}
				else
					cdata->binarypwd.data = nullptr;
				cdata->binarypwd.len = int(n);

				return to_int(true);
			}
		}
	}
	return 0;	// false
}


// --------------------------------------------------------------------------
// Private methods

void async_client::add_token(token_ptr tok)
{
	if (tok) {
		guard g(lock_);
		pendingTokens_.push_back(tok);
	}
}

void async_client::add_token(delivery_token_ptr tok)
{
	if (tok) {
	   guard g(lock_);
	   pendingDeliveryTokens_.push_back(tok);
	}
}

// Note that we uniquely identify a token by the address of its raw pointer,
// since the message ID is not unique.

void async_client::remove_token(token* tok)
{
	if (!tok)
		return;

	guard g(lock_);
	for (auto p=pendingDeliveryTokens_.begin();
					p!=pendingDeliveryTokens_.end(); ++p) {
		if (p->get() == tok) {
			delivery_token_ptr dtok = *p;
			pendingDeliveryTokens_.erase(p);

			// If there's a user callback registered, we can now call
			// delivery_complete()

			if (userCallback_) {
				const_message_ptr msg = dtok->get_message();
				if (msg && msg->get_qos() > 0) {
					callback* cb = userCallback_;
					g.unlock();
					cb->delivery_complete(dtok);
				}
			}
			return;
		}
	}
	for (auto p=pendingTokens_.begin(); p!=pendingTokens_.end(); ++p) {
		if (p->get() == tok) {
			pendingTokens_.erase(p);
			return;
		}
	}
}

// --------------------------------------------------------------------------
// Callback management

void async_client::set_callback(callback& cb)
{
	guard g(lock_);
	userCallback_ = &cb;

	int rc = MQTTAsync_setConnected(cli_, this, &async_client::on_connected);

	if (rc == MQTTASYNC_SUCCESS) {
		rc = MQTTAsync_setCallbacks(cli_, this,
									&async_client::on_connection_lost,
									&async_client::on_message_arrived,
									nullptr /*&async_client::on_delivery_complete*/);
	}
	else
		MQTTAsync_setConnected(cli_, nullptr, nullptr);

	if (rc != MQTTASYNC_SUCCESS) {
		userCallback_ = nullptr;
		throw exception(rc);
	}
}

void async_client::disable_callbacks()
{
	// TODO: It would be nice to disable callbacks at the C library level,
	// but the setCallback function currently does not accept a nullptr for
	// the "message arrived" parameter. So, for now we send it an empty
	// lambda function.
	int rc = MQTTAsync_setCallbacks(cli_, this, nullptr,
					[](void*,char*,int,MQTTAsync_message*) -> int {return to_int(true);},
					nullptr);

	if (rc != MQTTASYNC_SUCCESS)
		throw exception(rc);
}

void async_client::set_connected_handler(connection_handler cb)
{
	connHandler_ = cb;
	check_ret(::MQTTAsync_setConnected(cli_, this,
						&async_client::on_connected));
}

void async_client::set_connection_lost_handler(connection_handler cb)
{
	connLostHandler_ = cb;
	check_ret(::MQTTAsync_setConnectionLostCallback(cli_, this,
						&async_client::on_connection_lost));
}

void async_client::set_disconnected_handler(disconnected_handler cb)
{
	disconnectedHandler_ = cb;
	check_ret(::MQTTAsync_setDisconnected(cli_, this,
						&async_client::on_disconnected));
}

void async_client::set_message_callback(message_handler cb)
{
	msgHandler_ = cb;
	check_ret(::MQTTAsync_setMessageArrivedCallback(cli_, this,
						&async_client::on_message_arrived));
}

void async_client::set_update_connection_handler(update_connection_handler cb)
{
	updateConnectionHandler_ = cb;
	check_ret(::MQTTAsync_setUpdateConnectOptions(cli_, this,
						&async_client::on_update_connection));
}


// --------------------------------------------------------------------------
// Connect

token_ptr async_client::connect()
{ 
	return connect(connect_options());
}

token_ptr async_client::connect(connect_options opts)
{
	// TODO: We should update the MQTT version from the response
	//  	(when the server confirms the requested version)

	// If the options specified a new MQTT protocol version, we should
	// use it, otherwise default to the version requested when the client
	// was created.
	if (opts.opts_.MQTTVersion == 0 && mqttVersion_ >= 5)
		opts.opts_.MQTTVersion = mqttVersion_;
	else
		mqttVersion_ = opts.opts_.MQTTVersion;

	// The C lib is very picky about version and clean start/session
	if (opts.opts_.MQTTVersion >= 5)
		opts.opts_.cleansession = 0;
	else
		opts.opts_.cleanstart = 0;

	// TODO: If connTok_ is non-null, there could be a pending connect
	// which might complete after creating/assigning a new one. If that
	// happened, the callback would have the context address of the previous
	// token which was destroyed. So for now, keep the old one alive within
	// this function, and check the behavior of the C library...
	auto tmpTok = connTok_;
	connTok_ = token::create(token::Type::CONNECT, *this);
	add_token(connTok_);

	opts.set_token(connTok_);

	int rc = MQTTAsync_connect(cli_, &opts.opts_);

	if (rc != MQTTASYNC_SUCCESS) {
		remove_token(connTok_);
		connTok_.reset();
		throw exception(rc);
	}

	return connTok_;
}

token_ptr async_client::connect(connect_options opts, void* userContext,
								iaction_listener& cb)
{
	// If the options specified a new MQTT protocol version, we should
	// use it, otherwise default to the version requested when the client
	// was created.
	if (opts.opts_.MQTTVersion == 0 && mqttVersion_ >= 5)
		opts.opts_.MQTTVersion = mqttVersion_;
	else
		mqttVersion_ = opts.opts_.MQTTVersion;

	// The C lib is very picky about version and clean start/session
	if (opts.opts_.MQTTVersion >= 5)
		opts.opts_.cleansession = 0;
	else
		opts.opts_.cleanstart = 0;

	auto tmpTok = connTok_;
	connTok_ = token::create(token::Type::CONNECT, *this, userContext, cb);
	add_token(connTok_);

	opts.set_token(connTok_);

	int rc = MQTTAsync_connect(cli_, &opts.opts_);

	if (rc != MQTTASYNC_SUCCESS) {
		remove_token(connTok_);
		connTok_.reset();
		throw exception(rc);
	}

	return connTok_;
}

// --------------------------------------------------------------------------
// Re-connect

token_ptr async_client::reconnect()
{
	auto tok = connTok_;

	if (!tok)
		throw exception(MQTTASYNC_FAILURE, "Can't reconnect before a successful connect");

	tok->reset();
	add_token(tok);

	int rc = MQTTAsync_setConnected(cli_, this, &async_client::on_connected);

	if (rc == MQTTASYNC_SUCCESS)
		rc = MQTTAsync_reconnect(cli_);

	if (rc != MQTTASYNC_SUCCESS) {
		remove_token(tok);
		throw exception(rc);
	}

	return tok;
}

// --------------------------------------------------------------------------
// Disconnect

token_ptr async_client::disconnect(disconnect_options opts)
{
	auto tok = token::create(token::Type::DISCONNECT, *this);
	add_token(tok);

	opts.set_token(tok, mqttVersion_);

	int rc = MQTTAsync_disconnect(cli_, &opts.opts_);

	if (rc != MQTTASYNC_SUCCESS) {
		remove_token(tok);
		throw exception(rc);
	}

	return tok;
}

token_ptr async_client::disconnect(int timeout, void* userContext, iaction_listener& cb)
{
	auto tok = token::create(token::Type::DISCONNECT, *this, userContext, cb);
	add_token(tok);

	disconnect_options opts(timeout);
	opts.set_token(tok, mqttVersion_);

	int rc = MQTTAsync_disconnect(cli_, &opts.opts_);

	if (rc != MQTTASYNC_SUCCESS) {
		remove_token(tok);
		throw exception(rc);
	}

	return tok;
}

// --------------------------------------------------------------------------
// Queries

delivery_token_ptr async_client::get_pending_delivery_token(int msgID) const
{
	// Messages with QOS=1 or QOS=2 that require a response/acknowledge should
	// have a non-zero 16-bit message ID. The library keeps the token objects
	// for all of these messages that are in flight. When the acknowledge comes
	// back from the broker, the C++ library can look up the token from the
	// msgID and signal it, indicating completion.

	if (msgID > 0) {
		guard g(lock_);
		for (const auto& t : pendingDeliveryTokens_) {
			if (t->get_message_id() == msgID)
				return t;
		}
	}
	return delivery_token_ptr();
}

std::vector<delivery_token_ptr> async_client::get_pending_delivery_tokens() const
{
	std::vector<delivery_token_ptr> toks;
	guard g(lock_);
	for (const auto& t : pendingDeliveryTokens_) {
		if (t->get_message_id() > 0) {
			toks.push_back(t);
		}
	}
	return toks;
}

// --------------------------------------------------------------------------
// Publish

delivery_token_ptr async_client::publish(string_ref topic, const void* payload,
										 size_t n, int qos, bool retained)
{
	auto msg = message::create(std::move(topic), payload, n, qos, retained);
	return publish(std::move(msg));
}

delivery_token_ptr async_client::publish(string_ref topic, binary_ref payload,
										 int qos, bool retained)
{
	auto msg = message::create(std::move(topic), std::move(payload), qos, retained);
	return publish(std::move(msg));
}

delivery_token_ptr async_client::publish(string_ref topic,
										 const void* payload, size_t n,
										 int qos, bool retained, void* userContext,
										 iaction_listener& cb)
{
	auto msg = message::create(std::move(topic), payload, n, qos, retained);
	return publish(std::move(msg), userContext, cb);
}

delivery_token_ptr async_client::publish(const_message_ptr msg)
{
	auto tok = delivery_token::create(*this, msg);
	add_token(tok);

	delivery_response_options rspOpts(tok, mqttVersion_);

	int rc = MQTTAsync_sendMessage(cli_, msg->get_topic().c_str(),
								   &(msg->msg_), &rspOpts.opts_);

	if (rc == MQTTASYNC_SUCCESS) {
		tok->set_message_id(rspOpts.opts_.token);
	}
	else {
		remove_token(tok);
		throw exception(rc);
	}

	return tok;
}

delivery_token_ptr async_client::publish(const_message_ptr msg,
										 void* userContext, iaction_listener& cb)
{
	delivery_token_ptr tok = delivery_token::create(*this, msg, userContext, cb);
	add_token(tok);

	delivery_response_options rspOpts(tok, mqttVersion_);

	int rc = MQTTAsync_sendMessage(cli_, msg->get_topic().c_str(),
								   &(msg->msg_), &rspOpts.opts_);

	if (rc == MQTTASYNC_SUCCESS) {
		tok->set_message_id(rspOpts.opts_.token);
	}
	else {
		remove_token(tok);
		throw exception(rc);
	}

	return tok;
}

// --------------------------------------------------------------------------
// Subscribe

token_ptr async_client::subscribe(const string& topicFilter, int qos,
								  const subscribe_options& opts /*=subscribe_options()*/,
								  const properties& props /*=properties()*/)
{
	auto tok = token::create(token::Type::SUBSCRIBE, *this, topicFilter);
	tok->set_num_expected(0);	// Indicates non-array response for single val
	add_token(tok);

	auto rspOpts = response_options_builder(mqttVersion_)
		.token(tok)
		.subscribe_opts(opts)
		.properties(props)
		.finalize();

	int rc = MQTTAsync_subscribe(cli_, topicFilter.c_str(), qos, &rspOpts.opts_);

	if (rc != MQTTASYNC_SUCCESS) {
		remove_token(tok);
		throw exception(rc);
	}

	return tok;
}

token_ptr async_client::subscribe(const string& topicFilter, int qos,
								  void* userContext, iaction_listener& cb,
								  const subscribe_options& opts /*=subscribe_options()*/,
								  const properties& props /*=properties()*/)
{
	auto tok = token::create(token::Type::SUBSCRIBE, *this, topicFilter,
							 userContext, cb);
	tok->set_num_expected(0);
	add_token(tok);

	auto rspOpts = response_options_builder(mqttVersion_)
		.token(tok)
		.subscribe_opts(opts)
		.properties(props)
		.finalize();

	int rc = MQTTAsync_subscribe(cli_, topicFilter.c_str(), qos, &rspOpts.opts_);

	if (rc != MQTTASYNC_SUCCESS) {
		remove_token(tok);
		throw exception(rc);
	}

	return tok;
}

token_ptr async_client::subscribe(const_string_collection_ptr topicFilters,
								  const qos_collection& qos,
								  const std::vector<subscribe_options>& opts
									/*=std::vector<subscribe_options>()*/,
								  const properties& props /*=properties()*/)
{
	size_t n = topicFilters->size();

	if (n != qos.size())
		throw std::invalid_argument("Collection sizes don't match");

	auto tok = token::create(token::Type::SUBSCRIBE, *this, topicFilters);
	tok->set_num_expected(n);
	add_token(tok);

	auto rspOpts = response_options_builder(mqttVersion_)
		.token(tok)
		.subscribe_opts(opts)
		.properties(props)
		.finalize();

	int rc = MQTTAsync_subscribeMany(cli_, int(n), topicFilters->c_arr(),
									 const_cast<int*>(qos.data()), &rspOpts.opts_);

	if (rc != MQTTASYNC_SUCCESS) {
		remove_token(tok);
		throw exception(rc);
	}

	return tok;
}

token_ptr async_client::subscribe(const_string_collection_ptr topicFilters,
								  const qos_collection& qos,
								  void* userContext, iaction_listener& cb,
								  const std::vector<subscribe_options>& opts
									/*=std::vector<subscribe_options>()*/,
								  const properties& props /*=properties()*/)
{
	size_t n = topicFilters->size();

	if (n != qos.size())
		throw std::invalid_argument("Collection sizes don't match");

	auto tok = token::create(token::Type::SUBSCRIBE, *this,
							 topicFilters, userContext, cb);
	tok->set_num_expected(n);
	add_token(tok);

	auto rspOpts = response_options_builder(mqttVersion_)
		.token(tok)
		.subscribe_opts(opts)
		.properties(props)
		.finalize();

	int rc = MQTTAsync_subscribeMany(cli_, int(n), topicFilters->c_arr(),
									 const_cast<int*>(qos.data()), &rspOpts.opts_);

	if (rc != MQTTASYNC_SUCCESS) {
		remove_token(tok);
		throw exception(rc);
	}

	return tok;
}

// --------------------------------------------------------------------------
// Unsubscribe

token_ptr async_client::unsubscribe(const string& topicFilter,
									const properties& props /*=properties()*/)
{
	auto tok = token::create(token::Type::UNSUBSCRIBE, *this, topicFilter);
	tok->set_num_expected(0);	// Indicates non-array response for single val
	add_token(tok);

	auto rspOpts = response_options_builder(mqttVersion_)
		.token(tok)
		.properties(props)
		.finalize();

	int rc = MQTTAsync_unsubscribe(cli_, topicFilter.c_str(), &rspOpts.opts_);

	if (rc != MQTTASYNC_SUCCESS) {
		remove_token(tok);
		throw exception(rc);
	}

	return tok;
}

token_ptr async_client::unsubscribe(const_string_collection_ptr topicFilters,
									const properties& props /*=properties()*/)
{
	size_t n = topicFilters->size();

	auto tok = token::create(token::Type::UNSUBSCRIBE, *this, topicFilters);
	tok->set_num_expected(n);
	add_token(tok);

	auto rspOpts = response_options_builder(mqttVersion_)
		.token(tok)
		.properties(props)
		.finalize();

	int rc = MQTTAsync_unsubscribeMany(cli_, int(n),
									   topicFilters->c_arr(), &rspOpts.opts_);

	if (rc != MQTTASYNC_SUCCESS) {
		remove_token(tok);
		throw exception(rc);
	}

	return tok;
}

token_ptr async_client::unsubscribe(const_string_collection_ptr topicFilters,
									void* userContext, iaction_listener& cb,
									const properties& props /*=properties()*/)
{
	size_t n = topicFilters->size();

	auto tok = token::create(token::Type::UNSUBSCRIBE, *this, topicFilters,
							 userContext, cb);
	tok->set_num_expected(n);
	add_token(tok);

	auto rspOpts = response_options_builder(mqttVersion_)
		.token(tok)
		.properties(props)
		.finalize();

	int rc = MQTTAsync_unsubscribeMany(cli_, int(n), topicFilters->c_arr(), &rspOpts.opts_);

	if (rc != MQTTASYNC_SUCCESS) {
		remove_token(tok);
		throw exception(rc);
	}

	return tok;
}

token_ptr async_client::unsubscribe(const string& topicFilter,
									void* userContext, iaction_listener& cb,
									const properties& props /*=properties()*/)
{
	auto tok = token::create(token::Type::UNSUBSCRIBE , *this, topicFilter,
							 userContext, cb);
	add_token(tok);

	auto rspOpts = response_options_builder(mqttVersion_)
		.token(tok)
		.properties(props)
		.finalize();

	int rc = MQTTAsync_unsubscribe(cli_, topicFilter.c_str(), &rspOpts.opts_);

	if (rc != MQTTASYNC_SUCCESS) {
		remove_token(tok);
		throw exception(rc);
	}

	return tok;
}

// --------------------------------------------------------------------------

void async_client::start_consuming()
{
	// Make sure callbacks don't happen while we update the que, etc
	disable_callbacks();

	// TODO: Should we replace user callback?
	//userCallback_ = nullptr;

	que_.reset(new thread_queue<const_message_ptr>);

	int rc = MQTTAsync_setCallbacks(cli_, this,
									&async_client::on_connection_lost,
									&async_client::on_message_arrived,
									nullptr);

	if (rc != MQTTASYNC_SUCCESS)
		throw exception(rc);
}

void async_client::stop_consuming()
{
	try {
		disable_callbacks();
		que_.reset();
	}
	catch (...) {
		que_.reset();
		throw;
	}
}

/////////////////////////////////////////////////////////////////////////////
// end namespace mqtt
}

