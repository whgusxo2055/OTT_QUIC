#include "server/upload.h"

#include "utils/thumbnail.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

int handle_upload_request(int fd, const char *headers, const char *body, size_t body_len, db_context_t *db) {
    (void)headers;
    if (!body || body_len == 0 || !db) {
        return -1;
    }

    if (ensure_dir("data/videos") != 0 || ensure_dir("data/thumbs") != 0 || ensure_dir("data/segments") != 0) {
        return -1;
    }

    int video_id = 0;
    char base_name[64];
    snprintf(base_name, sizeof(base_name), "video_%ld", time(NULL));

    char video_path[256];
    snprintf(video_path, sizeof(video_path), "data/videos/%s.mp4", base_name);

    if (write_file(video_path, body, body_len) != 0) {
        return -1;
    }

    double duration = 0.0;
    video_probe_duration(video_path, &duration);

    char thumb_path[256];
    snprintf(thumb_path, sizeof(thumb_path), "data/thumbs/%s.jpg", base_name);
    video_extract_thumbnail(video_path, thumb_path, "00:00:05", 320, 180);

    if (db_create_video(db, base_name, "uploaded", video_path, thumb_path, (int)duration, &video_id) != SQLITE_OK) {
        return -1;
    }

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "tools/segment_video.sh %d %s", video_id, video_path);
    system(cmd);

    char resp[256];
    int len = snprintf(resp,
                       sizeof(resp),
                       "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n{\"status\":\"ok\",\"video_id\":%d}",
                       video_id);
    write(fd, resp, (size_t)len);
    return 0;
}
