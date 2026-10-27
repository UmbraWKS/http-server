[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

# C HTTP Server

A small multithreaded HTTP/1.1 file server written in C as a learning project. It serves static files over `GET` and accepts file uploads over `POST` (`multipart/form-data`). It uses only POSIX sockets and pthreads, with no external dependencies.

## Features

- `GET /<file>` serves a file from the server folder with the right `Content-Type`
- `POST` with `multipart/form-data` saves the uploaded file into the server folder
- One detached thread per connection
- Connection cap (returns `503` with `Retry-After` when full)
- Receive/send timeouts so idle clients can't hold a thread forever
- Request size limits (`431` for oversized headers, `413` for oversized bodies)
- Path traversal protection on both downloads and uploads
- URL decoding, with query strings and fragments ignored
- Appropriate status codes: `200`, `400`, `404`, `405`, `413`, `431`, `500`, `503`

## Requirements

- Linux (uses `strcasestr`, `strndup` and other GNU/POSIX extensions)
- `gcc` (or another C compiler) and `make`

## Build and run

```sh
make          # builds ./http_server
make run      # creates ./server-files/ if needed and starts the server
make debug    # builds with AddressSanitizer and UBSan, handy while developing
make clean
```

The server listens on port **8080**. The server folder is relative to the directory you start the binary from, so run it from the repository root.

## Usage

Put some files in `server-files/` and fetch them:

```sh
echo "hello" > server-files/hello.txt
curl -i http://localhost:8080/hello.txt
```

Upload a file (the name comes from the multipart `filename`):

```sh
curl -F "file=@photo.png" http://localhost:8080/upload
curl http://localhost:8080/photo.png --output photo.png
```

The path in the URL of a `POST` is not used for storage. Only the filename in the form data matters, and the file always lands in `server-files/`.

## Configuration

All settings are compile-time constants in [`http_server.h`](http_server.h):

| Macro | Default | Meaning |
|---|---|---|
| `PORT` | `8080` | Listening port |
| `SERVER_FOLDER` | `"./server-files/"` | Directory files are served from and saved to |
| `MAX_HEADER_SIZE` | 8 KB | Maximum request header size |
| `MAX_BODY_SIZE` | 10 MB | Maximum upload size |
| `MAX_FILE_NAME_SIZE` | 128 | Maximum file name length |
| `MAX_CLIENTS` | 64 | Simultaneous connections before replying `503` |
| `CONNECTION_TIMEOUT` | 10 s | Receive/send timeout per connection |
| `BUFFER_SIZE` | 64 KB | Per-connection read buffer |

## Project layout

```
.
├── main.c           # server, request parsing, response building
├── http_server.h    # types, status codes and configuration constants
├── Makefile
└── server-files/    # served/uploaded files (created by `make run`)
```

## Known limitations

This is a learning project, not a production server.

- Only `GET` and `POST`. Other methods get `405`
- Every response is sent with `Connection: close`, so there is no keep-alive
- No TLS and no authentication
- Uploads must be `multipart/form-data` with a single file part
- Whole files are loaded into memory for both uploads and downloads
- Uploading a file with an existing name overwrites it
- Only a handful of MIME types are recognized (html, txt, css, js, json, png, jpg, ico). Everything else is `application/octet-stream`
