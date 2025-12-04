#include "utils/thumbnail.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int video_extract_thumbnail(const char *input_path, const char *output_path, const char *timestamp) {
    if (!input_path || !output_path || !timestamp) {
        return -1;
    }
    char cmd[512];
    snprintf(cmd,
             sizeof(cmd),
             "ffmpeg -y -loglevel error -i \"%s\" -ss %s -vframes 1 -q:v 2 \"%s\"",
             input_path,
             timestamp,
             output_path);
    int rc = system(cmd);
    return rc == 0 ? 0 : -1;
}

int video_probe_duration(const char *input_path, double *out_seconds) {
    if (!input_path || !out_seconds) {
        return -1;
    }
    char cmd[512];
    snprintf(cmd,
             sizeof(cmd),
             "ffprobe -v error -show_entries format=duration -of default=noprint_wrappers=1:nokey=1 \"%s\"",
             input_path);
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        return -1;
    }
    char buf[128];
    if (!fgets(buf, sizeof(buf), fp)) {
        pclose(fp);
        return -1;
    }
    pclose(fp);
    *out_seconds = atof(buf);
    return 0;
}
