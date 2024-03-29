/*
  +----------------------------------------------------------------------+
  | PHP Version 5                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2007 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author: Alexandre Kalendarev akalend@mail.ru Copyright (c) 2009-2010 |
  | Lead:                                                                |
  | - Pieter de Zwart                                                    |
  | Maintainers:                                                         |
  | - Brad Rodriguez                                                     |
  | - Jonathan Tansavatdi                                                |
  +----------------------------------------------------------------------+
*/

/* $Id: amqp_connection.c 323845 2012-03-02 21:23:25Z pdezwart $ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "zend_exceptions.h"

#include <stdint.h>
#include <signal.h>
#include <amqp.h>
#include <amqp_framing.h>

#include <unistd.h>

#include "php_amqp.h"

#if PHP_MAJOR_VERSION == 5 && PHP_MINOR_VERSION >= 3
HashTable *amqp_connection_object_get_debug_info(zval *object, int *is_temp TSRMLS_DC) {
	zval *value;
	
	/* Super magic make shit work variable. Seriously though, without this using print_r and/or var_dump will either cause memory leak or crash. */
	*is_temp = 1;
	
	/* Get the envelope object from which to read */
	amqp_connection_object *connection = (amqp_connection_object *)zend_object_store_get_object(object TSRMLS_CC);
		
	/* Keep the first number matching the number of entries in this table*/
	ALLOC_HASHTABLE(connection->debug_info);
	ZEND_INIT_SYMTABLE_EX(connection->debug_info, 5 + 1, 0);
	
	/* Start adding values */
	MAKE_STD_ZVAL(value);
	ZVAL_STRINGL(value, connection->login, strlen(connection->login), 1);
	zend_hash_add(connection->debug_info, "login", strlen("login") + 1, &value, sizeof(zval *), NULL);

	MAKE_STD_ZVAL(value);
	ZVAL_STRINGL(value, connection->password, strlen(connection->password), 1);
	zend_hash_add(connection->debug_info, "password", strlen("password") + 1, &value, sizeof(zval *), NULL);

	MAKE_STD_ZVAL(value);
	ZVAL_STRINGL(value, connection->host, strlen(connection->host), 1);
	zend_hash_add(connection->debug_info, "host", strlen("host") + 1, &value, sizeof(zval *), NULL);
	
	MAKE_STD_ZVAL(value);
	ZVAL_STRINGL(value, connection->vhost, strlen(connection->vhost), 1);
	zend_hash_add(connection->debug_info, "vhost", strlen("vhost") + 1, &value, sizeof(zval *), NULL);
	
	MAKE_STD_ZVAL(value);
	ZVAL_LONG(value, connection->port);
	zend_hash_add(connection->debug_info, "port", strlen("port") + 1, &value, sizeof(zval *), NULL);

	/* Start adding values */
	return connection->debug_info;
}
#endif

/**
 * 	php_amqp_connect
 *	handles connecting to amqp
 *	called by connect() and reconnect()
 */
void php_amqp_connect(amqp_connection_object *connection, int persistent TSRMLS_DC)
{
	char str[256];
	char ** pstr = (char **) &str;
	void * old_handler;


	/* Clean up old memory allocations which are now invalid (new connection) */
	if (connection->connection_resource) {
		if (connection->connection_resource->slots) {
			int slot;
			for (slot = 1; slot < DEFAULT_CHANNELS_PER_CONNECTION; slot++) {
				if (connection->connection_resource->slots[slot] != 0) {
					/* We found the channel, disconnect it: */
					amqp_channel_close(connection->connection_resource->connection_state, slot, AMQP_REPLY_SUCCESS);

					/* Clean up our local storage */
					connection->connection_resource->slots[slot] = 0;
					connection->connection_resource->used_slots--;
				}
			}
			
			pefree(connection->connection_resource->slots, persistent);
		}
		pefree(connection->connection_resource, persistent);
	}
	
	/* Allocate space for the connection resource */
	connection->connection_resource = (amqp_connection_resource *)pemalloc(sizeof(amqp_connection_resource), persistent);
	memset(connection->connection_resource, 0, sizeof(amqp_connection_resource));

	/* Allocate space for the channel slots in the ring buffer */
	connection->connection_resource->slots = (amqp_channel_object **)pecalloc(DEFAULT_CHANNELS_PER_CONNECTION, sizeof(amqp_channel_object*), persistent);
	/* Initialize all the data */
	connection->connection_resource->used_slots = 0;

	/* Mark this as non persistent resource */
	connection->connection_resource->is_persistent = persistent;

	/* Create the connection */
	connection->connection_resource->connection_state = amqp_new_connection();

	/* Get the connection socket out */
	connection->connection_resource->fd = amqp_open_socket(connection->host, connection->port);

	/* Verify that we actually got a connectio back */
	if (connection->connection_resource->fd < 1) {
		/* Start ignoring SIGPIPE */
		old_handler = signal(SIGPIPE, SIG_IGN);

		amqp_destroy_connection(connection->connection_resource->connection_state);

		/* End ignoring of SIGPIPEs */
		signal(SIGPIPE, old_handler);

		zend_throw_exception(amqp_connection_exception_class_entry, "Socket error: could not connect to host.", 0 TSRMLS_CC);
		return;
	}

	amqp_set_sockfd(connection->connection_resource->connection_state, connection->connection_resource->fd);

	amqp_rpc_reply_t x = amqp_login(
		connection->connection_resource->connection_state,
		connection->vhost,
		0,							/* Channel max */
		FRAME_MAX,
		AMQP_HEARTBEAT,
		AMQP_SASL_METHOD_PLAIN,
		connection->login,
		connection->password
	);

	if (x.reply_type != AMQP_RESPONSE_NORMAL) {
		amqp_error(x, pstr);
		strcat(*pstr, " - Potential login failure.");
		zend_throw_exception(amqp_connection_exception_class_entry, *pstr, 0 TSRMLS_CC);
		return;
	}

	connection->is_connected = '\1';
}

/* 	php_amqp_disconnect
	handles disconnecting from amqp
	called by disconnect(), reconnect(), and d_tor
 */
void php_amqp_disconnect(amqp_connection_object *connection)
{
	void * old_handler;
	int slot;
	
	/* Pull the connection resource out for easy access */
	amqp_connection_resource *resource = connection->connection_resource;
	
	/* If it's persistent connection do not close this socket connection */ 
	if (connection->is_connected == '\1' && connection->connection_resource->is_persistent) {   
		return;
	}
	
	/*
	If we are trying to close the connection and the connection already closed, it will throw
	SIGPIPE, which is fine, so ignore all SIGPIPES
	*/

	/* Start ignoring SIGPIPE */
	old_handler = signal(SIGPIPE, SIG_IGN);
	
	if (connection->is_connected == '\1') {
		/* Close all open channels */
		for (slot = 1; slot < DEFAULT_CHANNELS_PER_CONNECTION; slot++) {
			if (resource->slots[slot] != 0) {
				/* We found the channel, disconnect it: */
				amqp_channel_close(connection->connection_resource->connection_state, slot, AMQP_REPLY_SUCCESS);

				/* Clean up our local storage */
				resource->slots[slot] = 0;
				resource->used_slots--;
			}
		}
	}

	
	if (resource && resource->connection_state && connection->is_connected == '\1') {
		amqp_connection_close(resource->connection_state, AMQP_REPLY_SUCCESS);
		amqp_destroy_connection(resource->connection_state);
		
		if (resource->fd) {
			close(resource->fd);
		}
	}

	connection->is_connected = '\0';


	/* End ignoring of SIGPIPEs */
	signal(SIGPIPE, old_handler);
	
	return;
}

int get_next_available_channel(amqp_connection_object *connection, amqp_channel_object *channel)
{
	int slot;
	
	/* Pull out the ring buffer for ease of use */
	amqp_connection_resource *resource = connection->connection_resource;
	
	/* Check if there are any open slots */
	if (resource->used_slots >= DEFAULT_CHANNELS_PER_CONNECTION) {
		return -1;
	}
	
	/* Go through the slots looking for an opening */
	for (slot = 1; slot < DEFAULT_CHANNELS_PER_CONNECTION; slot++) {
		if (resource->slots[slot] == 0) {
			/* Yay! we found a slot. Store the channel for later (disconnect needs to clean up connected channels) */
			resource->slots[slot] = channel;
			resource->used_slots++;
			
			/* Return the slot ID back so the channel can use it */
			return slot;
		}
	}
	
	return -1;
}

void remove_channel_from_connection(amqp_connection_object *connection, amqp_channel_object *channel)
{
	int slot;
	
	/* Pull out the ring buffer for ease of use */
	amqp_connection_resource *resource = connection->connection_resource;
	
	/* Check that there is actually an open connection */
	if (!resource) {
		return;
	}
	
	/* Go through the slots looking for an opening */
	for (slot = 1; slot < DEFAULT_CHANNELS_PER_CONNECTION; slot++) {
		if (resource->slots[slot] == channel) {
			/* We found the channel, disconnect it: */
			amqp_channel_close(connection->connection_resource->connection_state, channel->channel_id, AMQP_REPLY_SUCCESS);
			
			/* Clean up our local storage */
			resource->slots[slot] = 0;
			resource->used_slots--;
			
			return;
		}
	}
	
	return;
}

static void amqp_connection_resource_dtor_persistent(zend_rsrc_list_entry *rsrc TSRMLS_DC)
{
	amqp_connection_resource *resource = (amqp_connection_resource *)rsrc->ptr;

	if (resource->slots) {
		pefree(resource->slots, 1);
	}
	pefree(resource, 1);
}

void amqp_connection_dtor(void *object TSRMLS_DC)
{
	amqp_connection_object *connection = (amqp_connection_object*)object;

	php_amqp_disconnect(connection);

	/* Clean up all the strings */
	if (connection->host) {
		efree(connection->host);
	}
	
	if (connection->vhost) {
		efree(connection->vhost);
	}
	
	if (connection->login) {
		efree(connection->login);
	}
	
	if (connection->password) {
		efree(connection->password);
	}

	if (connection->connection_resource && connection->connection_resource->is_persistent == 0) {
		if (connection->connection_resource->slots) {
			efree(connection->connection_resource->slots);
		}
		efree(connection->connection_resource);
	}
	
	zend_object_std_dtor(&connection->zo TSRMLS_CC);

	efree(object);
}

zend_object_value amqp_connection_ctor(zend_class_entry *ce TSRMLS_DC)
{
	zend_object_value new_value;

	amqp_connection_object* connection = (amqp_connection_object*)emalloc(sizeof(amqp_connection_object));
	memset(connection, 0, sizeof(amqp_connection_object));

	zend_object_std_init(&connection->zo, ce TSRMLS_CC);

	new_value.handle = zend_objects_store_put(
		connection,
		(zend_objects_store_dtor_t)zend_objects_destroy_object,
		(zend_objects_free_object_storage_t)amqp_connection_dtor,
		NULL TSRMLS_CC
	);
	
#if 0 && PHP_MAJOR_VERSION == 5 && PHP_MINOR_VERSION >= 3
	zend_object_handlers *handlers;
	handlers = zend_get_std_object_handlers();
	handlers->get_debug_info = amqp_connection_object_get_debug_info;
	new_value.handlers = handlers;
#else
	new_value.handlers = zend_get_std_object_handlers();
#endif

	return new_value;
}


/* {{{ proto AMQPConnection::__construct([array optional])
 * The array can contain 'host', 'port', 'login', 'password', 'vhost' indexes
 */
PHP_METHOD(amqp_connection_class, __construct)
{
	zval *id;
	amqp_connection_object *connection;

	zval* iniArr = NULL;
	zval** zdata;

	/* Parse out the method parameters */
	if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "O|a", &id, amqp_connection_class_entry, &iniArr) == FAILURE) {
		return;
	}

	/* Pull the connection off of the store */
	connection = (amqp_connection_object *)zend_object_store_get_object(id TSRMLS_CC);
	
	/* Pull the login out of the $params array */
	zdata = NULL;
	if (iniArr && SUCCESS == zend_hash_find(HASH_OF (iniArr), "login", sizeof("login"), (void*)&zdata)) {
		convert_to_string(*zdata);
	}
	/* Validate the given login */
	if (zdata && Z_STRLEN_PP(zdata) > 0) {
		if (Z_STRLEN_PP(zdata) < 128) {
			connection->login = estrndup(Z_STRVAL_PP(zdata), Z_STRLEN_PP(zdata));
		} else {
			zend_throw_exception(amqp_connection_exception_class_entry, "Parameter 'login' exceeds 128 character limit.", 0 TSRMLS_CC);
			return;
		}
	} else {
		connection->login = estrndup(INI_STR("amqp.login"), strlen(INI_STR("amqp.login")) > 128 ? 128 : strlen(INI_STR("amqp.login")));
	}
	/* @TODO: write a macro to reduce code duplication */

	/* Pull the password out of the $params array */
	zdata = NULL;
	if (iniArr && SUCCESS == zend_hash_find(HASH_OF(iniArr), "password", sizeof("password"), (void*)&zdata)) {
		convert_to_string(*zdata);
	}
	/* Validate the given password */
	if (zdata && Z_STRLEN_PP(zdata) > 0) {
		if (Z_STRLEN_PP(zdata) < 128) {
			connection->password = estrndup(Z_STRVAL_PP(zdata), Z_STRLEN_PP(zdata));
		} else {
			zend_throw_exception(amqp_connection_exception_class_entry, "Parameter 'password' exceeds 128 character limit.", 0 TSRMLS_CC);
			return;
		}
	} else {
		connection->password = estrndup(INI_STR("amqp.password"), strlen(INI_STR("amqp.password")) > 128 ? 128 : strlen(INI_STR("amqp.password")));
	}

	/* Pull the host out of the $params array */
	zdata = NULL;
	if (iniArr && SUCCESS == zend_hash_find(HASH_OF(iniArr), "host", sizeof("host"), (void *)&zdata)) {
		convert_to_string(*zdata);
	}
	/* Validate the given host */
	if (zdata && Z_STRLEN_PP(zdata) > 0) {
		if (Z_STRLEN_PP(zdata) < 128) {
			connection->host = estrndup(Z_STRVAL_PP(zdata), Z_STRLEN_PP(zdata));
		} else {
			zend_throw_exception(amqp_connection_exception_class_entry, "Parameter 'host' exceeds 128 character limit.", 0 TSRMLS_CC);
			return;
		}
	} else {
		connection->host = estrndup(INI_STR("amqp.host"), strlen(INI_STR("amqp.host")) > 128 ? 128 : strlen(INI_STR("amqp.host")));
	}

	/* Pull the vhost out of the $params array */
	zdata = NULL;
	if (iniArr && SUCCESS == zend_hash_find(HASH_OF (iniArr), "vhost", sizeof("vhost"), (void*)&zdata)) {
		convert_to_string(*zdata);
	}
	/* Validate the given vhost */
	if (zdata && Z_STRLEN_PP(zdata) > 0) {
		if (Z_STRLEN_PP(zdata) < 128) {
			connection->vhost = estrndup(Z_STRVAL_PP(zdata), Z_STRLEN_PP(zdata));
		} else {
			zend_throw_exception(amqp_connection_exception_class_entry, "Parameter 'vhost' exceeds 128 character limit.", 0 TSRMLS_CC);
			return;
		}
	} else {
		connection->vhost = estrndup(INI_STR("amqp.vhost"), strlen(INI_STR("amqp.vhost")) > 128 ? 128 : strlen(INI_STR("amqp.vhost")));
	}

	connection->port = INI_INT("amqp.port");

	if (iniArr && SUCCESS == zend_hash_find(HASH_OF (iniArr), "port", sizeof("port"), (void*)&zdata)) {
		convert_to_long(*zdata);
		connection->port = (size_t)Z_LVAL_PP(zdata);
	}
}
/* }}} */


/* {{{ proto amqp::isConnected()
check amqp connection */
PHP_METHOD(amqp_connection_class, isConnected)
{
	zval *id;
	amqp_connection_object *connection;

	/* Try to pull amqp object out of method params */
	if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "O", &id, amqp_connection_class_entry) == FAILURE) {
		return;
	}

	/* Get the connection object out of the store */
	connection = (amqp_connection_object *)zend_object_store_get_object(id TSRMLS_CC);

	/* If the channel_connect is 1, we have a connection */
	if (connection->is_connected == '\1') {
		RETURN_TRUE;
	}

	/* We have no connection */
	RETURN_FALSE;
}
/* }}} */


/* {{{ proto amqp::connect()
create amqp connection */
PHP_METHOD(amqp_connection_class, connect)
{
	zval *id;
	amqp_connection_object *connection;

	/* Try to pull amqp object out of method params */
	if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "O", &id, amqp_connection_class_entry) == FAILURE) {
		return;
	}

	/* Get the connection object out of the store */
	connection = (amqp_connection_object *)zend_object_store_get_object(id TSRMLS_CC);
		
	/* Actually connect this resource to the broker */
	php_amqp_connect(connection, 0 TSRMLS_CC);
	
	/* @TODO: return connection success or failure */
	RETURN_TRUE;
}
/* }}} */


/* {{{ proto amqp::connect()
create amqp connection */
PHP_METHOD(amqp_connection_class, pconnect)
{
	zval *id;
	amqp_connection_object *connection;
	char *key;
	int key_len;
    list_entry *le, new_le;

	/* Try to pull amqp object out of method params */
	if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "O", &id, amqp_connection_class_entry) == FAILURE) {
		return;
	}

	/* Get the connection object out of the store */
	connection = (amqp_connection_object *)zend_object_store_get_object(id TSRMLS_CC);

	/* Look for an established resource */
	key_len = spprintf(&key, 0, "amqp_conn_res_%s_%d_%s_%s", connection->host, connection->port, connection->vhost, connection->login);
	
	if (zend_hash_find(&EG(persistent_list), key, key_len + 1, (void **)&le) == SUCCESS) {
		/* An entry for this connection resource already exists */
		zend_list_insert(le, le_amqp_connection_resource);
		
		/* Stash the connection resource in the connection */
		connection->connection_resource = le->ptr;
		
		/* Set connection status to connected */
		connection->is_connected = '\1';
		
		efree(key);
		RETURN_TRUE;
	}

	/* No resource found: Instantiate the underlying connection */
	php_amqp_connect(connection, 1 TSRMLS_CC);
	
	/* Store a reference in the persistence list */
    new_le.ptr = connection->connection_resource;
    new_le.type = le_amqp_connection_resource;
    zend_hash_add(&EG(persistent_list), key, key_len + 1, &new_le, sizeof(list_entry), NULL);
	
	/* Cleanup our key */
	efree(key);
	
	/* @TODO: return connection success or failure */
	RETURN_TRUE;
}
/* }}} */

/* {{{ proto amqp::disconnect()
destroy amqp connection */
PHP_METHOD(amqp_connection_class, disconnect)
{
	zval *id;
	amqp_connection_object *connection;

	/* Try to pull amqp object out of method params */
	if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "O", &id, amqp_connection_class_entry) == FAILURE) {
		return;
	}

	/* Get the connection object out of the store */
	connection = (amqp_connection_object *)zend_object_store_get_object(id TSRMLS_CC);

	php_amqp_disconnect(connection);

	RETURN_TRUE;
}

/* }}} */

/* {{{ proto amqp::reconnect()
recreate amqp connection */
PHP_METHOD(amqp_connection_class, reconnect)
{
	zval *id;
	amqp_connection_object *connection;

	/* Try to pull amqp object out of method params */
	if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "O", &id, amqp_connection_class_entry) == FAILURE) {
		return;
	}

	/* Get the connection object out of the store */
	connection = (amqp_connection_object *)zend_object_store_get_object(id TSRMLS_CC);

	if (connection->is_connected == '\1') {
		php_amqp_disconnect(connection);
	}
	
	php_amqp_connect(connection, 0 TSRMLS_CC);
	
	/* @TODO: return the success or failure of connect */
	RETURN_TRUE;
}
/* }}} */


/* {{{ proto amqp::getLogin()
get the login */
PHP_METHOD(amqp_connection_class, getLogin)
{
	zval *id;
	amqp_connection_object *connection;

	/* Get the login from the method params */
	if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "O", &id, amqp_connection_class_entry) == FAILURE) {
		return;
	}

	/* Get the connection object out of the store */
	connection = (amqp_connection_object *)zend_object_store_get_object(id TSRMLS_CC);

	/* Copy the login to the amqp object */
	RETURN_STRING(connection->login, 1);
}
/* }}} */


/* {{{ proto amqp::setLogin(string login)
set the login */
PHP_METHOD(amqp_connection_class, setLogin)
{
	zval *id;
	amqp_connection_object *connection;
	char *login;
	int login_len;

	/* @TODO: use macro when one is created for constructor */
	/* Get the login from the method params */
	if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "Os", &id, amqp_connection_class_entry, &login, &login_len) == FAILURE) {
		return;
	}

	/* Validate login length */
	if (login_len > 128) {
		zend_throw_exception(amqp_connection_exception_class_entry, "Invalid 'login' given, exceeds 128 characters limit.", 0 TSRMLS_CC);
		return;
	}

	/* Get the connection object out of the store */
	connection = (amqp_connection_object *)zend_object_store_get_object(id TSRMLS_CC);

	/* Copy the login to the amqp object */
	connection->login = estrndup(login, login_len);

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto amqp::getPassword()
get the password */
PHP_METHOD(amqp_connection_class, getPassword)
{
	zval *id;
	amqp_connection_object *connection;

	/* Get the password from the method params */
	if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "O", &id, amqp_connection_class_entry) == FAILURE) {
		return;
	}

	/* Get the connection object out of the store */
	connection = (amqp_connection_object *)zend_object_store_get_object(id TSRMLS_CC);

	/* Copy the password to the amqp object */
	RETURN_STRING(connection->password, 1);
}
/* }}} */


/* {{{ proto amqp::setPassword(string password)
set the password */
PHP_METHOD(amqp_connection_class, setPassword)
{
	zval *id;
	amqp_connection_object *connection;
	char *password;
	int password_len;

	/* Get the password from the method params */
	if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "Os", &id, amqp_connection_class_entry, &password, &password_len) == FAILURE) {
		return;
	}

	/* Validate password length */
	if (password_len > 128) {
		zend_throw_exception(amqp_connection_exception_class_entry, "Invalid 'password' given, exceeds 128 characters limit.", 0 TSRMLS_CC);
		return;
	}

	/* Get the connection object out of the store */
	connection = (amqp_connection_object *)zend_object_store_get_object(id TSRMLS_CC);

	/* Copy the password to the amqp object */
	connection->password = estrndup(password, password_len);

	RETURN_TRUE;
}
/* }}} */


/* {{{ proto amqp::getHost()
get the host */
PHP_METHOD(amqp_connection_class, getHost)
{
	zval *id;
	amqp_connection_object *connection;

	/* Get the host from the method params */
	if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "O", &id, amqp_connection_class_entry) == FAILURE) {
		return;
	}

	/* Get the connection object out of the store */
	connection = (amqp_connection_object *)zend_object_store_get_object(id TSRMLS_CC);

	/* Copy the host to the amqp object */
	RETURN_STRING(connection->host, 1);
}
/* }}} */


/* {{{ proto amqp::setHost(string host)
set the host */
PHP_METHOD(amqp_connection_class, setHost)
{
	zval *id;
	amqp_connection_object *connection;
	char *host;
	int host_len;

	/* Get the host from the method params */
	if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "Os", &id, amqp_connection_class_entry, &host, &host_len) == FAILURE) {
		return;
	}

	/* Validate host length */
	if (host_len > 1024) {
		zend_throw_exception(amqp_connection_exception_class_entry, "Invalid 'host' given, exceeds 1024 character limit.", 0 TSRMLS_CC);
		return;
	}

	/* Get the connection object out of the store */
	connection = (amqp_connection_object *)zend_object_store_get_object(id TSRMLS_CC);

	/* Copy the host to the amqp object */
	connection->host = estrndup(host, host_len);

	RETURN_TRUE;
}
/* }}} */


/* {{{ proto amqp::getPort()
get the port */
PHP_METHOD(amqp_connection_class, getPort)
{
	zval *id;
	amqp_connection_object *connection;

	/* Get the port from the method params */
	if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "O", &id, amqp_connection_class_entry) == FAILURE) {
		return;
	}

	/* Get the connection object out of the store */
	connection = (amqp_connection_object *)zend_object_store_get_object(id TSRMLS_CC);

	/* Copy the port to the amqp object */
	RETURN_LONG(connection->port);
}
/* }}} */


/* {{{ proto amqp::setPort(mixed port)
set the port */
PHP_METHOD(amqp_connection_class, setPort)
{
	zval *id;
	amqp_connection_object *connection;
	zval *zvalPort;
	int port;

	/* Get the port from the method params */
	if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "Oz", &id, amqp_connection_class_entry, &zvalPort) == FAILURE) {
		return;
	}

	/* Parse out the port*/
	switch (Z_TYPE_P(zvalPort)) {
		case IS_DOUBLE:
			port = (int)Z_DVAL_P(zvalPort);
			break;
		case IS_LONG:
			port = (int)Z_LVAL_P(zvalPort);
			break;
		case IS_STRING:
			convert_to_long(zvalPort);
			port = (int)Z_LVAL_P(zvalPort);
			break;
		default:
			port = 0;
	}

	/* Check the port value */
	if (port <= 0 || port > 65535) {
		zend_throw_exception(amqp_connection_exception_class_entry, "Invalid port given. Value must be between 1 and 65535.", 0 TSRMLS_CC);
		return;
	}

	/* Get the connection object out of the store */
	connection = (amqp_connection_object *)zend_object_store_get_object(id TSRMLS_CC);

	/* Copy the port to the amqp object */
	connection->port = port;

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto amqp::getVhost()
get the vhost */
PHP_METHOD(amqp_connection_class, getVhost)
{
	zval *id;
	amqp_connection_object *connection;

	/* Get the vhost from the method params */
	if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "O", &id, amqp_connection_class_entry) == FAILURE) {
		return;
	}

	/* Get the connection object out of the store */
	connection = (amqp_connection_object *)zend_object_store_get_object(id TSRMLS_CC);

	/* Copy the vhost to the amqp object */
	RETURN_STRING(connection->vhost, 1);
}
/* }}} */


/* {{{ proto amqp::setVhost(string vhost)
set the vhost */
PHP_METHOD(amqp_connection_class, setVhost)
{
	zval *id;
	amqp_connection_object *connection;
	char *vhost;
	int vhost_len;

	/* Get the vhost from the method params */
	if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "Os", &id, amqp_connection_class_entry, &vhost, &vhost_len) == FAILURE) {
		return;
	}

	/* Validate vhost length */
	if (vhost_len > 128) {
		zend_throw_exception(amqp_connection_exception_class_entry, "Parameter 'vhost' exceeds 128 characters limit.", 0 TSRMLS_CC);
		return;
	}

	/* Get the connection object out of the store */
	connection = (amqp_connection_object *)zend_object_store_get_object(id TSRMLS_CC);

	/* Copy the vhost to the amqp object */
	connection->vhost = estrndup(vhost, vhost_len);

	RETURN_TRUE;
}
/* }}} */


/*
*Local variables:
*tab-width: 4
*c-basic-offset: 4
*End:
*vim600: noet sw=4 ts=4 fdm=marker
*vim<6
*/
