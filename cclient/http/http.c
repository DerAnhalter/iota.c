/*
 * Copyright (c) 2018 IOTA Stiftung
 * https://github.com/iotaledger/iota.c
 *
 * Refer to the LICENSE file for licensing information
 */

#include <string.h>

#include "cclient/http/socket.h"
#include "cclient/service.h"
#include "http.h"
#include "http_parser.h"

typedef enum { IOTA_REQUEST_STATUS_OK, IOTA_REQUEST_STATUS_DONE, IOTA_REQUEST_STATUS_ERROR } IOTA_REQUEST_STATUS;

/**
 * @brief response context for http_parser
 *
 */
struct _response_ctx {
  http_parser* parser;
  char_buffer_t* response;
  size_t offset;
  IOTA_REQUEST_STATUS status;
};

char const* khttp_ApplicationJson = "application/json";
char const* khttp_ApplicationFormUrlencoded = "application/x-www-form-urlencoded";

static char const* header_template =
    "POST %s HTTP/1.1\r\n"
    "Host: %s\r\n"
    "X-IOTA-API-Version: %d\r\n"
    "Content-Type: %s\r\n"
    "Accept: %s\r\n"
    "Content-Length: %lu\r\n"
    "\r\n";

// Callback declarations for parser
static int request_parse_header_complete(struct _response_ctx* response);
static int request_parse_data(struct _response_ctx* response, unsigned char const* at, size_t length);
static int request_parse_message_complete(struct _response_ctx* response);

// Callback implementations for parser
static int request_parse_header_complete_cb(http_parser* parser) {
  return request_parse_header_complete((struct _response_ctx*)parser->data);
}

static int request_parse_data_cb(http_parser* parser, char const* at, size_t length) {
  return request_parse_data((struct _response_ctx*)parser->data, (unsigned char const*)at, length);
}

static int request_parse_message_complete_cb(http_parser* parser) {
  return request_parse_message_complete((struct _response_ctx*)parser->data);
}

// Callback implementation for response context
static int request_parse_header_complete(struct _response_ctx* response) {
  uint64_t data_len = response->parser->content_length;
  if (data_len == UINT64_MAX) {
    response->status = IOTA_REQUEST_STATUS_ERROR;
    return -1;
  }
  if (char_buffer_allocate(response->response, data_len) != RC_OK) {
    response->status = IOTA_REQUEST_STATUS_ERROR;
    return -1;
  }
  response->offset = 0;
  return RC_OK;
}

static int request_parse_data(struct _response_ctx* response, const unsigned char* at, size_t length) {
  memcpy(response->response->data + response->offset, at, length);
  response->offset += length;
  return RC_OK;
}

static int request_parse_message_complete(struct _response_ctx* response) {
  response->status = IOTA_REQUEST_STATUS_DONE;
  return RC_OK;
}

/**
 * @brief sent out http request header
 *
 * @param ctx[in] mbedtls context
 * @param http_settings[in] http configurations
 * @param data_length[in] number of bytes need to send
 * @return int number of bytes actually sent
 */
static int http_request_header(mbedtls_ctx_t* ctx, http_info_t const* http_settings, size_t data_length) {
  char header[1024] = {};
  sprintf(header, header_template, http_settings->path, http_settings->host, http_settings->api_version,
          http_settings->content_type, http_settings->accept, data_length);
  return mbedtls_socket_send(ctx, header, strlen(header));
}

/**
 * @brief sent out http payload
 *
 * @param ctx[in] mbedtls context
 * @param data[in] the buffer holding data
 * @param data_length[in] number of bytes should be sent
 * @return retcode_t error code
 */
static retcode_t http_request_data(mbedtls_ctx_t* ctx, char const* const data, size_t data_length) {
  char* ptr = (char*)data;
  while (data_length > 0) {
    int num_sent = mbedtls_socket_send(ctx, ptr, data_length);
    if (num_sent < 0) {
      return RC_UTILS_SOCKET_SEND;
    }
    ptr += num_sent;
    data_length -= num_sent;
  }
  return RC_OK;
}

/**
 * @brief read data from server
 *
 * @param ctx[in] mbedtls context
 * @param response[out] buffer that will hold the data
 * @return retcode_t error code
 */
static retcode_t http_response_read(mbedtls_ctx_t* ctx, char_buffer_t* response) {
  char buffer[RECEIVE_BUFFER_SIZE];
  int num_received = 0;
  // Setup parser settings - callbacks
  http_parser_settings settings = {};
  settings.on_headers_complete = &request_parse_header_complete_cb;
  settings.on_body = &request_parse_data_cb;
  settings.on_message_complete = &request_parse_message_complete_cb;
  // Setup parser
  http_parser parser = {};
  // Setup response structure
  struct _response_ctx response_context = {};
  response_context.parser = &parser;
  response_context.response = response;
  response_context.status = IOTA_REQUEST_STATUS_OK;
  response_context.offset = 0;
  // Initialize parser
  http_parser_init(&parser, HTTP_RESPONSE);
  parser.data = &response_context;
  // Loop over received data
  while ((num_received = mbedtls_socket_recv(ctx, buffer, RECEIVE_BUFFER_SIZE)) > 0) {
    int parsed = http_parser_execute(&parser, &settings, buffer, num_received);
    // A parsing error occured, or an error in a callback
    if (parsed < num_received || response_context.status == IOTA_REQUEST_STATUS_ERROR) {
      return RC_UTILS_SOCKET_RECV;
    } else if (response_context.status == IOTA_REQUEST_STATUS_DONE) {
      return RC_OK;
    }
    memset(buffer, 0, sizeof(buffer));
  }
  if (num_received <= 0) {
    return RC_CCLIENT_HTTP;
  }
  return RC_OK;
}

/**
 * @brief read/write network socket
 *
 * @param service_opaque[in] opaque object of client service
 * @param obj[in] buffer that holding data
 * @param response[out] buffer that receiving data
 * @return retcode_t error code
 */
static retcode_t cclient_socket_send(void const* const service_opaque, char_buffer_t const* const obj,
                                     char_buffer_t* const response) {
  int sockfd = -1;
  retcode_t result = RC_ERROR;
  iota_client_service_t const* const service = (iota_client_service_t const* const)service_opaque;
  http_info_t const* http_settings = &service->http;

  // HTTPS with ca auth
  mbedtls_ctx_t tls_ctx;
  sockfd = mbedtls_socket_connect(&tls_ctx, http_settings->host, http_settings->port, http_settings->ca_pem, NULL, NULL,
                                  &result);
  if (sockfd >= 0) {
    if (http_request_header(&tls_ctx, http_settings, obj->length) != -1) {
      if ((result = http_request_data(&tls_ctx, obj->data, obj->length)) == RC_OK) {
        result = http_response_read(&tls_ctx, response);
      }
    } else {
      result = RC_CCLIENT_HTTP_REQ;
    }
  } else {
    result = RC_UTILS_SOCKET_CONNECT;
  }
  mbedtls_socket_close(&tls_ctx);
  return result;
}

retcode_t iota_service_query(void const* const service_opaque, char_buffer_t const* const obj,
                             char_buffer_t* const response) {
  if (!service_opaque || !obj || !response) {
    return RC_NULL_PARAM;
  }

  retcode_t ret = cclient_socket_send(service_opaque, obj, response);
  size_t retry = 0;
  while (ret == RC_UTILS_SOCKET_RECV) {
    if (retry > CCLIENT_SOCKET_RETRY) {
      break;
    }
    ret = cclient_socket_send(service_opaque, obj, response);
    retry++;
  }
  return ret;
}
