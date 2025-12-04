#include "server/upload.h"

#include "utils/thumbnail.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

static const unsigned char *find_bytes(const unsigned char *haystack, size_t hay_len, const char *needle, size_t needle_len) {
    if (!haystack || !needle || hay_len == 0 || needle_len == 0 || needle_len > hay_len) {
        return NULL;
    }
    for (size_t i = 0; i + needle_len <= hay_len; ++i) {
        if (memcmp(haystack + i, needle, needle_len) == 0) {
            return haystack + i;
        }
    }
    return NULL;
}

static int ensure_dir(const char *path) {
    if (access(path, F_OK) == 0) {
        return 0;
    }
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", path);
    return system(cmd);
}

static int write_file(const char *path, const char *data, size_t len) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    size_t w = fwrite(data, 1, len, fp);
    fclose(fp);
    return w == len ? 0 : -1;
}

static int parse_multipart(const char *body,
                           size_t body_len,
                           const char *boundary,
                           char **out_file,
                           size_t *out_file_len,
                           char *title,
                           size_t title_len,
                           char *desc,
                           size_t desc_len) {
    const unsigned char *p = (const unsigned char *)body;
    const unsigned char *end = p + body_len;
    size_t b_len = strlen(boundary);
    *out_file = NULL;
    *out_file_len = 0;
    if (title) title[0] = '\0';
    if (desc) desc[0] = '\0';

    while (p < end) {
        const unsigned char *b = find_bytes(p, (size_t)(end - p), boundary, b_len);
        if (!b) break;
        p = b + b_len;
        if ((size_t)(end - p) < 2) break;
        if (*p == '-' && *(p + 1) == '-') break;
        const unsigned char *hdr_end = find_bytes(p, (size_t)(end - p), "\r\n\r\n", 4);
        if (!hdr_end) break;
        size_t hdr_len = (size_t)(hdr_end - p);
        char hdr[512] = {0};
        if (hdr_len >= sizeof(hdr)) hdr_len = sizeof(hdr) - 1;
        memcpy(hdr, p, hdr_len);

        const char *disp = strstr(hdr, "Content-Disposition:");
        const char *name = NULL;
        const char *filename = NULL;
        if (disp) {
            name = strstr(disp, "name=\"");
            if (name) name += 6;
            filename = strstr(disp, "filename=\"");
            if (filename) filename += 10;
        }
        const unsigned char *data_start = hdr_end + 4;
        const unsigned char *next = find_bytes(data_start, (size_t)(end - data_start), boundary, b_len);
        if (!next) break;
        size_t data_len = (size_t)(next - data_start);
        if (data_len >= 2 && *(next - 2) == '\r' && *(next - 1) == '\n') {
            data_len -= 2; /* trim \r\n before boundary */
        }

        if (filename) {
            *out_file = malloc(data_len);
            if (*out_file && data_len > 0) {
                memcpy(*out_file, data_start, data_len);
                *out_file_len = data_len;
            }
        } else if (name && strncmp(name, "title", 5) == 0 && title) {
            size_t copy = data_len < title_len - 1 ? data_len : title_len - 1;
            memcpy(title, data_start, copy);
            title[copy] = '\0';
        } else if (name && strncmp(name, "description", 11) == 0 && desc) {
            size_t copy = data_len < desc_len - 1 ? data_len : desc_len - 1;
            memcpy(desc, data_start, copy);
            desc[copy] = '\0';
        }

        p = next;
    }
    return (*out_file && *out_file_len > 0) ? 0 : -1;
}

int handle_upload_request(int fd, const char *headers, const char *body, size_t body_len, db_context_t *db) {
    if (!headers || !body || body_len == 0 || !db) {
        return -1;
    }

    const size_t MAX_UPLOAD_BYTES = (size_t)2 * 1024 * 1024 * 1024ULL; /* 2GB 제한 */
    if (body_len > MAX_UPLOAD_BYTES) {
        const char *resp = "HTTP/1.1 413 Payload Too Large\r\nContent-Type: application/json\r\n"
                           "Access-Control-Allow-Origin: *\r\n"
                           "Access-Control-Allow-Credentials: true\r\n\r\n"
                           "{\"status\":\"error\",\"message\":\"payload-too-large\"}";
        write(fd, resp, strlen(resp));
        return -1;
    }

    int video_id = 0;
    int video_created = 0;
    const char *ct = strstr(headers, "Content-Type: multipart/form-data; boundary=");
    if (!ct) {
        const char *resp = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n"
                           "Access-Control-Allow-Origin: *\r\n"
                           "Access-Control-Allow-Credentials: true\r\n\r\n"
                           "{\"status\":\"error\",\"message\":\"multipart-required\"}";
        write(fd, resp, strlen(resp));
        return -1;
    }
    ct = strchr(ct, '=');
    if (!ct) {
        const char *resp = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n"
                           "Access-Control-Allow-Origin: *\r\n"
                           "Access-Control-Allow-Credentials: true\r\n\r\n"
                           "{\"status\":\"error\",\"message\":\"missing-boundary\"}";
        write(fd, resp, strlen(resp));
        return -1;
    }
    ct++;
    char boundary[128];
    snprintf(boundary, sizeof(boundary), "--%.*s", 100, ct);
    /* 헤더 끝의 개행 제거 */
    size_t bl = strlen(boundary);
    while (bl > 0 && (boundary[bl - 1] == '\r' || boundary[bl - 1] == '\n')) {
        boundary[--bl] = '\0';
    }
    if (bl < 4 || bl >= sizeof(boundary) - 1) {
        const char *resp = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n"
                           "Access-Control-Allow-Origin: *\r\n"
                           "Access-Control-Allow-Credentials: true\r\n\r\n"
                           "{\"status\":\"error\",\"message\":\"invalid-boundary\"}";
        write(fd, resp, strlen(resp));
        return -1;
    }

    char *file_data = NULL;
    size_t file_len = 0;
    char title[128];
    char desc[256];
    if (parse_multipart(body, body_len, boundary, &file_data, &file_len, title, sizeof(title), desc, sizeof(desc)) != 0 || file_len == 0) {
        const char *resp = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n"
                           "Access-Control-Allow-Origin: *\r\n"
                           "Access-Control-Allow-Credentials: true\r\n\r\n"
                           "{\"status\":\"error\",\"message\":\"invalid-multipart\"}";
        write(fd, resp, strlen(resp));
        return -1;
    }
    title[sizeof(title) - 1] = '\0';
    desc[sizeof(desc) - 1] = '\0';

    if (ensure_dir("data/videos") != 0 || ensure_dir("data/thumbs") != 0 || ensure_dir("data/segments") != 0) {
        free(file_data);
        return -1;
    }

    char base_name[64];
    snprintf(base_name, sizeof(base_name), "video_%ld", (long)time(NULL));

    char video_path[256];
    snprintf(video_path, sizeof(video_path), "data/videos/%s.mp4", base_name);

    if (write_file(video_path, file_data, file_len) != 0) {
        free(file_data);
        return -1;
    }
    free(file_data);

    double duration = 0.0;
    video_probe_duration(video_path, &duration);

    char thumb_path[256];
    snprintf(thumb_path, sizeof(thumb_path), "data/thumbs/%s.jpg", base_name);
    video_extract_thumbnail(video_path, thumb_path, "00:00:05");

    if (db_create_video(db,
                        title[0] ? title : base_name,
                        desc[0] ? desc : "uploaded",
                        video_path,
                        thumb_path,
                        NULL,
                        (int)duration,
                        &video_id) != SQLITE_OK) {
        unlink(video_path);
        unlink(thumb_path);
        return -1;
    }
    video_created = 1;

    char cmd[512];
    char segment_dir[256];
    snprintf(segment_dir, sizeof(segment_dir), "data/segments/%d", video_id);

    snprintf(cmd, sizeof(cmd), "tools/segment_video.sh %d %s", video_id, video_path);
    int seg_rc = system(cmd);
    if (seg_rc != 0) {
        fprintf(stderr, "[upload] segment script failed rc=%d for video_id=%d path=%s\n", seg_rc, video_id, video_path);
        if (video_created) {
            db_delete_video_by_id(db, video_id);
        }
        unlink(video_path);
        unlink(thumb_path);
        return -1;
    }
    db_update_video_segment_path(db, video_id, segment_dir);

    char resp[256];
    int len = snprintf(resp,
                       sizeof(resp),
                       "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                       "Access-Control-Allow-Origin: *\r\n"
                       "Access-Control-Allow-Credentials: true\r\n\r\n"
                       "{\"status\":\"ok\",\"video_id\":%d}",
                       video_id);
    write(fd, resp, (size_t)len);
    return 0;
}
