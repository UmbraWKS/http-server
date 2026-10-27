#include <asm-generic/socket.h>
#define _GNU_SOURCE
#include "http_server.h"
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

void *handle_client(void *arg);
void construct_get_response(http_request_header_t *request_header,
                            http_response_header_t *response_header,
                            http_response_body_t *response_body);
bool save_file(const char *data, size_t data_len, const char *filename);
void construct_status_response(http_response_header_t *response_header);
bool analyze_request_header(http_request_header_t *header);
bool analyze_request_body(http_request_body_t *request_body,
                          long content_length, const char *boundary);
const char *get_file_extension(const char *file_name);
char *url_decode(const char *src);
const char *get_mime_type(const char *file_ext);
static http_method_t get_request_method(char *method_str, size_t len);
void compose_response_header(http_response_header_t *header);
char *convert_http_status(http_status_t status);
bool parse_received_body(int connfd, http_request_body_t *request_body,
                         http_request_header_t *request_header, char *body,
                         ssize_t body_bytes_received);
bool is_filename_safe(char *filename);
static const char *find_header_value(const char *raw, const char *name);

static atomic_int connected_clients = 0;
int main(void) {
  signal(SIGPIPE, SIG_IGN);
  int sockfd;
  socklen_t clilen;
  struct sockaddr_in servaddr, cliaddr;

  sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd == -1) {
    perror("Socket creation failed...\n");
    exit(1);
  }

  int opt = 1;
  if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
    perror("setsockopt failed...\n");
    exit(1);
  }

  memset(&servaddr, 0, sizeof(servaddr));

  // setting in-bound rules
  servaddr.sin_family = AF_INET;
  servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
  servaddr.sin_port = htons(PORT);

  if (bind(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) != 0) {
    perror("Sock bind failed...\n");
    exit(1);
  }

  if (listen(sockfd, 10) != 0) {
    perror("Listen failed...\n");
    exit(1);
  }

  clilen = sizeof(cliaddr);

  while (1) {
    int connfd = accept(sockfd, (struct sockaddr *)&cliaddr, &clilen);
    if (connfd < 0) {
      perror("Accept failed...\n");
      continue;
    }

    if (atomic_load(&connected_clients) >= MAX_CLIENTS) {
      static const char busy[] = "HTTP/1.1 503 Service Unavailable\r\n"
                                 "Content-Length: 0\r\n"
                                 "Retry-After: 1\r\n"
                                 "Connection: close\r\n\r\n";
      write(connfd, busy, sizeof(busy) - 1);
      close(connfd);
      continue;
    }

    int *arg = malloc(sizeof(int));
    if (!arg) {
      close(connfd);
      continue;
    }
    *arg = connfd;

    atomic_fetch_add(&connected_clients, 1);

    pthread_t client_thread;
    if (pthread_create(&client_thread, NULL, handle_client, arg) != 0) {
      atomic_fetch_sub(&connected_clients, 1);
      free(arg);
      close(connfd);
      continue;
    }
    pthread_detach(client_thread);
  }

  close(sockfd);

  return 0;
}

/*
 * After a connection to the client is established all the operations
 * are handled in this thread.
 */
void *handle_client(void *arg) {
  /*
   * this flag handles the purging of data received via recv() in case the
   * content size is too big, if this is not done the contents get stuck in the
   * kernel and a thread could remain attached
   */
  bool drain = false;

  int connfd = *(int *)arg;
  free(arg);

  struct timeval tv = {.tv_sec = CONNECTION_TIMEOUT};
  setsockopt(connfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(connfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  http_request_header_t *request_header =
      calloc(1, sizeof(http_request_header_t));
  http_request_body_t *request_body = calloc(1, sizeof(http_request_body_t));

  http_response_header_t *response_header =
      calloc(1, sizeof(http_response_header_t));
  http_response_body_t *response_body = calloc(1, sizeof(http_response_body_t));

  char buffer[BUFFER_SIZE];
  // total number of bytes received via recv
  ssize_t total = 0;
  ssize_t body_bytes_received = 0;
  char *header_end = NULL;

  if (!request_header || !request_body || !response_header || !response_body)
    goto cleanup;

  /*
   * 1. Read the headers
   * 2. Separate the headers from the body (or part of the body)
   * 3. Read the content length from the header
   * 4. Using the content length allocate space for the body and copy it (all or
   * just the first part)
   * 5. keep reading in a loop to receive the whole body and copy it to the
   * buffer N.B. Remember to compare the Content-Length with the MAX_BODY_SIZE
   * before starting to read
   */

  // it reads MAX_HEADER_SIZE to make sure it gets all the header, body
  // separation and processing happens later and accounts for the possible parts
  // of it read here
  while (!header_end && total < MAX_HEADER_SIZE) {
    ssize_t n = recv(connfd, buffer + total, sizeof(buffer) - 1 - total, 0);
    if (n <= 0)
      goto cleanup;
    total += n;
    buffer[total] = '\0';
    header_end = strstr(buffer, "\r\n\r\n");
  }

  if (!header_end || (header_end - buffer) + 4 > MAX_HEADER_SIZE) {
    response_header->status = HTTP_STATUS_HEADER_TOO_LARGE;
    construct_status_response(response_header);
    goto send_response;
  }
  request_header->header_len =
      (header_end - buffer) + 4; // +4 because of the delimiters
  // these are the bytes of body received with the header
  body_bytes_received = total - request_header->header_len;

  memcpy(request_header->raw_header, buffer, request_header->header_len - 1);
  request_header->raw_header[request_header->header_len - 1] = '\0';

  if (!analyze_request_header(request_header)) {
    response_header->status = HTTP_STATUS_BAD_REQUEST;
    construct_status_response(response_header);
    goto send_response;
  }

  switch (request_header->method) {
  case HTTP_METHOD_GET:
    construct_get_response(request_header, response_header, response_body);
    break;
  case HTTP_METHOD_POST:
    if (request_header->content_length <= 0 || !request_header->boundary) {
      response_header->status = HTTP_STATUS_BAD_REQUEST;
      drain = request_header->content_length > 0;
    } else if (request_header->content_length > MAX_BODY_SIZE) {
      response_header->status = HTTP_STATUS_CONTENT_TOO_LARGE;
      drain = true;
    } else if (!parse_received_body(connfd, request_body, request_header,
                                    buffer, body_bytes_received)) {
      response_header->status = HTTP_STATUS_BAD_REQUEST;
    } else if (!analyze_request_body(request_body,
                                     request_header->content_length,
                                     request_header->boundary)) {
      response_header->status = HTTP_STATUS_BAD_REQUEST;
    } else if (!save_file(request_body->body_content, request_body->file_length,
                          request_body->file_name)) {
      response_header->status = HTTP_STATUS_INTERNAL_SERVER_ERROR;
    } else {
      response_header->status = HTTP_STATUS_OK;
    }
    construct_status_response(response_header);
    break;
  case HTTP_METHOD_UNKNOWN:
    response_header->status = HTTP_STATUS_METHOD_NOT_ALLOWED;
    construct_status_response(response_header);
    break;
  }

  ssize_t bytes_written;
send_response:
  bytes_written =
      write(connfd, response_header->header, response_header->header_len);

  if (bytes_written < 0)
    goto cleanup;

  if (response_header->content_length > 0) {
    const char *payload = response_body->raw_file
                              ? response_body->raw_file
                              : convert_http_status(response_header->status);

    bytes_written = write(connfd, payload, response_header->content_length);
    if (bytes_written < 0)
      goto cleanup;
  }

cleanup:
  if (request_header && request_header->mimetype)
    free(request_header->mimetype);
  if (request_header && request_header->boundary)
    free(request_header->boundary);
  if (request_body && request_body->request_content) {
    request_body->body_content = NULL;
    free(request_body->request_content);
  }
  if (request_body && request_body->mimetype)
    free(request_body->mimetype);
  if (response_body && response_body->raw_file)
    free(response_body->raw_file);

  if (request_header)
    free(request_header);
  if (request_body)
    free(request_body);
  if (response_header)
    free(response_header);
  if (response_body)
    free(response_body);
  if (drain) {
    shutdown(connfd, SHUT_WR);
    struct timeval tv = {.tv_sec = 2, .tv_usec = 0};
    setsockopt(connfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    size_t drained = 0;
    ssize_t n;
    while (drained < 2 * MAX_BODY_SIZE &&
           (n = recv(connfd, buffer, sizeof(buffer), 0)) > 0) {
      drained += n;
    }
  }

  close(connfd);
  atomic_fetch_sub(&connected_clients, 1);
  return NULL;
}

/*
 * This function handles the body in the request, it requires the request
 * header, body and the bytes of the body received with the header. It first
 * copies the body part received with the header, then it starts to read for the
 * rest.
 */
bool parse_received_body(int connfd, http_request_body_t *request_body,
                         http_request_header_t *request_header, char *body,
                         ssize_t body_bytes_received) {
  request_body->request_content = malloc(request_header->content_length + 1);
  if (!request_body->request_content) {
    perror("[ERROR] failed to allocate memory for body");
    return false;
  }

  // the client may have sent more than content_length
  if (body_bytes_received > request_header->content_length)
    body_bytes_received = request_header->content_length;

  // copying the received body part into the body
  memcpy(request_body->request_content, body + request_header->header_len,
         body_bytes_received);

  request_body->bytes_written = body_bytes_received;
  request_body->bytes_expected = request_header->content_length;

  // keep calling recv() until I have all the body
  while (request_body->bytes_written < request_body->bytes_expected) {
    ssize_t bytes_left =
        request_body->bytes_expected - request_body->bytes_written;
    ssize_t to_read = bytes_left < BUFFER_SIZE ? bytes_left : BUFFER_SIZE;

    ssize_t read_size = recv(
        connfd, request_body->request_content + request_body->bytes_written,
        to_read, 0);

    if (read_size < 0) {
      perror("[ERROR] socket recv() error");
      return false;
    } else if (read_size == 0) {
      perror("[ERROR] client disconnected before finishing upload");
      return false;
    }

    request_body->bytes_written += read_size;
  }

  request_body->request_content[request_header->content_length] = '\0';
  return true;
}

/*
 * Composes the response for a GET request.
 */
void construct_get_response(http_request_header_t *request_header,
                            http_response_header_t *response_header,
                            http_response_body_t *response_body) {

  char file_path[sizeof(SERVER_FOLDER) + MAX_FILE_NAME_SIZE];
  struct stat st;

  if (!is_filename_safe(request_header->file_name)) {
    goto not_found;
  }

  snprintf(file_path, sizeof(file_path), "%s%s", SERVER_FOLDER,
           request_header->file_name);

  if (stat(file_path, &st) != 0 || !S_ISREG(st.st_mode)) {
    goto not_found;
  }

  FILE *file = fopen(file_path, "rb");
  if (!file) {
    goto not_found;
  }

  size_t size = st.st_size;
  response_body->raw_file = malloc(size > 0 ? size : 1);
  if (!response_body->raw_file ||
      fread(response_body->raw_file, 1, size, file) != size) {
    fclose(file);
    free(response_body->raw_file);
    response_body->raw_file = NULL;
    response_header->status = HTTP_STATUS_INTERNAL_SERVER_ERROR;
    construct_status_response(response_header);
    return;
  }
  fclose(file);

  response_header->content_length = size;
  response_header->mimetype =
      get_mime_type(get_file_extension(request_header->file_name));
  response_header->status = HTTP_STATUS_OK;
  compose_response_header(response_header);
  return;

not_found:
  response_header->status = HTTP_STATUS_NOT_FOUND;
  construct_status_response(response_header);
}

/*
 * All the files are saved in the SERVER_FOLDER folder independently
 * Returns true if the file was successfully saved, false if not
 */
bool save_file(const char *data, size_t data_len, const char *filename) {
  if (!is_filename_safe((char *)filename))
    return false;

  char sanitized[MAX_FILE_NAME_SIZE];
  snprintf(sanitized, sizeof(sanitized), "%s", filename);

  size_t folder_size = strlen(SERVER_FOLDER) + MAX_FILE_NAME_SIZE + 1;
  char path[folder_size];
  snprintf(path, sizeof(path), "%s%s", SERVER_FOLDER, sanitized);

  FILE *file = fopen(path, "wb");
  if (!file)
    return false;

  size_t written = fwrite(data, 1, data_len, file);
  fclose(file);

  return written == data_len;
}

/*
 * Composes the header fields of mimetype and content_length that are required
 * to be NOT NULL by compose_response_header, it needs the field status to be
 * NOT NULL.
 */
void construct_status_response(http_response_header_t *response_header) {
  response_header->content_length =
      strlen(convert_http_status(response_header->status));

  response_header->mimetype = "text/plain";

  compose_response_header(response_header);
}

/*
 * analyzes the header of the request and parses the data into the header.
 * Not all the possible header fields are implemented and not all the fields
 * set are actually used in the program.
 */
bool analyze_request_header(http_request_header_t *header) {
  header->mimetype = NULL;
  header->boundary = NULL;
  header->file_name[0] = '\0';
  header->connection = HTTP_CONNECTION_UNKNOWN;
  header->content_length = 0;
  header->method = HTTP_METHOD_UNKNOWN;

  char method_str[8], target[1024];
  if (sscanf(header->raw_header, "%7s %1023s", method_str, target) != 2)
    return false;
  header->method = get_request_method(method_str, strlen(method_str));

  // dropping "?query" and "#fragment", they are not part of the file name
  char *query = strpbrk(target, "?#");
  if (query)
    *query = '\0';

  char *decoded = url_decode(target);
  if (!decoded)
    return false;
  const char *name = decoded;
  while (*name == '/')
    name++;
  if (strlen(name) >= MAX_FILE_NAME_SIZE) {
    free(decoded);
    return false;
  }
  snprintf(header->file_name, MAX_FILE_NAME_SIZE, "%s", name);
  free(decoded);

  const char *value = find_header_value(header->raw_header, "Content-Length");
  if (value) {
    char *endp;
    long len = strtol(value, &endp, 10);
    // -1 marks an invalid value, POST requests with it get rejected
    header->content_length = (endp == value || len < 0) ? -1 : len;
  }

  value = find_header_value(header->raw_header, "Connection");
  if (value) {
    if (strncasecmp(value, "keep-alive", 10) == 0) {
      header->connection = HTTP_CONNECTION_KEEP_ALIVE;
    } else if (strncasecmp(value, "close", 5) == 0) {
      header->connection = HTTP_CONNECTION_CLOSE;
    }
  }

  value = find_header_value(header->raw_header, "Content-Type");
  if (!value)
    return true;
  const char *line_end = value + strcspn(value, "\r\n");

  header->mimetype = strndup(value, strcspn(value, ";\r\n"));

  // Content-Type: multipart/form-data; boundary=----
  const char *b = strcasestr(value, "boundary=");
  if (b && b < line_end) {
    b += 9;
    size_t boundary_len;
    if (*b == '"') {
      b++;
      boundary_len = strcspn(b, "\"\r\n");
    } else {
      boundary_len = strcspn(b, "; \t\r\n");
    }
    if (boundary_len > 0)
      header->boundary = strndup(b, boundary_len);
  }
  return true;
}

/*
 * Returns a pointer to the value of a header or NULL.
 */
static const char *find_header_value(const char *raw, const char *name) {
  char pattern[64];
  snprintf(pattern, sizeof(pattern), "\r\n%s:", name);
  const char *p = strcasestr(raw, pattern);
  if (!p)
    return NULL;
  p += strlen(pattern);
  while (*p == ' ' || *p == '\t')
    p++;
  return p;
}

/*
 * Analyzes the body and parses the contents.
 */
bool analyze_request_body(http_request_body_t *request_body,
                          long content_length, const char *boundary) {
  request_body->mimetype = NULL;
  request_body->body_content = NULL;
  request_body->file_length = 0;

  char *p_headers = request_body->request_content;
  char *start = strstr(p_headers, "\r\n\r\n");
  if (!start)
    return false;
  *start = '\0';
  start += 4;

  char *fn_str = strcasestr(p_headers, "filename=\"");
  if (!fn_str)
    return false;
  fn_str += 10;
  char *fn_end_str =
      strchr(fn_str, '"'); // also getting the pointer to the end position
  if (!fn_end_str)
    return false;
  size_t filename_len = fn_end_str - fn_str;
  // If the file name is too long respond with a server error
  if (filename_len == 0 || filename_len >= MAX_FILE_NAME_SIZE)
    return false;

  memcpy(request_body->file_name, fn_str, filename_len);
  request_body->file_name[filename_len] = '\0';
  if (!is_filename_safe(request_body->file_name))
    return false;

  // the content-type in the body is optional
  char *ct_str = strcasestr(p_headers, "Content-Type: ");
  if (ct_str) {
    ct_str += 14;
    request_body->mimetype = strndup(ct_str, strcspn(ct_str, "\r\n"));
  }

  /*
   * now i have to separate the actual body from the data.
   * The file starts at /r/n/r/n
   * Ends at \r\n<boundary>--\r\n
   */
  char *end =
      request_body->request_content + content_length - strlen(boundary) - 8;

  if (end <= start)
    return false;

  request_body->body_content = start;
  request_body->file_length = end - start;
  return true;
}

/*
 * Returns the request method in the enum format reading the string in the
 * header. Only GET and POST implemented.
 */
static http_method_t get_request_method(char *method_str, size_t len) {
  if (len == 3 && strncmp(method_str, "GET", len) == 0)
    return HTTP_METHOD_GET;
  if (len == 4 && strncmp(method_str, "POST", len) == 0)
    return HTTP_METHOD_POST;

  return HTTP_METHOD_UNKNOWN;
}

/*
 * Returns a pointer to the extension of the file name
 */
const char *get_file_extension(const char *file_name) {
  const char *dot = strrchr(file_name, '.');
  if (!dot || dot == file_name) {
    return "";
  }
  return dot + 1;
}

/*
 * Returns a string that contains the corresponding mimetype to the extension
 * passed. The passed parameter shall be the extension stripped from the file
 * name obtained from the function get_file_extension
 */
const char *get_mime_type(const char *file_ext) {
  if (strcasecmp(file_ext, "html") == 0 || strcasecmp(file_ext, "htm") == 0) {
    return "text/html";
  } else if (strcasecmp(file_ext, "txt") == 0) {
    return "text/plain";
  } else if (strcasecmp(file_ext, "jpg") == 0 ||
             strcasecmp(file_ext, "jpeg") == 0) {
    return "image/jpeg";
  } else if (strcasecmp(file_ext, "png") == 0) {
    return "image/png";
  } else if (strcasecmp(file_ext, "css") == 0) {
    return "text/css";
  } else if (strcasecmp(file_ext, "js") == 0) {
    return "text/javascript";
  } else if (strcasecmp(file_ext, "json") == 0) {
    return "application/json";
  } else if (strcasecmp(file_ext, "ico") == 0) {
    return "image/x-icon";
  } else {
    return "application/octet-stream";
  }
}

/*
 * Returns a pointer to a string contatining the decoded url
 */
char *url_decode(const char *src) {
  size_t src_len = strlen(src);
  char *decoded = malloc(src_len + 1);
  if (!decoded)
    return NULL;
  size_t decoded_len = 0;

  // decode %2x to hex
  for (size_t i = 0; i < src_len; i++) {
    if (src[i] == '%' && i + 2 < src_len) {
      int hex_val;
      sscanf(src + i + 1, "%2x", &hex_val);
      decoded[decoded_len++] = hex_val;
      i += 2;
    } else {
      decoded[decoded_len++] = src[i];
    }
  }

  decoded[decoded_len] = '\0';
  return decoded;
}

/*
 * Composes the complete header string ready to send a response.
 * The passed header shall have the fields status, content_length, mimetype
 * not NULL.
 */
void compose_response_header(http_response_header_t *header) {
  header->header_len = snprintf(header->header, MAX_HEADER_SIZE - 1,
                                "HTTP/1.1 %s\r\n"
                                "Content-Type: %s\r\n"
                                "Content-Length: %ld\r\n"
                                "Connection: close\r\n"
                                "\r\n",
                                convert_http_status(header->status),
                                header->mimetype, header->content_length);
}

/*
 * Returns a string matching the passed status.
 * Even though the status is selected by the programmer from an enum
 * also defined by the programmer, the switch implements a default:
 * that sends 503 in case a new status is implemented and forget to
 * add here.
 */
char *convert_http_status(http_status_t status) {
  switch (status) {
  case HTTP_STATUS_OK:
    return "200 OK";
  case HTTP_STATUS_NOT_FOUND:
    return "404 Not Found";
  case HTTP_STATUS_INTERNAL_SERVER_ERROR:
    return "500 Internal Server Error";
  case HTTP_STATUS_METHOD_NOT_ALLOWED:
    return "405 Method Not Allowed";
  case HTTP_STATUS_NO_CONTENT:
    return "204 No Content";
  case HTTP_STATUS_UNAUTHORIZED:
    return "401 Unauthorized";
  case HTTP_STATUS_CONTENT_TOO_LARGE:
    return "413 Content Too Large";
  case HTTP_STATUS_BAD_REQUEST:
    return "400 Bad Request";
  case HTTP_STATUS_HEADER_TOO_LARGE:
    return "431 Request Header Fields Too Large";
  case HTTP_STATUS_SERVICE_UNAVAILABLE:
  default:
    return "503 Service Unavailable";
  }
}

/*
 * Checks for path traversal.
 * Returns TRUE if the filename is clean.
 */
bool is_filename_safe(char *filename) {
  if (filename[0] == '\0' || filename[0] == '.' || filename[0] == '/')
    return false;
  return strstr(filename, "..") == NULL;
}
