// disconnect_options_test.h
// Unit tests for the disconnect_options class in the Paho MQTT C++ library.

/*******************************************************************************
 * Copyright (c) 2016 Guilherme M. Ferreira <guilherme.maciel.ferreira@gmail.com>
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
 *    Guilherme M. Ferreira - initial implementation and documentation
 *******************************************************************************/


#define UNIT_TESTS

#include <cstring>
#include "catch2_version.h"
#include "mqtt/disconnect_options.h"
#include "mock_async_client.h"

using namespace mqtt;

/////////////////////////////////////////////////////////////////////////////

static const int DFLT_TIMEOUT = 0;
static const std::string EMPTY_STR;

static constexpr token::Type TOKEN_TYPE = token::Type::DISCONNECT;

static mock_async_client cli;

// ----------------------------------------------------------------------
// Test default constructor
// ----------------------------------------------------------------------

TEST_CASE("disconnect_options dflt constructor", "[options]")
{
	mqtt::disconnect_options opts;

	REQUIRE(DFLT_TIMEOUT == (int) opts.get_timeout().count());
	REQUIRE(!opts.get_token());

	const auto& c_struct = opts.c_struct();

	REQUIRE(nullptr == c_struct.onSuccess);
	REQUIRE(nullptr == c_struct.onFailure);

	REQUIRE(DFLT_TIMEOUT == c_struct.timeout);
}

// ----------------------------------------------------------------------
// Test user constructor
// ----------------------------------------------------------------------

TEST_CASE("disconnect_options user constructor", "[options]")
{
	const int TIMEOUT = 10;

	auto tok = token::create(TOKEN_TYPE, cli);
	mqtt::disconnect_options opts { TIMEOUT };
	opts.set_token(tok, MQTTVERSION_DEFAULT);

	const auto& c_struct = opts.c_struct();

	REQUIRE(nullptr != c_struct.onSuccess);
	REQUIRE(nullptr != c_struct.onFailure);

	REQUIRE(TIMEOUT == (int) opts.get_timeout().count());
	REQUIRE(tok == opts.get_token());
}

// ----------------------------------------------------------------------
// Test the copy constructor
// ----------------------------------------------------------------------

TEST_CASE("disconnect_options copy ctor", "[options]")
{
	constexpr std::chrono::milliseconds TIMEOUT { 50 };

	mqtt::disconnect_options orgOpts { TIMEOUT };

	SECTION("simple options") {
    	mqtt::disconnect_options opts { orgOpts };

    	REQUIRE(TIMEOUT == opts.get_timeout());

    	REQUIRE(opts.get_properties().empty());

    	// Make sure it's a true copy, not linked to the original
    	orgOpts.set_timeout(0);
    	REQUIRE(TIMEOUT == opts.get_timeout());
	}

	SECTION("properties") {
		orgOpts.set_properties({{ mqtt::property::SESSION_EXPIRY_INTERVAL, 0 }});

		mqtt::disconnect_options opts { orgOpts };

		REQUIRE(opts.get_properties().contains(mqtt::property::SESSION_EXPIRY_INTERVAL));
		REQUIRE(1 == opts.c_struct().properties.count);
	}
}

// ----------------------------------------------------------------------
// Test the move constructor
// ----------------------------------------------------------------------

TEST_CASE("disconnect_options move_constructor", "[options]")
{
	constexpr std::chrono::milliseconds TIMEOUT { 50 };

	mqtt::disconnect_options orgOpts { TIMEOUT };

	SECTION("simple options") {
		mqtt::disconnect_options opts { std::move(orgOpts) };

		REQUIRE(TIMEOUT == opts.get_timeout());

    	REQUIRE(opts.get_properties().empty());
	}

	SECTION("properties") {
		orgOpts.set_properties({{ mqtt::property::SESSION_EXPIRY_INTERVAL, 0 }});

		mqtt::disconnect_options opts { std::move(orgOpts) };

		REQUIRE(opts.get_properties().contains(mqtt::property::SESSION_EXPIRY_INTERVAL));
		REQUIRE(1 == opts.c_struct().properties.count);

		// Check that the original was moved
		REQUIRE(orgOpts.get_properties().empty());
	}
}

// ----------------------------------------------------------------------
// Test set timeout
// ----------------------------------------------------------------------

TEST_CASE("disconnect_options set timeout", "[options]")
{
	mqtt::disconnect_options opts;
	const auto& c_struct = opts.c_struct();

	const int TIMEOUT = 5000;	// ms

	// Set with integer
	opts.set_timeout(TIMEOUT);
	REQUIRE(TIMEOUT == (int) opts.get_timeout().count());
	REQUIRE(TIMEOUT == c_struct.timeout);

	// Set with chrono duration
	opts.set_timeout(std::chrono::seconds(2*TIMEOUT/1000));
	REQUIRE(2*TIMEOUT == (int) opts.get_timeout().count());
	REQUIRE(2*TIMEOUT == c_struct.timeout);
}

// ----------------------------------------------------------------------
// Test set connect token
// ----------------------------------------------------------------------

TEST_CASE("disconnect_options set token", "[options]")
{
	auto tok = token::create(TOKEN_TYPE, cli);
	mqtt::disconnect_options opts;

	const auto& c_struct = opts.c_struct();

	REQUIRE(nullptr == c_struct.onSuccess);
	REQUIRE(nullptr == c_struct.onFailure);

	opts.set_token(mqtt::token_ptr(), MQTTVERSION_DEFAULT);
	REQUIRE(nullptr == c_struct.onSuccess);
	REQUIRE(nullptr == c_struct.onFailure);

	opts.set_token(tok, MQTTVERSION_DEFAULT);
	REQUIRE(nullptr != c_struct.onSuccess);
	REQUIRE(nullptr != c_struct.onFailure);

	REQUIRE(tok == opts.get_token());
}

