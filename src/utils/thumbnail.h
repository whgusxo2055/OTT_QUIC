#ifndef UTILS_THUMBNAIL_H
#define UTILS_THUMBNAIL_H

#include <stddef.h>

int video_extract_thumbnail(const char *input_path, const char *output_path, const char *timestamp);
int video_probe_duration(const char *input_path, double *out_seconds);

#endif // UTILS_THUMBNAIL_H
