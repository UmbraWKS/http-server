#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <sys/types.h>

// Implementing the basics
typedef enum {
  HTTP_STATUS_OK = 200,
  HTTP_STATUS_NO_CONTENT = 204,
  HTTP_STATUS_UNAUTHORIZED = 401,
  HTTP_STATUS_NOT_FOUND = 404,
  HTTP_STATUS_METHOD_NOT_ALLOWED = 405,
  HTTP_STATUS_INTERNAL_SERVER_ERROR = 500,
  HTTP_STATUS_CONTENT_TOO_LARGE = 413,
  HTTP_STATUS_BAD_REQUEST = 400,
  HTTP_STATUS_HEADER_TOO_LARGE = 431,
  HTTP_STATUS_SERVICE_UNAVAILABLE = 503,
} http_status_t;

typedef enum {
  HTTP_METHOD_UNKNOWN = 0,
  HTTP_METHOD_GET,
  HTTP_METHOD_POST,
} http_method_t;

typedef enum {
  HTTP_CONNECTION_UNKNOWN = 0,
  HTTP_CONNECTION_CLOSE,
  HTTP_CONNECTION_KEEP_ALIVE,
} http_connection_t;

// port the server listens on
#define PORT 8080
// allocating a max size for the header at 8k
#define MAX_HEADER_SIZE 8192
// buffer to read the incoming requests
#define BUFFER_SIZE 65536
// the root folder containing the server files
#define SERVER_FOLDER "./server-files/"
// maximum limits for some strings
#define MAX_FILE_NAME_SIZE 128
// 10MB max upload size
#define MAX_BODY_SIZE (10L * 1024 * 1024)
// time in seconds before a connection gets closed if nothing is sent
#define CONNECTION_TIMEOUT 10
// max number of clients simultaneously connected
#define MAX_CLIENTS 64

typedef struct {
  // the raw contents received
  char raw_header[MAX_HEADER_SIZE];

  char file_name[MAX_FILE_NAME_SIZE];
  char *mimetype;
  // this is the --boundary in the body
  char *boundary;
  long content_length;
  http_connection_t connection;
  http_method_t method;
  ssize_t header_len; // the length of the header
} http_request_header_t;

/*
 * in case of POST requests the filename is contained in the body,
 * the header will contain only the path
 */
typedef struct {
  // the complete body complete of boundary
  char *request_content;
  // pointer to the start of the actual file inside request_content
  char *body_content;
  char *mimetype;
  char file_name[MAX_FILE_NAME_SIZE];
  long file_length; // length of the file after removing the --boundary data
  size_t bytes_expected;
  size_t bytes_written;
} http_request_body_t;

typedef struct {
  char header[MAX_HEADER_SIZE];

  char file_name[MAX_FILE_NAME_SIZE];
  http_status_t status;
  const char *mimetype;
  http_connection_t connection;
  long content_length;
  ssize_t header_len; // the actual length of the header
} http_response_header_t;

typedef struct {
  char *raw_file;
} http_response_body_t;

#endif
